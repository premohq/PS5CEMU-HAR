// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the 3DS's controls on the DualSense, for the launcher and for Azahar's input. Each
// 3DS button (its circle pad and C-stick in four directions each) has a DualSense input, stored by
// name in the launcher's settings (ps5settings::N3ds::buttons) where it is not the default: A on
// Circle and B on Cross, where the 3DS has them, the circle pad on the left stick, the C-stick on
// the right one. The touchpad is the touch screen.

#pragma once

#include "../app/emulator.h"
#include "../frontend/settings.h"

#include <string>
#include <vector>

namespace ps5azahar
{
	enum class Button
	{
		A, B, X, Y,
		L, R, ZL, ZR,
		Start, Select, Home,
		Up, Down, Left, Right,
		CircleUp, CircleDown, CircleLeft, CircleRight,
		CStickUp, CStickDown, CStickLeft, CStickRight,
		Count,
	};

	std::vector<ps5emu::ButtonMapping> ListMappings(const ps5settings::N3ds& settings);
	// The buttons a DS has, as the DS side maps them (port/melonds/input.cpp): the 3DS's of the same
	// name, the circle pad's directions as a second D-pad
	const std::vector<Button>& DsButtons();
	// One button's mapping, as ListMappings has it (a DS's circle pad named as the D-pad it is)
	ps5emu::ButtonMapping Mapping(const ps5settings::N3ds& settings, Button button, bool ds = false);
	ps5emu::PadInput MappedInput(const ps5settings::N3ds& settings, Button button);
	void SetMapping(ps5settings::N3ds& settings, size_t index, ps5emu::PadInput input);
	void ClearMapping(ps5settings::N3ds& settings, size_t index);
	void ResetControls(ps5settings::N3ds& settings);

	// A DualSense input's name, as the launcher shows it and the settings keep it.
	const char* InputName(ps5emu::PadInput input);
}
