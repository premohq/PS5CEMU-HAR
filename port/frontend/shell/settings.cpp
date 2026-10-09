// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: Settings (docs/UI-REDESIGN.md, 6.5): what the side you are on can change, found at a
// glance. One short list of pages with icons down the left, the side's own and the few every side
// shares, and nothing of another side; the page's rows across the rest of the screen, each a control
// (a toggle, a slider, pips, segments, swatches), the focused one with a line saying what it does, its
// longer help behind Triangle. Game settings are the same pages, scoped to one game: only what a game
// can have its own of, each value its side's (Default) until changed, Square back to Default.

#include "shell_internal.h"
#include "../actions.h"
#include "../sound.h"
#include "../../app/boxart.h"
#include "../../app/pack_updates.h"
#include "../../app/paths.h"
#include "../../app/updates.h"
#include "../../app/usb_devices.h"
#include "../../azahar/azahar.h"
#include "../../azahar/controls.h"
#include "../../azahar/library.h"
#include "../../melonds/library.h"
#include "../../melonds/melonds.h"
#include "../../ps5/privilege.h"
#include "../../ps5/display.h"
#include "../../ps5/log.h"
#include "../../ps5/pad.h"

#include <algorithm>
#include <cmath>

namespace ps5shell
{
	using namespace ui::tokens;
	using namespace ps5actions;

	namespace
	{
		constexpr const char* kBorderThemes[] = {"None", "Midnight", "Waves", "Aurora", "Shell", "PS5CEMU-HAR"};
		constexpr const char* kRegions[] = {"Automatic", "Japan", "USA", "Europe", "Australia", "China", "Korea", "Taiwan"};
		constexpr const char* kLanguages[] = {"Automatic", "Japanese", "English", "French", "German", "Italian", "Spanish",
			"Chinese (simplified)", "Korean", "Dutch", "Portuguese", "Russian", "Chinese (traditional)"};
		constexpr const char* kResolutions[] = {"1x (400 × 240)", "2x (800 × 480)", "3x (1200 × 720)", "4x (1600 × 960)", "5x (2000 × 1200)",
			"6x (2400 × 1440)", "7x (2800 × 1680)", "8x (3200 × 1920)", "9x (3600 × 2160)", "10x (4000 × 2400)"};
		constexpr const char* kLayouts[] = {"Top above bottom", "Top screen only", "Large top screen", "Side by side"};
		constexpr const char* kTextureFilters[] = {"None", "Anime4K", "Bicubic", "ScaleForce", "xBRZ", "MMPX"};
		constexpr const char* kUpscaleFilters[] = {"Linear", "Bicubic", "Bicubic Hermite", "Nearest neighbour"};
		constexpr const char* kMusic[] = {"setup", "off"};
		constexpr const char* kMusicNames[] = {"Setup theme", "Off"};
		constexpr int kHolds[] = {400, 600, 800, 1000, 1200, 1500};
		constexpr int kClocks[] = {25, 50, 75, 100, 125, 150, 200, 300, 400}; // the 3DS's CPU clock, as its in-game menu has them
		constexpr const char* kDsFilters[] = {"Sharp", "Smooth", "Square pixels"};

		int ClockIndex(int clock)
		{
			for (int i = 0; i < (int)std::size(kClocks); i++)
				if (kClocks[i] >= clock)
					return i;
			return (int)std::size(kClocks) - 1;
		}

		std::vector<std::string> Options(std::initializer_list<const char*> list)
		{
			return std::vector<std::string>(list.begin(), list.end());
		}

		template<size_t N>
		std::vector<std::string> Options(const char* const (&list)[N])
		{
			return std::vector<std::string>(std::begin(list), std::end(list));
		}

		std::string Percent(int value)
		{
			return fmt::format("{}%", value);
		}
	}

	std::vector<SettingsPage> Shell::SettingsPages() const
	{
		// the side's pages, as docs/UI-REDESIGN.md 6.5 lists them; the shared ones on every side
		std::vector<SettingsPage> pages;
		const char* every = IsWiiU() ? "For every Wii U game." : Is3ds() ? "For every 3DS game." : "For every DS game.";
		pages.push_back({"graphics", "Graphics", every, Icon::Monitor});
		if (!IsWiiU())
			pages.push_back({"screens", "Screens", every, Icon::Screens});
		pages.push_back({"audio", "Audio", "The games' sound, and the launcher's.", Icon::Speaker});
		pages.push_back({"controls", "Controls", IsWiiU() ? "What each player's DualSense is to the Wii U." : Is3ds() ? "The DualSense as the 3DS." : "The DualSense as the DS.",
			Icon::Pad});
		if (IsWiiU())
			pages.push_back({"usb", "USB devices", "Cemu's emulated portals.", Icon::Usb});
		if (!IsWiiU())
			pages.push_back({"system", "System", Is3ds() ? "The emulated 3DS's region, language and speed." : "The emulated DS's language and BIOS.", Icon::Chip});
		pages.push_back({"files", "Game files", IsWiiU() ? "Where your Wii U games are, and installs into the Wii U's storage." :
										Is3ds()		 ? "Where your 3DS games are, and installs into the 3DS's storage." :
													   "Where your DS games are, and where their saves go.",
			Icon::Folder});
		if (Is3ds())
			pages.push_back({"artic", "Artic Base", "Play from your 3DS, over your network.", Icon::Artic});
		pages.push_back({"online", "Online", IsWiiU() ? "Box art, graphic packs and PS5CEMU-HAR itself." : "Box art and PS5CEMU-HAR itself.", Icon::Globe});
		pages.push_back({"launcher", "Launcher", "Where it opens, how it looks, and how it reads.", Icon::Sparkle});
		pages.push_back({"diagnostics", "Diagnostics", "The Setup check, logs, caches and what this session is.", Icon::Pulse});
		pages.push_back({"about", "About", "Who made what.", Icon::Info});
		if (!InGameSettings())
			return pages;
		// a game's settings: only the pages with something a game can have its own of (SideRows' rows
		// with a setting: Graphics, Screens, Audio and System)
		std::vector<SettingsPage> game;
		for (const SettingsPage& page : pages)
			if (page.id == "graphics" || page.id == "screens" || page.id == "audio" || page.id == "system")
				game.push_back({page.id, page.title, "For " + m_gameName + " only. Its other settings are its side's.", page.icon});
		return game;
	}

	void Shell::SettingsOpen(const std::string& page, bool onRows, int row)
	{
		if (InGameSettings())
			GameSettingsClose();
		m_page = page;
		m_onRows = onRows;
		m_settingRow = row;
		m_onBar = false;
		m_tab = ScreenId::Settings;
		m_holdProgress = 0;
		m_message.clear();
		Show(ScreenId::Settings);
	}

	std::string Shell::PacksStatus() const
	{
		using State = ps5packs::Status::State;
		const auto status = ps5packs::GetStatus();
		const std::string installed = ps5packs::InstalledVersion();
		switch (status.state)
		{
		case State::Checking: return "Checking GitHub…";
		case State::Downloading:
			return status.total ? fmt::format("Downloading: {}%", (int)(status.received * 100 / status.total)) :
								  fmt::format("Downloading: {} MB", status.received >> 20);
		case State::Installing: return "Installing…";
		case State::UpToDate: return "Up to date: " + installed;
		case State::Done: return "Installed: " + status.version;
		case State::Failed: return status.message;
		case State::Idle: break;
		}
		return installed.empty() ? "Cross to download" : installed;
	}

	std::string Shell::AppUpdateStatus()
	{
		using State = ps5update::Status::State;
		const auto status = ps5update::GetStatus();
		switch (status.state)
		{
		case State::Checking: return "Checking GitHub…";
		case State::UpToDate: return "Up to date: " + ps5update::Readable(PS5CEMU_VERSION);
		case State::Available: return ps5update::Readable(status.latest) + ": Cross to install";
		case State::Downloading:
			return status.total ? fmt::format("Downloading: {}%", (int)(status.received * 100 / status.total)) : "Downloading…";
		case State::Verifying: return "Checking the download…";
		case State::Installing: return "Installing…";
		case State::Ready: return "Installed: Cross to restart";
		case State::Failed: return status.message;
		case State::Idle: break;
		}
		return "Cross to check";
	}

	void Shell::PollPacks()
	{
		if (ps5packs::GetStatus().state == ps5packs::Status::State::Done && !m_packsReloaded && m_status.coreReady && IsWiiU())
		{
			m_packsReloaded = true;
			ps5emu::ReloadGraphicPacks();
		}
	}

	// -- the rows -------------------------------------------------------------------------------------

	std::vector<Row> Shell::SideRows(const std::string& page, const ps5settings::Launcher& settings)
	{
		using Kind = Row::Kind;
		const ps5settings::N3ds& hand = m_side == System::Nds ? settings.nds : settings.n3ds;
		std::vector<Row> rows;
		auto toggle = [&](const char* id, const char* label, bool on, const std::string& description, const std::string& help) {
			Row row;
			row.kind = Kind::Toggle;
			row.id = id;
			row.label = label;
			row.on = on;
			row.value = on ? "On" : "Off";
			row.description = description;
			row.help = help;
			rows.push_back(row);
			return &rows.back();
		};
		auto choice = [&](const char* id, const char* label, std::vector<std::string> options, int index, const std::string& description,
						  const std::string& help, Kind kind = Kind::Choice) {
			Row row;
			row.kind = kind;
			row.id = id;
			row.label = label;
			row.options = std::move(options);
			row.index = std::clamp(index, 0, (int)row.options.size() - 1);
			row.value = row.options[row.index];
			row.description = description;
			row.help = help;
			rows.push_back(row);
			return &rows.back();
		};
		auto slider = [&](const char* id, const char* label, int value, int most, const std::string& description, const std::string& help) {
			Row row;
			row.kind = Kind::Slider;
			row.id = id;
			row.label = label;
			row.fraction = (float)value / most;
			row.value = Percent(value);
			row.description = description;
			row.help = help;
			rows.push_back(row);
			return &rows.back();
		};
		auto action = [&](Kind kind, const char* id, const std::string& label, const std::string& value, const std::string& description,
						  const std::string& help) {
			Row row;
			row.kind = kind;
			row.id = id;
			row.label = label;
			row.value = value;
			row.description = description;
			row.help = help;
			rows.push_back(row);
			return &rows.back();
		};
		// a Wii U row Cemu's core must be up for
		auto cemu = [&](Row* row) {
			row->needsCore = true;
			if (!m_status.coreReady)
			{
				row->dimmed = true;
				row->description = "Cemu did not start, so this cannot be changed.";
			}
		};
		const std::string side = ps5launcher::SideName(m_side);
		auto own = [&](Row* row, const char* setting) { row->setting = setting; };

		if (page == "graphics" && IsWiiU())
		{
			own(choice("upscaling", "Upscaling to 4K", Options(kUpscaleFilters), settings.upscaleFilter, "How the picture is scaled to the TV.",
					"Bicubic is sharp, Bicubic Hermite a little softer, Linear softer still; Nearest neighbour keeps pixels square."),
				"upscaleFilter");
			own(toggle("highframerate", "120 Hz output", settings.highFrameRate, "For displays that take 120 Hz.",
					"The 119.88 Hz mode, on displays that support it. Games still run at their own speed, and a frame that misses a refresh "
					"waits 8 ms for the next instead of 17."),
				"highFrameRate");
			std::vector<std::string> pacing;
			for (int i = 1; i <= 3; i++)
				pacing.push_back(ps5display::FramePacingName(i, settings.highFrameRate));
			own(choice("framepacing", "Frame pacing", pacing, settings.framePacing - 1, "Holds a game to an even frame rate.",
					"Each frame stays on screen for at least two or three refreshes, so a game that cannot hold the display's rate runs at an "
					"even one: 60 or 40 fps with 120 Hz output, 30 or 20 without. 60 fps with 120 Hz output suits a game at 4K that sometimes "
					"drops under 60; 30 fps at 60 Hz suits one at 8K. Also in the in-game menu.",
					Kind::Segmented),
				"framePacing");
			own(toggle("overlay", "Performance overlay", settings.overlay, "Frame rate, CPU and memory in a corner.",
					"Frames per second, CPU and memory use in the top left corner, as Cemu shows them. Also in the in-game menu."),
				"overlay");
			own(toggle("async", "Async shader compile", settings.asyncShaders, "No stutter while new shaders build.",
					"On, a new shader is built while the game carries on, so it does not stutter, but some things may be missing for a moment. "
					"Off, the game waits for each one: stutter, but nothing drawn wrong."),
				"asyncShaders");
		}
		else if (page == "graphics" && Is3ds())
		{
			own(choice("resolution", "Internal resolution", Options(kResolutions), hand.resolution - 1, "Higher is sharper and asks more of the GPU.",
					"How large the 3DS's 3D scenes are drawn before they are scaled to the TV. Higher is sharper and asks more of the GPU, and "
					"each time a game reads a picture back the wait grows with it.",
					Kind::Stepper),
				"resolution");
			own(choice("filter", "Texture filter", Options(kTextureFilters), hand.textureFilter, "Smooths textures as they are scaled up.",
					"Smooths the game's textures as they are scaled up; None keeps them as the 3DS draws them. A filter redraws every texture "
					"at the internal resolution: at high resolutions it is the costliest setting. If a game stutters, try None first."),
				"textureFilter");
			own(toggle("customtextures", "Custom textures", hand.customTextures, "Texture packs from azahar/load/textures.",
					"Texture packs in /data/ps5cemu/azahar/load/textures/<title ID>, as the desktop Azahar loads them."),
				"customTextures");
		}
		else if (page == "graphics")
		{
			own(choice("dsfilter", "Screen filter", Options(kDsFilters), hand.dsFilter, "How the DS's screens are scaled.",
					"How the DS's 256 x 192 screens are scaled to the TV. Sharp keeps every pixel square and even at any size; Smooth blurs "
					"them together; Square pixels takes the nearest, uneven where the size is not a whole number. The in-game menu changes it "
					"too.",
					Kind::Segmented),
				"dsFilter");
			const bool executable = ps5privilege::Current().executable;
			own(toggle("dsjit", "Recompiler", hand.dsJit,
					executable ? "melonDS's JIT: much faster than its interpreter." : "No executable memory: the interpreter runs.",
					"Runs the DS's two CPUs on melonDS's recompiler, in the executable memory the PS5 gives (Diagnostics says whether it "
					"does), much faster than its interpreter. Off: the interpreter, slower, for a game the recompiler gets wrong. Applies to "
					"the next game."),
				"dsJit");
		}
		else if (page == "screens")
		{
			own(choice("layout", "Screen layout", Options(kLayouts), hand.layout, "How the two screens share the TV.",
					"One above the other, the top one alone, the top one large with the bottom one beside it, or the two side by side. In a "
					"game, touchpad click + R1 goes to the next."),
				"layout");
			own(choice("border", "Border", Options(kBorderThemes), hand.border, "Artwork around the screens.",
					"Artwork around the screens, never over them. It follows every layout, and the in-game menu changes it too.",
					Kind::Swatches),
				"border");
		}
		else if (page == "audio")
		{
			own(slider("volume", IsWiiU() ? "Wii U games' volume" : Is3ds() ? "3DS games' volume" : "DS games' volume",
					IsWiiU() ? settings.volume : hand.volume, 100, "The games' sound.", "The games' sound on this side. Left and Right change it by 10%."),
				"volume");
			if (IsWiiU())
				toggle("gamepadspeaker", "GamePad speaker", settings.gamePadSpeaker, "The Wii U GamePad's own sound, on the DualSense.",
					"The sounds games play on the Wii U GamePad's speaker, from player 1's DualSense speaker. Many games send their whole sound "
					"there too, so it is off unless you want it. Applies to the next game.");
			const int music = (int)(std::find(std::begin(kMusic), std::end(kMusic), settings.music) - std::begin(kMusic));
			choice("music", "Launcher music", Options(kMusicNames), music, "The music under the menus.",
				"The launcher's own music, in the spirit of a console's setup screen, or none. Every side has the same.");
			slider("musicvolume", "Music volume", settings.musicVolume, 100, "How loud the menus' music is.",
				"How loud the launcher's music is. Left and Right change it by 10%.");
			toggle("menusounds", "Menu sounds", settings.menuSounds, "The sounds of moving and choosing.",
				"The launcher's sounds as you move, choose and go back.");
		}
		else if (page == "controls")
		{
			toggle("rumble", "Vibration", settings.rumble, "The DualSense's rumble, in games and in the launcher.",
				"Off, the DualSense never rumbles: not for a game's vibration, nor for the launcher's. Every side has the same.");
			if (IsWiiU())
			{
				for (int player = 0; player < ps5pad::kMaxPlayers; player++)
				{
					const auto controls = m_status.coreReady ? ps5emu::GetPlayerControls(player) : ps5emu::PlayerControls{};
					Row* row = action(Kind::Link, "player", fmt::format("Player {}", player + 1), m_status.coreReady ? TypeName(controls.type) : "",
						controls.connected ? "Cross: this player's controller, motion and buttons." : "No DualSense for this player yet.",
						"What the game sees in this player's hands, and its motion, vibration, deadzones and buttons. Player 1 is the signed-in "
						"user who started the app; the other signed-in users' DualSenses are players 2 to 4.");
					row->index = player;
					row->dimmed = m_status.coreReady && !controls.connected;
					cemu(row);
				}
			}
			else
			{
				if (Is3ds())
					toggle("motion", "Motion controls", hand.motion, "The DualSense's motion as the 3DS's.",
						"The DualSense's gyroscope and accelerometer as the 3DS's own, for the games that aim or steer by tilting it.");
				slider("deadzone", "Stick deadzone", hand.deadzone, 50, "How far a stick moves before it counts.",
					Is3ds() ? "How far a stick moves before the game sees it, for the circle pad and the C-stick. Raise it if something drifts "
							  "when you let go of the stick; lower it for finer control." :
							  "How far the left stick moves before it presses the DS's D-pad. Raise it if a direction is pressed when you let go "
							  "of the stick.")
					->value = Percent(hand.deadzone);
				int mapped = 0;
				if (Is3ds())
				{
					const auto mappings = ps5azahar::ListMappings(hand);
					mapped = (int)std::count_if(mappings.begin(), mappings.end(), [](const ps5emu::ButtonMapping& m) { return !m.input.empty(); });
				}
				else
					for (ps5azahar::Button button : ps5azahar::DsButtons())
						mapped += ps5azahar::MappedInput(hand, button) != ps5emu::PadInput::None;
				action(Kind::Link, "buttons", "Buttons", Plural(mapped, "button set", "buttons set"), "Which DualSense button is which.",
					Is3ds() ? "Which DualSense button is which of the 3DS's. A is on Circle and B on Cross by default, where the 3DS has them; "
							  "the circle pad is the left stick, the C-stick the right one, and the touchpad the touch screen." :
							  "Which DualSense button is which of the DS's. A is on Circle and B on Cross by default, where the DS has them; the "
							  "left stick is a second D-pad, the touchpad the touch screen, and R3 held blows into the microphone.");
				action(Kind::Hold, "resetcontrols", "Reset to defaults", m_message.empty() ? "Hold Cross" : m_message,
					Is3ds() ? "Default buttons, motion and deadzone." : "Default buttons and deadzone.", "The default buttons, motion and deadzone.");
			}
		}
		else if (page == "usb")
		{
			for (ps5usb::Device device : ps5usb::kDevices)
			{
				Row* row = toggle("usb", ps5usb::Name(device), m_status.coreReady && ps5usb::Enabled(device),
					device == ps5usb::Device::Skylanders ? "For the Skylanders games." :
					device == ps5usb::Device::Infinity	 ? "For Disney Infinity 3.0." :
														   "For LEGO Dimensions.",
					"Cemu's emulated portal, plugged in as a game starts. In the game, the menu's USB devices category puts figures on it: dumps "
					"in " + ps5usb::Folder(device) + ". A real portal on the PS5's USB is not reached.");
				row->index = (int)device;
				cemu(row);
			}
		}
		else if (page == "system" && Is3ds())
		{
			// Azahar looks for it in its own files, which only the 3DS side opens
			ps5emu::Game home;
			const bool homeMenu = m_prepared == System::N3ds && ps5azahar::HomeMenu(hand.region, home);
			own(choice("region", "Region", Options(kRegions), hand.region + 1, "The emulated 3DS's region.",
					"Automatic takes each game's own region. A game made for another region may refuse to start or show other languages. "
					"Applies to the next game."),
				"region");
			own(choice("language", "Language", Options(kLanguages), hand.language + 1, "The emulated 3DS's language.",
					"The language games that follow the console's show their text in. Applies to the next game."),
				"language");
			std::vector<std::string> clocks;
			for (int clock : kClocks)
				clocks.push_back(fmt::format("{}%", clock));
			own(choice("cpuclock", "CPU clock", clocks, ClockIndex(hand.cpuClock), "The emulated 3DS's CPU speed.",
					"How fast the emulated CPU runs, against a 3DS's: higher can steady a game that slows down where a 3DS would not, and asks "
					"more of the PS5; lower can speed up one held back by its own pacing. 100% is a 3DS. Also in the in-game menu."),
				"cpuClock");
			Row* row = action(Kind::Action, "homemenu", "Home Menu", homeMenu ? "Start" : "Run Artic Setup first",
				"The 3DS Home Menu, from your console's files.",
				"Starts the 3DS Home Menu, once Artic Base's setup has copied your own console's system files (Artic Base).");
			row->dimmed = !homeMenu;
		}
		else if (page == "system")
		{
			own(choice("language", "Language", Options(kLanguages), hand.language + 1, "The emulated DS's language.",
					"The language games that follow the console's show their text in, up to Chinese: the DS has no others (Korean and the "
					"rest are English). Applies to the next game."),
				"language");
			const bool found = ps5melonds::OwnBiosFound();
			own(toggle("dsownbios", "Your DS's BIOS", hand.dsOwnBios, found ? "Found in melonds/bios." : "Not found: melonDS's own run the games.",
					"bios7.bin, bios9.bin and firmware.bin, dumped from your own DS, in /data/ps5cemu/melonds/bios: melonDS runs on them when "
					"they are there and this is on. Without them, its own replacements (FreeBIOS) run most games as well. Applies to the next "
					"game."),
				"dsOwnBios");
		}
		else if (page == "files")
		{
			const std::string& folder = GamesFolder(m_side);
			action(Kind::Link, "folder", fmt::format("{} games", SideTitle(m_side)), ShortPath(folder, 34),
				"Where your games are. Cross picks another.",
				IsWiiU() ? "Your Wii U games: .wua, .wud, .wux, or folders with code, content and meta. Encrypted .wud and .wux need their keys "
						   "in /data/ps5cemu/keys.txt." :
				Is3ds()	 ? "Your 3DS games: .3ds or .cci, .cxi, .3dsx and Azahar's compressed dumps, decrypted, here or in the folders in it. "
						   "CIA files are installed (Install a CIA file). Encrypted dumps need the 3DS's aes_keys.txt in "
						   "/data/ps5cemu/azahar/sysdata." :
						   "Your DS games: .nds files, here or in the folders in it.");
			action(Kind::Action, "rescan", "Look for games now", m_scanning ? "Looking…" : "", "This side's folder, again.",
				"Looks through this side's folder again, as it does each time the side opens.");
			if (IsWiiU())
			{
				const bool keys = IsFile(std::string(ps5paths::kRoot) + "/keys.txt");
				action(Kind::Action, "keys", "Wii U disc keys", keys ? "keys.txt found" : "keys.txt missing", "Only encrypted .wud and .wux need them.",
					"Encrypted .wud and .wux dumps need their disc keys in /data/ps5cemu/keys.txt, one a line. Decrypted dumps, .wua and "
					"folders need none.");
				cemu(action(Kind::Link, "install", "Install updates and DLC", "", "An update, DLC or game (code, content and meta).",
					"Installs into the Wii U's storage (mlc01), as Cemu's Install game title, update or DLC does. Updates and DLC in the game "
					"files folder work as they are, too."));
			}
			else if (Is3ds())
				action(Kind::Link, "installcia", "Install a CIA file", "", "A game, an update or DLC.",
					"Installs a CIA into the 3DS's storage, as Azahar's Install CIA does: an update or DLC goes with its game, and a game joins "
					"the library. Circle cancels while it runs.");
		}
		else if (page == "artic")
			action(Kind::Link, "artic", "Artic Base", settings.n3ds.articAddress, "Play a game from your 3DS, and set up from it.",
				"Artic Base plays a game from a 3DS on your network, its saves staying on the 3DS; Artic Setup copies a 3DS's system files "
				"into Azahar, for the Home Menu and the games that need them.");
		else if (page == "online")
		{
			toggle("boxart", "Box art from GameTDB", settings.boxArt, "Covers for the library, downloaded once.",
				"The first time a game shows up, its cover is downloaded from GameTDB (art.gametdb.com) by the ID on its box. Off: nothing "
				"more is downloaded.");
			if (IsWiiU())
				action(Kind::Action, "packs", "Community graphic packs", PacksStatus(), "Cross checks GitHub for newer ones.",
					"Cemu's community graphic packs, from GitHub's latest release when it is newer than the ones installed. The packs bundled "
					"with the app stay as the fallback.");
			action(Kind::Action, "appupdate", "PS5CEMU-HAR updates", AppUpdateStatus(), "Cross checks GitHub for a newer version.",
				"PS5CEMU-HAR asks GitHub for its latest release each time it starts. Cross asks again, or installs the newer version it found: "
				"the release's files are downloaded, checked and put in place of these, and the app starts again. Your games, saves and "
				"settings stay as they are.");
		}
		else if (page == "launcher")
		{
			choice("starton", "Start on", Options({"The side last used", "Ask each time"}), settings.ui.startOn == "ask" ? 1 : 0,
				"Where PS5CEMU-HAR opens.",
				"The side last used: PS5CEMU-HAR opens on the side you were on, in its Library, and after a game on that game.\nAsk each time: "
				"it shows the three sides first, as the start screen did.",
				Kind::Segmented);
			toggle("pictures", "Game pictures behind menus", settings.ui.gamePictures, "The focused game's cover, softened, behind the screens.",
				"The focused game's picture fills the screen behind the menus, softened and dimmed, so moving the focus changes the whole "
				"screen. Off: the game's colours only.");
			toggle("largertext", "Larger text", settings.ui.largerText, "Every size of text a step up.",
				"Every style of text a step larger: captions from 20 to 23, reading text from 26 to 30.");
			toggle("highcontrast", "High contrast", settings.ui.highContrast, "Opaque panels, full-strength text, a thicker ring.",
				"Opaque panels instead of glass, all text at full strength, a thicker focus ring and no grain or pictures behind the menus.");
			toggle("reducemotion", "Reduce motion", settings.ui.reduceMotion, "Fades in place of movement.",
				"Springs become short fades; nothing drifts, breathes or rises behind the menus.");
			int hold = 2;
			for (int i = 0; i < 6; i++)
				if (kHolds[i] <= settings.ui.holdMs)
					hold = i;
			choice("holdms", "Hold to confirm", Options({"0.4 s", "0.6 s", "0.8 s", "1.0 s", "1.2 s", "1.5 s"}), hold,
				"How long Cross is held for what cannot be undone.",
				"Clearing caches, Artic Setup and resetting controls are held to confirm, never pressed twice. This is how long.", Kind::Stepper);
		}
		else if (page == "diagnostics")
		{
			action(Kind::Link, "setupcheck", "Setup check", "", "Storage, recompilers, games, keys and box art, checked.",
				"Each check has a status, what it means and what to do. It is also shown on the first start and whenever a check fails as "
				"the app starts.");
			action(Kind::Action, "copylogs", "Copy logs to USB", m_diagnosticsDone[0], "The logs a report needs, onto a USB drive.",
				"Copies the last five sessions' logs and the settings into a dated folder on a USB drive, to attach to a report.");
			if (IsWiiU())
				action(Kind::Hold, "clearcaches", "Clear shader caches", m_diagnosticsDone[1].empty() ? "Hold Cross" : m_diagnosticsDone[1],
					"For a game that crashes on a bad cache.", "Deletes Cemu's shader caches. Games build them again as they run: the first minutes stutter.");
			else if (Is3ds())
				action(Kind::Hold, "clearcaches", "Clear shader caches", m_diagnosticsDone[2].empty() ? "Hold Cross" : m_diagnosticsDone[2],
					"For a game that crashes on a bad cache.",
					"Deletes Azahar's shader caches. Games build them again as they run: the first minutes stutter.");
		}
		(void)side;
		return rows;
	}

	std::vector<Row> Shell::SettingRows(const std::string& page)
	{
		if (!InGameSettings())
			return SideRows(page, m_settings);
		// a game's: its rows only, each its own value or the side's (Default)
		const std::string side = ps5launcher::SideName(m_side);
		const auto found = m_settings.games.find(ps5settings::GameKey(side, m_gameTitle));
		std::vector<Row> rows;
		for (Row& row : SideRows(page, m_gameView))
		{
			if (row.setting.empty())
				continue;
			row.fromSide = found == m_settings.games.end() || !found->second.count(row.setting);
			rows.push_back(std::move(row));
		}
		if (!rows.empty())
		{
			Row reset;
			reset.kind = Row::Kind::Hold;
			reset.id = "resetgame";
			reset.label = "Back to defaults";
			reset.value = m_message.empty() ? "Hold Cross" : m_message;
			reset.description = "Every setting of this game its side's again.";
			reset.help = "Clears this game's own settings: it starts with its side's again, as every game without settings of its own does.";
			rows.push_back(reset);
		}
		return rows;
	}

	std::string Shell::PanelText(const std::string& page) const
	{
		if (InGameSettings())
			return {};
		if (page == "diagnostics")
		{
			std::string text;
			for (const auto& line : m_status.diagnostics)
				text += line + "\n";
			return text;
		}
		if (page == "files" && IsDs())
			return "Saves: /data/ps5cemu/melonds/saves (a .sav beside the game is taken the first time). Save states: melonds/states. Cheats: "
				   "melonds/cheats/<game file>.mch, melonDS's format. DSi games, the DS's wireless and its GBA slot are not there.";
		if (page == "usb")
			return "Figure dumps go in /data/ps5cemu/figures: skylanders, infinity and dimensions. Switch a portal on here, start the game, then "
				   "put figures on it from the in-game menu (touchpad click + Options > USB devices).";
		if (page == "about")
			return "Cemu, the Wii U emulator, by the Cemu team and its contributors; RADV on the PS5 by Mihawk-99 and mpereiraesaa; the "
				   "community graphic packs' authors.\nAzahar, the 3DS emulator, by the Azahar team and the Citra contributors before them; "
				   "Mihawk-99's PS5 port of it and dynarmic.\nmelonDS, the DS emulator, by Arisotura and the melonDS team.\nBox art and game "
				   "information: GameTDB. The font: Lexend.\nAn unofficial port, not affiliated with the Cemu, Azahar or melonDS teams, Nintendo "
				   "or Sony.\n\nWii U games: /data/ps5cemu/games, storage and saves: /data/ps5cemu/mlc01, keys: /data/ps5cemu/keys.txt\n3DS "
				   "games: /data/ps5cemu/azahar/games, storage: /data/ps5cemu/azahar/sdmc\nDS games: /data/ps5cemu/melonds/games, saves: "
				   "/data/ps5cemu/melonds/saves\n\nVersion " +
				ps5update::Readable(PS5CEMU_VERSION);
		return {};
	}

	// -- changing a row ---------------------------------------------------------------------------------

	void Shell::SetRowIndex(ps5settings::Launcher& settings, const std::string& id, int index)
	{
		ps5settings::N3ds& hand = m_side == System::Nds ? settings.nds : settings.n3ds;
		if (id == "starton")
			settings.ui.startOn = index == 1 ? "ask" : "last";
		else if (id == "music")
			settings.music = kMusic[std::clamp(index, 0, 1)];
		else if (id == "holdms")
			settings.ui.holdMs = kHolds[std::clamp(index, 0, 5)];
		else if (id == "upscaling")
			settings.upscaleFilter = std::clamp(index, 0, 3);
		else if (id == "framepacing")
			settings.framePacing = std::clamp(index + 1, 1, 3);
		else if (id == "resolution")
			hand.resolution = std::clamp(index + 1, 1, 10);
		else if (id == "filter")
			hand.textureFilter = std::clamp(index, 0, 5);
		else if (id == "layout")
			hand.layout = std::clamp(index, 0, 3);
		else if (id == "border")
			hand.border = std::clamp(index, 0, 5);
		else if (id == "region")
			hand.region = std::clamp(index - 1, -1, 6);
		else if (id == "language")
			hand.language = std::clamp(index - 1, -1, 11);
		else if (id == "cpuclock")
			hand.cpuClock = kClocks[std::clamp(index, 0, (int)std::size(kClocks) - 1)];
		else if (id == "dsfilter")
			hand.dsFilter = std::clamp(index, 0, 2);
	}

	bool Shell::ApplyRow(ps5settings::Launcher& settings, const Row& row, int step, bool cross)
	{
		ps5settings::N3ds& hand = m_side == System::Nds ? settings.nds : settings.n3ds;
		const std::string& id = row.id;
		switch (row.kind)
		{
		case Row::Kind::Toggle:
		{
			if (!cross && step != 0 && ((step > 0) == row.on))
			{
				m_feedback.Play(ui::Cue::Edge);
				return false;
			}
			const bool on = !row.on;
			if (id == "pictures")
				settings.ui.gamePictures = on;
			else if (id == "menusounds")
				settings.menuSounds = on;
			else if (id == "gamepadspeaker")
				settings.gamePadSpeaker = on;
			else if (id == "rumble")
				settings.rumble = on;
			else if (id == "boxart")
				settings.boxArt = on;
			else if (id == "largertext")
				settings.ui.largerText = on;
			else if (id == "highcontrast")
				settings.ui.highContrast = on;
			else if (id == "reducemotion")
				settings.ui.reduceMotion = on;
			else if (id == "highframerate")
				settings.highFrameRate = on;
			else if (id == "overlay")
				settings.overlay = on;
			else if (id == "async")
				settings.asyncShaders = on;
			else if (id == "customtextures")
				hand.customTextures = on;
			else if (id == "motion")
				hand.motion = on;
			else if (id == "dsjit")
				hand.dsJit = on;
			else if (id == "dsownbios")
				hand.dsOwnBios = on;
			else
				return false;
			m_feedback.Play(ui::Cue::Toggle);
			return true;
		}
		case Row::Kind::Choice:
		case Row::Kind::Stepper:
		case Row::Kind::Segmented:
		case Row::Kind::Swatches:
		{
			const int count = (int)row.options.size();
			if (cross && row.kind == Row::Kind::Choice)
			{
				// the dropdown: its choice is made where the row's value lives (the side's, or the game's)
				const Row chosen = row;
				OpenPicker(InGameSettings() ? m_gameName : SystemName(m_side), row.label, row.options, row.index,
					[this, chosen](int index) { ChooseIndex(chosen, index); });
				return false;
			}
			int index = row.index + (cross ? 1 : step);
			if (row.kind == Row::Kind::Choice || cross)
				index = (index + count) % count; // round
			else if (index < 0 || index >= count)
			{
				m_feedback.Play(ui::Cue::Edge);
				return false;
			}
			SetRowIndex(settings, id, index);
			m_feedback.Play(ui::Cue::Focus);
			return true;
		}
		case Row::Kind::Slider:
		{
			int* value = id == "musicvolume" ? &settings.musicVolume :
				id == "volume"				 ? (IsWiiU() ? &settings.volume : &hand.volume) :
				id == "deadzone"			 ? &hand.deadzone :
											   nullptr;
			if (!value)
				return false;
			const int most = id == "deadzone" ? 50 : 100, by = id == "deadzone" ? 5 : 10;
			int next = *value + (cross ? by : step * by);
			if (cross && next > most)
				next = 0;
			next = std::clamp(next, 0, most);
			if (next == *value)
			{
				m_feedback.Play(ui::Cue::Edge);
				return false;
			}
			*value = next;
			m_feedback.Play(ui::Cue::Focus);
			return true;
		}
		default: return false;
		}
	}

	// What a changed setting changes at once: the launcher's sound, the DualSense's rumble, box art
	void Shell::SettingsChanged(const std::string& id)
	{
		ps5sound::SetMusic(m_settings.music, m_settings.musicVolume);
		ps5sound::SetMenuSounds(m_settings.menuSounds);
		ps5pad::SetVibrationEnabled(m_settings.rumble);
		if (id == "boxart")
		{
			ps5boxart::SetEnabled(m_settings.boxArt);
			if (m_settings.boxArt)
				FetchBoxArt();
		}
		SaveSettings();
	}

	// A game's own value, from its view of the settings after a row changed it
	void Shell::KeepGameSetting(const std::string& setting, const ps5settings::Launcher& view)
	{
		const std::string side = ps5launcher::SideName(m_side);
		int value = 0;
		if (setting.empty() || !ps5settings::Value(view, side, setting, value))
			return;
		m_settings.games[ps5settings::GameKey(side, m_gameTitle)][setting] = value;
		SaveSettings();
		m_gameView = ps5settings::ForGame(m_settings, side, m_gameTitle);
	}

	void Shell::ChooseIndex(const Row& row, int index)
	{
		if (InGameSettings())
		{
			ps5settings::Launcher view = m_gameView;
			SetRowIndex(view, row.id, index);
			KeepGameSetting(row.setting, view);
			return;
		}
		SetRowIndex(m_settings, row.id, index);
		SettingsChanged(row.id);
	}

	void Shell::ResetGameSetting(const Row& row)
	{
		const std::string side = ps5launcher::SideName(m_side);
		const auto found = m_settings.games.find(ps5settings::GameKey(side, m_gameTitle));
		if (row.setting.empty() || found == m_settings.games.end() || !found->second.erase(row.setting))
		{
			m_feedback.Play(ui::Cue::Edge);
			return;
		}
		if (found->second.empty())
			m_settings.games.erase(found);
		SaveSettings();
		m_gameView = ps5settings::ForGame(m_settings, side, m_gameTitle);
		m_feedback.Play(ui::Cue::Back);
	}

	void Shell::GameSettingsOpen(int game)
	{
		if (game < 0 || game >= (int)m_games.size())
			return;
		const ps5emu::Game& g = m_games[game].entry.game;
		if (g.titleId == 0)
		{
			m_feedback.Play(ui::Cue::Denied);
			Toast("This game has no ID to keep settings by");
			return;
		}
		m_gameFrom = m_screen == ScreenId::Hub ? ScreenId::Hub : ScreenId::Library;
		m_sidePage = m_page;
		m_gameTitle = g.titleId;
		m_gameName = g.name;
		m_gameView = ps5settings::ForGame(m_settings, ps5launcher::SideName(m_side), g.titleId);
		const auto pages = SettingsPages();
		m_page = pages.empty() ? "graphics" : pages.front().id;
		m_onRows = false;
		m_settingRow = 0;
		m_onBar = false;
		m_message.clear();
		Show(ScreenId::Settings);
		m_screenAt = m_now;
	}

	void Shell::GameSettingsClose()
	{
		m_gameTitle = 0;
		m_gameName.clear();
		m_page = m_sidePage;
		m_onRows = false;
		m_message.clear();
		if (m_gameFrom == ScreenId::Hub)
			Show(ScreenId::Hub);
		else
			ShowTab(ScreenId::Library);
	}

	// A row changed by Left or Right (step) or Cross
	void Shell::ChangeSetting(const Row& row, int step, bool cross)
	{
		const std::string& id = row.id;
		if (row.dimmed && (row.needsCore || id == "homemenu"))
		{
			m_feedback.Play(ui::Cue::Denied);
			return;
		}
		if (row.kind == Row::Kind::Hold)
		{
			if (cross)
			{
				m_feedback.Play(ui::Cue::Denied); // held, not pressed
				m_message.clear();
			}
			return;
		}
		if (InGameSettings())
		{
			// the game's own value: from its view of the settings, kept as its own
			ps5settings::Launcher view = m_gameView;
			if (ApplyRow(view, row, step, cross))
				KeepGameSetting(row.setting, view);
			return;
		}
		if (id == "usb")
		{
			// Cemu's settings.xml keeps it, as its Emulated USB Devices window does
			const auto device = (ps5usb::Device)row.index;
			ps5usb::SetEnabled(device, !ps5usb::Enabled(device));
			m_feedback.Play(ui::Cue::Toggle);
			return;
		}
		if (row.kind != Row::Kind::Action && row.kind != Row::Kind::Link)
		{
			if (ApplyRow(m_settings, row, step, cross))
				SettingsChanged(id);
			return;
		}
		if (!cross)
			return;
		m_feedback.Play(ui::Cue::Select);
		auto& hand = Handheld(m_side);
		if (id == "folder")
		{
			m_filesSide = m_side;
			m_pageFrom = ScreenId::Settings;
			FilesOpen(0);
		}
		else if (id == "rescan")
		{
			if (Is3ds())
				ps5azahar::StartScan(hand.gamesFolder);
			else if (IsDs())
				ps5melonds::StartScan(hand.gamesFolder);
			else if (m_status.coreReady)
				ps5emu::Rescan();
			m_scanning = CoreReady();
		}
		else if (id == "packs")
			ps5packs::Start(); // GitHub's latest community packs, when newer
		else if (id == "appupdate")
		{
			// installing shows over the screen (the update sheet); otherwise GitHub is asked again
			const auto state = ps5update::GetStatus().state;
			if (state == ps5update::Status::State::Available)
				ps5update::Install();
			else if (state == ps5update::Status::State::Ready)
				ps5update::Restart();
			else
				ps5update::Check();
		}
		else if (id == "player")
			PlayerOpen(row.index);
		else if (id == "install")
		{
			m_filesSide = System::WiiU;
			m_pageFrom = ScreenId::Settings;
			FilesOpen(1);
		}
		else if (id == "installcia")
		{
			m_filesSide = System::N3ds;
			m_pageFrom = ScreenId::Settings;
			FilesOpen(2);
		}
		else if (id == "buttons")
		{
			m_pageFrom = ScreenId::Settings;
			MappingOpen();
		}
		else if (id == "artic")
		{
			m_pageFrom = ScreenId::Settings;
			ArticOpen();
		}
		else if (id == "homemenu")
		{
			// the 3DS Home Menu, from the console's files Artic Setup copied
			ps5emu::Game home;
			if (ps5azahar::HomeMenu(hand.region, home))
				LaunchGame(home, "Starting the 3DS", {760, 240, 400, 400});
			else
				m_feedback.Play(ui::Cue::Denied);
		}
		else if (id == "copylogs")
			m_diagnosticsDone[0] = CopyLogsToUsb();
		else if (id == "setupcheck")
			SetupOpen(false);
	}

	void Shell::HoldDone(const std::string& id)
	{
		m_feedback.Play(ui::Cue::Hold);
		if (id == "clearcaches")
			m_diagnosticsDone[Is3ds() ? 2 : 1] = ClearShaderCaches(Is3ds());
		else if (id == "resetcontrols")
		{
			ps5azahar::ResetControls(Handheld(m_side));
			SaveSettings();
			m_message = "Done";
		}
		else if (id == "resetgame")
		{
			m_settings.games.erase(ps5settings::GameKey(ps5launcher::SideName(m_side), m_gameTitle));
			SaveSettings();
			m_gameView = ps5settings::ForGame(m_settings, ps5launcher::SideName(m_side), m_gameTitle);
			m_message = "Done: its side's settings";
		}
	}

	void Shell::SettingsUpdate(const ui::Press& press, const ui::Actions& actions)
	{
		const auto pages = SettingsPages();
		if (pages.empty())
		{
			if (InGameSettings() && press.button == Button::Circle)
				GameSettingsClose();
			return;
		}
		int page = 0;
		for (int i = 0; i < (int)pages.size(); i++)
			if (pages[i].id == m_page)
				page = i;
		m_page = pages[page].id;
		auto rows = SettingRows(m_page);
		m_settingRow = std::clamp(m_settingRow, 0, std::max(0, (int)rows.size() - 1));
		// what is held to confirm
		if (m_onRows && !rows.empty() && rows[m_settingRow].kind == Row::Kind::Hold)
		{
			const float seconds = m_settings.ui.holdMs / 1000.0f;
			m_holdProgress = actions.Held(Button::Cross) ? actions.HeldFor(Button::Cross) / seconds : 0.0f;
			if (m_holdProgress >= 1 && !m_holdFired)
			{
				m_holdFired = true;
				HoldDone(rows[m_settingRow].id);
			}
			if (!actions.Held(Button::Cross))
				m_holdFired = false;
		}
		else
			m_holdProgress = 0;
		const Button b = press.button;
		if (b == Button::Count)
			return;
		if (!m_onRows)
		{
			switch (b)
			{
			case Button::Up:
				if (page == 0)
				{
					if (InGameSettings())
						m_feedback.Play(ui::Cue::Edge, press.repeat);
					else
						FocusBar();
				}
				else
				{
					m_page = pages[page - 1].id;
					m_feedback.Play(ui::Cue::Focus);
				}
				break;
			case Button::Down:
				if (page + 1 >= (int)pages.size())
					m_feedback.Play(ui::Cue::Edge, press.repeat);
				else
				{
					m_page = pages[page + 1].id;
					m_feedback.Play(ui::Cue::Focus);
				}
				break;
			case Button::L2:
			case Button::R2:
				m_page = b == Button::L2 ? pages.front().id : pages.back().id;
				m_feedback.Play(ui::Cue::Focus);
				break;
			case Button::Right:
			case Button::Cross:
				if (!rows.empty())
				{
					m_onRows = true;
					m_settingRow = 0;
					m_message.clear();
					m_feedback.Play(ui::Cue::Select);
				}
				else
					m_feedback.Play(ui::Cue::Edge);
				break;
			case Button::Circle:
				m_feedback.Play(ui::Cue::Back);
				if (InGameSettings())
					GameSettingsClose();
				else
				{
					ShowTab(ScreenId::Library);
					m_barFocus = 3;
				}
				break;
			default: break;
			}
			return;
		}
		if (rows.empty())
		{
			m_onRows = false;
			return;
		}
		const Row& row = rows[m_settingRow];
		switch (b)
		{
		case Button::Circle:
			m_onRows = false;
			m_feedback.Play(ui::Cue::Back);
			break;
		case Button::Up:
		case Button::Down:
		{
			const int next = m_settingRow + (b == Button::Down ? 1 : -1);
			if (next < 0 || next >= (int)rows.size())
				m_feedback.Play(ui::Cue::Edge, press.repeat);
			else
			{
				m_settingRow = next;
				m_message.clear();
				m_feedback.Play(ui::Cue::Focus);
			}
			break;
		}
		case Button::Triangle: OpenHelp(row.label, row.help); break;
		case Button::Square:
			if (InGameSettings())
				ResetGameSetting(row);
			break;
		case Button::Left:
			if (row.kind == Row::Kind::Link || row.kind == Row::Kind::Action || row.kind == Row::Kind::Hold)
			{
				m_onRows = false;
				m_feedback.Play(ui::Cue::Back);
			}
			else
				ChangeSetting(row, -1, false);
			break;
		case Button::Right: ChangeSetting(row, 1, false); break;
		case Button::Cross: ChangeSetting(row, 0, true); break;
		case Button::L2:
		case Button::R2:
			m_settingRow = b == Button::L2 ? 0 : (int)rows.size() - 1;
			m_feedback.Play(ui::Cue::Focus);
			break;
		default: break;
		}
	}

	void Shell::SettingsDraw(Canvas& canvas)
	{
		// a game's settings have no tabs to go to
		DrawBar(canvas, !InGameSettings());
		const auto pages = SettingsPages();
		WantBackdrop(nullptr);
		if (pages.empty())
			return;
		int page = 0;
		for (int i = 0; i < (int)pages.size(); i++)
			if (pages[i].id == m_page)
				page = i;
		m_page = pages[page].id;

		// the pages: one short list, each with its icon (3.0.0's, the side's own)
		canvas.PushAlpha(Enter(1));
		const float railTop = 150, railBottom = 1040, step = 64;
		const float listHeight = pages.size() * step;
		m_railScroll.target = std::clamp(page * step - 380, 0.0f, std::max(0.0f, listHeight - (railBottom - railTop)));
		m_railScroll.omega = kScrollOmega;
		m_railScroll.Update(m_dt);
		canvas.PushClip({0, railTop - 10, 540, railBottom - railTop + 10});
		for (int i = 0; i < (int)pages.size(); i++)
		{
			const float ry = railTop + i * step - m_railScroll.value;
			const bool selected = i == page;
			const Box item{96, ry, 420, step - 6};
			if (selected)
			{
				canvas.Rect(item, 16, m_onRows ? 0x0fffffff : Surface2());
				if (!m_onBar && !m_onRows)
					Focus(item, 16);
			}
			canvas.Draw(pages[i].icon, {item.x + 22, item.CentreY() - 15, 30, 30}, selected ? Accent() : Secondary());
			const ui::TextStyle style = Style({26, selected ? ui::Weight::SemiBold : ui::Weight::Medium, 1.0f});
			const ui::TextBlock text = m_fonts.Layout(style, pages[i].title, item.w - 90, 1);
			canvas.Text(text, item.x + 72, item.CentreY() - text.height * 0.5f, selected ? kText : Secondary());
		}
		canvas.PopClip();
		canvas.PopAlpha();

		// the page: its title, what it is for, its rows across the rest of the screen
		const SettingsPage& current = pages[page];
		canvas.PushAlpha(Enter(2));
		const float px = 600, pw = 1920 - kSafeX - px;
		float ty = 140;
		if (InGameSettings())
		{
			canvas.Text(Style(kOverlineStyle), px, 140, "Game settings · " + m_gameName, Accent(), pw, 1);
			ty += 36;
		}
		canvas.Text(Style(kTitleStyle), px, ty, current.title, kText, pw, 1);
		canvas.Text(Style({22, ui::Weight::Regular, 1.3f}), px, ty + 62, current.subtitle, Secondary(), pw, 1);
		auto rows = SettingRows(m_page);
		m_settingRow = std::clamp(m_settingRow, 0, std::max(0, (int)rows.size() - 1));
		const float ry = ty + 112;
		const float rowH = 96;
		// rows past the page's height scroll with the focus
		const float available = 1080 - 120 - ry;
		const float scroll = std::max(0.0f, (m_settingRow + 1) * (rowH + 12) - available);
		canvas.PushClip({px - 20, ry - 12, pw + 40, available + 24});
		for (int i = 0; i < (int)rows.size(); i++)
		{
			const Box box{px, ry + i * (rowH + 12) - scroll, pw, rowH};
			const bool focused = m_onRows && !m_onBar && i == m_settingRow;
			DrawRow(canvas, rows[i], box, focused);
			if (focused && rows[i].kind == Row::Kind::Hold)
				HoldRing(canvas, box, m_holdProgress);
		}
		canvas.PopClip();
		// what the page says under its rows
		const std::string panel = PanelText(m_page);
		if (!panel.empty())
			canvas.Text(Style({22, ui::Weight::Regular, 1.45f}), px, ry + rows.size() * (rowH + 12) + (rows.empty() ? 0 : 20), panel, Secondary(), pw, 16);
		canvas.PopAlpha();
	}
}
