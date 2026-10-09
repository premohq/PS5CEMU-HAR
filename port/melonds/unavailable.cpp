// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: melonds.h for a build without melonDS's core. The DS side still lists DS games, and
// one says why it cannot start.

#include "melonds.h"

namespace ps5melonds
{
	bool Available()
	{
#ifdef PS5CEMU_LAUNCHER_PREVIEW
		return true; // the launcher's preview lists the sample games as they are
#else
		return false;
#endif
	}

	bool LaunchGame(const ps5emu::Game&, const ps5settings::N3ds&, std::string& error)
	{
		error = "melonDS's core, which plays DS games, is not part of this build yet.";
		return false;
	}

	void RunGame()
	{
	}

	bool CoreTouched()
	{
		return false;
	}
}
