// SPDX-License-Identifier: GPL-3.0-or-later
#include "controls.h"

#include <array>

namespace ps5azahar
{
	namespace
	{
		using ps5emu::PadInput;

		struct Entry
		{
			const char* key;   // in the settings
			const char* label; // in the launcher
			PadInput defaultInput;
		};

		constexpr std::array<Entry, (size_t)Button::Count> kButtons = {{
			{"a", "A", PadInput::Circle},
			{"b", "B", PadInput::Cross},
			{"x", "X", PadInput::Triangle},
			{"y", "Y", PadInput::Square},
			{"l", "L", PadInput::L1},
			{"r", "R", PadInput::R1},
			{"zl", "ZL", PadInput::L2},
			{"zr", "ZR", PadInput::R2},
			{"start", "Start", PadInput::Options},
			{"select", "Select", PadInput::Create},
			{"home", "Home", PadInput::None},
			{"up", "D-pad up", PadInput::Up},
			{"down", "D-pad down", PadInput::Down},
			{"left", "D-pad left", PadInput::Left},
			{"right", "D-pad right", PadInput::Right},
			{"circle_up", "Circle pad up", PadInput::LeftStickUp},
			{"circle_down", "Circle pad down", PadInput::LeftStickDown},
			{"circle_left", "Circle pad left", PadInput::LeftStickLeft},
			{"circle_right", "Circle pad right", PadInput::LeftStickRight},
			{"cstick_up", "C-stick up", PadInput::RightStickUp},
			{"cstick_down", "C-stick down", PadInput::RightStickDown},
			{"cstick_left", "C-stick left", PadInput::RightStickLeft},
			{"cstick_right", "C-stick right", PadInput::RightStickRight},
		}};

		constexpr const char* kInputNames[] = {"", "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3",
			"Create", "Options", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Left stick up", "Left stick down",
			"Left stick left", "Left stick right", "Right stick up", "Right stick down", "Right stick left", "Right stick right"};
		static_assert(std::size(kInputNames) == (size_t)PadInput::RightStickRight + 1);

		PadInput InputByName(const std::string& name)
		{
			for (size_t i = 1; i < std::size(kInputNames); i++)
				if (name == kInputNames[i])
					return (PadInput)i;
			return PadInput::None;
		}
	}

	const char* InputName(PadInput input)
	{
		return kInputNames[(size_t)input];
	}

	PadInput MappedInput(const ps5settings::N3ds& settings, Button button)
	{
		const Entry& entry = kButtons[(size_t)button];
		const auto it = settings.buttons.find(entry.key);
		return it == settings.buttons.end() ? entry.defaultInput : InputByName(it->second);
	}

	std::vector<ps5emu::ButtonMapping> ListMappings(const ps5settings::N3ds& settings)
	{
		std::vector<ps5emu::ButtonMapping> mappings;
		for (size_t i = 0; i < kButtons.size(); i++)
			mappings.push_back({kButtons[i].label, InputName(MappedInput(settings, (Button)i))});
		return mappings;
	}

	const std::vector<Button>& DsButtons()
	{
		static const std::vector<Button> buttons = {Button::A, Button::B, Button::X, Button::Y, Button::L, Button::R, Button::Start, Button::Select,
			Button::Up, Button::Down, Button::Left, Button::Right, Button::CircleUp, Button::CircleDown, Button::CircleLeft, Button::CircleRight};
		return buttons;
	}

	ps5emu::ButtonMapping Mapping(const ps5settings::N3ds& settings, Button button, bool ds)
	{
		static const char* kDsCircle[] = {"D-pad up, by stick", "D-pad down, by stick", "D-pad left, by stick", "D-pad right, by stick"};
		const bool circle = button >= Button::CircleUp && button <= Button::CircleRight;
		const char* label = ds && circle ? kDsCircle[(int)button - (int)Button::CircleUp] : kButtons[(size_t)button].label;
		return {label, InputName(MappedInput(settings, button))};
	}

	void SetMapping(ps5settings::N3ds& settings, size_t index, PadInput input)
	{
		if (index >= kButtons.size())
			return;
		if (input == kButtons[index].defaultInput)
			settings.buttons.erase(kButtons[index].key);
		else
			settings.buttons[kButtons[index].key] = input == PadInput::None ? "None" : InputName(input);
	}

	void ClearMapping(ps5settings::N3ds& settings, size_t index)
	{
		SetMapping(settings, index, PadInput::None);
	}

	void ResetControls(ps5settings::N3ds& settings)
	{
		settings.buttons.clear();
		settings.motion = true;
		settings.deadzone = 15;
	}
}
