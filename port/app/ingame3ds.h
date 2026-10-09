// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the in-game menu over a 3DS game, a panel down the left as the Wii U's (ingame.h,
// side_menu.h) in Azahar's gold (menu_canvas.h), with the game's box art at its top. It is drawn with Cemu's ImGui and its Vulkan backend into Azahar's frames,
// through the hook patches/azahar adds to Azahar's renderer; the game's loop (port/azahar/core.cpp)
// applies what it changes. Both sides include this header, so it names no Cemu or Azahar type.
//
// A DS game on the 3DS side (melonDS: port/melonds) has the same menu, drawn into the frames of its
// screens (port/melonds/screens.cpp), with the DS's rows where the 3DS's would not apply: a screen
// filter for the internal resolution and texture filter, the lid for motion controls, no CPU clock
// and no amiibo.

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

namespace ps5ingame3ds
{
	// What the menu shows and changes
	struct Settings
	{
		int layout = 2;			  // ps5azahar::Layout
		bool swapScreens = false; // the bottom screen where the top one is
		int resolution = 6;		  // times the 3DS's 400x240
		int textureFilter = 0;	  // None, Anime4K, Bicubic, ScaleForce, xBRZ, MMPX
		int volume = 100;		  // percent
		bool performance = false; // the frame rate and speed in a corner
		int cpuClock = 100;		  // the emulated CPU's speed, percent of the 3DS's
		int speedLimit = 100;	  // the emulation's top speed, percent of the 3DS's; 0: none (for this game only)
		bool motion = true;
		int deadzone = 15;		  // percent
		bool aOnCircle = true;	  // A on Circle and B on Cross, where the 3DS has them
		int border = 0;			  // kBorderNames
		// the DS's (Console::Nds)
		int screenFilter = 0;	  // how its screens are scaled to the TV: kScreenFilters
		bool lidClosed = false;	  // the DS closed, for the games that ask (for this game only)
	};

	// Which console the game is, whose rows the menu shows
	enum class Console
	{
		N3ds, // Azahar's
		Nds,  // melonDS's
	};
	constexpr const char* kScreenFilters[] = {"Sharp", "Smooth", "Square pixels"};
	constexpr int kScreenFilterCount = (int)std::size(kScreenFilters);

	// Azahar's frame being recorded (its renderer's FrontendOverlayTarget): before its render pass
	// begins, then in it
	struct Target
	{
		VkInstance instance;
		VkPhysicalDevice physicalDevice;
		VkDevice device;
		uint32_t queueFamily;
		VkQueue queue;
		VkRenderPass renderPass;
		uint32_t imageCount;
		VkCommandBuffer commandBuffer;
		uint32_t width, height;
		bool insideRenderPass;
		// which of Azahar's renderers (a loaded save state makes it again, device and all): a new
		// device can have the old one's handle, so it is told apart by this
		uint64_t generation;
	};

	// When the game starts: its name and title ID, the settings it starts with, and its console.
	void Start(const std::string& name, uint64_t titleId, const Settings& settings, Console console = Console::N3ds);
	// The menu's top, once: GameTDB's facts on a line ("Nintendo  /  2015"), and the TGA of the
	// game's box art or icon (empty: none).
	void SetGame(const std::string& details, const std::string& coverPath);
	// Touchpad click + Options.
	void ToggleMenu();
	bool MenuOpen();

	// For the game's loop: a change made in the menu since the last call (the settings as they are
	// now), and whether the library was chosen.
	bool TakeChanges(Settings& settings);
	bool TakeLibraryRequest();
	// The performance overlay's numbers, about once a second: frames per second, speed in percent,
	// and where a frame's time goes (a line of text; empty: none).
	void SetPerformance(double fps, double speed, const std::string& breakdown);

	// Save states (the menu's Save states page), in slots 1 to kStateSlots. From the game's loop:
	// each slot's time (empty: nothing saved) and what came of the last save or load; for it, a save
	// (load false) or load chosen in the menu, once.
	constexpr int kStateSlots = 5;
	void SetStateSlots(const std::vector<std::string>& times);
	void SetStateMessage(const std::string& message);
	bool TakeStateRequest(bool& load, int& slot);

	// The same page's amiibo and cheats. From the game's loop: the amiibo files (names in
	// /data/ps5cemu/amiibo), the game's cheats (name, on) and what came of the last request; for it,
	// what the menu asked for, once.
	void SetExtras(const std::vector<std::string>& amiibo, const std::vector<std::pair<std::string, bool>>& cheats);
	void SetExtrasMessage(const std::string& message);
	struct ExtrasRequest
	{
		enum Kind
		{
			None,
			Amiibo,		  // scan amiibo[index]
			RemoveAmiibo,
			Cheat,		  // turn cheats[index] on or off
		} kind = None;
		int index = 0;
	};
	bool TakeExtrasRequest(ExtrasRequest& request);

	// The border: artwork drawn around the screens, never over them, with a frame round each (a soft
	// shadow, a hairline, a ring for Shell). From the game's loop: the theme with its picture (RGBA,
	// top row first; empty for none), and where the screens are on a width x height picture.
	constexpr const char* kBorderNames[] = {"None", "Midnight", "Waves", "Aurora", "Shell", "PS5CEMU-HAR"};
	constexpr int kBorderCount = (int)std::size(kBorderNames);
	struct ScreenRect
	{
		float left, top, right, bottom;
	};
	void SetBorder(int theme, std::vector<uint8_t> rgba, int width, int height);
	// The theme's picture from the app's assets/borders (tools/render-borders.py), as SetBorder takes it
	void LoadBorder(int theme);
	void SetScreens(const std::vector<ScreenRect>& screens, float width, float height);

	// For Azahar's renderer, twice a frame, where it records its commands.
	void Record(const Target& target);

	// A game asking for text (Azahar's software keyboard): the port's keyboard, drawn over the game
	// like the menu, typed with the D-pad and Cross.
	struct KeyboardRequest
	{
		std::string hint;				  // what the game asks for; may be empty
		int maxLength = 0;				  // 0: no limit
		std::vector<std::string> buttons; // the game's buttons, left to right; the last one confirms
	};
	// From Azahar's emulation thread.
	void OpenKeyboard(const KeyboardRequest& request);
	bool KeyboardOpen();
	// For the game's loop: the text typed and the button chosen (an index into buttons), once.
	bool TakeKeyboardResult(std::string& text, int& button);
	// The game refused what was typed (too long, empty...): the keyboard opens again, saying why.
	void KeyboardError(const std::string& message);
}
