// SPDX-License-Identifier: MPL-2.0
// PS5Cemu: Cemu's core on the PS5 (emulator.h).
//
// CommonInit and CreateDefaultMlcFiles follow Cemu's CemuCommonInit (src/main.cpp) and
// CemuApp::CreateDefaultMLCFiles (src/gui/wxgui/CemuApp.cpp), and LaunchGame its
// MainWindow::FileLoad and VulkanCanvas; this file keeps their MPL-2.0 licence.

#include "emulator.h"
#include "boxart.h"
#include "ingame.h"
#include "pack_updates.h"
#include "lang.h"
#include "paths.h"
#include "zip_games.h"
#include "updates.h"
#include "usb_devices.h"
#include "../frontend/settings.h"
#include "../ps5/display.h"
#include "../ps5/kernel.h"
#include "../ps5/log.h"
#include "../ps5/notify.h"
#include "../ps5/pad.h"
#include "../ps5/privilege.h"
#include "../ps5/threads.h"
#include "ps5platform/exec.h"
#include "ps5platform/heap.h"
#include "PS5PadController.h"

#include "audio/IAudioAPI.h"
#include "audio/IAudioInputAPI.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/TitleList/SaveList.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/ParsedMetaXml.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "input/InputManager.h"
#include "util/crypto/aes128.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <thread>
#include <vector>
#include <x86intrin.h>

extern "C" int32_t sceSystemServiceParamGetInt(int32_t paramId, int32_t* value);
extern uint64 _rdtscFrequency; // Cafe/HW/Espresso/PPCTimer.cpp

// The platform layer's statistics (RADV's link brings it): weak, so a link check without RADV
// still links, the calls skipped
#pragma weak ps5_heap_stats
#pragma weak ps5_exec_live

// MemMapperPS5.cpp
void PS5Cemu_MemMapperUsage(size_t& committed, size_t& jit);
// patches/cemu: the GPU's time on Cemu's command buffers, when measured (VulkanRenderer.cpp)
extern bool g_ps5GpuTimestamps;
extern std::atomic<uint64> g_ps5GpuBusyNs;
// patches/cemu 0038: frames longer than 20, 34 and 50 ms, and the longest (LatteRenderTarget.cpp),
// and the GPU thread's waits on the GPU by what it waits for, in TSC ticks (VulkanRenderer.cpp)
extern std::atomic<uint32> g_ps5FramesOver[3];
extern std::atomic<uint32> g_ps5LongestFrameUs;
extern std::atomic<uint64> g_ps5GpuWaitTicks[6];
// patches/cemu: Cemu's VulkanPipelineStableCache.cpp
void PS5Cemu_PipelineCacheProgress(uint32& loaded, uint32& queued, uint32& lastIndex);

namespace ps5emu
{
	namespace
	{
		bool s_firstStart = false;
		bool s_coreStarted = false; // Cemu's settings are loaded: this session is Cemu's
		std::atomic<bool> s_exitAfterGame{false}; // SetExitAfterGame: read by the in-game menus' threads
		// the draws Cemu records in a command buffer before it submits it (PS5Cemu_SubmitDraws)
		constexpr uint32_t kCemuSubmitDraws = 300;
		std::atomic<uint32_t> s_submitDraws{kCemuSubmitDraws};
		// the frames the GPU may still be drawing as the next is presented (PS5Cemu_FramesInFlight)
		std::atomic<uint32_t> s_framesInFlight{1};

		// What a call to each clock (and a yield) costs on this console, in nanoseconds, measured as a
		// game starts: Cemu's GPU thread takes its millisecond clock at every draw call and its timer on
		// its waits, and those that call into the kernel cost far more here than on a PC.
		void LogClockCosts()
		{
			const auto nsPerCall = [](int count, auto&& call) {
				timespec start{}, end{};
				clock_gettime(CLOCK_MONOTONIC, &start);
				for (int i = 0; i < count; i++)
					call();
				clock_gettime(CLOCK_MONOTONIC, &end);
				return ((end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec)) / count;
			};
			volatile uint64_t sink = 0;
			constexpr int kCount = 5000;
			const double monotonic = nsPerCall(kCount, [&] {
				timespec now;
				clock_gettime(CLOCK_MONOTONIC, &now);
				sink = sink + now.tv_nsec;
			});
			const double tickCount = nsPerCall(kCount, [&] { sink = sink + GetTickCount(); });
			const double timer = nsPerCall(kCount, [&] { sink = sink + HighResolutionTimer::now().getTick(); });
			const double processTime = nsPerCall(kCount, [&] { sink = sink + sceKernelGetProcessTime(); });
			const double tsc = nsPerCall(kCount, [&] { sink = sink + __rdtsc(); });
			const double yield = nsPerCall(kCount / 4, [] { std::this_thread::yield(); });
			ps5log::Line("[clock] ns a call: clock_gettime {:.0f}, Cemu's GetTickCount {:.0f}, its timer {:.0f}, "
						 "sceKernelGetProcessTime {:.0f}, rdtsc {:.0f}, yield {:.0f}",
				monotonic, tickCount, timer, processTime, tsc, yield);
		}

		// draws a frame over Cemu's performance monitor's last few seconds (what its overlay shows)
		uint32_t DrawsPerFrame()
		{
			uint64_t draws = 0, frames = 0;
			for (const auto& cycle : performanceMonitor.cycle)
			{
				draws += cycle.drawCallCounter;
				frames += cycle.frameCounter;
			}
			return frames ? (uint32_t)(draws / frames) : 0;
		}

		// the app's heap in use, MiB (0 without the platform's statistics)
		uint64_t HeapMiB()
		{
			if (!ps5_heap_stats)
				return 0;
			struct ps5_heap_stats stats{};
			ps5_heap_stats(&stats);
			return stats.mapped_bytes >> 20;
		}

		// The ID on a game's box (GameTDB's) from its meta.xml: the product code's last part and the
		// company code's last two digits (WUP-P-ALZE and 0001: ALZE01)
		std::string BoxId(const std::string& productCode, const std::string& companyCode)
		{
			const size_t dash = productCode.rfind('-');
			const std::string product = dash == std::string::npos ? productCode : productCode.substr(dash + 1);
			if (product.size() != 4 || companyCode.size() < 2)
				return {};
			std::string id = product + companyCode.substr(companyCode.size() - 2);
			for (char& c : id)
			{
				c = (char)std::toupper((unsigned char)c);
				if (!std::isalnum((unsigned char)c))
					return {};
			}
			return id;
		}

		bool CreateDirectories(const fs::path& path)
		{
			std::error_code ec;
			return fs::exists(path, ec) || fs::create_directories(path, ec);
		}

		// As CemuApp::CreateDefaultMLCFiles.
		bool CreateDefaultMlcFiles(const fs::path& mlc)
		{
			const fs::path directories[] = {
				mlc,
				mlc / "sys",
				mlc / "usr",
				mlc / "usr/title/00050000", // base
				mlc / "usr/title/0005000c", // dlc
				mlc / "usr/title/0005000e", // update
				mlc / "usr/save/00050010/1004a000/user/common/db", // Mii Maker save folders
				mlc / "usr/save/00050010/1004a100/user/common/db",
				mlc / "usr/save/00050010/1004a200/user/common/db",
				mlc / "sys/title/0005001b/1005c000/content", // lang files
			};
			for (const auto& path : directories)
				if (!CreateDirectories(path))
					return false;
			try
			{
				const auto langDir = mlc / "sys/title/0005001b/1005c000/content";
				if (const auto langFile = langDir / "language.txt"; !fs::exists(langFile))
				{
					std::ofstream file(langFile);
					for (const char* lang : {"ja", "en", "fr", "de", "it", "es", "zh", "ko", "nl", "pt", "ru", "zh"})
						file << fmt::format(R"("{}",)", lang) << std::endl;
				}
				if (const auto countryFile = langDir / "country.txt"; !fs::exists(countryFile))
				{
					std::ofstream file(countryFile);
					for (sint32 i = 0; i < NCrypto::GetCountryCount(); i++)
					{
						const char* countryCode = NCrypto::GetCountryAsString(i);
						if (boost::iequals(countryCode, "NN"))
							file << "NULL," << std::endl;
						else
							file << fmt::format(R"("{}",)", countryCode) << std::endl;
					}
				}
			}
			catch (const std::exception& ex)
			{
				ps5log::Line("[emu] cannot write the MLC's language files: {}", ex.what());
				return false;
			}
			return true;
		}

		// The console's language as the Wii U's (sceSystemServiceParamGetInt, parameter 1).
		CafeConsoleLanguage SystemLanguage()
		{
			int32_t language = 1;
			if (sceSystemServiceParamGetInt(1, &language) != 0)
				return CafeConsoleLanguage::EN;
			switch (language)
			{
			case 0: return CafeConsoleLanguage::JA;
			case 2: case 22: return CafeConsoleLanguage::FR;
			case 3: case 20: return CafeConsoleLanguage::ES;
			case 4: return CafeConsoleLanguage::DE;
			case 5: return CafeConsoleLanguage::IT;
			case 6: return CafeConsoleLanguage::NL;
			case 7: case 17: return CafeConsoleLanguage::PT;
			case 8: return CafeConsoleLanguage::RU;
			case 9: return CafeConsoleLanguage::KO;
			case 10: return CafeConsoleLanguage::TW;
			case 11: return CafeConsoleLanguage::ZH;
			default: return CafeConsoleLanguage::EN;
			}
		}

		// The settings the PS5 needs whatever the file says, and the defaults of a first start.
		void ApplyPlatformSettings()
		{
			auto& config = GetConfig();
			config.graphic_api = kVulkan;
			config.audio_api = IAudioAPI::PS5AudioOut;
			config.tv_device = L"ps5-audioout";
			config.tv_channels = kStereo;
			if (s_firstStart)
			{
				config.mlc_path = ps5paths::kMlc;
				config.game_paths = {ps5paths::kGames};
				config.tv_volume = 100;
				config.pad_device = L""; // no separate GamePad speaker
				config.console_language = SystemLanguage();
				config.async_compile = true;
				config.vsync = 1;
				config.overlay.position = ScreenPosition::kDisabled; // the in-game menu shows it
				config.notification.position = ScreenPosition::kTopLeft;
			}
			if (config.game_paths.empty())
				config.game_paths = {ps5paths::kGames};
		}

		// The community graphic packs bundled with the app (assets/graphicPacks, with its
		// version.txt) go where Cemu's own downloader puts them, once per bundled release, unless
		// newer ones were downloaded there since (pack_updates.h). Packs of your own elsewhere in
		// graphicPacks/ are left alone.
		void InstallBundledGraphicPacks()
		{
			std::error_code ec;
			const fs::path bundled = ps5paths::BundledGraphicPacks();
			const fs::path target = ActiveSettings::GetUserDataPath("graphicPacks/downloadedGraphicPacks");
			std::string bundledVersion, installedVersion;
			{
				std::ifstream file(bundled / "version.txt");
				std::getline(file, bundledVersion);
			}
			if (bundledVersion.empty())
				return;
			{
				std::ifstream file(target / "version.txt");
				std::getline(file, installedVersion);
			}
			if (installedVersion == bundledVersion || ps5packs::Newer(installedVersion, bundledVersion))
				return;
			ps5log::Line("[emu] installing community graphic packs {} (had '{}')", bundledVersion, installedVersion);
			fs::remove_all(target, ec);
			fs::create_directories(target, ec);
			fs::copy(bundled, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
			if (ec)
				ps5log::Line("[emu] graphic packs: {}", ec.message());
		}

		// Player 1 is the GamePad on the DualSense of whoever started PS5Cemu, players 2-4 Pro
		// Controllers on the other signed-in users' DualSenses. Only when no profile exists yet. A
		// game sees players 2-4's only while their DualSense is there (DetachAbsentPlayers).
		void DefaultControllers()
		{
			auto& input = InputManager::instance();
			for (size_t player = 0; player < 4; player++)
			{
				if (input.get_controller(player))
					continue;
				auto pad = std::make_shared<PS5PadController>((int)player);
				const auto type = player == 0 ? EmulatedController::Type::VPAD : EmulatedController::Type::Pro;
				if (auto emulated = input.set_controller(player, type, pad))
				{
					emulated->set_default_mapping(pad);
					input.save(player);
					ps5log::Line("[emu] player {}: {} on the DualSense", player + 1, player == 0 ? "GamePad" : "Pro Controller");
				}
			}
		}

		const char* CpuModeName(CPUMode mode)
		{
			switch (mode)
			{
			case CPUMode::SinglecoreInterpreter: return "interpreter";
			case CPUMode::SinglecoreRecompiler: return "single-core recompiler";
			case CPUMode::DualcoreRecompiler:
			case CPUMode::MulticoreRecompiler: return "multi-core recompiler";
			case CPUMode::Auto: break;
			}
			return "recompiler";
		}

		// As CemuCommonInit.
		// What the launcher's Wii U side needs: Cemu's settings, graphic packs, controllers and game
		// and save lists. The emulated Wii U itself waits for a game (StartSystem), so the 3DS side
		// can follow in the same process with nothing of Cemu's running.
		void LibraryInit()
		{
			AES128_init();
			GetConfigHandle().Load();
			ApplyPlatformSettings();
			GetConfigHandle().Save();
			if (NetworkConfig::XMLExists())
				n_config.Load();
			GraphicPack2::LoadAll();
			InputManager::instance().load();
			DefaultControllers();
			// the players with Wii Remotes point with their DualSenses (PS5PadController.h)
			for (size_t player = 0; player < 4; player++)
			{
				const auto emulated = InputManager::instance().get_controller(player);
				PS5PadController::SetWiiRemote((int)player, emulated && emulated->type() == EmulatedController::Type::Wiimote);
			}
			fsc_init(); // the game scan mounts each title to read it; CafeSystem::Initialize starts it over
			CafeTitleList::Initialize(ActiveSettings::GetUserDataPath("title_list_cache.xml"));
			for (auto& it : GetConfig().game_paths)
				CafeTitleList::AddScanPath(_utf8ToPath(it));
			const fs::path mlcPath = ActiveSettings::GetMlcPath();
			if (!mlcPath.empty())
				CafeTitleList::SetMLCPath(mlcPath);
			CafeTitleList::Refresh();
			CafeSaveList::Initialize();
			if (!mlcPath.empty())
			{
				CafeSaveList::SetMLCPath(mlcPath);
				CafeSaveList::Refresh();
			}
		}

		// The emulated Wii U, once, as a game starts: its timers, crash handler, sound, memory space,
		// IOSU and threads (CafeSystem::Initialize), the overlay and Vulkan
		bool s_systemStarted = false;

		bool StartSystem(std::string& error)
		{
			if (s_systemStarted)
				return true;
			s_systemStarted = true;
			PPCTimer_init();
			ExceptionHandler_Init();
			IAudioAPI::InitializeStatic();
			IAudioInputAPI::InitializeStatic();
			CafeSystem::Initialize();
			LatteOverlay_init();
			if (!InitializeGlobalVulkan())
			{
				error = ps5lang::Tr("the PS5 Vulkan driver did not start");
				return false;
			}
			ps5log::Line("[emu] the emulated Wii U started");
			return true;
		}

		// the game playing and the settings it started with (SetGameSettings)
		uint64_t s_gameTitle = 0;
		std::unique_ptr<ps5settings::Launcher> s_gameSettings;

		// The menu's settings: Cemu's in settings.xml, and those the launcher also has in its file,
		// which it writes into Cemu's when the next game starts (ApplyOptions): into the game's own
		// settings when it has some, else the Wii U side's (ps5settings::SaveChanges).
		void SaveInGameSettings()
		{
			auto& config = GetConfig();
			GetConfigHandle().Save();
			const ps5settings::Launcher before = s_gameSettings ? *s_gameSettings : ps5settings::Load();
			ps5settings::Launcher after = before;
			after.upscaleFilter = config.upscale_filter;
			after.overlay = config.overlay.position != ScreenPosition::kDisabled;
			after.volume = config.tv_volume;
			after.asyncShaders = config.async_compile;
			after.shaderNotices = config.notification.shader_compiling;
			for (int player = 0; player < 4; player++)
				after.touchpadClick[player] = TouchpadClick(player);
			after.framePacing = ps5display::FramePacing();
			ps5settings::SaveChanges("wiiu", s_gameTitle, before, after);
			// what was saved is what the game now has: the next change is measured from it
			s_gameSettings = std::make_unique<ps5settings::Launcher>(after);
		}
	}

	void SetGameSettings(uint64_t titleId, const ps5settings::Launcher& settings)
	{
		s_gameTitle = titleId;
		s_gameSettings = std::make_unique<ps5settings::Launcher>(settings);
	}

	bool InitializeCore(std::string& error)
	{
		if (s_coreStarted)
			return true; // back on the Wii U side after the 3DS's, in the same process
		std::set<fs::path> failedWriteAccess;
		ActiveSettings::SetPaths(false, ps5paths::Eboot(), ps5paths::kRoot, ps5paths::kRoot, ps5paths::kCache,
			ps5paths::CemuData(), failedWriteAccess);
		if (!failedWriteAccess.empty())
		{
			error = ps5lang::TrF("PS5CEMU-HAR cannot write to {0}. Is the HEN loaded?", _pathToUtf8(*failedWriteAccess.begin()));
			return false;
		}
		cemuLog_createLogFile(false); // log.txt in /data/ps5cemu, as on the desktop
		CreateDirectories(ActiveSettings::GetConfigPath("controllerProfiles"));
		CreateDirectories(fs::path(PS5CEMU_DATA "/amiibo")); // both emulators' in-game menus scan from here
		CreateDirectories(ps5paths::kGames);
		CreateDirectories(ps5paths::kLogs);
		ps5log::Line("[emu] app folder: {}", ps5paths::AppDir());
		// (RADV's shader cache folder is set as the app starts: main_ps5.cpp)

		GetConfigHandle().SetFilename(ActiveSettings::GetConfigPath("settings.xml").generic_wstring());
		std::error_code ec;
		s_firstStart = !fs::exists(ActiveSettings::GetConfigPath("settings.xml"), ec);
		NetworkConfig::LoadOnce();
		if (s_firstStart)
		{
			ApplyPlatformSettings();
			GetConfigHandle().Save();
		}
		if (!CreateDefaultMlcFiles(ActiveSettings::GetMlcPath()))
		{
			error = ps5lang::TrF("PS5CEMU-HAR cannot create the MLC folder {0}", _pathToUtf8(ActiveSettings::GetMlcPath()));
			return false;
		}
		InstallBundledGraphicPacks();
		ActiveSettings::Init();
		LibraryInit();
		s_coreStarted = true;
		ps5log::Line("[emu] Cemu's library is ready (the emulated Wii U starts with a game): {} game folders, MLC {}", GetConfig().game_paths.size(), _pathToUtf8(ActiveSettings::GetMlcPath()));
		return true;
	}

	namespace
	{
		std::thread s_coreThread;
		std::atomic<int> s_coreState{0}; // 0 not started, 1 starting, 2 finished
		bool s_coreOk = false;			 // with s_coreError: as InitializeCore said, once finished
		std::string s_coreError;
	}

	void StartCore(const Options& options)
	{
		if (s_coreState != 0)
			return;
		s_coreState = 1;
		const uint64_t started = sceKernelGetProcessTime();
		s_coreThread = std::thread([options, started] {
			std::string error;
			const bool ok = InitializeCore(error);
			if (ok)
				ApplyOptions(options);
			ps5log::Line("[emu] Cemu's library {} in {} ms, beside the launcher", ok ? "loaded" : "failed", (sceKernelGetProcessTime() - started) / 1000);
			s_coreOk = ok;
			s_coreError = error;
			s_coreState = 2;
		});
	}

	bool CoreStarted(bool& ok, std::string& error)
	{
		if (s_coreState != 2)
			return false;
		if (s_coreThread.joinable())
			s_coreThread.join();
		ok = s_coreOk;
		error = s_coreError;
		return true;
	}

	void FinishCoreStart()
	{
		if (s_coreThread.joinable())
			s_coreThread.join();
	}

	void ApplyOptions(const Options& options)
	{
		auto& config = GetConfig();
		config.tv_volume = std::clamp(options.volume, 0, 100);
		config.upscale_filter = std::clamp(options.upscaleFilter, (int)kLinearFilter, (int)kFsrFilter);
		config.async_compile = options.asyncShaders;
		// the GamePad's own sound: on the DualSense's speaker (cemu/PS5AudioAPI.h), or nowhere, as
		// before; Cemu opens it as the game starts
		config.pad_device = options.gamePadSpeaker ? L"ps5-padspeaker" : L""; // PS5AudioAPI::kPadSpeakerId
		if (options.gamePadSpeaker && config.pad_volume <= 0)
			config.pad_volume = 100;
		config.overlay.position = options.overlay ? ScreenPosition::kTopLeft : ScreenPosition::kDisabled;
		// read as Cemu draws its notices, so the in-game menu's change shows at once (#37)
		config.notification.shader_compiling = options.shaderNotices;
		for (int player = 0; player < 4; player++)
			ApplyTouchpadClick(player, options.touchpadClick[player]);
		if (options.overlay)
			config.overlay.fps = config.overlay.cpu_usage = config.overlay.ram_usage = true;
		const bool newFolder = !options.gamesFolder.empty() &&
			(config.game_paths.size() != 1 || config.game_paths.front() != options.gamesFolder);
		if (newFolder)
		{
			config.game_paths = {options.gamesFolder};
			CafeTitleList::ClearScanPaths();
			CafeTitleList::AddScanPath(_utf8ToPath(options.gamesFolder));
			CafeTitleList::Refresh();
			ps5log::Line("[emu] games folder is now {}", options.gamesFolder);
		}
		GetConfigHandle().Save();
	}

	void SetSubmitDraws(int draws)
	{
		s_submitDraws = draws > 0 ? (uint32_t)draws : kCemuSubmitDraws;
		if (draws > 0)
			ps5log::Line("[vulkan] Cemu submits a command buffer every {} draws (cemuSubmitDraws in ps5cemu.json; its own is {})", draws,
				kCemuSubmitDraws);
	}

	void SetFramesInFlight(int frames)
	{
		s_framesInFlight = frames >= 2 ? 2u : 1u;
		if (frames >= 2)
			ps5log::Line("[vulkan] the GPU may still draw the previous two frames as the next is presented (framesInFlight in ps5cemu.json; Cemu's own is 1)");
	}

	bool Scanning()
	{
		return CafeTitleList::IsScanning();
	}

	std::vector<Game> ListGames()
	{
		std::vector<Game> games;
		for (const TitleId titleId : CafeTitleList::GetAllTitleIds())
		{
			GameInfo2 info = CafeTitleList::GetGameInfo(titleId);
			if (!info.IsValid() || info.IsSystemDataTitle())
				continue;
			TitleInfo& base = info.GetBase();
			if (!base.IsValid() || base.GetAppTitleId() != titleId)
				continue; // updates and DLC are listed with their base
			Game game;
			game.titleId = titleId;
			// the game's own name in the menus' language, its English one where it has none
			ParsedMetaXml* meta = base.GetMetaInfo();
			game.name = meta ? meta->GetLongName((CafeConsoleLanguage)ps5lang::NintendoLanguage()) : base.GetMetaTitleName();
			if (game.name.empty())
				game.name = fmt::format("{:016x}", titleId);
			game.path = base.GetPath();
			if (meta)
				game.gameId = BoxId(meta->GetProductCode(), meta->GetCompanyCode());
			game.hasUpdate = info.HasUpdate();
			game.version = game.hasUpdate ? info.GetUpdate().GetAppTitleVersion() : base.GetAppTitleVersion();
			game.dlcCount = (uint32_t)info.GetAOC().size();
			switch (base.GetFormat())
			{
			case TitleInfo::TitleDataFormat::WIIU_ARCHIVE: game.format = "WUA"; break;
			case TitleInfo::TitleDataFormat::WUD:
				game.format = boost::iequals(_pathToUtf8(game.path.extension()), ".wux") ? "WUX" : "WUD";
				break;
			case TitleInfo::TitleDataFormat::NUS: game.format = "NUS"; break;
			case TitleInfo::TitleDataFormat::WUHB: game.format = "WUHB"; break;
			default: game.format = "FOLDER"; break;
			}
			games.push_back(std::move(game));
		}
		// ZIP entries have no Cemu title metadata until the player starts them.
		// The loader will unpack only the chosen game into the private cache.
		for (const auto& folder : GetConfig().game_paths)
			ps5zip::AddArchives(folder, ps5zip::System::WiiU, games);
		std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) { return boost::ilexicographical_compare(a.name, b.name); });
		return games;
	}

	bool LaunchGame(const Game& game, std::string& error)
	{
		ps5log::Line("[emu] launching {} ({:016x}) from {}", game.name, game.titleId, _pathToUtf8(game.path));
		ps5ingame::SetGame(game); // the in-game menu's top: its name, box art and GameTDB's facts
		if (!StartSystem(error))
			return false;
		TitleInfo launchTitle{game.path};
		if (launchTitle.IsValid())
		{
			CafeTitleList::AddTitleFromPath(game.path);
			TitleId baseTitleId;
			if (!CafeTitleList::FindBaseTitleId(launchTitle.GetAppTitleId(), baseTitleId))
			{
				// tr: why a Wii U game did not start
				error = ps5lang::Tr("The game's base files were not found.");
				return false;
			}
			const auto status = CafeSystem::PrepareForegroundTitle(baseTitleId);
			if (status == CafeSystem::PREPARE_STATUS_CODE::UNABLE_TO_MOUNT)
			{
				error = ps5lang::Tr("The game could not be mounted. Check that its files are still in the game files folder.");
				return false;
			}
			if (status != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
			{
				error = ps5lang::Tr("The game could not be started.");
				return false;
			}
		}
		else
		{
			const CafeTitleFileType fileType = DetermineCafeSystemFileType(game.path);
			if (fileType != CafeTitleFileType::RPX && fileType != CafeTitleFileType::ELF)
			{
				error = ps5lang::Tr("This is not a Wii U game PS5CEMU-HAR can start.");
				if (launchTitle.GetInvalidReason() == TitleInfo::InvalidReason::NO_DISC_KEY)
					error += std::string(" ") + ps5lang::Tr("Its disc key is missing from /data/ps5cemu/keys.txt.");
				else if (launchTitle.GetInvalidReason() == TitleInfo::InvalidReason::NO_TITLE_TIK)
					error += std::string(" ") + ps5lang::Tr("Its title.tik is missing.");
				return false;
			}
			if (CafeSystem::PrepareForegroundTitleFromStandaloneRPX(game.path) != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
			{
				error = ps5lang::Tr("The executable could not be started.");
				return false;
			}
		}

		// as VulkanCanvas: the renderer, then the surface for the main window
		try
		{
			g_renderer = std::make_unique<VulkanRenderer>();
			VulkanRenderer::GetInstance()->InitializeSurface({(sint32)ps5display::kWidth, (sint32)ps5display::kHeight}, true);
		}
		catch (const std::exception& ex)
		{
			error = ps5lang::TrF("The Vulkan renderer did not start: {0}", ex.what());
			return false;
		}
		// players 2-4 with no DualSense have no controller in the game (#29), after the game's profile
		// had its say about controllers (PrepareForegroundTitle)
		DetachAbsentPlayers();
		CafeSystem::LaunchForegroundTitle();
		ps5log::Line("[emu] {} is running ({})", CafeSystem::GetForegroundTitleName(), CpuModeName(ActiveSettings::GetCPUMode()));
		// Cemu's own profile for the game (gameProfiles/), which can set the CPU mode and more
		ps5log::Line("[emu] game profile: {}; CPU mode {}", g_current_game_profile->IsLoaded() ?
			(g_current_game_profile->IsDefaultProfile() ? "Cemu's bundled one" : "the user's own") : "none",
			g_current_game_profile->GetCPUMode().has_value() ? "from the profile" : "from Cemu's settings");
		// what the game's speed rests on: Cemu's timers count the monotonic clock's nanoseconds, and
		// the PowerPC's time base is the TSC, measured against that clock at start
		timespec resolution{};
		clock_getres(CLOCK_MONOTONIC, &resolution);
		ps5log::Line("[emu] clocks: monotonic resolution {} ns, TSC {:.2f} MHz", resolution.tv_nsec, _rdtscFrequency / 1e6);
		LogClockCosts();
		return true;
	}

	bool RendererStarted()
	{
		return g_renderer != nullptr;
	}

	// What memory is used and left, once a minute in a game: what keeps growing while a game plays
	// on is a leak. The heap is every malloc and new of the app's (Cemu's, RADV's, Azahar's), which
	// the platform layer serves from direct memory (ps5platform/heap.h); Cemu's guest memory and
	// recompiled code are its MemMapper's; executable direct memory the platform's (exec.h): Azahar's
	// recompiled code, and Cemu's where the HEN gives no JIT memory (it then counts in both).
	void LogMemory()
	{
		size_t flexible = 0, direct = 0;
		off_t start = 0;
		sceKernelAvailableFlexibleMemorySize(&flexible);
		sceKernelAvailableDirectMemorySize(0, (off_t)sceKernelGetDirectMemorySize(), 0, &start, &direct);
		size_t committed = 0, jit = 0;
		PS5Cemu_MemMapperUsage(committed, jit);
		std::string heap = "no heap statistics";
		if (ps5_heap_stats)
		{
			struct ps5_heap_stats stats{};
			ps5_heap_stats(&stats);
			heap = fmt::format("heap {} MiB (peak {} MiB, {} segments, {} arenas, {} from libc)", stats.mapped_bytes >> 20,
				stats.peak_bytes >> 20, stats.segments, stats.arenas, stats.libc_fallbacks);
		}
		uint64_t execRegions = 0, execBytes = 0;
		if (ps5_exec_live)
			ps5_exec_live(&execRegions, &execBytes);
		ps5log::Line("[memory] {}; Cemu: {} MiB committed, {} MiB recompiled code; executable direct memory {} MiB in {} regions; "
			"flexible memory free {} MiB, largest free block of direct memory {} MiB",
			heap, committed >> 20, jit >> 20, execBytes >> 20, execRegions, flexible >> 20, direct >> 20);
	}

	void RunGame()
	{
		// tr: the PS5's notification as a Wii U game starts
		ps5notify::Send(ps5lang::Tr("Touchpad click + Options: the in-game menu (screens, picture, volume, controls, the library)"));
		uint64_t polls = 0;
		LogMemory();
		uint32_t loggedFrames = LatteGPUState.frameCounter;
		uint64_t loggedAt = sceKernelGetProcessTime();
		// where Cemu's GPU thread's time went, as running totals (Cemu patch 0025): waiting for the
		// game's commands (the emulated CPU behind), on the PS5's GPU, or for results the game asked for
		// a game that stops drawing after it has drawn (a hang): where its threads are, at 8 s and again
		// at 20 s, so a thread stuck in one place can be told from one that spins
		uint32_t watchedFrames = LatteGPUState.frameCounter;
		uint64_t lastFrameAt = sceKernelGetProcessTime();
		int hangDumps = 0;
		uint64_t idleAt = performanceMonitor.gpuTime_idleTime.getTotal(), hostGpuAt = performanceMonitor.gpuTime_waitForHostGPU.getTotal(),
				 asyncAt = performanceMonitor.gpuTime_waitForAsync.getTotal(), gpuBusyAt = g_ps5GpuBusyNs.load();
		uint64_t gpuWaitAt[6];
		for (int i = 0; i < 6; i++)
			gpuWaitAt[i] = g_ps5GpuWaitTicks[i].load();
		ps5usb::GameStarted(); // the portals the game was started with
		ps5usb::Refresh();
		for (;;)
		{
			sceKernelUsleep(16000);
			if (LatteGPUState.frameCounter != watchedFrames)
			{
				watchedFrames = LatteGPUState.frameCounter;
				lastFrameAt = sceKernelGetProcessTime();
				hangDumps = 0;
			}
			else if (watchedFrames > 0 && hangDumps < 2)
			{
				const uint64_t stalled = sceKernelGetProcessTime() - lastFrameAt;
				if (stalled >= (hangDumps == 0 ? 8000000u : 20000000u))
				{
					ps5threads::DumpStacks(fmt::format("no frame for {} s", stalled / 1000000).c_str());
					hangDumps++;
				}
			}
			// the in-game menu's graphic pack changes and figures, made here as Cemu's windows make them
			ServiceGraphicPackRequests();
			ps5usb::ServiceRequests();
			if (++polls % 120 == 0)
			{
				ps5pad::Rescan(); // controllers joining or leaving, about every two seconds
				AttachJoinedPlayers(); // and theirs in the game with them
			}
			if (polls % 3750 == 0)
				LogMemory();
			if (polls % 625 == 0)
			{
				// about every ten seconds: the game's frame rate (frames Cemu's GPU thread finished,
				// as its overlay counts them), so settings can be compared from the boot log
				const uint32_t frames = LatteGPUState.frameCounter;
				const uint64_t now = sceKernelGetProcessTime();
				// while the game starts: how far the pipeline cache's loading is, so where a load
				// stops can be told (the same pipeline every time, or anywhere)
				uint32 loaded = 0, queued = 0, lastIndex = 0;
				PS5Cemu_PipelineCacheProgress(loaded, queued, lastIndex);
				const std::string pipelines = frames == loggedFrames || loaded != queued ?
					fmt::format("; pipeline cache {} compiled, {} read of {}", loaded, queued, lastIndex + 1) : std::string();
				const uint64_t idle = performanceMonitor.gpuTime_idleTime.getTotal(), hostGpu = performanceMonitor.gpuTime_waitForHostGPU.getTotal(),
							   async = performanceMonitor.gpuTime_waitForAsync.getTotal();
				const auto percent = [&](uint64_t ticks) { return now > loggedAt ? PPCTimer_tscToMicroseconds(ticks) * 100.0 / (now - loggedAt) : 0.0; };
				// the GPU itself, when its time is measured (profiling runs): how much more it could draw
				const uint64_t gpuBusy = g_ps5GpuBusyNs.load();
				const std::string gpu = g_ps5GpuTimestamps && now > loggedAt ?
					fmt::format("; GPU busy {:.0f}%", (gpuBusy - gpuBusyAt) / 1000.0 * 100.0 / (now - loggedAt)) : std::string();
				// the frames a player feels: how many took longer than 20, 34 and 50 ms, and the longest
				const uint32_t over20 = g_ps5FramesOver[0].exchange(0), over34 = g_ps5FramesOver[1].exchange(0),
							   over50 = g_ps5FramesOver[2].exchange(0), longestUs = g_ps5LongestFrameUs.exchange(0);
				// and what the GPU thread waited on the GPU for: the previous frame at a swap, a swapchain
				// image, its fence, a present to finish, presenting, or another command buffer
				uint64_t gpuWaits[6];
				for (int i = 0; i < 6; i++)
					gpuWaits[i] = g_ps5GpuWaitTicks[i].load();
				// the GPU thread: idle (the emulated CPU behind), or waiting on the PS5's GPU (it behind),
				// or neither (the GPU thread itself the limit)
				if (now > loggedAt)
					ps5log::Line("[perf] {:.1f} fps over {:.0f} s; frames over 20/34/50 ms: {}/{}/{}, the longest {:.1f} ms; GPU thread "
						"idle {:.0f}%, waiting on the GPU {:.0f}% (the previous frame {:.0f}, an image {:.0f}, its fence {:.0f}, a present {:.0f}, "
						"presenting {:.0f}, other {:.0f}), for results {:.0f}%; {} draws a frame{}; accurate barriers {}{}; heap {} MiB; "
						"descriptor sets {}, image samplers {}",
						(frames - loggedFrames) * 1e6 / (now - loggedAt), (now - loggedAt) / 1e6, over20, over34, over50, longestUs / 1000.0,
						percent(idle - idleAt), percent(hostGpu - hostGpuAt), percent(gpuWaits[0] - gpuWaitAt[0]),
						percent(gpuWaits[1] - gpuWaitAt[1]), percent(gpuWaits[2] - gpuWaitAt[2]), percent(gpuWaits[3] - gpuWaitAt[3]),
						percent(gpuWaits[4] - gpuWaitAt[4]), percent(gpuWaits[5] - gpuWaitAt[5]), percent(async - asyncAt), DrawsPerFrame(), gpu,
						GetConfig().vk_accurate_barriers ? "on" : "off", pipelines, HeapMiB(), performanceMonitor.vk.numDescriptorSets.get(),
						performanceMonitor.vk.numDescriptorSamplerTextures.get());
				loggedFrames = frames;
				loggedAt = now;
				idleAt = idle, hostGpuAt = hostGpu, asyncAt = async, gpuBusyAt = gpuBusy;
				for (int i = 0; i < 6; i++)
					gpuWaitAt[i] = gpuWaits[i];
			}
			switch (ps5pad::TakeShortcut())
			{
			case ps5pad::Shortcut::Menu:
				ps5ingame::ToggleMenu();
				break;
			case ps5pad::Shortcut::SwapScreens:
				ps5ingame::SwapScreens();
				ps5log::Line("[ingame] main screen: {}", LatteGPUState.isDRCPrimary ? "GamePad" : "TV");
				break;
			case ps5pad::Shortcut::CornerScreen:
				ps5ingame::ToggleCornerScreen();
				ps5log::Line("[ingame] the other screen in a corner: toggled");
				break;
			case ps5pad::Shortcut::None:
				break;
			}
			if (ps5ingame::TakeSaveRequest())
				SaveInGameSettings();
			if (ps5ingame::TakeLibraryRequest())
			{
				SaveInGameSettings();
				return;
			}
		}
	}

	void RestartToLibrary()
	{
		// nothing of the launcher's still on the network or writing: a restart with a cover being
		// fetched ended the app instead of starting it over (it waits for one a few seconds at most).
		// An update check can still be waiting on the PS5's resolver when a game fails at once, as an
		// Artic Base one can (#45): it is waited for too.
		ps5boxart::Stop();
		ps5update::WaitForCheck(10000);
		ps5log::Line("[emu] back to the library: starting PS5Cemu over");
		// Cemu's settings only where Cemu ran: a 3DS session never read them, and would write its
		// defaults over them
		if (s_coreStarted)
			GetConfigHandle().Save();
		// the program the app was mounted from (ShadowMountPlus's record, which follows an install on
		// a USB drive), the usual install, the one found running and where the PS5 mounts the app: each
		// tried until one takes, as a HEN may start the app from where the console refuses one (#48)
		std::error_code ec;
		std::vector<std::string> eboots;
		auto consider = [&](const std::string& eboot) {
			if (!eboot.empty() && fs::exists(eboot, ec) && std::find(eboots.begin(), eboots.end(), eboot) == eboots.end())
				eboots.push_back(eboot);
		};
		if (const std::string source = ps5paths::MountSource(); !source.empty())
			consider(source + "/eboot.bin");
		consider(ps5paths::kMountedEboot);
		consider(ps5paths::Eboot());
		consider(std::string(ps5paths::kSystemMount) + "/eboot.bin");
		if (eboots.empty())
			eboots.push_back(ps5paths::Eboot()); // none found: the console may still see it
		for (const std::string& eboot : eboots)
		{
			ps5log::FlushAll(); // the log's lines in the file before the process is replaced
			const int result = sceSystemServiceLoadExec(eboot.c_str(), nullptr);
			// it does not come back when it works; allow for one that returns before ending the process
			if (result == 0)
				for (int i = 0; i < 100; i++)
					sceKernelUsleep(100000);
			ps5log::Line("[emu] LoadExec({}) returned {:#x}", eboot, (uint32_t)result);
		}
		// none took: the app ends, which is said rather than it just closing
		ps5notify::Send(ps5lang::Tr("PS5CEMU-HAR could not start itself again to show the library: start it again from the home screen."));
		ps5log::FlushAll();
	}

	void ExitApp()
	{
		ps5boxart::Stop();
		ps5update::WaitForCheck(10000);
		ps5log::Line("[emu] the game is over: PS5Cemu closes (a home screen tile's --exit-after-game)");
		if (s_coreStarted)
			GetConfigHandle().Save();
		ps5log::FlushAll();
		// as Forwarder Manager closes itself: the system ends the process
		const int result = sceSystemServiceLoadExec("exit", nullptr);
		if (result == 0)
			for (int i = 0; i < 100; i++)
				sceKernelUsleep(100000);
		ps5log::Line("[emu] LoadExec(exit) returned {:#x}: the library instead", (uint32_t)result);
	}

	void SetExitAfterGame(bool exit)
	{
		s_exitAfterGame = exit;
	}

	bool ExitAfterGame()
	{
		return s_exitAfterGame;
	}
}

// patches/cemu: Cemu's VulkanRenderer::SubmitCommandBuffer, for each command buffer it begins. Each
// submission costs the driver a copy of its words and an AGC submission, and the GPU a reset of its
// state and a write-back of its caches (docs/DRIVER-PERFORMANCE.md)
uint32 PS5Cemu_SubmitDraws()
{
	return ps5emu::s_submitDraws.load(std::memory_order_relaxed);
}

// patches/cemu 0038: Cemu's VulkanRenderer::SwapBuffer, at each swap: how many frames back it waits
// for (1, the previous one, as Cemu does; 2, the one before it)
uint32 PS5Cemu_FramesInFlight()
{
	return ps5emu::s_framesInFlight.load(std::memory_order_relaxed);
}
