// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: melonDS's core on the PS5, as Azahar's is (port/azahar/azahar.h): a DS game from the
// DS side, with the DS side's settings (nds: its screens, border, sound, controls and the ds... ones)
// and the game's own over them, on melonDS's software renderer, its two screens shown on VideoOut
// (screens.h), the DualSense as the DS, the sound through AudioOut, and the handhelds' in-game menu
// (ps5ingame3ds) over it. The launcher includes this header, so it names no melonDS type.
//
// Its data is under kRoot:
//   saves/<game file>.sav     the game's save, as melonDS keeps it (a .sav beside the game is taken
//                             the first time)
//   states/<game file>.ml1-5  save states, melonDS's own format
//   cheats/<game file>.mch    cheats, melonDS's own format (or the .mch beside the game)
//   bios/                     a DS's own bios7.bin, bios9.bin and firmware.bin, when there; without
//                             them melonDS's own replacements run the game (FreeBIOS)

#pragma once

#include "../app/emulator.h"
#include "../app/paths.h"
#include "../frontend/settings.h"

#include <string>

namespace ps5melonds
{
	constexpr const char* kRoot = PS5CEMU_DATA "/melonds";

	// Whether this build has melonDS's core.
	bool Available();

	// melonDS's core with the DS side's settings (the game's own over them), the screens on
	// VideoOut, then the game. False, with a reason, when it cannot start.
	bool LaunchGame(const ps5emu::Game& game, const ps5settings::N3ds& settings, std::string& error);
	// Runs while the game does. Returns when the player asks for the library.
	void RunGame();
	// Whether the last LaunchGame took VideoOut: after a failure from there on, only a fresh process
	// can show the launcher again.
	bool CoreTouched();
	// Whether a DS's own BIOS and firmware are in kRoot/bios (the Setup check's)
	bool OwnBiosFound();
}
