// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the DS games in the 3DS side's game files folder, for the launcher: the 3DS library's
// scan (port/azahar/library.cpp) lists them with the 3DS's games, and melonDS starts them.
//
// A DS game's name and publisher are its banner's English title (two or three lines: the name, a
// subtitle, the publisher), its box ID the game code in its header (AMCE: GameTDB's, for its box art),
// and its icon the banner's 32x32 picture (16 colours in 8x8 tiles), written once as a TGA the
// launcher shows (in /data/ps5cemu/covers/ds). A DS game has no title ID like a 3DS game's, so it is
// given one from its header (kIdTag, its game code and its header's checksum), which stays the same
// wherever the file is kept.

#pragma once

#include "../app/emulator.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ps5melonds
{
	// The files a DS game can be: .nds (and .srl, a developer's or DSi's), .dsi
	bool IsDsFile(const std::string& path);

	// The title IDs given to DS games: their top byte, which no 3DS title ID has
	constexpr uint64_t kIdTag = 0xD5ull << 56;
	inline bool IsDsTitle(uint64_t titleId)
	{
		return (titleId >> 56) == (kIdTag >> 56);
	}

	// What a DS file is, read from its header and banner
	struct Title
	{
		bool readable = false; // a DS ROM this could read
		std::string name, publisher;
		std::string gameCode;  // AMCE; empty for homebrew without one
		uint64_t titleId = 0;  // kIdTag, the header's checksum, the game code
		uint8_t version = 0;   // the ROM's revision
		bool dsiOnly = false;  // a DSi game (melonDS's DS mode cannot start it)
		bool dsiEnhanced = false;
		std::vector<uint8_t> icon; // 32x32 BGRA, top-down; transparent where the DS's is
	};
	Title Inspect(const std::string& path);

	// The game for the 3DS side's library, its icon written for the launcher. False when the file is
	// not a DS game.
	bool ReadGame(const std::string& path, ps5emu::Game& game);
	// A DS game's icon as a TGA, or empty when it has none.
	std::string CoverPath(uint64_t titleId);
}
