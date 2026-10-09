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
#include "paths.h"
#include "usb_devices.h"
#include "../frontend/settings.h"
#include "../ps5/display.h"
#include "../ps5/kernel.h"
#include "../ps5/log.h"
#include "../ps5/notify.h"
#include "../ps5/pad.h"
#include "../ps5/privilege.h"
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

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <fstream>

extern "C" int32_t sceSystemServiceParamGetInt(int32_t paramId, int32_t* value);
extern uint64 _rdtscFrequency; // Cafe/HW/Espresso/PPCTimer.cpp

// The platform layer's statistics (RADV's link brings it): weak, so a link check without RADV
// still links, the calls skipped
#pragma weak ps5_heap_stats
#pragma weak ps5_exec_live

// MemMapperPS5.cpp
void PS5Cemu_MemMapperUsage(size_t& committed, size_t& jit);
// patches/cemu: Cemu's VulkanPipelineStableCache.cpp
void PS5Cemu_PipelineCacheProgress(uint32& loaded, uint32& queued, uint32& lastIndex);

namespace ps5emu
{
	namespace
	{
		bool s_firstStart = false;
		bool s_coreStarted = false; // Cemu's settings are loaded: this session is Cemu's
		// the draws Cemu records in a command buffer before it submits it (PS5Cemu_SubmitDraws)
		constexpr uint32_t kCemuSubmitDraws = 300;
		std::atomic<uint32_t> s_submitDraws{kCemuSubmitDraws};

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
		// Controllers on the other signed-in users' DualSenses. Only when no profile exists yet.
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
				error = "the PS5 Vulkan driver did not start";
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
			error = fmt::format("PS5CEMU-HAR cannot write to {}. Is the HEN loaded?", _pathToUtf8(*failedWriteAccess.begin()));
			return false;
		}
		cemuLog_createLogFile(false); // log.txt in /data/ps5cemu, as on the desktop
		CreateDirectories(ActiveSettings::GetConfigPath("controllerProfiles"));
		CreateDirectories(fs::path(PS5CEMU_DATA "/amiibo")); // both emulators' in-game menus scan from here
		CreateDirectories(ps5paths::kGames);
		CreateDirectories(ps5paths::kLogs);
		ps5log::Line("[emu] app folder: {}", ps5paths::AppDir());
		// RADV keeps its shader cache in /app0 unless told otherwise, and a jailbroken process has none
		setenv("MESA_SHADER_CACHE_DIR", ps5paths::kRadvCache, 1);

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
			error = fmt::format("PS5CEMU-HAR cannot create the MLC folder {}", _pathToUtf8(ActiveSettings::GetMlcPath()));
			return false;
		}
		InstallBundledGraphicPacks();
		ActiveSettings::Init();
		LibraryInit();
		s_coreStarted = true;
		ps5log::Line("[emu] Cemu's library is ready (the emulated Wii U starts with a game): {} game folders, MLC {}", GetConfig().game_paths.size(), _pathToUtf8(ActiveSettings::GetMlcPath()));
		return true;
	}

	void ApplyOptions(const Options& options)
	{
		auto& config = GetConfig();
		config.tv_volume = std::clamp(options.volume, 0, 100);
		config.upscale_filter = std::clamp(options.upscaleFilter, (int)kLinearFilter, (int)kNearestNeighborFilter);
		config.async_compile = options.asyncShaders;
		// the GamePad's own sound: on the DualSense's speaker (cemu/PS5AudioAPI.h), or nowhere, as
		// before; Cemu opens it as the game starts
		config.pad_device = options.gamePadSpeaker ? L"ps5-padspeaker" : L""; // PS5AudioAPI::kPadSpeakerId
		if (options.gamePadSpeaker && config.pad_volume <= 0)
			config.pad_volume = 100;
		config.overlay.position = options.overlay ? ScreenPosition::kTopLeft : ScreenPosition::kDisabled;
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
			game.name = base.GetMetaTitleName();
			if (game.name.empty())
				game.name = fmt::format("{:016x}", titleId);
			game.path = base.GetPath();
			if (ParsedMetaXml* meta = base.GetMetaInfo())
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
				error = "The game's base files were not found.";
				return false;
			}
			const auto status = CafeSystem::PrepareForegroundTitle(baseTitleId);
			if (status == CafeSystem::PREPARE_STATUS_CODE::UNABLE_TO_MOUNT)
			{
				error = "The game could not be mounted. Check that its files are still in the game files folder.";
				return false;
			}
			if (status != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
			{
				error = "The game could not be started.";
				return false;
			}
		}
		else
		{
			const CafeTitleFileType fileType = DetermineCafeSystemFileType(game.path);
			if (fileType != CafeTitleFileType::RPX && fileType != CafeTitleFileType::ELF)
			{
				error = "This is not a Wii U game PS5CEMU-HAR can start.";
				if (launchTitle.GetInvalidReason() == TitleInfo::InvalidReason::NO_DISC_KEY)
					error += " Its disc key is missing from /data/ps5cemu/keys.txt.";
				else if (launchTitle.GetInvalidReason() == TitleInfo::InvalidReason::NO_TITLE_TIK)
					error += " Its title.tik is missing.";
				return false;
			}
			if (CafeSystem::PrepareForegroundTitleFromStandaloneRPX(game.path) != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
			{
				error = "The executable could not be started.";
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
			error = fmt::format("The Vulkan renderer did not start: {}", ex.what());
			return false;
		}
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
		ps5notify::Send("Touchpad + Options: the PS5 CEMU menu (screens, picture, volume, controls, library)");
		uint64_t polls = 0;
		LogMemory();
		uint32_t loggedFrames = LatteGPUState.frameCounter;
		uint64_t loggedAt = sceKernelGetProcessTime();
		ps5usb::GameStarted(); // the portals the game was started with
		ps5usb::Refresh();
		for (;;)
		{
			sceKernelUsleep(16000);
			// the in-game menu's graphic pack changes and figures, made here as Cemu's windows make them
			ServiceGraphicPackRequests();
			ps5usb::ServiceRequests();
			if (++polls % 120 == 0)
				ps5pad::Rescan(); // controllers joining or leaving, about every two seconds
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
				if (now > loggedAt)
					ps5log::Line("[perf] {:.1f} fps over {:.0f} s; accurate barriers {}{}; heap {} MiB; descriptor sets {}, image samplers {}",
						(frames - loggedFrames) * 1e6 / (now - loggedAt),
						(now - loggedAt) / 1e6, GetConfig().vk_accurate_barriers ? "on" : "off", pipelines, HeapMiB(), performanceMonitor.vk.numDescriptorSets.get(),
						performanceMonitor.vk.numDescriptorSamplerTextures.get());
				loggedFrames = frames;
				loggedAt = now;
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
		// fetched ended the app instead of starting it over (it waits for one a few seconds at most)
		ps5boxart::Stop();
		ps5log::Line("[emu] back to the library: starting PS5Cemu over");
		// Cemu's settings only where Cemu ran: a 3DS session never read them, and would write its
		// defaults over them
		if (s_coreStarted)
			GetConfigHandle().Save();
		// the program the app was mounted from (ShadowMountPlus's record, which follows an install on
		// a USB drive), else the usual install, else the one found running
		std::error_code ec;
		const std::string source = ps5paths::MountSource();
		const std::string eboot = !source.empty() ? source + "/eboot.bin" :
			fs::exists(ps5paths::kMountedEboot, ec) ? ps5paths::kMountedEboot : ps5paths::Eboot();
		const int result = sceSystemServiceLoadExec(eboot.c_str(), nullptr);
		// it does not come back when it works; allow for one that returns before ending the process
		if (result == 0)
			for (int i = 0; i < 100; i++)
				sceKernelUsleep(100000);
		ps5log::Line("[emu] LoadExec({}) returned {:#x}", eboot, (uint32_t)result);
	}
}

// patches/cemu: Cemu's VulkanRenderer::SubmitCommandBuffer, for each command buffer it begins. Each
// submission costs the driver a copy of its words and an AGC submission, and the GPU a reset of its
// state and a write-back of its caches (docs/DRIVER-PERFORMANCE.md)
uint32 PS5Cemu_SubmitDraws()
{
	return ps5emu::s_submitDraws.load(std::memory_order_relaxed);
}
