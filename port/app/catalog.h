// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: each side's game list as last seen, /data/ps5cemu/library.json (docs/UI-REDESIGN.md,
// 5.5). The launcher draws a side from it at once while that side's scan runs, and the scan's list
// replaces it when it finishes, keeping what the catalogue knows of each game by title ID: when a scan
// first found it (Recently added), whether it is a favourite, when it was last played and for how
// long, and its two ambient colours. Deleting the file only costs one full scan of each side.

#pragma once

#include "emulator.h"

#include <cstdint>
#include <vector>

namespace ps5catalog
{
	enum class System
	{
		WiiU,
		N3ds,
		Nds,
	};

	struct Entry
	{
		ps5emu::Game game;
		uint32_t ambient[2] = {}; // RGBA, from the cover (0: not known yet)
		int64_t added = 0;		  // seconds since 1970 a scan first found it; 0: in the first scan
		bool favourite = false;
		int64_t lastPlayed = 0;	  // seconds since 1970; 0: never
		uint32_t minutesPlayed = 0;
	};

	// /data/ps5cemu/library.json, read once (nothing when it is missing)
	void Load();
	bool Save();
	// A side's games, sorted by name
	const std::vector<Entry>& Games(System side);
	Entry* Find(System side, uint64_t titleId);
	// After a side's scan: its list replaced, what is known of each game kept by title ID
	void Replace(System side, const std::vector<ps5emu::Game>& scanned);
	void SetFavourite(System side, uint64_t titleId, bool favourite);
	void SetAmbient(System side, uint64_t titleId, const uint32_t ambient[2]);
	// A game started (now), and ended after so many minutes
	void Played(System side, uint64_t titleId, int64_t when);
	void AddMinutes(System side, uint64_t titleId, uint32_t minutes);
	// The launcher's old recent lists, which have no times: their order kept as last-played times,
	// for the games that have none yet
	void Migrate(System side, const std::vector<uint64_t>& recent);
}
