// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: Settings (docs/UI-REDESIGN.md, 6.5): everything the app can change, in one list. The
// pages down the left, General first, then the side you are on, then the other side, then Help; the
// page's rows on the right, each a control (a toggle, a slider, pips, segments, swatches), the
// focused one with a line saying what it does, and its longer help in the panel beside it and behind
// Triangle. Every row of the classic launcher's two Settings tabs has its place here (Appendix A).

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
		std::vector<SettingsPage> pages = {
			{"display", "Display", "How the launcher looks and where it opens.", 0, m_side},
			{"sound", "Sound", "The menus' music and sounds, and the games' volume.", 0, m_side},
			{"controllers", "Controllers", "The DualSense, for every player.", 0, m_side},
			{"folders", "Games and folders", "Where each side's games are.", 0, m_side},
			{"online", "Online and updates", "Box art, graphic packs and PS5CEMU-HAR itself.", 0, m_side},
			{"accessibility", "Accessibility", "Larger text, more contrast, less motion.", 0, m_side},
		};
		auto wiiu = [&](int section) {
			pages.push_back({"wiiu-graphics", "Graphics", "For every Wii U game.", section, System::WiiU});
			pages.push_back({"wiiu-controllers", "Controllers", "What each player's DualSense is to the Wii U.", section, System::WiiU});
			pages.push_back({"wiiu-usb", "USB devices", "Cemu's emulated portals.", section, System::WiiU});
			pages.push_back({"wiiu-installs", "Install updates and DLC", "Into the Wii U's storage.", section, System::WiiU});
		};
		auto n3ds = [&](int section) {
			pages.push_back({"3ds-graphics", "Graphics", "For every 3DS game.", section, System::N3ds});
			pages.push_back({"3ds-screens", "Screens and borders", "For every 3DS and DS game; in a game, its menu changes them at once.", section,
				System::N3ds});
			pages.push_back({"3ds-controls", "Controls", "The DualSense as the 3DS, and as the DS.", section, System::N3ds});
			pages.push_back({"3ds-ds", "DS games", "The DS games in the 3DS library, on melonDS.", section, System::N3ds});
			pages.push_back({"3ds-system", "System and Home Menu", "The emulated 3DS's region and language.", section, System::N3ds});
			pages.push_back({"3ds-installs", "Install CIA files", "Into the 3DS's storage.", section, System::N3ds});
			pages.push_back({"3ds-artic", "Artic Base", "Play from your 3DS, over your network.", section, System::N3ds});
		};
		if (Is3ds())
		{
			n3ds(1);
			wiiu(2);
		}
		else
		{
			wiiu(1);
			n3ds(2);
		}
		pages.push_back({"setup", "Setup check", "Why something does not work, before anything else.", 3, m_side});
		pages.push_back({"diagnostics", "Diagnostics", "Logs, caches and what this session is.", 3, m_side});
		pages.push_back({"about", "About", "Who made what.", 3, m_side});
		return pages;
	}

	void Shell::SettingsOpen(const std::string& page, bool onRows, int row)
	{
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
		if (ps5packs::GetStatus().state == ps5packs::Status::State::Done && !m_packsReloaded && m_status.coreReady && !Is3ds())
		{
			m_packsReloaded = true;
			ps5emu::ReloadGraphicPacks();
		}
	}

	std::vector<Row> Shell::SettingRows(const std::string& page)
	{
		using Kind = Row::Kind;
		auto& n3ds = m_settings.n3ds;
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
		// a row only the side's own runtime can do: on the other side, Cross goes there first
		auto sided = [&](Row* row, System side, bool needsCore) {
			row->sided = true;
			row->side = side;
			if (side != m_side)
			{
				row->description = side == System::N3ds ? "Cross goes to the 3DS side for it." : "Cross goes to the Wii U side for it.";
				if (row->kind != Kind::Link)
					row->value = side == System::N3ds ? "On the 3DS side" : "On the Wii U side";
			}
			else if (needsCore && !m_status.coreReady)
			{
				row->dimmed = true;
				row->description = "Cemu did not start, so this cannot be changed.";
			}
		};

		if (page == "display")
		{
			choice("starton", "Start on", Options({"The side last used", "Ask each time"}), m_settings.ui.startOn == "ask" ? 1 : 0,
				"Where PS5CEMU-HAR opens.",
				"The side last used: PS5CEMU-HAR opens on the side you were on, and after a game on that game.\nAsk each time: it shows the two "
				"sides first, as the start screen did.",
				Kind::Segmented);
			toggle("pictures", "Game pictures behind menus", m_settings.ui.gamePictures, "The focused game's cover, softened, behind the screens.",
				"The focused game's picture fills the screen behind the menus, softened and dimmed, so moving the focus changes the whole "
				"screen. Off: the game's colours only.");
		}
		else if (page == "sound")
		{
			const int music = (int)(std::find(std::begin(kMusic), std::end(kMusic), m_settings.music) - std::begin(kMusic));
			choice("music", "Launcher music", Options(kMusicNames), music, "The music under the menus.",
				"The launcher's own music, in the spirit of a console's setup screen, or none.");
			slider("musicvolume", "Music volume", m_settings.musicVolume, 100, "How loud the menus' music is.",
				"How loud the launcher's music is. Left and Right change it by 10%.");
			toggle("menusounds", "Menu sounds", m_settings.menuSounds, "The sounds of moving and choosing.",
				"The launcher's sounds as you move, choose and go back.");
			slider("volume-wiiu", "Wii U games' volume", m_settings.volume, 100, "The Wii U games' sound.",
				"The Wii U games' sound. Left and Right change it by 10%.");
			slider("volume-3ds", "3DS and DS games' volume", n3ds.volume, 100, "The 3DS and DS games' sound.",
				"The 3DS games' sound, and the DS games' in the 3DS library. Left and Right change it by 10%.");
			toggle("gamepadspeaker", "GamePad speaker", m_settings.gamePadSpeaker, "The Wii U GamePad's own sound, on the DualSense.",
				"The sounds games play on the Wii U GamePad's speaker, from player 1's DualSense speaker. Many games send their whole sound "
				"there too, so it is off unless you want it. Applies to the next game.");
		}
		else if (page == "controllers")
		{
			toggle("rumble", "Vibration", m_settings.rumble, "The DualSense's rumble, in games and in the launcher.",
				"Off, the DualSense never rumbles: not for a game's vibration, nor for the launcher's.");
			action(Kind::Link, "page:wiiu-controllers", "Wii U players", "", "Each player's emulated controller, motion and buttons.",
				"What the Wii U sees in each player's hands.");
			action(Kind::Link, "page:3ds-controls", "3DS controls", "", "Motion, deadzone and buttons.", "The DualSense as the 3DS.");
		}
		else if (page == "folders")
		{
			action(Kind::Link, "folder-wiiu", "Wii U games", ShortPath(m_settings.gamesFolder, 34), "Where your Wii U games are. Cross picks another.",
				"Your Wii U games: .wua, .wud, .wux, or folders with code, content and meta. Encrypted .wud and .wux need their keys in "
				"/data/ps5cemu/keys.txt.");
			action(Kind::Link, "folder-3ds", "3DS games", ShortPath(n3ds.gamesFolder, 34), "Where your 3DS games are. Cross picks another.",
				"Your 3DS games: .3ds or .cci, .cxi, .3dsx and Azahar's compressed dumps, decrypted, here or in the folders in it. CIA files are "
				"installed (Install CIA files). Encrypted dumps need the 3DS's aes_keys.txt in /data/ps5cemu/azahar/sysdata.");
			action(Kind::Action, "rescan", "Look for games now", m_scanning ? "Looking…" : "", "This side's folder, again.",
				"Looks through this side's folder again, as it does each time the side opens.");
			const bool keys = IsFile(std::string(ps5paths::kRoot) + "/keys.txt");
			action(Kind::Action, "keys", "Wii U disc keys", keys ? "keys.txt found" : "keys.txt missing", "Only encrypted .wud and .wux need them.",
				"Encrypted .wud and .wux dumps need their disc keys in /data/ps5cemu/keys.txt, one a line. Decrypted dumps, .wua and "
				"folders need none.");
		}
		else if (page == "online")
		{
			toggle("boxart", "Box art from GameTDB", m_settings.boxArt, "Covers for the library, downloaded once.",
				"The first time a game shows up, its cover is downloaded from GameTDB (art.gametdb.com) by the ID on its box. Off: nothing "
				"more is downloaded.");
			action(Kind::Action, "packs", "Community graphic packs", PacksStatus(), "Cross checks GitHub for newer ones.",
				"Cemu's community graphic packs, from GitHub's latest release when it is newer than the ones installed. The packs bundled "
				"with the app stay as the fallback.");
			action(Kind::Action, "appupdate", "PS5CEMU-HAR updates", AppUpdateStatus(), "Cross checks GitHub for a newer version.",
				"PS5CEMU-HAR asks GitHub for its latest release each time it starts. Cross asks again, or installs the newer version it "
				"found: the release's files are downloaded, checked and put in place of these, and the app starts again. Your games, saves "
				"and settings stay as they are.");
		}
		else if (page == "accessibility")
		{
			toggle("largertext", "Larger text", m_settings.ui.largerText, "Every size of text a step up.",
				"Every style of text a step larger: captions from 20 to 23, reading text from 26 to 30.");
			toggle("highcontrast", "High contrast", m_settings.ui.highContrast, "Opaque panels, full-strength text, a thicker ring.",
				"Opaque panels instead of glass, all text at full strength, a thicker focus ring and no grain or pictures behind the menus.");
			toggle("reducemotion", "Reduce motion", m_settings.ui.reduceMotion, "Fades in place of movement.",
				"Springs become short fades; nothing drifts, breathes or rises behind the menus.");
			int hold = 2;
			for (int i = 0; i < 6; i++)
				if (kHolds[i] <= m_settings.ui.holdMs)
					hold = i;
			choice("holdms", "Hold to confirm", Options({"0.4 s", "0.6 s", "0.8 s", "1.0 s", "1.2 s", "1.5 s"}), hold,
				"How long Cross is held for what cannot be undone.",
				"Clearing caches, Artic Setup and resetting controls are held to confirm, never pressed twice. This is how long.", Kind::Stepper);
		}
		else if (page == "wiiu-graphics")
		{
			choice("upscaling", "Upscaling to 4K", Options(kUpscaleFilters), m_settings.upscaleFilter, "How the picture is scaled to the TV.",
				"Bicubic is sharp, Bicubic Hermite a little softer, Linear softer still; Nearest neighbour keeps pixels square.");
			toggle("highframerate", "120 Hz output", m_settings.highFrameRate, "For displays that take 120 Hz.",
				"The 119.88 Hz mode, on displays that support it. Games still run at their own speed, and a frame that misses a refresh waits "
				"8 ms for the next instead of 17.");
			std::vector<std::string> pacing;
			for (int i = 1; i <= 3; i++)
				pacing.push_back(ps5display::FramePacingName(i, m_settings.highFrameRate));
			choice("framepacing", "Frame pacing", pacing, m_settings.framePacing - 1, "Holds a game to an even frame rate.",
				"Each frame stays on screen for at least two or three refreshes, so a game that cannot hold the display's rate runs at an "
				"even one: 60 or 40 fps with 120 Hz output, 30 or 20 without. 60 fps with 120 Hz output suits a game at 4K that sometimes "
				"drops under 60; 30 fps at 60 Hz suits one at 8K. Also in the in-game menu.",
				Kind::Segmented);
			toggle("overlay", "Performance overlay", m_settings.overlay, "Frame rate, CPU and memory in a corner.",
				"Frames per second, CPU and memory use in the top left corner, as Cemu shows them. Also in the in-game menu.");
			toggle("async", "Async shader compile", m_settings.asyncShaders, "No stutter while new shaders build.",
				"On, a new shader is built while the game carries on, so it does not stutter, but some things may be missing for a moment. "
				"Off, the game waits for each one: stutter, but nothing drawn wrong.");
		}
		else if (page == "wiiu-controllers")
		{
			for (int player = 0; player < ps5pad::kMaxPlayers; player++)
			{
				const bool here = !Is3ds() && m_status.coreReady;
				const auto controls = here ? ps5emu::GetPlayerControls(player) : ps5emu::PlayerControls{};
				Row* row = action(Kind::Link, "player", fmt::format("Player {}", player + 1), here ? TypeName(controls.type) : "",
					controls.connected ? "Cross: this player's controller, motion and buttons." : "No DualSense for this player yet.",
					"What the game sees in this player's hands, and its motion, vibration, deadzones and buttons. Player 1 is the signed-in user "
					"who started the app; the other signed-in users' DualSenses are players 2 to 4.");
				row->index = player;
				row->dimmed = here && !controls.connected;
				sided(row, System::WiiU, true);
			}
		}
		else if (page == "wiiu-usb")
		{
			for (ps5usb::Device device : ps5usb::kDevices)
			{
				const bool here = !Is3ds() && m_status.coreReady;
				Row* row = toggle("usb", ps5usb::Name(device), here && ps5usb::Enabled(device),
					device == ps5usb::Device::Skylanders ? "For the Skylanders games." :
					device == ps5usb::Device::Infinity	 ? "For Disney Infinity 3.0." :
														   "For LEGO Dimensions.",
					"Cemu's emulated portal, plugged in as a game starts. In the game, the menu's USB devices category puts figures on it: dumps "
					"in " + ps5usb::Folder(device) + ". A real portal on the PS5's USB is not reached.");
				row->index = (int)device;
				sided(row, System::WiiU, true);
			}
		}
		else if (page == "wiiu-installs")
		{
			sided(action(Kind::Link, "install", "Install from a folder", "", "An update, DLC or game (code, content and meta).",
					  "Installs into the Wii U's storage (mlc01), as Cemu's Install game title, update or DLC does. Updates and DLC in the game "
					  "files folder work as they are, too."),
				System::WiiU, true);
		}
		else if (page == "3ds-graphics")
		{
			choice("resolution", "Internal resolution", Options(kResolutions), n3ds.resolution - 1, "Higher is sharper and asks more of the GPU.",
				"How large the 3DS's 3D scenes are drawn before they are scaled to the TV. Higher is sharper and asks more of the GPU, and each "
				"time a game reads a picture back the wait grows with it.",
				Kind::Stepper);
			choice("filter", "Texture filter", Options(kTextureFilters), n3ds.textureFilter, "Smooths textures as they are scaled up.",
				"Smooths the game's textures as they are scaled up; None keeps them as the 3DS draws them. A filter redraws every texture at "
				"the internal resolution: at high resolutions it is the costliest setting. If a game stutters, try None first.");
			toggle("customtextures", "Custom textures", n3ds.customTextures, "Texture packs from azahar/load/textures.",
				"Texture packs in /data/ps5cemu/azahar/load/textures/<title ID>, as the desktop Azahar loads them.");
		}
		else if (page == "3ds-screens")
		{
			choice("layout", "Screen layout", Options(kLayouts), n3ds.layout, "How the two screens share the TV.",
				"One above the other, the top one alone, the top one large with the bottom one beside it, or the two side by side, for 3DS "
				"and DS games alike. In a game, touchpad click + R1 goes to the next.");
			choice("border", "Border", Options(kBorderThemes), n3ds.border, "Artwork around the screens.",
				"Artwork around the 3DS's and the DS's screens, never over them. It follows every layout, and the in-game menu changes it too.",
				Kind::Swatches);
		}
		else if (page == "3ds-controls")
		{
			toggle("motion", "Motion controls", n3ds.motion, "The DualSense's motion as the 3DS's.",
				"The DualSense's gyroscope and accelerometer as the 3DS's own, for the games that aim or steer by tilting it.");
			slider("deadzone", "Stick deadzone", n3ds.deadzone, 50, "How far a stick moves before it counts.",
				"How far a stick moves before the game sees it, for the circle pad and the C-stick. Raise it if something drifts when you let "
				"go of the stick; lower it for finer control.")
				->value = Percent(n3ds.deadzone);
			const auto mappings = ps5azahar::ListMappings(n3ds);
			const int mapped = (int)std::count_if(mappings.begin(), mappings.end(), [](const ps5emu::ButtonMapping& m) { return !m.input.empty(); });
			action(Kind::Link, "buttons3ds", "Buttons", Plural(mapped, "button set", "buttons set"), "Which DualSense button is which.",
				"Which DualSense button is which of the 3DS's. A is on Circle and B on Cross by default, where the 3DS has them; the circle pad "
				"is the left stick, the C-stick the right one, and the touchpad the touch screen. DS games take the same buttons, the circle "
				"pad's directions as their D-pad's too, and R3 held as their microphone.");
			action(Kind::Hold, "reset3ds", "Reset to defaults", m_message.empty() ? "Hold Cross" : m_message, "Default buttons, motion and deadzone.",
				"The default buttons, motion and deadzone.");
		}
		else if (page == "3ds-system")
		{
			// Azahar looks for it in its own files, which only the 3DS side opens: asked on the Wii U side, its
			// logging would start and run on beside a Wii U game
			ps5emu::Game home;
			const bool homeMenu = m_prepared == System::N3ds && ps5azahar::HomeMenu(n3ds.region, home);
			choice("region", "Region", Options(kRegions), n3ds.region + 1, "The emulated 3DS's region.",
				"Automatic takes each game's own region. A game made for another region may refuse to start or show other languages. Applies "
				"to the next game.");
			choice("language", "Language", Options(kLanguages), n3ds.language + 1, "The emulated 3DS's language.",
				"The language games that follow the console's show their text in, the DS games' too (up to Chinese: the DS has no others). "
				"Applies to the next game.");
			Row* row = action(Kind::Action, "homemenu", "Home Menu", homeMenu ? "Start" : "Run Artic Setup first",
				"The 3DS Home Menu, from your console's files.",
				"Starts the 3DS Home Menu, once Artic Base's setup has copied your own console's system files (Artic Base).");
			row->dimmed = m_prepared == System::N3ds && !homeMenu;
			sided(row, System::N3ds, false);
		}
		else if (page == "3ds-ds")
		{
			choice("dsfilter", "Screen filter", Options({"Sharp", "Smooth", "Square pixels"}), n3ds.dsFilter, "How the DS's screens are scaled.",
				"How the DS's 256 x 192 screens are scaled to the TV. Sharp keeps every pixel square and even at any size; Smooth blurs them "
				"together; Square pixels takes the nearest, uneven where the size is not a whole number. The in-game menu changes it too.",
				Kind::Segmented);
			const bool executable = ps5privilege::Current().executable;
			toggle("dsjit", "Recompiler", n3ds.dsJit, executable ? "melonDS's JIT: much faster than its interpreter." : "No executable memory: the interpreter runs.",
				"Runs the DS's two CPUs on melonDS's recompiler, in the executable memory the PS5 gives (Diagnostics says whether it does), "
				"much faster than its interpreter. Off: the interpreter, slower, for a game the recompiler gets wrong. Applies to the next "
				"DS game.");
			const bool found = ps5melonds::OwnBiosFound();
			toggle("dsownbios", "Your DS's BIOS", n3ds.dsOwnBios, found ? "Found in melonds/bios." : "Not found: melonDS's own run the games.",
				"bios7.bin, bios9.bin and firmware.bin, dumped from your own DS, in /data/ps5cemu/melonds/bios: melonDS runs on them when "
				"they are there and this is on. Without them, its own replacements (FreeBIOS) run most games as well. Applies to the next "
				"DS game.");
		}
		else if (page == "3ds-installs")
		{
			sided(action(Kind::Link, "installcia", "Install a CIA file", "", "A game, an update or DLC.",
					  "Installs a CIA into the 3DS's storage, as Azahar's Install CIA does: an update or DLC goes with its game, and a game joins "
					  "the library. Circle cancels while it runs."),
				System::N3ds, false);
		}
		else if (page == "3ds-artic")
		{
			sided(action(Kind::Link, "artic", "Artic Base", m_settings.n3ds.articAddress, "Play a game from your 3DS, and set up from it.",
					  "Artic Base plays a game from a 3DS on your network, its saves staying on the 3DS; Artic Setup copies a 3DS's system "
					  "files into Azahar, for the Home Menu and the games that need them."),
				System::N3ds, false);
		}
		else if (page == "setup")
			action(Kind::Link, "setupcheck", "Open the Setup check", "", "Storage, recompilers, games, keys and box art, checked.",
				"Each check has a status, what it means and what to do. It is also shown on the first start and whenever a check fails as "
				"the app starts.");
		else if (page == "diagnostics")
		{
			action(Kind::Action, "copylogs", "Copy logs to USB", m_diagnosticsDone[0], "The logs a report needs, onto a USB drive.",
				"Copies the last five sessions' logs and the settings into a dated folder on a USB drive, to attach to a report.");
			action(Kind::Hold, "clearcaches-wiiu", "Clear Wii U shader caches", m_diagnosticsDone[1].empty() ? "Hold Cross" : m_diagnosticsDone[1],
				"For a game that crashes on a bad cache.",
				"Deletes Cemu's shader caches. Games build them again as they run: the first minutes stutter.");
			action(Kind::Hold, "clearcaches-3ds", "Clear 3DS shader caches", m_diagnosticsDone[2].empty() ? "Hold Cross" : m_diagnosticsDone[2],
				"For a game that crashes on a bad cache.",
				"Deletes Azahar's shader caches. Games build them again as they run: the first minutes stutter.");
		}
		return rows;
	}

	std::string Shell::PanelText(const std::string& page) const
	{
		if (page == "diagnostics")
		{
			std::string text;
			for (const auto& line : m_status.diagnostics)
				text += line + "\n";
			return text;
		}
		if (page == "3ds-ds")
			return "DS games (.nds) go in the 3DS games' folder, beside the 3DS's, and show in the 3DS library: DS on their badge, and a DS "
				   "games filter. melonDS plays them, with the 3DS's screens, border, sound and controls, and the same in-game menu.\n\nSaves: "
				   "/data/ps5cemu/melonds/saves (a .sav beside the game is taken the first time). Save states: melonds/states. Cheats: "
				   "melonds/cheats/<game file>.mch, melonDS's format.\n\nDSi games, the DS's wireless and its GBA slot are not there.";
		if (page == "wiiu-usb")
			return "Figure dumps go in /data/ps5cemu/figures: skylanders, infinity and dimensions. Switch a portal on here, start the game, then "
				   "put figures on it from the in-game menu (touchpad click + Options > USB devices).";
		if (page == "about")
			return "Cemu, the Wii U emulator, by the Cemu team and its contributors; RADV on the PS5 by Mihawk-99 and mpereiraesaa; the "
				   "community graphic packs' authors.\nAzahar, the 3DS emulator, by the Azahar team and the Citra contributors before them; "
				   "Mihawk-99's PS5 port of it and dynarmic.\nmelonDS, the DS emulator, by Arisotura and the melonDS team.\nBox art and game "
				   "information: GameTDB. The font: Lexend.\nAn unofficial port, not affiliated with the Cemu, Azahar or melonDS teams, Nintendo "
				   "or Sony.\n\nWii U games: /data/ps5cemu/games, storage and saves: /data/ps5cemu/mlc01, keys: /data/ps5cemu/keys.txt\n3DS "
				   "and DS games: /data/ps5cemu/azahar/games, 3DS storage: /data/ps5cemu/azahar/sdmc, DS saves: /data/ps5cemu/melonds/saves"
				   "\n\nVersion " +
				ps5update::Readable(PS5CEMU_VERSION);
		return {};
	}

	void Shell::SetChoice(const std::string& id, int index)
	{
		auto& n3ds = m_settings.n3ds;
		if (id == "starton")
			m_settings.ui.startOn = index == 1 ? "ask" : "last";
		else if (id == "music")
			m_settings.music = kMusic[std::clamp(index, 0, 1)];
		else if (id == "holdms")
			m_settings.ui.holdMs = kHolds[std::clamp(index, 0, 5)];
		else if (id == "upscaling")
			m_settings.upscaleFilter = std::clamp(index, 0, 3);
		else if (id == "framepacing")
			m_settings.framePacing = std::clamp(index + 1, 1, 3);
		else if (id == "resolution")
			n3ds.resolution = std::clamp(index + 1, 1, 10);
		else if (id == "filter")
			n3ds.textureFilter = std::clamp(index, 0, 5);
		else if (id == "layout")
			n3ds.layout = std::clamp(index, 0, 3);
		else if (id == "border")
			n3ds.border = std::clamp(index, 0, 5);
		else if (id == "region")
			n3ds.region = std::clamp(index - 1, -1, 6);
		else if (id == "language")
			n3ds.language = std::clamp(index - 1, -1, 11);
		else if (id == "dsfilter")
			n3ds.dsFilter = std::clamp(index, 0, 2);
		SaveSettings();
		ps5sound::SetMusic(m_settings.music, m_settings.musicVolume);
	}

	// A row changed by Left or Right (step) or Cross
	void Shell::ChangeSetting(const Row& row, int step, bool cross)
	{
		auto& n3ds = m_settings.n3ds;
		const std::string& id = row.id;
		if (row.dimmed && row.sided && row.side == m_side)
		{
			m_feedback.Play(ui::Cue::Denied);
			return;
		}
		// a row of the other side's runtime: that side opens first, then the row's page again
		if (row.sided && row.side != m_side && cross)
		{
			const std::string page = m_page;
			const int at = m_settingRow;
			OpenSide(row.side, false);
			SettingsOpen(page, true, at);
			auto rows = SettingRows(page);
			if (at < (int)rows.size())
				ChangeSetting(rows[at], 0, true);
			return;
		}
		switch (row.kind)
		{
		case Row::Kind::Toggle:
		{
			if (!cross && step != 0 && ((step > 0) == row.on))
			{
				m_feedback.Play(ui::Cue::Edge);
				return;
			}
			const bool on = !row.on;
			if (id == "pictures")
				m_settings.ui.gamePictures = on;
			else if (id == "menusounds")
				m_settings.menuSounds = on, ps5sound::SetMenuSounds(on);
			else if (id == "gamepadspeaker")
				m_settings.gamePadSpeaker = on;
			else if (id == "rumble")
				m_settings.rumble = on, ps5pad::SetVibrationEnabled(on);
			else if (id == "boxart")
			{
				m_settings.boxArt = on;
				ps5boxart::SetEnabled(on);
				if (on)
					FetchBoxArt();
			}
			else if (id == "largertext")
				m_settings.ui.largerText = on;
			else if (id == "highcontrast")
				m_settings.ui.highContrast = on;
			else if (id == "reducemotion")
				m_settings.ui.reduceMotion = on;
			else if (id == "highframerate")
				m_settings.highFrameRate = on;
			else if (id == "overlay")
				m_settings.overlay = on;
			else if (id == "async")
				m_settings.asyncShaders = on;
			else if (id == "customtextures")
				n3ds.customTextures = on;
			else if (id == "motion")
				n3ds.motion = on;
			else if (id == "dsjit")
				n3ds.dsJit = on;
			else if (id == "dsownbios")
				n3ds.dsOwnBios = on;
			else if (id == "usb")
			{
				// Cemu's settings.xml keeps it, as its Emulated USB Devices window does
				const auto device = (ps5usb::Device)row.index;
				ps5usb::SetEnabled(device, !ps5usb::Enabled(device));
				m_feedback.Play(ui::Cue::Toggle);
				return;
			}
			m_feedback.Play(ui::Cue::Toggle);
			SaveSettings();
			return;
		}
		case Row::Kind::Choice:
		case Row::Kind::Stepper:
		case Row::Kind::Segmented:
		case Row::Kind::Swatches:
		{
			const int count = (int)row.options.size();
			if (cross && row.kind == Row::Kind::Choice)
			{
				const std::string rowId = id;
				OpenPicker(m_page.rfind("3ds", 0) == 0 ? "Nintendo 3DS" : m_page.rfind("wiiu", 0) == 0 ? "Wii U" : "Settings", row.label, row.options,
					row.index, [this, rowId](int index) { SetChoice(rowId, index); });
				return;
			}
			int index = row.index + (cross ? 1 : step);
			if (row.kind == Row::Kind::Choice || cross)
				index = (index + count) % count; // round
			else if (index < 0 || index >= count)
			{
				m_feedback.Play(ui::Cue::Edge);
				return;
			}
			SetChoice(id, index);
			m_feedback.Play(ui::Cue::Focus);
			return;
		}
		case Row::Kind::Slider:
		{
			int* value = id == "musicvolume" ? &m_settings.musicVolume : id == "volume-wiiu" ? &m_settings.volume : id == "volume-3ds" ? &n3ds.volume :
				id == "deadzone"												  ? &n3ds.deadzone :
																					nullptr;
			if (!value)
				return;
			const int most = id == "deadzone" ? 50 : 100, by = id == "deadzone" ? 5 : 10;
			int next = *value + (cross ? by : step * by);
			if (cross && next > most)
				next = 0;
			next = std::clamp(next, 0, most);
			if (next == *value)
			{
				m_feedback.Play(ui::Cue::Edge);
				return;
			}
			*value = next;
			SaveSettings();
			ps5sound::SetMusic(m_settings.music, m_settings.musicVolume);
			m_feedback.Play(ui::Cue::Focus);
			return;
		}
		case Row::Kind::Hold:
			if (cross)
			{
				m_feedback.Play(ui::Cue::Denied);
				m_message.clear();
			}
			return;
		case Row::Kind::Action:
		case Row::Kind::Link:
			break;
		}
		if (!cross)
			return;
		m_feedback.Play(ui::Cue::Select);
		if (id.rfind("page:", 0) == 0)
			SettingsOpen(id.substr(5), true);
		else if (id == "folder-wiiu" || id == "folder-3ds")
		{
			m_filesSide = id == "folder-3ds" ? System::N3ds : System::WiiU;
			m_pageFrom = ScreenId::Settings;
			FilesOpen(0);
		}
		else if (id == "rescan")
		{
			if (Is3ds())
				ps5azahar::StartScan(n3ds.gamesFolder);
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
		else if (id == "buttons3ds")
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
			if (ps5azahar::HomeMenu(n3ds.region, home))
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
		if (id == "clearcaches-wiiu")
			m_diagnosticsDone[1] = ClearShaderCaches(false);
		else if (id == "clearcaches-3ds")
			m_diagnosticsDone[2] = ClearShaderCaches(true);
		else if (id == "reset3ds")
		{
			ps5azahar::ResetControls(m_settings.n3ds);
			SaveSettings();
			m_message = "Done";
		}
	}

	void Shell::SettingsUpdate(const ui::Press& press, const ui::Actions& actions)
	{
		const auto pages = SettingsPages();
		int page = 0;
		for (int i = 0; i < (int)pages.size(); i++)
			if (pages[i].id == m_page)
				page = i;
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
					FocusBar();
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
			{
				// the section before or after
				const int section = pages[page].section;
				int to = page;
				if (b == Button::R2)
					while (to < (int)pages.size() - 1 && pages[to].section == section)
						to++;
				else
				{
					while (to > 0 && pages[to].section == section)
						to--;
					while (to > 0 && pages[to - 1].section == pages[to].section)
						to--;
				}
				m_page = pages[to].id;
				m_feedback.Play(ui::Cue::Focus);
				break;
			}
			case Button::Right:
			case Button::Cross:
				if (m_page == "setup" && b == Button::Cross)
				{
					m_feedback.Play(ui::Cue::Select);
					SetupOpen(false);
				}
				else if (!rows.empty())
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
				ShowTab(ScreenId::Home);
				m_feedback.Play(ui::Cue::Back);
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

	void Shell::DrawScreensPreview(Canvas& canvas, const Box& box)
	{
		// the TV, the border and the 3DS's two screens in the chosen layout
		static const uint32_t kBorders[][2] = {{0xff0d0705, 0xff0d0705}, {0xff5c2a1b, 0xff2a1206}, {0xff2a9de0, 0xff1a70b0},
			{0xffc8b44f, 0xffd06a9a}, {0xffb0d5e8, 0xff7ea8c0}, {0xffffa95a, 0xff3fb6f4}};
		const int border = std::clamp(m_settings.n3ds.border, 0, 5);
		canvas.LinearGradient(box, 16, kBorders[border][0], kBorders[border][1], box.x, box.y, box.Right(), box.Bottom());
		canvas.Ring(box, 16, 1.5f, kGlassEdge);
		const uint32_t top = 0xff8a4a1d, bottom = 0xff7a3a1a;
		const float w = box.w, h = box.h;
		auto screen = [&](float x, float y, float sw, float sh, uint32_t colour) {
			canvas.LinearGradient({box.x + x * w, box.y + y * h, sw * w, sh * h}, 6, colour, ui::Mix(colour, 0xff3333dd, 0.5f), box.x + x * w,
				box.y + y * h, box.x + (x + sw) * w, box.y + (y + sh) * h);
		};
		switch (std::clamp(m_settings.n3ds.layout, 0, 3))
		{
		case 0:
			screen(0.3f, 0.06f, 0.4f, 0.42f, top);
			screen(0.34f, 0.52f, 0.32f, 0.42f, bottom);
			break;
		case 1: screen(0.12f, 0.08f, 0.76f, 0.84f, top); break;
		case 2:
			screen(0.06f, 0.16f, 0.64f, 0.68f, top);
			screen(0.73f, 0.43f, 0.22f, 0.3f, bottom);
			break;
		case 3:
			screen(0.04f, 0.3f, 0.44f, 0.44f, top);
			screen(0.52f, 0.3f, 0.44f, 0.44f, bottom);
			break;
		}
	}

	void Shell::SettingsDraw(Canvas& canvas)
	{
		DrawBar(canvas, true);
		const auto pages = SettingsPages();
		int page = 0;
		for (int i = 0; i < (int)pages.size(); i++)
			if (pages[i].id == m_page)
				page = i;
		m_page = pages[page].id;
		WantBackdrop(nullptr);

		// the pages, by section
		canvas.PushAlpha(Enter(1));
		const float railTop = 150, railBottom = 1040;
		std::vector<std::pair<float, int>> items; // y, page index (-1 - section for a heading)
		float y = 0;
		int section = -1;
		for (int i = 0; i < (int)pages.size(); i++)
		{
			if (pages[i].section != section)
			{
				section = pages[i].section;
				y += section == 0 ? 0 : 22;
				items.push_back({y, -1 - section});
				y += 40;
			}
			items.push_back({y, i});
			y += 50;
		}
		float focusY = 0;
		for (const auto& [iy, index] : items)
			if (index == page)
				focusY = iy;
		m_railScroll.target = std::clamp(focusY - 380, 0.0f, std::max(0.0f, y - (railBottom - railTop)));
		m_railScroll.omega = kScrollOmega;
		m_railScroll.Update(m_dt);
		canvas.PushClip({0, railTop - 10, 520, railBottom - railTop + 10});
		for (const auto& [iy, index] : items)
		{
			const float ry = railTop + iy - m_railScroll.value;
			if (index < 0)
			{
				const int s = -1 - index;
				const System side = s == 1 ? m_side : Is3ds() ? System::WiiU : System::N3ds;
				const char* names[4] = {"General", side == System::N3ds ? "Nintendo 3DS" : "Wii U", side == System::N3ds ? "Nintendo 3DS" : "Wii U", "Help"};
				float hx = 114;
				if (s == 1 || s == 2)
				{
					canvas.Rect({hx, ry + 4, 12, 12}, 3, AccentOf(side));
					hx += 22;
				}
				canvas.Text(Style(kOverlineStyle), hx, ry, names[s], Secondary());
				continue;
			}
			const bool selected = index == page;
			const Box item{96, ry - 12, 400, 48};
			if (selected)
			{
				canvas.Rect(item, 12, m_onRows ? 0x0fffffff : Surface2());
				canvas.Rect({item.x + 12, item.y + 12, 4, 24}, 2, AccentOf(pages[index].section == 1 || pages[index].section == 2 ? pages[index].side : m_side));
				if (!m_onBar && !m_onRows)
					Focus(item, 12);
			}
			canvas.Text(Style({26, selected ? ui::Weight::Medium : ui::Weight::Regular, 1.0f}), selected ? 130 : 114, ry, pages[index].title,
				selected ? kText : Secondary(), 360, 1);
		}
		canvas.PopClip();
		canvas.PopAlpha();

		// the page
		const SettingsPage& current = pages[page];
		canvas.PushAlpha(Enter(2));
		const float px = 580, pw = 700;
		float tx = px;
		if (current.section == 1 || current.section == 2)
			tx += Badge(canvas, px, 152, current.side) + 16;
		canvas.Text(Style(kTitleStyle), tx, 140, current.title, kText, pw + 500 - (tx - px), 1);
		// as wide as the page, or the rows where a preview stands beside them
		const bool preview = m_page == "3ds-screens";
		canvas.Text(Style({22, ui::Weight::Regular, 1.3f}), px, 202, current.subtitle, Secondary(), preview ? pw + 20 : 1240, 1);
		auto rows = SettingRows(m_page);
		m_settingRow = std::clamp(m_settingRow, 0, std::max(0, (int)rows.size() - 1));
		float ry = 252;
		const float rowH = 104;
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
			canvas.Text(Style({22, ui::Weight::Regular, 1.45f}), px, ry + rows.size() * (rowH + 12) + (rows.empty() ? 0 : 20), panel, Secondary(), pw + 520, 16);

		// the panel at the right: a preview, and the focused row's help
		const float qx = 1320, qw = 504;
		float qy = 252;
		if (preview)
		{
			canvas.Text(Style(kOverlineStyle), qx, qy - 34, "Preview", Secondary());
			DrawScreensPreview(canvas, {qx, qy, qw, qw * 9 / 16});
			qy += qw * 9 / 16 + 30;
			const int layout = std::clamp(m_settings.n3ds.layout, 0, 3), border = std::clamp(m_settings.n3ds.border, 0, 5);
			static const char* kLayoutsShort[] = {"Top above bottom", "Top screen only", "Large top screen", "Side by side"};
			static const char* kBordersShort[] = {"no", "the Midnight", "the Waves", "the Aurora", "the Shell", "the PS5CEMU-HAR"};
			const ui::TextBlock note = canvas.Text(Style({22, ui::Weight::Regular, 1.4f}), qx, qy,
				fmt::format("{} on {} border. The bottom screen is the touchpad: click to tap, hold the click to drag.", kLayoutsShort[layout],
					kBordersShort[border]),
				Secondary(), qw, 4);
			qy += note.height + 26;
		}
		if (m_onRows && !rows.empty() && !rows[m_settingRow].help.empty() && !panel.size())
		{
			const ui::TextBlock help = m_fonts.Layout(Style({22, ui::Weight::Regular, 1.45f}), rows[m_settingRow].help, qw - 56, 12);
			const Box card{qx, qy, qw, help.height + 92};
			Panel(canvas, card, kRadiusCard);
			canvas.Text(Style(kOverlineStyle), card.x + 28, card.y + 26, "About this setting", Tertiary());
			canvas.Text(help, card.x + 28, card.y + 60, Secondary());
		}
		canvas.PopAlpha();
	}
}
