// SPDX-License-Identifier: GPL-3.0-or-later
// PS5Cemu: the output, shared by the launcher's and Cemu's Vulkan surfaces. RADV's VideoOut display
// (PS5_Mesa's wsi_common_videoout.c) has a 3840x2160 mode at 59.94 Hz, one at 119.88 Hz where the
// title declares high-frame-rate output and the display takes it, and the same at 2560x1440 and
// 1920x1080, which VideoOut scales to the screen. The emulators take the 3840x2160 one: Cemu scales
// the game's picture to it with its upscaling filter, and the launcher draws its 1920x1080 layout at
// twice the size.

#pragma once

#include "../app/lang.h"

#include <cstdint>
#include <string>

namespace ps5display
{
	constexpr uint32_t kWidth = 3840, kHeight = 2160;

	// The 119.88 Hz mode for the next surface, where the display has it.
	void SetHighFrameRate(bool highFrameRate);
	// VideoOut configured again for the next surface, once the surface before it is gone: the driver
	// opens VideoOut once a process and settles its mode then, so after the launcher's surface (the
	// new launcher draws through the driver) a game's renderer asks for its own rate here first
	// (PS5_Mesa patch 0006). Nothing before VideoOut is open, nor with a driver without it.
	void ConfigureOutput(bool highFrameRate);
	bool HighFrameRate();
	// The refresh rate of the surface made last, in millihertz (59940 or 119880); 0 before one.
	void SetOutputRefresh(uint32_t millihertz);
	uint32_t OutputRefresh();

	// Frame pacing: VideoOut shows each frame for at least refreshes refreshes (1 to 3), from now
	// on and for an output opened later. 2 holds a game to an even 60 fps at 119.88 Hz and to an even
	// 30 fps at 59.94 Hz; 3 to 40 and 20. False when the driver has no frame pacing or VideoOut
	// refuses it.
	bool SetFramePacing(int refreshes);
	int FramePacing(); // as last set, 1 before
	// What frame pacing holds a game to, for the menus: "Off", or "30 fps" and the like at 120 Hz
	// output (highFrameRate) or at 60 Hz.
	inline std::string FramePacingName(int refreshes, bool highFrameRate)
	{
		if (refreshes <= 1 || refreshes > 3)
			return ps5lang::Tr("Off");
		// tr: a frame rate: frame pacing holds the game to it
		return ps5lang::TrF("{0} fps", (highFrameRate ? 120 : 60) / refreshes);
	}
}
