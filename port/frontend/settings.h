// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the launcher's settings, /data/ps5cemu/ps5cemu.json. Cemu's own settings stay in its
// settings.xml; the launcher writes the few it manages (game folder, volume, overlay, upscaling
// filter) into both. Azahar's are here (n3ds), and given to it when a 3DS game starts; a DS game,
// which the 3DS side lists too, starts on melonDS with the same ones (its screens, border, sound and
// controls) and the few of its own (ds...).

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ps5settings
{
	// Azahar's side
	struct N3ds
	{
		std::string gamesFolder = "/data/ps5cemu/azahar/games";
		int resolution = 6;			  // the internal resolution, times the 3DS's 400x240
		int layout = 2;				  // the screens' layout (ps5azahar::Layout): the top one large
		int textureFilter = 0;		  // none
		int volume = 100;			  // percent
		bool motion = true;			  // the DualSense's motion sensors as the 3DS's
		int deadzone = 15;			  // the sticks', percent
		bool performance = false;	  // the frame rate and speed over the game (its in-game menu)
		int region = -1;			  // the emulated 3DS's region: -1 automatic (the game's), else Azahar's region_value
		int language = -1;			  // the emulated 3DS's language: -1 as Azahar sets it, else its SystemLanguage (0 to 11)
		bool customTextures = false;  // texture packs from azahar/load/textures/<title ID> (Azahar's custom_textures)
		int border = 0;				  // artwork around the screens: 0 none, else ps5ingame3ds::kBorderNames' theme
		int cpuClock = 100;			  // the emulated CPU's speed, percent of the 3DS's (Azahar's cpu_clock_percentage)
		// only in ps5cemu.json: every 3DS vertex position computed invariantly (Azahar patch 0010), as
		// Azahar does on Apple GPUs, for Pokemon X's moving black stipples (#23). Off, as on a PC,
		// until a console has shown it fixes them, and what it costs
		bool invariantPosition = false;
		// the DS's games (melonDS: port/melonds)
		int dsFilter = 0;			  // how the DS's screens are scaled to the TV: sharp, smooth, square pixels (ps5melonds::screens::Filter)
		bool dsJit = true;			  // melonDS's recompiler where the console gives executable memory; off: its interpreter, slower
		bool dsOwnBios = true;		  // the DS's own BIOS and firmware when they are in melonds/bios; off (or missing): melonDS's FreeBIOS
		std::string articAddress;	  // the last Artic Base server's IPv4 address, a 3DS on the network
		int gameCount = -1;			  // the library's games when last looked for (the start screen's); -1: never
		std::map<std::string, std::string> buttons; // 3DS button: DualSense input, where not the default
		uint64_t lastGame = 0;		  // title ID, or one made from the path
		std::vector<uint64_t> recent; // newest first, at most four
	};

	// The new launcher's own (docs/UI-REDESIGN.md, 9.8), under "ui"
	struct Ui
	{
		std::string startOn = "last"; // "last": the side last used; "ask": the side chooser
		std::string lastSide;		 // "wiiu" or "3ds": the side last used, restored at every start
		bool largerText = false;
		bool highContrast = false;
		bool reduceMotion = false;
		int holdMs = 800;			 // hold to confirm, 400 to 1500
		bool gamePictures = true;	 // the focused game's picture behind the menus
		bool setupDone = false;		 // the Setup check was shown once
		int libraryFilter[2] = {0, 0}; // per side (Wii U, 3DS): All, Recently added, Favourites, Graphic packs on (Wii U) or DS games (3DS)
		int librarySort[2] = {1, 1};   // Recently played, A to Z (the default), Release year, How it runs
	};

	struct Launcher
	{
		std::string gamesFolder = "/data/ps5cemu/games";
		int upscaleFilter = 1;		  // how the game's picture is scaled to 4K: Cemu's upscale_filter
		bool highFrameRate = false;	  // the 119.88 Hz mode where the display has it
		// frame pacing (ps5/display.h): each frame shown for at least this many refreshes, 1 to 3;
		// 2 is an even 60 fps at 119.88 Hz and an even 30 at 59.94 Hz (Settings > Video, in-game Graphics)
		int framePacing = 1;
		bool overlay = false;		  // Cemu's performance overlay from the start
		bool asyncShaders = true;	  // Cemu's async_compile (Settings > Video, and the in-game menu's Graphics)
		bool gamePadSpeaker = false;  // the GamePad's sound on player 1's DualSense speaker (Settings > Audio)
		bool rumble = true;
		bool pinCpuThreads = false;	  // an experiment (ps5/threads.h): only in ps5cemu.json
		// only in ps5cemu.json: RADV_DEBUG for the Vulkan driver (e.g. "nongg,nohiz"), to narrow down
		// a GPU hang by turning hardware features off; empty by default
		std::string radvDebug;
		// only in ps5cemu.json: more of the Vulkan driver's environment, name to value, for its
		// performance experiments (RADV_, MESA_ and ACO_ names only), e.g.
		// {"RADV_THREADED_RECORDING": "1", "RADV_PS5_GPU_TIME": "1"}; empty by default
		std::map<std::string, std::string> radvEnvironment;
		// only in ps5cemu.json: how many draws Cemu records in a command buffer before it submits it,
		// for the driver's performance experiments (docs/DRIVER-PERFORMANCE.md); 0, the default,
		// keeps Cemu's own 300
		int cemuSubmitDraws = 0;
		int volume = 100;			  // the TV sound, in percent
		int gameCount = -1;			  // the Wii U library's games when last looked for (the start screen's); -1: never
		uint64_t lastGame = 0;		  // title ID
		std::vector<uint64_t> recent; // newest first, at most four
		N3ds n3ds;
		// The launcher's own sound, on both sides (frontend/sound.h): its music ("setup" or "off") and the music's volume in percent, and the menu's sounds.
		std::string music = "setup";
		int musicVolume = 50;
		bool boxArt = true;			  // box art from GameTDB (Settings > Online and updates)
		bool menuSounds = true;
		// The emulator the launcher opens on, the one last played ("wiiu" or "3ds"); empty: the
		// start screen, to choose.
		std::string side;
		// Why the last game did not start, when that needed a fresh process to show (the launcher
		// shows it once, then clears it).
		std::string launchError;
		Ui ui;
	};

	Launcher Load();
	bool Save(const Launcher& settings);
	// The game played last and first among the recent ones (at most four).
	void AddRecent(uint64_t& lastGame, std::vector<uint64_t>& recent, uint64_t titleId);
	inline void AddRecent(Launcher& settings, uint64_t titleId) { AddRecent(settings.lastGame, settings.recent, titleId); }
}
