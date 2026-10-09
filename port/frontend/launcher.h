// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: what the launcher (shell.h) and main_ps5.cpp share: the two sides, what the launcher
// shows of their state, and the game it returns.

#pragma once

#include "../app/emulator.h"
#include "settings.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ps5launcher
{
	enum class System
	{
		WiiU, // Cemu
		N3ds, // Azahar
	};

	struct Status
	{
		bool coreReady = false;	 // Cemu started: its library can open
		std::string notice;		 // a problem to show on Cemu's home screen (empty: none)
		std::string notice3ds;	 // and on Azahar's
		std::string launchError; // why the last Wii U game did not start, said once (empty: it did)
		std::string launchError3ds; // and the last 3DS game
		std::vector<std::string> diagnostics; // the lines Settings > Diagnostics shows
	};

	struct Choice
	{
		System system;
		ps5emu::Game game;
	};
}
