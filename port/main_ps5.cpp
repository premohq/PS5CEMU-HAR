// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the entry point (Cemu's src/main.cpp on the desktop).
//
//  1. out of the sandbox: /data, and JIT memory for the recompilers (ps5/privilege.h);
//  2. the boot log, the DualSense and Cemu's core (settings, MLC, graphic packs, the game scan);
//  3. the launcher until a game is chosen (frontend/shell.h), on the side last used;
//  4. the game, on Cemu's or Azahar's Vulkan renderer, until the in-game menu (touchpad + Options)
//     asks for the library, which starts PS5CEMU-HAR over (app/emulator.h, RestartToLibrary) on
//     that emulator's side.

#include "app/emulator.h"
#include "app/lang.h"
#include "app/pack_updates.h"
#include "app/paths.h"
#include "app/updates.h"
#include "azahar/azahar.h"
#include "azahar/library.h"
#include "frontend/launcher.h"
#include "frontend/settings.h"
#include "frontend/shell.h"
#include "ps5/crash.h"
#include "ps5/display.h"
#include "ps5/kernel.h"
#include "ps5/log.h"
#include "ps5/notify.h"
#include "ps5/pad.h"
#include "ps5/privilege.h"
#include "ps5/threads.h"
#include "ps5/window.h"

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
			fmt::format("PS5CEMU-HAR {}: Cemu at {}, Azahar at {}", PS5CEMU_VERSION, PS5CEMU_CEMU_COMMIT, PS5CEMU_AZAHAR_COMMIT),
			ps5lang::TrF("Firmware {0}", Firmware()),
			privileges.summary,
			ps5log::Path()[0] ? ps5lang::TrF("Logs: {0}, {1}/log.txt", ps5log::Path(), ps5paths::kRoot) :
								ps5lang::Tr("Boot log not written (/data is unreachable)"),
		};
	}

	ps5emu::Options Options(const ps5settings::Launcher& settings)
	{
		return {settings.gamesFolder, settings.overlay, settings.volume, settings.upscaleFilter, settings.asyncShaders, settings.gamePadSpeaker};
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
}

// What Cemu's src/main.cpp defines for the rest of Cemu.
std::atomic_bool g_isGPUInitFinished = false;
// Cemu's command line asks for a console window with some options; there is none on the PS5.
void requireConsole() {}

int main(int argc, char* argv[])
{
	ps5log::Line("[main] PS5CEMU-HAR {} starting (Cemu at {}, Azahar at {})", PS5CEMU_VERSION, PS5CEMU_CEMU_COMMIT, PS5CEMU_AZAHAR_COMMIT);
	// before any thread starts: the HEN jailbreaks the process as it is
	const ps5privilege::Result privileges = ps5privilege::Acquire();
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
	// still before any thread: the game folders and drives the HEN may have left out (#16)
	if (privileges.filesystem)
		ps5privilege::ReachFolders({settings.gamesFolder, settings.n3ds.gamesFolder});
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
	ps5threads::SetPinning(settings.pinCpuThreads);
	ps5log::ForwardDriverMessages();
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
	ps5pad::Init();
	ps5pad::SetVibrationEnabled(settings.rumble);
	ps5window::Initialize();

	ps5launcher::Status status;
	status.diagnostics = Diagnostics(ps5privilege::Current());
	std::string error;
	if (!privileges.filesystem)
	{
		status.notice = status.notice3ds =
			ps5lang::Tr("PS5CEMU-HAR cannot reach /data. Load a HEN with PPSA99360 in its app jailbreak list, or elfldr, then restart PS5CEMU-HAR.");
		ps5log::Line("[main] {}", status.notice);
		ps5notify::Send(status.notice);
	}
	else if (!settings.launchError.empty())
	{
		// the last game failed after its renderer started, and the process was started over to show it
		(settings.side == "3ds" ? status.launchError3ds : status.launchError) = settings.launchError;
		settings.launchError.clear();
		ps5settings::Save(settings);
	}

	// One emulator per session: neither starts until the start screen's choice (or the side the last
	// game was on), and leaving that side starts the app over. Cemu's core (its guest memory, system
	// threads, crash handler, graphic packs and game scan) runs only for the Wii U; Azahar (its game
	// scan, and its core once a game starts) only for the 3DS.
	// Each time the launcher moves to a side (a game that did not start brings the launcher back).
	// Either side gives way to the other in the same process: Azahar's core and Cemu's emulated
	// Wii U both start only for a game, so until then only their game lists and settings are loaded.
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
		ps5log::Line("[main] the Wii U side: Azahar's core is not started");
		status.diagnostics.push_back(ps5lang::Tr("This session: Cemu (Wii U); Azahar's core is not started"));
		if (!privileges.filesystem)
			return;
		std::string coreError;
		if (!ps5emu::InitializeCore(coreError))
		{
			status.notice = ps5lang::TrF("Cemu did not start: {0}", coreError);
			ps5log::Line("[main] {}", status.notice);
			ps5notify::Send(status.notice);
			return;
		}
		status.coreReady = true;
		ps5emu::ApplyOptions(Options(settings));
		if (!privileges.jit)
			ps5notify::Send(ps5lang::Tr("No JIT memory: Wii U games run on the interpreter, much slower. Is PPSA99360 in your HEN's app jailbreak list?"));
		ps5emu::LogMemory(); // Cemu's start, against the 3DS side's
	};

	if (freshStart && privileges.filesystem)
		ps5update::Start();
	for (;;)
	{
		SetHighFrameRate(false); // the launcher at 59.94 Hz
		ps5display::SetFramePacing(1); // and the 3DS's games every refresh
		std::optional<ps5launcher::Choice> choice;
		const ps5shell::Outcome outcome = ps5shell::Run(settings, status, prepare, choice);
		ps5update::Stop(); // nothing of the launcher's runs beside a game
		ps5packs::Stop();
		if (outcome != ps5shell::Outcome::Chosen || !choice)
		{
			// the launcher could not draw, and there is nothing else to show: the boot log says why. Wait
			// for the player to close the app from the PS5's menu
			ps5log::Line("[main] the launcher could not start or stopped drawing");
			ps5notify::Send(ps5lang::Tr("The launcher could not start. The boot log in /data/ps5cemu/logs says why."));
			for (;;)
				sceKernelUsleep(1000000);
		}
		const ps5emu::Game& game = choice->game;

		if (choice->system == ps5launcher::System::N3ds)
		{
			// VideoOut configured again once the new launcher's surface is gone, as before Cemu's below:
			// Azahar's surface follows on the output the launcher presented on, at 59.94 Hz
			SetHighFrameRate(false);
			if (ps5azahar::LaunchGame(game, settings.n3ds, error))
			{
				ps5azahar::RunGame();
				RememberSide("3ds");
				ps5emu::RestartToLibrary();
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

		ps5emu::ApplyOptions(Options(settings));
		SetHighFrameRate(settings.highFrameRate);
		ps5display::SetFramePacing(settings.framePacing);
		ps5window::Initialize();
		if (ps5emu::LaunchGame(game, error))
		{
			ps5emu::RunGame();
			RememberSide("wiiu");
			ps5emu::RestartToLibrary();
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
