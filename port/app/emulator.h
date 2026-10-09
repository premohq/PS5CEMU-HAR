// SPDX-License-Identifier: GPL-3.0-or-later
// PS5Cemu: Cemu's core, started and driven the way its desktop frontend does (src/main.cpp's
// CemuCommonInit, gui/wxgui's CemuApp::OnInit and MainWindow::FileLoad), without wxWidgets.
//
// The launcher includes this header, so it names no Cemu type.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps5settings
{
	struct Launcher;
}

namespace ps5emu
{
	struct Game
	{
		uint64_t titleId = 0;		 // the base title
		std::string name;			 // from meta.xml
		std::filesystem::path path;	 // what Cemu launches
		uint16_t version = 0;		 // the base's, or the update's when installed
		bool hasUpdate = false;
		uint32_t dlcCount = 0;
		std::string format;			 // WUA, WUD, WUX, folder (code/content/meta) or RPX; 3DS, CIA, 3DSX...; NDS, DSI
		std::string publisher;		 // a 3DS game's, from its SMDH; a DS game's, from its banner
		std::string gameId;			 // the ID on its box, GameTDB's (boxart.h): ALZE01 (Wii U), AREE (3DS), AMCE (DS)
		bool nds = false;			 // a Nintendo DS game, which the DS side lists and melonDS plays (port/melonds)
	};

	// What the launcher's settings change in Cemu's.
	struct Options
	{
		std::string gamesFolder;
		bool overlay = false; // the performance overlay from the start
		int volume = 100;	  // the TV's, in percent
		int upscaleFilter = 1; // Cemu's upscale_filter: 0 linear, 1 bicubic, 2 bicubic Hermite, 3 nearest
		bool asyncShaders = true; // Cemu's async_compile
		bool gamePadSpeaker = false; // the GamePad's sound on player 1's DualSense speaker (Cemu's pad_device)
	};

	// Paths, settings (with PS5 defaults on first start), MLC, graphic packs, controllers and
	// Cemu's own initialisation. False, with a reason, when something essential is missing.
	bool InitializeCore(std::string& error);

	// Writes the launcher's settings into Cemu's (settings.xml). A new games folder is scanned.
	void ApplyOptions(const Options& options);

	// The game about to start and the settings it starts with (its side's, with its own over them:
	// ps5settings::ForGame), so that what its menu changes is saved where it belongs
	// (ps5settings::SaveChanges).
	void SetGameSettings(uint64_t titleId, const ps5settings::Launcher& settings);

	// How many draws Cemu records in a command buffer before it submits it, from the process's start
	// (ps5cemu.json's cemuSubmitDraws, a driver experiment: docs/DRIVER-PERFORMANCE.md); 0 keeps
	// Cemu's own 300.
	void SetSubmitDraws(int draws);

	// Whether Cemu is still looking for games (the library fills in when it is done).
	bool Scanning();

	// The games Cemu found in the game files folder and the MLC, sorted by name. Does not wait
	// for a scan in progress: it lists what was found so far.
	std::vector<Game> ListGames();

	// The Vulkan renderer on VideoOut, then the title. False, with a reason, when it cannot start.
	bool LaunchGame(const Game& game, std::string& error);

	// Whether LaunchGame got as far as the renderer: a failure after that point needs a fresh
	// process (RestartToLibrary) before the launcher can show again.
	bool RendererStarted();

	// Runs while the game does, handling the port's shortcuts. Returns when the player asks for the
	// library; the caller then restarts the app (RestartToLibrary).
	void RunGame();

	// Starts PS5Cemu over (sceSystemServiceLoadExec on its own eboot), which shows the library with
	// the last game selected. Cemu cannot yet shut a game down and start another reliably in one
	// process (MainWindow::EndEmulation says so), so leaving a game means a fresh process; PS5SX2
	// goes back to its shelf the same way. Returns only if the restart did not take.
	void RestartToLibrary();

	// A game's icon (meta/iconTex.tga) as an uncompressed top-down TGA in /data/ps5cemu/covers,
	// extracted on first use; empty when the game has none or it cannot be read.
	std::string CoverPath(uint64_t titleId);
	// Its boot screen (meta/bootTvTex.tga, the 1280 x 720 picture the Wii U shows while it loads), the
	// same way in /data/ps5cemu/covers/boot: the launcher's backdrop for it (docs/UI-REDESIGN.md 7.4).
	// Mounts the title, so the launcher asks for one a frame at most.
	std::string BootScreenPath(uint64_t titleId);

	// The community graphic packs (graphic_packs.cpp): what Cemu's Graphic Packs window shows for
	// one game, with its choices saved to settings.xml as that window saves them.
	struct GraphicPackChoice
	{
		std::string category;			  // "Resolution"; empty for a pack's presets without one
		std::vector<std::string> presets; // those the pack's conditions allow now, in its order
		int active = 0;					  // in presets
	};
	struct GraphicPackInfo
	{
		std::string name;		 // "Graphics"
		std::string folder;		 // "Mods", the folders between the game and the pack ("" for none)
		std::string description; // lines end with '\n'
		bool enabled = false;
		std::vector<GraphicPackChoice> choices; // one dropdown each, as in Cemu's window
	};
	std::vector<GraphicPackInfo> ListGraphicPacks(uint64_t titleId);
	int EnabledGraphicPackCount(uint64_t titleId);
	// Turns a pack of the game (by its index in ListGraphicPacks) on or off; returns its new state.
	bool ToggleGraphicPack(uint64_t titleId, size_t index);
	// Chooses one of a pack's presets in a category, as Cemu's window does, and turns the pack on.
	// The presets other categories allow can change with it (their conditions).
	void SetGraphicPackPreset(uint64_t titleId, size_t index, const std::string& category, const std::string& preset);
	// The in-game menu's graphic packs (the running game's): its thread asks, and the game's main
	// thread (RunGame) makes the change as Cemu's Graphic Packs window does while a game runs: the
	// pack activated or deactivated at once, its shaders and patches reloaded; a pack that replaces
	// textures takes effect when the game starts again. The menu reads the list as the main thread
	// last made it.
	struct RunningGraphicPacks
	{
		std::vector<GraphicPackInfo> packs;
		std::vector<bool> nextStart; // per pack: changed, but only the next start shows it
		uint32_t version = 0;		 // goes up each time the list is made again
	};
	void RequestGraphicPackToggle(size_t index);
	void RequestGraphicPackPreset(size_t index, const std::string& category, const std::string& preset);
	void RequestGraphicPackList();
	RunningGraphicPacks GetRunningGraphicPacks();
	// RunGame's: the requests made, and the list made again when asked
	void ServiceGraphicPackRequests();
	// Every pack read again (after newer community packs were installed: pack_updates.h), with the
	// choices settings.xml keeps.
	void ReloadGraphicPacks();

	// Cemu's controller settings for the four players (controllers.cpp), saved in its controller
	// profiles (controllerProfiles/controller0-3.xml) as its Input Settings window saves them. Each
	// player's controller is that player's DualSense.
	enum class EmulatedType
	{
		None,		 // no controller for this player
		GamePad,	 // the Wii U GamePad
		Pro,		 // the Wii U Pro Controller
		Classic,	 // the Classic Controller Pro
		Wiimote,	 // the Wii Remote
		Nunchuk,	 // the Wii Remote with a Nunchuk
	};
	struct PlayerControls
	{
		EmulatedType type = EmulatedType::None;
		bool connected = false; // the player's DualSense
		bool hasMotion = false; // the emulated controller has motion sensors (GamePad, Wii Remote)
		bool motion = false;	// the DualSense's are its
		int rumble = 0;			// vibration strength, percent
		int leftDeadzone = 25, rightDeadzone = 25; // percent of the stick's travel
	};
	PlayerControls GetPlayerControls(int player);
	// A new emulated controller, with the default buttons for it.
	void SetEmulatedType(int player, EmulatedType type);
	void SetMotion(int player, bool enabled);
	void SetRumble(int player, int percent);
	void SetDeadzones(int player, int left, int right);
	// The default buttons and settings for the emulated controller the player has.
	void ResetControls(int player);

	// What a mapping can be: a DualSense input. Sticks and triggers count in one direction each.
	enum class PadInput
	{
		None,
		Cross, Circle, Square, Triangle,
		L1, R1, L2, R2, L3, R3,
		Create, Options,
		Up, Down, Left, Right,
		LeftStickUp, LeftStickDown, LeftStickLeft, LeftStickRight,
		RightStickUp, RightStickDown, RightStickLeft, RightStickRight,
	};
	struct ButtonMapping
	{
		std::string button; // the emulated controller's ("ZL", "Left stick up")
		std::string input;	// the DualSense's ("L2"), empty when unmapped
	};
	std::vector<ButtonMapping> ListMappings(int player);
	void SetMapping(int player, size_t index, PadInput input);
	void ClearMapping(int player, size_t index);

	// Installing a game, update or DLC into the MLC, as Cemu's "Install game title, update or DLC"
	// does (install.cpp): from a folder with code, content and meta.
	struct InstallCandidate
	{
		enum class Kind
		{
			None,	// not a title Cemu can install (note says why)
			Game,
			Update,
			Dlc,
			System,
		} kind = Kind::None;
		std::string name;		  // the game's, from meta.xml
		uint64_t titleId = 0;
		uint16_t version = 0;
		int installedVersion = -1; // what the MLC has in its place; -1 for nothing
		std::string note;
	};
	InstallCandidate InspectInstall(const std::string& folder);
	// Copies it on a thread of its own. False, with a reason, when it cannot start.
	bool StartInstall(const std::string& folder, std::string& error);
	struct InstallStatus
	{
		enum class State
		{
			Idle,
			Running,
			Done,
			Failed,
			Cancelled,
		} state = State::Idle;
		uint64_t copied = 0, total = 0; // bytes
		std::string message;			// the reason, when it failed
	};
	InstallStatus GetInstallStatus();
	// Stops an install and puts back what was there.
	void CancelInstall();
	// The boot log's memory line, once a minute in a game (Cemu's or Azahar's).
	void LogMemory();

	// Looks for games again (after an install), as at start: Scanning() is true until done.
	void Rescan();
}
