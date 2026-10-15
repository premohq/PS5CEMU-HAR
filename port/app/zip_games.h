// SPDX-License-Identifier: GPL-3.0-or-later
// ZIP-backed game library and on-demand game extraction for all three PS5CEMU-HAR systems.
#pragma once

#include "emulator.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ps5zip
{
	enum class System { WiiU, N3ds, Nds };

	// Discover ZIPs in the configured games folder, up to the same four nested levels as
	// the handheld libraries. Only the central directory is read, not the ROM data.
	void AddArchives(const std::filesystem::path& folder, System system, std::vector<ps5emu::Game>& games);

	bool IsArchiveGame(const ps5emu::Game& game);

	// Unambiguous home-screen tile argument even for several ROMs in one ZIP.
	std::string TileKey(const ps5emu::Game& game);

	// Unpacks just the selected ROM, or the complete code/content/meta title directory.
	// A completed extraction is reused until the archive's size or modification time changes.
	// The returned Game preserves its library ID, but points to the regular file/folder the
	// existing emulator core already knows how to start. On failure the original is untouched.
	bool Prepare(const ps5emu::Game& game, ps5emu::Game& ready, std::string& error);
}
