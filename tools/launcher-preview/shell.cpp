// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the new launcher on a PC (tools/preview-shell.sh), to see its screens without a console.
//
// The launcher's own code (port/frontend/shell, the UI kit in port/ui, the catalogue, the settings,
// GameTDB's facts and the compatibility list; port/azahar's library and controls) runs as it does on
// the console, and the kit draws with the PC's Vulkan (Mesa's lavapipe will do) into an image read
// back after each frame, which the script's shots save. In place of Cemu and the console it has
// samples (console.cpp): Wii U games, graphic packs, controllers, and the 3DS games the script
// makes (tools/launcher-preview/make-3ds-samples.py).
//
//   shell-preview OUTPUT_FOLDER SCRIPT GAMES_FOLDER 3DS_GAMES_FOLDER
//
// The script has one command a line ('#' starts a comment):
//   press BUTTON...  each button down for 3 frames, then up for 3, one after another
//                    (touchpad+options: the two together)
//   hold BUTTON N    down for N frames, then up for 3
//   wait N           N frames
//   shot NAME        the frame on screen as OUTPUT_FOLDER/NAME.png
//   record NAME      from here, every other frame (30 a second) as OUTPUT_FOLDER/NAME/00000.png on,
//                    for a video of the launcher (ffmpeg -framerate 30 -i NAME/%05d.png makes one)
//   stop             no more recording
// Buttons: up down left right cross circle square triangle l1 r1 l2 r2 l3 r3 options create
// touchpad, and the sticks: ls-up ls-down ls-left ls-right rs-up rs-down rs-left rs-right.
//
// The launcher starts as it would
// after earlier sessions, on the Wii U side's Library; with PREVIEW_FIRST set, as on a first start (the
// Setup check, then the first side with games); PREVIEW_ASK sets Start on: Ask each time; PREVIEW_UPDATE has
// a newer release found; PREVIEW_NO_DATA, /data out of reach; PREVIEW_LAUNCH_ERROR, the last game not
// started.

#include "console.h"

#include "app/compatibility.h"
#include "app/gameinfo.h"
#include "app/updates.h"
#include "azahar/library.h"
#include "melonds/library.h"
#include "frontend/shell.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name);

// in place of host_ps5.cpp's: the PC's Vulkan loader, an image read back into the shots' frame, the
// repository's font, and the preview's clock, which the script moves on 1/60 s a frame
ps5shell::Host ps5shell::DefaultHost()
{
	Host host;
	host.getInstanceProcAddr = vkGetInstanceProcAddr;
	host.target = [] {
		ui::Target target;
		target.width = preview::kWidth;
		target.height = preview::kHeight;
		target.readback = [](const uint8_t* bgra, uint32_t width, uint32_t height, size_t pitch) {
			const uint32_t w = std::min<uint32_t>(width, preview::kWidth), h = std::min<uint32_t>(height, preview::kHeight);
			for (uint32_t y = 0; y < h; y++)
				std::memcpy(&preview::frame[(size_t)y * preview::kWidth * 4], bgra + y * pitch, (size_t)w * 4);
		};
		return target;
	};
	host.assets = "port/ui"; // its fonts/lexend.sdf, as the app's assets/ui has it
	host.fontFolders = {"/usr/share/fonts"};
	host.clock = [] { return preview::timeUs / 1e6; };
	host.afterFrame = [] {
		preview::timeUs += 16667;
		preview::AdvanceScript();
	};
	return host;
}

int main(int argc, char* argv[])
{
	if (argc != 6)
	{
		std::fprintf(stderr, "shell-preview OUTPUT_FOLDER SCRIPT GAMES_FOLDER 3DS_GAMES_FOLDER DS_GAMES_FOLDER\n");
		return 2;
	}
	preview::output = argv[1];
	ps5gameinfo::SetFolder("port/app/gametdb"); // the repository's: the preview runs from its root
	ps5compat::SetPath("docs/COMPATIBILITY.md");
	if (std::getenv("PREVIEW_UPDATE"))
		ps5update::Start(); // as the app's check finding a newer release
	preview::LoadScript(argv[2]);

	ps5settings::Launcher settings;
	settings.gamesFolder = argv[3];
	settings.n3ds.gamesFolder = argv[4];
	settings.nds.gamesFolder = argv[5];
	if (!std::getenv("PREVIEW_FIRST"))
	{
		// earlier sessions: the side last used, the games last played, the counts their scans saved
		settings.ui.setupDone = true;
		settings.ui.lastSide = "wiiu";
		settings.lastGame = 0x00050000101C9400;
		settings.recent = {0x00050000101C9400, 0x0005000010143500, 0x000500001010EC00, 0x0005000010101D00};
		settings.n3ds.lastGame = 0x0004000000053F00;
		settings.n3ds.recent = {0x0004000000053F00, 0x0004000000030600, 0x000400000017C100};
		settings.gameCount = 11;
		settings.n3ds.gameCount = 6;
		settings.nds.gameCount = 4;
	}
	if (std::getenv("PREVIEW_ASK"))
		settings.ui.startOn = "ask";
	ps5launcher::Status status;
	status.diagnostics = {"PS5CEMU-HAR preview: Cemu at 32e6628, Azahar at 4aef900", "Jailbroken by the HEN: /data reachable, executable memory",
		"Boot log: /data/ps5cemu/logs/boot.log", "Cemu's log: /data/ps5cemu/log.txt"};
	if (std::getenv("PREVIEW_NO_DATA"))
		status.notice = status.notice3ds = status.noticeDs =
			"PS5CEMU-HAR cannot reach /data. Load a HEN with PPSA99360 in its app jailbreak list, or elfldr, then restart PS5CEMU-HAR.";
	else if (std::getenv("PREVIEW_LAUNCH_ERROR"))
		status.notice = "The game could not start: its disc key is missing from /data/ps5cemu/keys.txt.";

	// as main_ps5.cpp's: a side opens when the launcher opens it (the preview's Cemu starts at once)
	std::optional<ps5launcher::Choice> choice;
	const ps5shell::Outcome outcome = ps5shell::Run(settings, status, [&](ps5launcher::System system) {
		if (system == ps5launcher::System::N3ds)
			ps5azahar::StartScan(settings.n3ds.gamesFolder);
		else if (system == ps5launcher::System::Nds)
			ps5melonds::StartScan(settings.nds.gamesFolder);
		else
			status.coreReady = true;
	}, choice);
	std::fprintf(stderr, "[preview] the launcher ended: %s\n", choice ? choice->game.name.c_str() : "no game");
	return outcome == ps5shell::Outcome::Chosen ? 0 : 1;
}
