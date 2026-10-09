// SPDX-License-Identifier: GPL-3.0-or-later
#include "ingame.h"
#include "boxart.h"
#include "emulator.h"
#include "gameinfo.h"
#include "lang.h"
#include "menu_canvas.h"
#include "paths.h"
#include "side_menu.h"
#include "tga.h"
#include "usb_devices.h"
#include "../ps5/display.h"
#include "../ps5/kernel.h"
#include "../ps5/log.h"
#include "../ps5/pad.h"

#include "audio/IAudioAPI.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/OS/libs/nfc/nfc.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"
#include "config/CemuConfig.h"
#include "imgui/imgui_extension.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace
{
	using ps5lang::Percent;
	using ps5lang::Tr;
	using ps5lang::TrF;
	using ps5lang::TrMark;
	using ps5lang::TrP;

	std::atomic<bool> s_menuOpen{false};
	std::atomic<uint64_t> s_menuOpenedAt{0}; // sceKernelGetProcessTime
	std::atomic<bool> s_cornerScreen{false};
	std::atomic<bool> s_saveRequested{false};
	std::atomic<bool> s_libraryRequested{false};
	std::atomic<bool> s_keyboardShift{false};

	struct Pointer
	{
		bool finger = false;
		float x = 0.5f, y = 0.5f;
		bool touching = false;
	};
	std::mutex s_pointerMutex;
	Pointer s_pointer;

	// The rest belongs to the render thread, which draws the overlays.
	struct Area
	{
		bool visible = false;
		float x = 0, y = 0, width = 0, height = 0;
	};
	Area s_gamePadArea; // where the GamePad's picture is this frame
	uint32_t s_buttons = 0, s_pressed = 0; // player 1's buttons, and those pressed since the last frame
	uint32_t s_menuButtons = 0;			   // the same, with the left stick's directions as the D-pad's
	std::string s_gameName;
	// amiibo dumps in /data/ps5cemu/amiibo, looked for as the menu opens; the one chosen, and what
	// came of the last scan
	std::vector<std::string> s_amiibo;
	int s_amiiboIndex = 0;
	std::string s_amiiboMessage;

	void FindAmiibo()
	{
		s_amiibo.clear();
		std::error_code error;
		for (const auto& entry : std::filesystem::directory_iterator(PS5CEMU_DATA "/amiibo", error))
		{
			std::string extension = entry.path().extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
			if (entry.is_regular_file(error) && extension == ".bin")
				s_amiibo.push_back(entry.path().filename().string());
		}
		std::sort(s_amiibo.begin(), s_amiibo.end());
		s_amiiboIndex = s_amiibo.empty() ? 0 : std::clamp(s_amiiboIndex, 0, (int)s_amiibo.size() - 1);
	}
	// The menu: its panel's place in the list, which category is open (the render thread's), and a
	// new opening, asked for by ToggleMenu on the game's loop
	ps5menu::SideMenu s_side;
	std::atomic<bool> s_menuFresh{false};
	int s_controlsPlayer = 0;

	// The game, for the menu's top: set as it starts (SetGame), read by the render thread
	std::mutex s_gameMutex;
	std::string s_gameDetails;	// GameTDB's publisher and year, and the version
	std::string s_coverPath;	// its box art, or its icon
	// the picture, made by the render thread the first time the menu opens
	bool s_coverTried = false;
	ImTextureID s_cover = nullptr;
	int s_coverWidth = 0, s_coverHeight = 0;

	float UiScale()
	{
		// VideoOut is 3840x2160: the overlays at the size they have on a 1080p screen
		return std::max(1.0f, ImGui::GetIO().DisplaySize.y / 1080.0f);
	}

	using ps5menu::DrawCursor;

	void CloseMenu()
	{
		s_menuOpen = false;
		s_saveRequested = true;
	}

	void SetVolume(int volume)
	{
		GetConfig().tv_volume = std::clamp(volume, 0, 100);
		std::shared_lock lock(g_audioMutex);
		if (g_tvAudio)
			g_tvAudio->SetVolume(GetConfig().tv_volume);
	}

	// The launcher's look, in Cemu's dark blue (menu_canvas.h)
	using ps5menu::Canvas;

	const char* TypeName(ps5emu::EmulatedType type)
	{
		switch (type)
		{
		case ps5emu::EmulatedType::GamePad: return Tr("Wii U GamePad");
		case ps5emu::EmulatedType::Pro: return Tr("Wii U Pro Controller");
		case ps5emu::EmulatedType::Classic: return Tr("Classic Controller");
		case ps5emu::EmulatedType::Wiimote: return Tr("Wii Remote");
		case ps5emu::EmulatedType::Nunchuk: return Tr("Wii Remote + Nunchuk");
		case ps5emu::EmulatedType::None: break;
		}
		return Tr("No controller");
	}

	// The emulated controllers a player can have: Cemu has two GamePads at most.
	std::vector<ps5emu::EmulatedType> TypesFor(int player)
	{
		int otherGamePads = 0;
		for (int other = 0; other < ps5pad::kMaxPlayers; other++)
			if (other != player && ps5emu::GetPlayerControls(other).type == ps5emu::EmulatedType::GamePad)
				otherGamePads++;
		std::vector<ps5emu::EmulatedType> types;
		if (otherGamePads < 2)
			types.push_back(ps5emu::EmulatedType::GamePad);
		for (auto type : {ps5emu::EmulatedType::Pro, ps5emu::EmulatedType::Classic, ps5emu::EmulatedType::Wiimote, ps5emu::EmulatedType::Nunchuk})
			types.push_back(type);
		return types;
	}

	// The menu's rows as things are now: a few settings, and categories that open in place
	std::vector<ps5menu::Row> MenuRows()
	{
		auto& config = GetConfig();
		// tr: how a game's picture is scaled: the methods' names
		static const char* kFilters[] = {TrMark("Bilinear"), TrMark("Bicubic"), TrMark("Bicubic Hermite"), TrMark("Nearest neighbour")};
		const bool gamePadMain = LatteGPUState.isDRCPrimary;
		const bool stretch = config.fullscreen_scaling == kStretch;
		const bool overlay = config.overlay.position != ScreenPosition::kDisabled;
		const int filter = std::clamp((int)config.upscale_filter, 0, 3);
		const int player = s_controlsPlayer;
		const auto controls = ps5emu::GetPlayerControls(player);
		const auto mappings = ps5emu::ListMappings(player);
		// A, B, X and Y come first on the controllers that have them: on Circle where the Wii U has
		// A, or on Cross
		const bool faceButtons = mappings.size() > 3 && mappings[0].button == "A" && mappings[3].button == "Y";
		const bool aOnCircle = faceButtons && mappings[0].input == "Circle";
		using Row = ps5menu::Row;
		std::vector<Row> rows;
		const std::string on = Tr("On"), off = Tr("Off");
		// tr: the screens in the Wii U's menu: the TV's picture and the GamePad's
		const std::string tv = Tr("TV"), gamePad = Tr("GamePad");
		rows.push_back({"screens", Tr("Screens"), fmt::format("{} \u00b7 {}", gamePadMain ? gamePad : tv, stretch ? Tr("Stretched") : Tr("Its shape")), false,
			Tr("The TV's and the GamePad's pictures, and how they fill the screen."), {
			{"main", Tr("Main screen"), gamePadMain ? gamePad : tv, true,
				Tr("Which picture fills the TV: the TV's or the GamePad's. In the game, touchpad click + L1 swaps them.")},
			{"corner", gamePadMain ? Tr("TV in a corner") : Tr("GamePad in a corner"), s_cornerScreen ? on : off, true,
				Tr("The other screen, small in the bottom right corner. In the game, touchpad click + R1.")},
			{"scaling", Tr("Picture"), stretch ? Tr("Stretched") : Tr("Its own shape"), true,
				Tr("Its own shape keeps the picture's proportions, with bars where they differ from the screen's; Stretched fills it.")},
		}});
		rows.push_back({"graphics", Tr("Graphics"), Tr(kFilters[filter]), false,
			Tr("Upscaling, Cemu's accuracy settings and the performance overlay. Kept for the next games."), {
			{"upscaling", Tr("Upscaling to 4K"), Tr(kFilters[filter]), true,
				Tr("How the picture is scaled to the screen: Bicubic is sharp, Hermite softer, Bilinear softer still.")},
			{"barriers", Tr("Accurate barriers"), config.vk_accurate_barriers ? on : off, true,
				Tr("Off can raise the frame rate, but some games then flicker or show wrong shadows. It changes at once.")},
			{"async", Tr("Async shader compile"), config.async_compile ? on : off, true,
				Tr("On, new shaders build while the game carries on: no stutter, but things may be missing for a moment.")},
			{"pacing", Tr("Frame pacing"), ps5display::FramePacingName(ps5display::FramePacing(), ps5display::OutputRefresh() > 100000), true,
				Tr("Holds the game to an even rate: 60 fps for 4K at 120 Hz, 30 for 8K at 60 Hz. Off shows each frame when it is done.")},
			{"overlay", Tr("Performance overlay"), overlay ? on : off, true,
				Tr("Frames per second, CPU and memory use in the top left corner, as Cemu shows them.")},
		}});
		// the game's graphic packs, as the main thread last listed them: each one on or off, and the
		// presets of those that are on
		{
			const auto running = ps5emu::GetRunningGraphicPacks();
			std::vector<Row> packs;
			for (size_t i = 0; i < running.packs.size(); i++)
			{
				const auto& pack = running.packs[i];
				const bool nextStart = i < running.nextStart.size() && running.nextStart[i];
				// tr: a setting that the game takes only when it starts again: "On (next start)"
				packs.push_back({fmt::format("pack:{}", i), pack.folder.empty() ? pack.name : pack.folder + " / " + pack.name,
					nextStart ? TrF("{0} (next start)", pack.enabled ? on : off) : (pack.enabled ? on : off), true,
					nextStart ? Tr("This pack replaces textures: the game shows the change when it starts again.") :
								Tr("Cross turns it on or off; the game shows it at once.")});
				if (!pack.enabled)
					continue;
				for (size_t c = 0; c < pack.choices.size(); c++)
				{
					const auto& choice = pack.choices[c];
					if (choice.presets.empty())
						continue;
					packs.push_back({fmt::format("preset:{}:{}", i, c), "   " + (choice.category.empty() ? std::string(Tr("Preset")) : choice.category),
						choice.presets[std::clamp(choice.active, 0, (int)choice.presets.size() - 1)], true,
						Tr("Left and Right choose another; the game shows it at once.")});
				}
			}
			if (packs.empty())
				packs.push_back({"nopacks", running.version ? Tr("None for this game") : Tr("Reading the packs…"), "", false,
					Tr("The community graphic packs have none for this game.")});
			const auto enabled = std::count_if(running.packs.begin(), running.packs.end(), [](const auto& pack) { return pack.enabled; });
			rows.push_back({"packs", Tr("Graphic packs"), running.packs.empty() ? std::string(Tr("None")) : TrP(enabled, "{0} on", "{0} on"), false,
				Tr("The game's community graphic packs: resolution, frame rate and mods, as Cemu's Graphic Packs window has them."), packs});
		}
		// the toys-to-life portals: each device's switch, and the figures on the ones plugged in
		{
			std::vector<Row> usb;
			for (ps5usb::Device device : ps5usb::kDevices)
			{
				const int d = (int)device;
				const bool on = ps5usb::Enabled(device), plugged = ps5usb::Plugged(device);
				usb.push_back({fmt::format("usb:{}", d), ps5usb::Name(device),
					on != plugged ? TrF("{0} (next start)", on ? Tr("On") : Tr("Off")) : std::string(on ? Tr("On") : Tr("Off")), true,
					on != plugged ? Tr("The game sees the change when it starts again: the portal is plugged in as a game starts.") :
									Tr("Plugged in as a game starts, as in Cemu. Its figures are below while it is on.")});
				if (!plugged)
					continue;
				const auto figures = ps5usb::Figures(device);
				const auto slots = ps5usb::Slots(device);
				for (size_t s = 0; s < slots.size(); s++)
					// tr: {0} is a folder of figure dumps
					usb.push_back({fmt::format("figure:{}:{}", d, s), "   " + slots[s].label,
						slots[s].figure.empty() ? std::string(Tr("Empty")) : fs::path(slots[s].figure).stem().string(), true,
						figures.empty() ? TrF("Put figure dumps in {0} to put them on here.", ps5usb::Folder(device)) :
										  std::string(Tr("Left and Right put the next figure on it; Empty takes it off."))});
			}
			const std::string error = ps5usb::LastError();
			if (!error.empty())
				usb.push_back({"usberror", error, "", false, Tr("The last figure could not be put on.")});
			const auto plugged = std::count_if(std::begin(ps5usb::kDevices), std::end(ps5usb::kDevices), [](ps5usb::Device device) { return ps5usb::Plugged(device); });
			rows.push_back({"usb", Tr("USB devices"), plugged ? TrP(plugged, "{0} on", "{0} on") : std::string(Tr("Off")), false,
				Tr("Skylanders, Disney Infinity and LEGO Dimensions figures on Cemu's emulated portals, as amiibo are scanned."), usb});
		}
		Row volume{"volume", Tr("Volume"), Percent(config.tv_volume), true, Tr("The game's sound. Left and Right change it by 10%.")};
		volume.slider = config.tv_volume / 100.0f;
		rows.push_back(volume);
		// tr: amiibo is Nintendo's name for its figures, as it writes it in the language
		rows.push_back({"amiibo", Tr("Amiibo"), !s_amiiboMessage.empty() ? s_amiiboMessage : s_amiibo.empty() ? std::string(Tr("None")) : s_amiibo[s_amiiboIndex],
			true,
			s_amiibo.empty() ? Tr("Put amiibo dumps (.bin) in /data/ps5cemu/amiibo to scan them here.") :
							   Tr("Left and Right choose an amiibo dump; Cross touches it to the GamePad when the game asks for one.")});
		rows.push_back({"controls", Tr("Controls"), TypeName(ps5emu::GetPlayerControls(0).type), false,
			Tr("Each player's controller, motion, vibration, deadzones and A and B."), {
			{"player", Tr("Player"), controls.connected ? std::to_string(player + 1) : TrF("{0} (no DualSense)", player + 1), true,
				Tr("Whose controller the settings below are: Left and Right choose the player.")},
			{"type", Tr("Emulated controller"), TypeName(controls.type), true,
				Tr("What the game sees in this player's hands. Most games want the GamePad for player 1.")},
			{"motion", Tr("Motion controls"), !controls.hasMotion ? std::string(Tr("None on this one")) : controls.motion ? on : off, true,
				Tr("The DualSense's gyroscope and accelerometer, for the games that aim or steer by tilting.")},
			{"rumble", Tr("Vibration"), controls.rumble ? Percent(controls.rumble) : off, true,
				Tr("How strongly the DualSense rumbles. Left and Right change it by 10%.")},
			{"left", Tr("Left stick deadzone"), Percent(controls.leftDeadzone), true,
				Tr("How far the left stick moves before the game sees it. Raise it if a character drifts.")},
			{"right", Tr("Right stick deadzone"), Percent(controls.rightDeadzone), true,
				Tr("How far the right stick moves before the game sees it. Raise it if the camera drifts.")},
			{"layout", Tr("A and B"), !faceButtons ? std::string("-") : aOnCircle ? Tr("A on Circle") : Tr("A on Cross"), true,
				Tr("A on Circle as on the Wii U, or on Cross, with X and Y swapped to match. Every button: launcher's Settings > Controls.")},
		}});
		Row library{"library", Tr("Quit to the library"), "", false,
			Tr("Leaves the game for the library. What you have not saved in the game is lost, so Cross is held.")};
		library.apart = true;
		library.hold = true;
		rows.push_back(library);
		return rows;
	}

	// The quick actions above the list: back to the game, the TV and GamePad swapped, the graphic
	// packs, and the amiibo chosen in the list touched to the GamePad
	std::vector<ps5menu::Tile> Tiles()
	{
		// tr: the quick actions: big tiles at the top of the in-game menu, a word each
		return {{"resume", Tr("Resume"), "resume"}, {"main", Tr("Screens"), "swap", LatteGPUState.isDRCPrimary ? Tr("GamePad main") : Tr("TV main")},
			{"packs", Tr("Graphic packs"), "packs"}, {"amiibo", Tr("Amiibo"), "amiibo", s_amiibo.empty() ? Tr("None") : Tr("Scan")}};
	}

	// What a row chosen or changed does
	void Act(const ps5menu::Action& action)
	{
		auto& config = GetConfig();
		const std::string& id = action.id;
		const int change = action.change;
		const bool chosen = action.chosen;
		const int player = s_controlsPlayer;
		const auto controls = ps5emu::GetPlayerControls(player);
		if (id == "resume")
			CloseMenu();
		else if (id == "main")
			ps5ingame::SwapScreens();
		else if (id == "corner")
			ps5ingame::ToggleCornerScreen();
		else if (id == "upscaling")
			config.upscale_filter = (std::clamp((int)config.upscale_filter, 0, 3) + (change < 0 ? 3 : 1)) % 4;
		else if (id == "scaling")
			config.fullscreen_scaling = config.fullscreen_scaling == kStretch ? kKeepAspectRatio : kStretch;
		else if (id == "overlay")
		{
			const bool overlay = config.overlay.position != ScreenPosition::kDisabled;
			config.overlay.position = overlay ? ScreenPosition::kDisabled : ScreenPosition::kTopLeft;
			config.overlay.fps = config.overlay.cpu_usage = config.overlay.ram_usage = true;
		}
		else if (id == "barriers")
		{
			// the renderer reads it at each draw; settings.xml keeps it, as Cemu's menu does
			config.vk_accurate_barriers = !config.vk_accurate_barriers;
			ps5log::Line("[ingame] accurate barriers: {}", config.vk_accurate_barriers ? "on" : "off");
		}
		else if (id == "pacing")
			// VideoOut takes it from the next flip on
			ps5display::SetFramePacing((ps5display::FramePacing() - 1 + (change < 0 ? 2 : 1)) % 3 + 1);
		else if (id == "async")
		{
			// read as each new pipeline is made; Cemu's compile threads run either way
			config.async_compile = !config.async_compile;
			ps5log::Line("[ingame] async shader compile: {}", config.async_compile ? "on" : "off");
		}
		else if (id == "amiibo" && !s_amiibo.empty())
		{
			s_amiiboMessage.clear();
			if (!chosen)
				s_amiiboIndex = (s_amiiboIndex + change + (int)s_amiibo.size()) % (int)s_amiibo.size();
			else
			{
				const std::string& name = s_amiibo[s_amiiboIndex];
				uint32 nfcError = 0;
				const bool scanned = nfc::TouchTagFromFile(fs::path(PS5CEMU_DATA "/amiibo") / name, &nfcError);
				// tr: {0} is an amiibo dump's file name
				s_amiiboMessage = scanned ? TrF("Scanned {0}", name) : TrF("Not scanned (error {0})", fmt::format("{:#x}", nfcError));
				ps5log::Line("[ingame] amiibo {}: {}", name, s_amiiboMessage);
			}
		}
		else if (id == "volume")
			// Cross goes up by 10, and from 100 back to 0
			SetVolume(chosen && config.tv_volume >= 100 ? 0 : config.tv_volume + change * 10);
		else if (id == "library")
			s_libraryRequested = true; // held to here
		else if (id.rfind("usb:", 0) == 0)
		{
			const auto device = (ps5usb::Device)std::stoi(id.substr(4));
			ps5usb::SetEnabled(device, !ps5usb::Enabled(device));
		}
		else if (id.rfind("figure:", 0) == 0)
		{
			// the next figure in the folder, or the one before; Empty between the last and the first
			int d = 0;
			size_t slot = 0;
			std::sscanf(id.c_str(), "figure:%d:%zu", &d, &slot);
			const auto device = (ps5usb::Device)d;
			const auto figures = ps5usb::Figures(device);
			const auto slots = ps5usb::Slots(device);
			if (slot < slots.size())
			{
				std::vector<std::string> choices{""};
				choices.insert(choices.end(), figures.begin(), figures.end());
				const auto at = std::find(choices.begin(), choices.end(), slots[slot].figure);
				const int now = at == choices.end() ? 0 : (int)(at - choices.begin());
				const int count = (int)choices.size();
				ps5usb::RequestFigure(device, slot, choices[(now + (change < 0 ? count - 1 : 1)) % count]);
			}
		}
		else if (id.rfind("pack:", 0) == 0)
			ps5emu::RequestGraphicPackToggle(std::stoul(id.substr(5)));
		else if (id.rfind("preset:", 0) == 0)
		{
			// the next or the one before of the presets the menu shows
			size_t index = 0, choiceIndex = 0;
			std::sscanf(id.c_str(), "preset:%zu:%zu", &index, &choiceIndex);
			const auto running = ps5emu::GetRunningGraphicPacks();
			if (index < running.packs.size() && choiceIndex < running.packs[index].choices.size())
			{
				const auto& choice = running.packs[index].choices[choiceIndex];
				const int count = (int)choice.presets.size();
				if (count > 0)
					ps5emu::RequestGraphicPackPreset(index, choice.category,
						choice.presets[(std::clamp(choice.active, 0, count - 1) + (change < 0 ? count - 1 : 1)) % count]);
			}
		}
		else if (id == "player")
			s_controlsPlayer = (player + change + ps5pad::kMaxPlayers) % ps5pad::kMaxPlayers;
		else if (id == "type")
		{
			const auto types = TypesFor(player);
			int at = 0;
			for (int t = 0; t < (int)types.size(); t++)
				if (types[t] == controls.type)
					at = t;
			ps5emu::SetEmulatedType(player, types[(at + change + (int)types.size()) % types.size()]);
		}
		else if (id == "motion" && controls.hasMotion)
			ps5emu::SetMotion(player, !controls.motion);
		else if (id == "rumble")
		{
			int rumble = controls.rumble + change * 10;
			if (chosen && rumble > 100)
				rumble = 0;
			ps5emu::SetRumble(player, std::clamp(rumble, 0, 100));
			if (rumble > 0)
				ps5pad::SetVibrationEnabled(true);
		}
		else if (id == "left" || id == "right")
		{
			const bool leftStick = id == "left";
			int value = (leftStick ? controls.leftDeadzone : controls.rightDeadzone) + change * 5;
			if (chosen && value > 50)
				value = 0;
			value = std::clamp(value, 0, 50);
			ps5emu::SetDeadzones(player, leftStick ? value : controls.leftDeadzone, leftStick ? controls.rightDeadzone : value);
		}
		else if (id == "layout")
		{
			const auto mappings = ps5emu::ListMappings(player);
			if (mappings.size() > 3 && mappings[0].button == "A" && mappings[3].button == "Y")
			{
				using ps5emu::PadInput;
				const bool aOnCircle = mappings[0].input == "Circle";
				ps5emu::SetMapping(player, 0, aOnCircle ? PadInput::Cross : PadInput::Circle);
				ps5emu::SetMapping(player, 1, aOnCircle ? PadInput::Circle : PadInput::Cross);
				ps5emu::SetMapping(player, 2, aOnCircle ? PadInput::Square : PadInput::Triangle);
				ps5emu::SetMapping(player, 3, aOnCircle ? PadInput::Triangle : PadInput::Square);
			}
		}
	}

	void DrawMenu(float scale)
	{
		using ps5menu::Weight;
		const ps5menu::Fonts fonts{ImGui_GetFontFace((int)Weight::SemiBold, ps5menu::kHeading * scale),
			ImGui_GetFontFace((int)Weight::Medium, ps5menu::kBody * scale), ImGui_GetFontFace((int)Weight::Regular, ps5menu::kLabel * scale),
			ImGui_GetFontFace((int)Weight::Regular, ps5menu::kCaption * scale), ImGui_GetFontFace((int)Weight::SemiBold, ps5menu::kChip * scale)};
		if (!fonts.heading || !fonts.body || !fonts.label || !fonts.caption || !fonts.chip)
			return; // ready next frame
		if (s_menuFresh.exchange(false))
		{
			// opened: on its first row, every category closed, the shortcut's buttons not counted
			s_side.Reset();
			FindAmiibo();
			ps5emu::RequestGraphicPackList(); // the main thread lists them for the menu
			ps5usb::Refresh();				  // the figures in their folders
			std::lock_guard lock(s_gameMutex);
			if (s_gameName.empty())
				s_gameName = CafeSystem::GetForegroundTitleName();
		}
		bool close = false;
		const std::vector<ps5menu::Tile> tiles = Tiles();
		const ps5menu::Action action = s_side.Update(MenuRows(), tiles, s_menuButtons, sceKernelGetProcessTime(), close);
		if (close)
		{
			CloseMenu();
			return;
		}
		if (!action.id.empty())
			Act(action);
		ImGuiIO& io = ImGui::GetIO();
		const ImVec2 origin{(io.DisplaySize.x - 1920.0f * scale) * 0.5f, (io.DisplaySize.y - 1080.0f * scale) * 0.5f};
		const Canvas canvas{ImGui::GetForegroundDrawList(), scale, origin, ps5menu::kBlue};
		ps5menu::Header header;
		header.system = "Wii U";
		// tr: the in-game menu's badge: the game goes on behind the menu (Cemu cannot pause it)
		header.status = Tr("Running"); // Cemu has no safe pause yet
		{
			std::lock_guard lock(s_gameMutex);
			header.title = s_gameName;
			header.details = s_gameDetails;
		}
		header.cover = s_cover;
		header.coverWidth = (float)s_coverWidth;
		header.coverHeight = (float)s_coverHeight;
		s_side.Draw(canvas, fonts, header, MenuRows(), Tiles(),
			{{"cross", Tr("Choose")}, {"leftright", Tr("Change")}, {"circle", s_side.Open().empty() ? Tr("Back to the game") : Tr("Back")}});
	}
}

namespace ps5ingame
{
	void SetGame(const ps5emu::Game& game)
	{
		// the menu's typeface, Lexend's three weights, for Cemu's ImGui to make at the menu's sizes, with
		// the console's font for what Lexend lacks (a Russian menu, a Japanese title), cut to the characters
		// the language's words and the game's name have (docs/UI-REDESIGN.md, 7.7)
		const ImWchar* ranges = ps5menu::GlyphRanges(game.name);
		int mergeFace = 0;
		const auto& merge = ps5menu::MergeFont(mergeFace);
		for (const ps5menu::Weight weight : {ps5menu::Weight::Regular, ps5menu::Weight::Medium, ps5menu::Weight::SemiBold})
		{
			const auto& file = ps5menu::FontFile(weight);
			if (file.empty())
				continue;
			ImGui_SetFontFace((int)weight, file.data(), (int)file.size(), ranges);
			if (!merge.empty())
				ImGui_SetFontFaceMerge((int)weight, merge.data(), (int)merge.size(), ranges, mergeFace);
		}
		if (!merge.empty())
			ps5log::Line("[ingame] the menu's second font: {} (face {}, {} KiB)", ps5lang::GetMenuFont().path, mergeFace, merge.size() >> 10);
		ps5gameinfo::Info info;
		const bool known = ps5gameinfo::Find(ps5boxart::System::WiiU, game.gameId, info);
		std::string details;
		auto add = [&](const std::string& part) {
			if (!part.empty())
				details += (details.empty() ? "" : "  /  ") + part;
		};
		if (known)
		{
			add(info.publisher);
			add(ps5gameinfo::Year(info.released));
		}
		add(fmt::format("v{}", game.version));
		std::string cover = ps5boxart::Path(ps5boxart::System::WiiU, game.gameId);
		if (cover.empty())
			cover = ps5emu::CoverPath(game.titleId);
		std::lock_guard lock(s_gameMutex);
		s_gameName = game.name;
		s_gameDetails = details;
		s_coverPath = cover;
	}

	void SetGamePadPointer(bool finger, float x, float y, bool touching)
	{
		std::lock_guard lock(s_pointerMutex);
		s_pointer.finger = finger;
		if (finger || touching)
		{
			s_pointer.x = x;
			s_pointer.y = y;
		}
		s_pointer.touching = touching;
	}

	bool OverlayTakesInput()
	{
		return s_menuOpen || swkbd_hasKeyboardInputHook();
	}

	void ToggleMenu()
	{
		if (s_menuOpen)
		{
			CloseMenu();
			return;
		}
		s_menuOpenedAt = sceKernelGetProcessTime();
		s_menuFresh = true;
		s_menuOpen = true;
	}

	void SwapScreens()
	{
		LatteGPUState.isDRCPrimary = !LatteGPUState.isDRCPrimary;
	}

	void ToggleCornerScreen()
	{
		s_cornerScreen = !s_cornerScreen;
	}

	bool TakeSaveRequest()
	{
		return s_saveRequested.exchange(false);
	}

	bool TakeLibraryRequest()
	{
		return s_libraryRequested.exchange(false);
	}
}

// Called by Cemu's patched code (patches/cemu), on the render thread.

// ImGui's input, each frame (imgui_extension.cpp): the D-pad and the left stick move between items
// and Cross chooses, as with ImGui's gamepad navigation. Circle, Triangle and Options are read
// here instead: ImGui gives them uses that do not suit the overlays (Circle drops the selection).
// Over the menu or the keyboard, the touchpad is also the mouse.
void PS5Cemu_ImGuiInput()
{
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
	ps5pad::Data data{};
	const bool connected = ps5pad::Read(0, data);
	const uint32_t buttons = connected ? data.buttons : 0;
	s_pressed = buttons & ~s_buttons;
	s_buttons = buttons;
	s_menuButtons = buttons;
	if (connected)
	{
		if (data.leftY < 64)
			s_menuButtons |= ps5pad::kUp;
		else if (data.leftY > 192)
			s_menuButtons |= ps5pad::kDown;
		if (data.leftX < 64)
			s_menuButtons |= ps5pad::kLeft;
		else if (data.leftX > 192)
			s_menuButtons |= ps5pad::kRight;
	}

	const bool touchpadHeld = buttons & ps5pad::kTouchPad; // a shortcut's, not the overlays'
	auto key = [&](ImGuiKey imguiKey, uint32_t mask) { io.AddKeyEvent(imguiKey, !touchpadHeld && (buttons & mask)); };
	key(ImGuiKey_GamepadDpadUp, ps5pad::kUp);
	key(ImGuiKey_GamepadDpadDown, ps5pad::kDown);
	key(ImGuiKey_GamepadDpadLeft, ps5pad::kLeft);
	key(ImGuiKey_GamepadDpadRight, ps5pad::kRight);
	key(ImGuiKey_GamepadFaceDown, ps5pad::kCross);
	auto stick = [&](ImGuiKey negative, ImGuiKey positive, uint8_t raw) {
		const float value = connected ? std::clamp((raw - 128) / 127.0f, -1.0f, 1.0f) : 0.0f;
		io.AddKeyAnalogEvent(negative, value < -0.5f, std::max(-value, 0.0f));
		io.AddKeyAnalogEvent(positive, value > 0.5f, std::max(value, 0.0f));
	};
	stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, data.leftX);
	stick(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, data.leftY);

	// Cemu's keyboard reads these itself: Circle deletes, Options is done (swkbd.cpp)
	const bool keyboard = !s_menuOpen && swkbd_hasKeyboardInputHook();
	io.NavInputs[ImGuiNavInput_Cancel] = keyboard && !touchpadHeld && (buttons & ps5pad::kCircle) ? 1.0f : 0.0f;
	io.NavInputs[ImGuiNavInput_Input] = keyboard && !touchpadHeld && (buttons & ps5pad::kOptions) ? 1.0f : 0.0f;
	if (keyboard && (s_pressed & ps5pad::kTriangle))
		s_keyboardShift = true;

	io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
	io.MouseDown[0] = false;
	io.MouseDrawCursor = false; // RenderOverlay draws one the size of the rest
	if (ps5ingame::OverlayTakesInput() && connected && data.touchCount > 0)
	{
		float width, height;
		ps5pad::TouchResolution(0, width, height);
		io.MousePos = {std::clamp(data.touch[0].x / width, 0.0f, 1.0f) * io.DisplaySize.x,
			std::clamp(data.touch[0].y / height, 0.0f, 1.0f) * io.DisplaySize.y};
		io.MouseDown[0] = touchpadHeld;
	}
}

bool PS5Cemu_MenuOpen()
{
	return s_menuOpen;
}

// Before ImGui's frame, outside its render pass (patches/cemu, VulkanRenderer::ImguiBegin): the
// game's box art for the menu, the first time it opens (kept for the session: the app starts over
// after the game).
void PS5Cemu_ImguiUploads()
{
	if (!s_menuOpen || s_coverTried || !g_renderer)
		return;
	s_coverTried = true;
	std::string path;
	{
		std::lock_guard lock(s_gameMutex);
		path = s_coverPath;
	}
	std::vector<uint8_t> rgba;
	int width = 0, height = 0;
	if (path.empty() || !ps5tga::Load(path, 264, 352, rgba, width, height))
	{
		ps5log::Line("[ingame] no box art for the menu ({})", path.empty() ? "none fetched" : path);
		return;
	}
	std::vector<uint8> rgb((size_t)width * height * 3);
	for (size_t i = 0; i < (size_t)width * height; i++)
		std::memcpy(&rgb[i * 3], &rgba[i * 4], 3);
	s_cover = g_renderer->GenerateTexture(rgb, {width, height});
	if (s_cover)
	{
		s_coverWidth = width;
		s_coverHeight = height;
	}
}

// Triangle on Cemu's keyboard: shift (swkbd.cpp).
bool PS5Cemu_KeyboardShiftPressed()
{
	return s_keyboardShift.exchange(false);
}

// Whether the screen not shown whole goes in a corner (LatteRenderTarget.cpp).
bool PS5Cemu_SecondScreenInCorner(bool /*gamePadIsPrimary*/)
{
	return s_cornerScreen;
}

// Where the GamePad's picture is this frame, whole or in the corner, or that it is not shown.
void PS5Cemu_SetGamePadArea(bool visible, sint32 x, sint32 y, sint32 width, sint32 height)
{
	s_gamePadArea = {visible, (float)x, (float)y, (float)width, (float)height};
}

// Over the game's picture, in its ImGui frame: the menu, and the touchpad's cursor, on the menu or
// the keyboard while they are up, otherwise on the GamePad's picture where it is shown.
void PS5Cemu_RenderOverlay()
{
	const float scale = UiScale();
	if (s_menuOpen)
	{
		DrawMenu(scale);
		return;
	}
	ImDrawList* draw = ImGui::GetForegroundDrawList();
	if (ps5ingame::OverlayTakesInput())
	{
		if (ImGui::IsMousePosValid())
			DrawCursor(draw, ImGui::GetIO().MousePos, ImGui::GetIO().MouseDown[0], scale);
		return;
	}
	Pointer pointer;
	{
		std::lock_guard lock(s_pointerMutex);
		pointer = s_pointer;
	}
	if ((pointer.finger || pointer.touching) && s_gamePadArea.visible)
		DrawCursor(draw, {s_gamePadArea.x + pointer.x * s_gamePadArea.width, s_gamePadArea.y + pointer.y * s_gamePadArea.height},
			pointer.touching, scale);
}
