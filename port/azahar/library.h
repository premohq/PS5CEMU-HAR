// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the 3DS games in Azahar's game files folder, for the launcher, and the DS games beside
// them (port/melonds/library.h), which the 3DS side lists too and melonDS plays.
//
// Each game's name, publisher, title ID and icon come from its SMDH, which is read where each kind
// of file keeps it: an NCCH's ExeFS ("icon"), as a .3ds/.cci (an NCSD's first partition), .cxi or
// .app holds it; a CIA's meta section; a .3dsx's extended header. Encrypted dumps cannot be read
// (Azahar needs decrypted ones too), and are listed by their file names. The icons (48x48 RGB565 in
// 8x8 tiles) are written once as TGAs the launcher shows (in /data/ps5cemu/covers/3ds).

#pragma once

#include "../app/emulator.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ps5azahar
{
	// Looks for games in a folder and the folders in it, on a thread of its own.
	void StartScan(const std::string& folder);
	bool Scanning();
	// What the last scan found, sorted by name. A game without a title ID (a .3dsx, an encrypted
	// dump) has one made from its path; a DS game one made from its header (ps5melonds::IsDsTitle).
	std::vector<ps5emu::Game> ListGames();
	// A game's icon as a TGA (a DS game's too), or empty when it has none.
	std::string CoverPath(uint64_t titleId);

	// What a file is, read from it: for the scan, and for anything else that needs to know.
	struct Title
	{
		bool readable = false;	 // a 3DS file this could read (decrypted, where that matters)
		bool encrypted = false;
		std::string name, publisher;
		uint64_t titleId = 0;
		uint16_t version = 0;
		std::string format;		 // 3DS, CIA, CXI, APP, 3DSX, ELF, Z3DS...
		std::string boxId;		 // the ID on its box (GameTDB's), from its product code: CTR-P-AREE is AREE
		std::vector<uint8_t> icon; // 48x48 BGRA, top-down, when it has one
	};
	Title Inspect(const std::string& path);
}
