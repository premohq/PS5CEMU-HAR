// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: what the launcher (shell.h) and main_ps5.cpp share: the three sides, what the launcher
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
		Nds,  // melonDS
	};

	// The side's name in ps5cemu.json ("wiiu", "3ds", "ds") and the one players read
	inline const char* SideName(System side)
	{
		return side == System::N3ds ? "3ds" : side == System::Nds ? "ds" : "wiiu";
	}
	inline System SideNamed(const std::string& name)
	{
		return name == "3ds" ? System::N3ds : name == "ds" ? System::Nds : System::WiiU;
	}
	inline const char* SideTitle(System side)
	{
		return side == System::N3ds ? "3DS" : side == System::Nds ? "DS" : "Wii U";
	}

	struct Status
	{
		bool coreReady = false;	 // Cemu started: its library can open
		std::string notice;		 // a problem to show on the Wii U side's Library (empty: none)
		std::string notice3ds;	 // and on the 3DS side's
		std::string noticeDs;	 // and on the DS side's
		std::vector<std::string> diagnostics; // the lines Settings > Diagnostics shows
	};

	struct Choice
	{
		System system;
		ps5emu::Game game;
	};
}
