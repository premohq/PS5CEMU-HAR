// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the new launcher (docs/UI-REDESIGN.md), drawn on the GPU with the UI kit (port/ui):
// the Wii U, 3DS and DS sides of one shell, switched in one press, each with its Library and its own
// Settings, a game's hub, Game menu and game settings, and the pages for graphic packs, a player's
// controls and buttons, the folder browser and installs, Artic Base and Diagnostics, the Setup check,
// the update sheet and the launch.
//
// It returns the game chosen, with everything of its own gone first (its device, its threads,
// VideoOut handed back to the driver for the emulator's renderer: 4.2, rule 1).

#pragma once

#include "launcher.h"
#include "settings.h"
#include "../ui/gfx.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ps5shell
{
	// Where the launcher draws and finds its files: on the console VideoOut through the driver the app
	// links (host_ps5.cpp); the preview on a PC gives its own (tools/launcher-preview)
	struct Host
	{
		PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
		std::function<ui::Target()> target;
		std::string assets;						  // the app's assets/ui
		std::vector<std::string> fontFolders;	  // fonts for what Lexend lacks
		std::function<double()> clock;			  // seconds
		std::function<void()> afterFrame;		  // the preview's script
	};
	Host DefaultHost();
	void SetHost(Host host);

	enum class Outcome
	{
		Chosen,	   // a game: the choice
		Classic,   // it could not start before VideoOut was taken
		Restart,   // it could not go on after VideoOut was taken
		Failed,	   // nothing can show
	};

	// The launcher until a game is chosen, prepare called each time a side opens: it loads that side's
	// game list and settings. Before a game is returned, the launcher's background work has stopped.
	Outcome Run(ps5settings::Launcher& settings, ps5launcher::Status& status, const std::function<void(ps5launcher::System)>& prepare,
		std::optional<ps5launcher::Choice>& choice);
	// Why the last Run did not choose a game, as the boot log has it
	const std::string& LastFailure();
}
