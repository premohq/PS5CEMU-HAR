// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: box art for the launcher's game details, from GameTDB (art.gametdb.com), which has
// the Wii U's, the 3DS's and the DS's covers by the ID printed on each game's box (ALZE01 for a Wii U
// game, AREE for a 3DS one, AMCE for a DS one). They are fetched in the background over HTTP, as the
// Wii's homebrew loaders fetch theirs, decoded, scaled to the launcher's cover and kept as TGAs it
// shows: covers/boxart/<wiiu|3ds|ds>/<ID>.tga. An ID GameTDB has no cover for is remembered
// (<ID>.none) and not asked for again.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps5boxart
{
	enum class System
	{
		WiiU,
		N3ds,
		Nds, // a DS game, on the 3DS side
	};

	// The largest a cover is kept: the launcher's cover area at 4K
	constexpr int kMaxWidth = 576, kMaxHeight = 704;

	// The cover's TGA when it has been fetched, else empty.
	std::string Path(System system, const std::string& id);
	// Fetches the covers not fetched yet, one at a time, on a thread of its own.
	void Fetch(System system, const std::vector<std::string>& ids);
	// How many covers have arrived since the start: the launcher shows a new one when it changes.
	uint32_t Arrivals();
	// Whether GameTDB answered in this session: 1 it did, 0 it did not (no network), -1 not asked yet
	// (the Setup check's).
	int Answered();
	// The launcher's setting: when off, nothing more is fetched.
	void SetEnabled(bool enabled);
	// Before a game starts: nothing more is fetched in this session (the app starts over after the
	// game), and a cover being fetched is waited for, a few seconds at most.
	void Stop();

	// A TGA's size (its header), for laying it out.
	bool ImageSize(const std::string& path, int& width, int& height);
}
