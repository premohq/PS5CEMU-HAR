// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the entry point (Cemu's src/main.cpp on the desktop).
//
//  1. out of the sandbox: /data, and JIT memory for the recompilers (ps5/privilege.h);
//  2. the boot log, the DualSense and Cemu's core (settings, MLC, graphic packs, the game scan);
//  3. the launcher until a game is chosen (frontend/shell.h), on the side last used;
//  4. the game, on Cemu's or Azahar's Vulkan renderer, or melonDS's screens on VideoOut, with its side's
//     settings and the game's own over them (frontend/settings.h, ForGame), until the in-game menu
//     (touchpad + Options) asks for the library, which starts PS5CEMU-HAR over (app/emulator.h,
//     RestartToLibrary) on that emulator's side.

#include "app/achievements.h"
#include "app/emulator.h"
#include "app/forwarders.h"
#include "app/lang.h"
#include "app/mods.h"
#include "app/netfs.h"
#include "app/pack_updates.h"
#include "app/paths.h"
#include "app/updates.h"
#include "app/zip_games.h"
#include "azahar/azahar.h"
#include "azahar/library.h"
#include "frontend/launcher.h"
#include "melonds/library.h"
#include "melonds/melonds.h"
#include "frontend/settings.h"
#include "frontend/shell.h"
#include "ps5/crash.h"
#include "ps5/display.h"
#include "ps5/driver_clock.h"
#include "ps5/kernel.h"
#include "ps5/log.h"
#include "ps5/notify.h"
#include "ps5/pad.h"
#include "ps5/privilege.h"
#include "ps5/profiler.h"
#include "ps5/threads.h"
#include "ps5/window.h"
#include "stream/stream.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

// Cemu's recompiler: the exact stfs conversion, or cvtsd2ss (Cemu patch 0024)
extern bool g_ps5AccurateSingleStores;
// patches/cemu: the GPU's time on Cemu's command buffers (VulkanRenderer.cpp), for profiling runs
extern bool g_ps5GpuTimestamps;
// patches/cemu: Cemu's samplers without custom border colours (VulkanRenderer.cpp)
extern bool g_ps5StandardBorderColors;
// ps5/heap_wrap.c: the title heap's per-thread cache of small blocks (weak: a link check has no heap)
extern "C" __attribute__((weak)) int g_ps5HeapThreadCache;
// patches/cemu: Cemu's millisecond clock on the TSC (Common/unix/platform.cpp)
void PS5Cemu_UseTscTickCount(bool enabled);
// patches/cemu: Cemu's other host clocks on the TSC (Common/unix/platform.cpp)
void PS5Cemu_UseTscClocks(bool enabled);
// patches/cemu: an idle emulated core's spin before it sleeps, in PAUSEs (coreinit_Thread.cpp), and
// the least time between the main core's idle looks at the system events, in TSC ticks (0037)
namespace coreinit
{
	extern uint32_t g_ps5IdleSpinIterations;
	extern uint64_t g_ps5IdleEventTicks;
}
// patches/cemu 0037: how long a thread retries the taken scheduler lock before it sleeps, in PAUSEs
// (coreinit_Scheduler.cpp)
extern uint32_t g_ps5SchedulerSpins;
// patches/compiler-rt: thread_local reads from a per-thread cache (weak: builtins without the patch
// have none)
extern "C" __attribute__((weak)) int __ps5_emutls_fast;
extern "C" uint64_t sceKernelGetTscFrequency();

// The PS4 SDK's version record: its size is set by the caller (libkernel)
struct SceKernelSwVersion
{
	size_t size;
	char text[0x1c];
	uint32_t version;
};
extern "C" int32_t sceKernelGetSystemSwVersion(SceKernelSwVersion* version);

namespace
{
	// The console's firmware, as its settings show it ("11.60"), for the boot log and Diagnostics:
	// most reports hinge on it
	std::string Firmware()
	{
		SceKernelSwVersion version{};
		version.size = sizeof(version);
		if (sceKernelGetSystemSwVersion(&version) != 0)
			return "unknown";
		version.text[sizeof(version.text) - 1] = 0;
		return fmt::format("{} ({:#010x})", version.text, version.version);
	}

	// A diagnostic for SYSTEM_ILLEGAL_FUNCTION_CALL (0xA002030A), which ends the app with no signal for
	// a crash handler: every import as the console resolved it. With /data/ps5cemu/got-range.txt
	// ("SELF GOT_START GOT_END", this build's ELF addresses of this function and of .got, in hex), the
	// .got as loaded goes to logs/got.bin, to be read against the ELF's relocations: an import the
	// console does not give the app points where the others it lacks do. Nothing without the file.
	void DumpImports()
	{
		FILE* range = std::fopen("/data/ps5cemu/got-range.txt", "rb");
		if (!range)
			return;
		char text[128] = {};
		(void)!std::fread(text, 1, sizeof(text) - 1, range);
		std::fclose(range);
		char* at = text;
		const uint64_t self = std::strtoull(at, &at, 16), start = std::strtoull(at, &at, 16), end = std::strtoull(at, &at, 16);
		if (!self || end <= start || end - start > (1u << 20))
			return;
		const uintptr_t base = (uintptr_t)&DumpImports - (uintptr_t)self;
		FILE* out = std::fopen("/data/ps5cemu/logs/got.bin", "wb");
		if (!out)
			return;
		std::fwrite((const void*)(uintptr_t)(start + base), 1, (size_t)(end - start), out);
		std::fclose(out);
		ps5log::Line("[diag] imports: {} slots in logs/got.bin, the eboot {:#x} above its ELF", (end - start) / 8, base);
	}

	std::vector<std::string> Diagnostics(const ps5privilege::Result& privileges)
	{
		return {
			fmt::format("PS5CEMU-HAR {}: Cemu at {}, Azahar at {}, melonDS at {}", PS5CEMU_VERSION, PS5CEMU_CEMU_COMMIT, PS5CEMU_AZAHAR_COMMIT,
				PS5CEMU_MELONDS_COMMIT),
			ps5lang::TrF("Firmware {0}", Firmware()),
			privileges.summary,
			ps5log::Path()[0] ? ps5lang::TrF("Logs: {0}, {1}/log.txt", ps5log::Path(), ps5paths::kRoot) :
								ps5lang::Tr("Boot log not written (/data is unreachable)"),
		};
	}

	ps5emu::Options Options(const ps5settings::Launcher& settings)
	{
		return {settings.gamesFolder, settings.overlay, settings.volume, settings.upscaleFilter, settings.asyncShaders, settings.gamePadSpeaker,
			settings.shaderNotices, {settings.touchpadClick[0], settings.touchpadClick[1], settings.touchpadClick[2], settings.touchpadClick[3]}};
	}

	// The side the launcher opens on when the app starts over (and only then), and why the game did
	// not start, when there is a reason.
	void RememberSide(const char* side, const std::string& launchError = {})
	{
		// the game's menu may have saved settings since the launcher's were read
		ps5settings::Launcher settings = ps5settings::Load();
		settings.side = side;
		settings.launchError = launchError;
		ps5settings::Save(settings);
	}

	// The 119.88 Hz mode for the next game, where the setting asks for it. The driver configures
	// VideoOut once a process, when a renderer first asks for the display's modes, and offers 119.88
	// Hz only to a title whose param.json declares high-frame-rate output. It reads /app0's unless
	// PS5_VIDEOOUT_PARAM_JSON names another, and a process the HEN has jailbroken has no /app0
	// (app/paths.h), so it is told where the app's is; with the setting off it is told of one that
	// is not there, which declares nothing, so the display stays at 59.94 Hz in a sandboxed process too.
	void SetHighFrameRate(bool highFrameRate)
	{
		ps5display::SetHighFrameRate(highFrameRate);
		const std::string paramJson = ps5paths::AppDir() + (highFrameRate ? "/sce_sys/param.json" : "/sce_sys/no-high-frame-rate.json");
		setenv("PS5_VIDEOOUT_PARAM_JSON", paramJson.c_str(), 1);
		// the new launcher opened VideoOut through the driver at 59.94 Hz: the game's rate asked for again
		ps5display::ConfigureOutput(highFrameRate);
	}

	// A home screen tile's arguments (app/forwarders.h, the PS5 Forwarder Format's): --rom <path> names
	// the game to start at once, a full path or a file's name in its side's library, and
	// --exit-after-game closes the app after it instead of showing the library
	struct TileGame
	{
		std::string rom;
		bool exitAfterGame = false;
		ps5launcher::System side = ps5launcher::System::WiiU;
	};

	TileGame ReadArguments(int argc, char* argv[])
	{
		TileGame tile;
		for (int i = 0; i < argc; i++)
		{
			const std::string arg = argv[i] ? argv[i] : "";
			if (arg == "--rom" && i + 1 < argc && argv[i + 1])
				tile.rom = argv[++i];
			else if (arg == "--exit-after-game")
				tile.exitAfterGame = true;
		}
		// the side by the file's type: the DS's and the 3DS's are their own, the rest the Wii U's (.wua,
		// .wud, .wux, .rpx, a folder)
		std::string extension = std::filesystem::path(tile.rom).extension().string();
		for (char& c : extension)
			c = (char)std::tolower((unsigned char)c);
		for (const char* ds : {".nds", ".srl", ".dsi"})
			if (extension == ds)
				tile.side = ps5launcher::System::Nds;
		for (const char* n3ds : {".3ds", ".cci", ".cxi", ".cia", ".3dsx", ".app", ".elf", ".axf", ".z3ds", ".zcci", ".zcxi", ".z3dsx"})
			if (extension == n3ds)
				tile.side = ps5launcher::System::N3ds;
		return tile;
	}

	// Whether the game chosen is the tile's (by its path, or its file's name)
	bool IsTileGame(const TileGame& tile, const ps5emu::Game& game)
	{
		if (tile.rom.empty())
			return false;
		return tile.rom.find('/') == std::string::npos ? game.path.filename().string() == tile.rom : ps5zip::TileKey(game) == tile.rom;
	}
}

// What Cemu's src/main.cpp defines for the rest of Cemu.
std::atomic_bool g_isGPUInitFinished = false;
// Cemu's command line asks for a console window with some options; there is none on the PS5.
void requireConsole() {}

int main(int argc, char* argv[])
{
	ps5log::Line("[main] PS5CEMU-HAR {} starting (Cemu at {}, Azahar at {}, melonDS at {})", PS5CEMU_VERSION, PS5CEMU_CEMU_COMMIT,
		PS5CEMU_AZAHAR_COMMIT, PS5CEMU_MELONDS_COMMIT);
	// before any thread starts: the HEN jailbreaks the process as it is. The game folders are read
	// first, so a HEN is asked only when something the app needs is out of reach without one
	// (ps5cemu.json as it is: the defaults when /data is not open yet)
	const ps5settings::Launcher early = ps5settings::Load();
	const ps5privilege::Result privileges =
		ps5privilege::Acquire({early.gamesFolder, early.n3ds.gamesFolder, early.nds.gamesFolder}, early.alwaysAskHen);
	if (privileges.filesystem)
		ps5log::Open(ps5paths::kLogs);
	ps5log::Line("[main] firmware {}", Firmware());
	ps5log::Line("[main] {}", privileges.summary);
	if (privileges.filesystem)
		DumpImports();
	ps5crash::Install(); // Cemu's own replaces it, in a session Cemu runs in
	{
		// how the console starts the CPU's floating point: desktop systems keep denormals (0x1f80);
		// flush-to-zero (bit 15) or denormals-are-zero (bit 6) would make Cemu's and Azahar's float
		// results differ from a desktop's
		uint32_t mxcsr = 0;
		asm volatile("stmxcsr %0" : "=m"(mxcsr));
		ps5log::Line("[main] MXCSR {:#06x}: flush-to-zero {}, denormals-are-zero {}", mxcsr, mxcsr & 0x8000 ? "on" : "off",
			mxcsr & 0x40 ? "on" : "off");
	}
	ps5threads::Initialize(); // before any thread starts: they inherit the main thread's CPUs

	ps5settings::Launcher settings = ps5settings::Load();
	// the update notice, on a fresh start only (not each time a game hands back to the library)
	const bool freshStart = settings.side.empty();
	const TileGame tile = ReadArguments(argc, argv);
	if (!tile.rom.empty())
		ps5log::Line("[main] a home screen tile: --rom {}{} ({} side)", tile.rom, tile.exitAfterGame ? " --exit-after-game" : "",
			ps5launcher::SideTitle(tile.side));
	// still before any thread: the game folders and drives the HEN may have left out (#16)
	if (privileges.filesystem)
		ps5privilege::ReachFolders({settings.gamesFolder, settings.n3ds.gamesFolder, settings.nds.gamesFolder});
	// the experiment that gives the Wii U's emulated CPU cores and GPU thread cores of their own: once
	// the elevation helper is done (its timing starts threads) and before any thread that should
	// inherit the other CPUs
	ps5threads::SetIsolation(settings.isolateCores);
	// the first thread but main: the boot log's writer, after the elevation helper is done with the
	// process (it must find it single-threaded)
	ps5log::StartWriter();
	{
		// the app's folder decided here, the process's root now final (app/paths.h, #26)
		bool image = false;
		const std::string source = ps5paths::MountSource(&image);
		ps5log::Line("[main] app folder {}{}", ps5paths::AppDir(),
			!source.empty() ? fmt::format(" (mounted from {})", source) : image ? " (mounted from an image)" : "");
	}
	// the menus' language, from the app's assets (so only once its folder is decided), before
	// anything says a word: the PS5's, unless Settings names another
	ps5lang::Load(settings.ui.language);
	// a RetroAchievements account signed in from Settings > Online (6.16): the file alone, no network
	ps5cheevos::Load();
	{
		// the network shares (6.14), as /net/<name>: connected only when a file on one is first read
		std::vector<ps5netfs::Share> shares;
		if (ps5netfs::load(std::string(ps5paths::kRoot) + "/network-shares.txt", shares) && !shares.empty())
		{
			ps5netfs::set_shares(shares);
			ps5log::Line("[netfs] {} network share(s)", shares.size());
		}
	}
	ps5threads::SetPinning(settings.pinCpuThreads);
	// the driver's messages into the boot log (a thread: after the elevation helper too)
	ps5log::ForwardDriverMessages();
	ps5threads::SetCompileBelowNormal(settings.compileBelowNormal);
	// before a game starts: about 20 ns a PAUSE on the console's CPU
	coreinit::g_ps5IdleSpinIterations = (uint32_t)settings.idleSpinUs * 50;
	if (settings.idleSpinUs)
		ps5log::Line("[threads] an idle emulated core spins {} us before it sleeps (idleSpinUs in ps5cemu.json)", settings.idleSpinUs);
	// thread_local reads from a per-thread cache instead of a libkernel call each (fastTls)
	if (&__ps5_emutls_fast)
	{
		__ps5_emutls_fast = settings.fastTls ? 1 : 0;
		ps5log::Line("[threads] thread_local reads: {} (fastTls in ps5cemu.json)",
			settings.fastTls ? "a per-thread cache" : "pthread_getspecific each time");
	}
	// the scheduler lock retried a while before a thread sleeps on it in the kernel (schedulerSpinUs),
	// and the main core's idle loop looking at the system events at most so often (idleEventsUs)
	g_ps5SchedulerSpins = (uint32_t)settings.schedulerSpinUs * 50;
	coreinit::g_ps5IdleEventTicks = (uint64_t)settings.idleEventsUs * (sceKernelGetTscFrequency() / 1000000);
	ps5log::Line("[threads] Cemu's scheduler lock: {} (schedulerSpinUs); the main core's idle loop looks at the system events "
				 "{} (idleEventsUs)",
		settings.schedulerSpinUs ? fmt::format("retried {} us before a thread sleeps on it", settings.schedulerSpinUs) :
								   std::string("a thread sleeps on it at once"),
		settings.idleEventsUs ? fmt::format("every {} us at most", settings.idleEventsUs) : std::string("at every turn"));
	if (settings.profile && privileges.filesystem)
		ps5profile::Start();
	// before Cemu's renderer starts: with the profiler, the [perf] lines say how busy the GPU is too
	g_ps5GpuTimestamps = settings.profile;
	g_ps5StandardBorderColors = settings.standardBorderColors;
	if (settings.standardBorderColors)
		ps5log::Line("[vulkan] Cemu's samplers: standard border colours only (standardBorderColors in ps5cemu.json)");
	// before Cemu starts: its millisecond clock, taken at every draw call
	PS5Cemu_UseTscTickCount(settings.tscTickCount);
	ps5log::Line("[clock] Cemu's millisecond clock: {} (tscTickCount in ps5cemu.json)", settings.tscTickCount ? "the TSC" : "clock_gettime");
	// and its other clocks: the emulated vsync's timer and the audio output's, which the main emulated
	// core reads at every switch between the game's threads (tscClocks), once the TSC's reported rate
	// has checked out: every timer on it runs at that rate
	const bool tscClocks = settings.tscClocks && ps5clock::TscAgrees();
	PS5Cemu_UseTscClocks(tscClocks);
	ps5log::Line("[clock] Cemu's timers: {} (tscClocks in ps5cemu.json)", tscClocks ? "the TSC" : "clock_gettime");
	// before either emulator's Vulkan driver starts: the driver's clock without a system call a read
	// (driverClockTsc in ps5cemu.json)
	if (settings.driverClockTsc)
		ps5clock::UseTscForDriver();
	else
		ps5log::Line("[vulkan] the driver's clock is clock_gettime (driverClockTsc in ps5cemu.json)");
	// the driver's submissions on a thread of its own, off the GPU thread that records them
	// (asyncSubmit; patches/mesa 0008), and how long its waits for the GPU sleep between looks
	// (gpuWaitSleepUs; 0009). Read once, as the driver makes its first submission or wait
	if (settings.asyncSubmit)
		setenv("RADV_PS5_SUBMIT_THREAD", "1", 1);
	if (settings.gpuWaitSleepUs > 0)
		setenv("RADV_PS5_WAIT_SLEEP_US", std::to_string(settings.gpuWaitSleepUs).c_str(), 1);
	ps5log::Line("[vulkan] submissions: {} (asyncSubmit in ps5cemu.json); waits for the GPU sleep {} us between looks "
				 "(gpuWaitSleepUs)", settings.asyncSubmit ? "on the driver's own thread" : "on the thread that records them",
		settings.gpuWaitSleepUs > 0 ? settings.gpuWaitSleepUs : 1000);
	// before Cemu recompiles any of a game's code
	// before the game's threads start: they keep their own lists of small blocks
	if (&g_ps5HeapThreadCache)
	{
		g_ps5HeapThreadCache = settings.heapThreadCache;
		if (settings.heapThreadCache)
			ps5log::Line("[heap] small blocks cached per thread (heapThreadCache in ps5cemu.json)");
	}
	g_ps5AccurateSingleStores = settings.accurateSingleStores;
	if (settings.accurateSingleStores)
		ps5log::Line("[emu] single-precision stores: Cemu's exact conversion (accurateSingleStores in ps5cemu.json)");
	// before either emulator's Vulkan driver starts, which reads them once. RADV keeps its shader
	// cache in /app0 unless told otherwise, and a jailbroken process has none: told here, so the 3DS's
	// side has it too (it was set as Cemu started, which a 3DS game's process never does)
	setenv("MESA_SHADER_CACHE_DIR", ps5paths::kRadvCache, 1);
	// The driver keeps the shaders it compiles for the next start, as on a PC (shaderDiskCache in
	// ps5cemu.json, on by default). Mesa keeps no disk cache in a process whose effective user or
	// group is not its real one, and the HEN makes only the effective ones root: the real ones are
	// made the effective ones.
	if (!settings.shaderDiskCache)
		setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
	else if (geteuid() != getuid() || getegid() != getgid())
	{
		const int user = setreuid(geteuid(), geteuid()), group = setregid(getegid(), getegid());
		ps5log::Line("[vulkan] RADV's shader cache: real user {} (euid {}){}, real group {} (egid {}){}", getuid(), geteuid(),
			user ? " (refused)" : "", getgid(), getegid(), group ? " (refused)" : "");
	}
	ps5log::Line("[vulkan] RADV's shader cache in {}: {}", ps5paths::kRadvCache,
		!settings.shaderDiskCache ? "off (shaderDiskCache in ps5cemu.json)" :
		geteuid() == getuid() && getegid() == getgid() ? "on" : "off (the console kept the real user)");
	// before either emulator's Vulkan driver starts, which reads it once
	if (!settings.radvDebug.empty())
	{
		setenv("RADV_DEBUG", settings.radvDebug.c_str(), 1);
		ps5log::Line("[vulkan] RADV_DEBUG={} (radvDebug in ps5cemu.json)", settings.radvDebug);
	}
	for (const auto& [name, value] : settings.radvEnvironment)
	{
		// the driver's own variables only: the rest of the environment is the app's
		const bool driver = name.rfind("RADV_", 0) == 0 || name.rfind("MESA_", 0) == 0 || name.rfind("ACO_", 0) == 0;
		if (!driver || name == "RADV_DEBUG")
		{
			ps5log::Line("[vulkan] {} in radvEnvironment left out: only RADV_, MESA_ and ACO_ names, and radvDebug for RADV_DEBUG", name);
			continue;
		}
		setenv(name.c_str(), value.c_str(), 1);
		ps5log::Line("[vulkan] {}={} (radvEnvironment in ps5cemu.json)", name, value);
	}
	ps5emu::SetSubmitDraws(settings.cemuSubmitDraws);
	ps5emu::SetFramesInFlight(settings.framesInFlight);
	ps5pad::Init();
	ps5pad::SetVibrationEnabled(settings.rumble);
	ps5window::Initialize();
	// the remote touch screen, when it is on: the app's, so it runs from here through the game
	ps5stream::Configure(settings.remoteScreen);

	ps5launcher::Status status;
	status.diagnostics = Diagnostics(ps5privilege::Current());
	status.autoLaunch = tile.rom;
	status.autoLaunchSide = tile.side;
	// after the game: the library, or for a tile's game (--exit-after-game) the PS5's home screen, as
	// the in-game menus say (SetExitAfterGame, below)
	auto afterGame = [&](const char* side) {
		if (ps5emu::ExitAfterGame())
			ps5emu::ExitApp();
		RememberSide(side);
		ps5emu::RestartToLibrary();
	};
	std::string error;
	if (!privileges.filesystem)
	{
		status.notice = status.notice3ds = status.noticeDs =
			ps5lang::Tr("PS5CEMU-HAR cannot reach /data. Load a HEN with PPSA99360 in its app jailbreak list, or elfldr, then restart PS5CEMU-HAR.");
		ps5log::Line("[main] {}", status.notice);
		ps5notify::Send(status.notice);
	}
	else if (!settings.launchError.empty())
	{
		// the last game failed after its renderer started, and the process was started over to show it
		(settings.side == "3ds" ? status.launchError3ds : settings.side == "ds" ? status.launchErrorDs : status.launchError) = settings.launchError;
		settings.launchError.clear();
		ps5settings::Save(settings);
	}

	// One emulator per session: none starts until a side opens (the side last used, or the chooser's).
	// Cemu's core (its guest memory, system threads, crash handler, graphic packs and game scan) runs
	// only for the Wii U; Azahar (its game scan, and its core once a game starts) only for the 3DS;
	// melonDS (its game scan, and its core once a game starts) only for the DS.
	// Each time the launcher moves to a side (a game that did not start brings the launcher back).
	// Each side gives way to another in the same process: the emulators' cores start only for a game,
	// so until then only their game lists and settings are loaded.
	std::optional<ps5launcher::System> started;
	const size_t sessionLine = status.diagnostics.size();
	auto prepare = [&](ps5launcher::System system) {
		if (started == system)
			return;
		started = system;
		status.diagnostics.resize(sessionLine);
		if (system == ps5launcher::System::N3ds)
		{
			ps5log::Line("[main] the 3DS side: Cemu's emulated Wii U is not started");
			status.diagnostics.push_back(ps5lang::Tr("This session: Azahar (3DS); Cemu's emulated Wii U is not started"));
			ps5azahar::StartScan(settings.n3ds.gamesFolder);
			ps5emu::LogMemory(); // the 3DS side's start, against Cemu's
			return;
		}
		if (system == ps5launcher::System::Nds)
		{
			ps5log::Line("[main] the DS side: Cemu's emulated Wii U is not started");
			status.diagnostics.push_back("This session: melonDS (DS); Cemu's emulated Wii U is not started");
			ps5melonds::StartScan(settings.nds.gamesFolder);
			ps5emu::LogMemory();
			return;
		}
		ps5log::Line("[main] the Wii U side: Azahar's core is not started");
		status.diagnostics.push_back(ps5lang::Tr("This session: Cemu (Wii U); Azahar's core is not started"));
		if (!privileges.filesystem || status.coreReady || status.coreStarting)
			return;
		// Cemu's library loads on a thread of its own while the launcher draws, from its catalogue: the
		// app opens at once (the launcher polls it, and says why when it fails)
		status.coreStarting = true;
		ps5emu::StartCore(Options(settings));
		if (!privileges.jit)
			ps5notify::Send(ps5lang::Tr("No JIT memory: Wii U games run on the interpreter, much slower. Is PPSA99360 in your HEN's app jailbreak list?"));
	};

	if (freshStart && privileges.filesystem)
		ps5update::Start();
	// how long the app took to get here, from the PS5 starting it: the launcher's first frame follows
	ps5log::Line("[main] the launcher starts {} ms into the app", sceKernelGetProcessTime() / 1000);
	for (;;)
	{
		SetHighFrameRate(false); // the launcher at 59.94 Hz
		ps5display::SetFramePacing(1); // and the 3DS's games every refresh
		std::optional<ps5launcher::Choice> choice;
		const ps5shell::Outcome outcome = ps5shell::Run(settings, status, prepare, choice);
		const uint64_t stopping = sceKernelGetProcessTime();
		ps5emu::FinishCoreStart(); // a 3DS or DS game chosen while Cemu's library was still loading waits for it
		ps5update::Stop(); // nothing of the launcher's runs beside a game
		ps5packs::Stop();
		ps5mods::Stop();
		ps5forwarders::Stop();
		ps5log::Line("[main] the launcher's work stopped in {} ms", (sceKernelGetProcessTime() - stopping) / 1000);
		if (outcome != ps5shell::Outcome::Chosen || !choice)
		{
			// the launcher could not draw, and there is nothing else to show: the boot log says why. Wait
			// for the player to close the app from the PS5's menu
			ps5log::Line("[main] the launcher could not start or stopped drawing");
			// the splash goes, so the notification is seen, and it says why (as 3.0.0's did)
			sceSystemServiceHideSplashScreen();
			const std::string& why = ps5shell::LastFailure();
			ps5notify::Send(why.empty() ? std::string(ps5lang::Tr("The launcher could not start. The boot log in /data/ps5cemu/logs says why.")) :
										  ps5lang::TrF("The launcher could not start: {0}. The boot log in /data/ps5cemu/logs has more.", why));
			for (;;)
				sceKernelUsleep(1000000);
		}
		const ps5emu::Game& game = choice->game;
		const char* side = ps5launcher::SideName(choice->system);
		// a tile's game that closes the app after it: its in-game menu says so instead of the library (#48)
		ps5emu::SetExitAfterGame(tile.exitAfterGame && IsTileGame(tile, game));
		// the side's settings, and the game's own over them
		const ps5settings::Launcher forGame = ps5settings::ForGame(settings, side, game.titleId);
		if (settings.games.count(ps5settings::GameKey(side, game.titleId)))
			ps5log::Line("[main] {} starts with settings of its own", game.name);
		// a handheld's touchpad is its touch screen, whatever a Wii U player's click was made earlier on
		if (choice->system != ps5launcher::System::WiiU)
			for (int player = 0; player < ps5pad::kMaxPlayers; player++)
				ps5pad::SetTouchpadButton(player, false);
		// the side's turbo button, held, presses again and again in the game (Settings > Controls)
		{
			const std::string& turbo = choice->system == ps5launcher::System::N3ds ? forGame.n3ds.turbo :
				choice->system == ps5launcher::System::Nds						   ? forGame.nds.turbo :
																					 forGame.turbo;
			ps5pad::SetTurbo(ps5pad::ButtonNamed(turbo));
			if (!turbo.empty())
				ps5log::Line("[main] turbo button: {}", turbo);
		}

		// Extract on demand, not during library scans. The user can keep ZIPs on USB or
		// a network share; the checked output lives in /data/ps5cemu/cache/zip-games.
		// Keep game.titleId and the original path for settings, favourites and tiles.
		ps5emu::Game preparedGame;
		if (!ps5zip::Prepare(game, preparedGame, error))
		{
			ps5log::Line("[zip] {} could not be prepared: {}", game.name, error);
			if (choice->system == ps5launcher::System::Nds)
			{
				status.noticeDs = error;
				settings.side = "ds";
			}
			else if (choice->system == ps5launcher::System::N3ds)
			{
				status.launchError3ds = error;
				settings.side = "3ds";
			}
			else
			{
				status.launchError = error;
				settings.side = "wiiu";
			}
			continue;
		}

		if (choice->system == ps5launcher::System::Nds)
		{
			// melonDS, its screens on VideoOut at 59.94 Hz, as Azahar's below; its threads on every CPU
			SetHighFrameRate(false);
			ps5threads::EndIsolation();
			if (ps5melonds::LaunchGame(preparedGame, forGame.nds, error))
			{
				ps5melonds::RunGame();
				afterGame("ds");
				return 0;
			}
			ps5log::Line("[main] {} did not start: {}", game.name, error);
			if (ps5melonds::CoreTouched())
			{
				// the screens took VideoOut: show why from a fresh process
				RememberSide("ds", error);
				ps5emu::RestartToLibrary();
				return 1;
			}
			status.noticeDs = "The game could not start: " + error;
			settings.side = "ds";
			continue;
		}

		if (choice->system == ps5launcher::System::N3ds)
		{
			// VideoOut configured again once the new launcher's surface is gone, as before Cemu's below:
			// Azahar's surface follows on the output the launcher presented on, at 59.94 Hz; its threads
			// on every CPU
			SetHighFrameRate(false);
			ps5threads::EndIsolation();
			if (ps5azahar::LaunchGame(preparedGame, forGame.n3ds, error))
			{
				ps5azahar::RunGame();
				afterGame("3ds");
				return 0;
			}
			ps5log::Line("[main] {} did not start: {}", game.name, error);
			if (ps5azahar::CoreTouched())
			{
				// Azahar's core is half started: show why from a fresh process
				RememberSide("3ds", error);
				ps5emu::RestartToLibrary();
				return 1;
			}
			status.launchError3ds = error;
			settings.side = "3ds"; // the launcher shows why on Azahar's side
			continue;
		}

		ps5emu::ApplyOptions(Options(forGame));
		ps5emu::SetGameSettings(game.titleId, forGame);
		SetHighFrameRate(forGame.highFrameRate);
		ps5display::SetFramePacing(forGame.framePacing);
		ps5window::Initialize();
		if (ps5emu::LaunchGame(preparedGame, error))
		{
			ps5emu::RunGame();
			afterGame("wiiu");
			return 0;
		}
		ps5log::Line("[main] {} did not start: {}", game.name, error);
		if (!ps5emu::RendererStarted())
		{
			status.launchError = error;
			settings.side = "wiiu";
			continue;
		}
		// Cemu's renderer holds VideoOut: show the reason from a fresh process
		RememberSide("wiiu", error);
		ps5emu::RestartToLibrary();
		return 1;
	}
}
