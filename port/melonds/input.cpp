// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the DualSense as the DS (frontend.h). The DS's buttons are the 3DS's of the same name,
// mapped as the DS side's controls say (port/azahar/controls.h's mapping, nds), and its D-pad takes the circle
// pad's inputs too: the left stick, past the deadzone. The touch screen is the touchpad, as on the 3DS
// side: a finger moves a cursor over the bottom screen, and clicking the touchpad touches it there. R3
// held blows into the microphone, unless it is mapped to a button.

#include "frontend.h"
#include "../azahar/controls.h"
#include "../ps5/pad.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>

namespace ps5melonds::input
{
	namespace
	{
		using ps5azahar::Button;
		using ps5emu::PadInput;

		// melonDS's keys, by their bit in its mask, and the 3DS buttons they are
		constexpr Button kKeys[12] = {Button::A, Button::B, Button::Select, Button::Start, Button::Right, Button::Left, Button::Up,
			Button::Down, Button::R, Button::L, Button::X, Button::Y};
		// the circle pad's directions, as the D-pad's (melonDS's bits 4 to 7: right, left, up, down)
		constexpr Button kCircle[4] = {Button::CircleRight, Button::CircleLeft, Button::CircleUp, Button::CircleDown};

		struct Sample
		{
			uint32_t buttons = 0;
			float leftX = 0, leftY = 0, rightX = 0, rightY = 0; // -1 to 1, up positive
		};

		std::mutex s_mutex;
		std::array<PadInput, 12> s_keys{};
		std::array<PadInput, 4> s_circle{};
		float s_threshold = 0.4f; // how far a stick goes before the D-pad does
		bool s_micFree = true;	  // R3 is no button's

		melonDS::u32 s_mask = 0xFFF; // under s_mutex, with the five below
		bool s_touching = false;
		float s_cursorX = 0.5f, s_cursorY = 0.5f; // 0 to 1 over the bottom screen
		bool s_cursorVisible = false;
		std::atomic<bool> s_blowing{false};
		// the game's loop's: what was held while the menu was open, kept from the game until let go
		uint32_t s_heldOver = 0;
		bool s_sticksHeldOver = false, s_touchHeldOver = false;

		constexpr float kStickRest = 0.25f;

		float Stick(uint8_t value)
		{
			return std::clamp((value - 128.0f) / 127.0f, -1.0f, 1.0f);
		}

		bool Has(const Sample& sample, uint32_t button)
		{
			return (sample.buttons & button) != 0;
		}

		// How far an input is pressed, 0 to 1 (as Azahar's input reads it: port/azahar/input.cpp)
		float Amount(const Sample& sample, PadInput input)
		{
			switch (input)
			{
			case PadInput::Cross: return Has(sample, ps5pad::kCross);
			case PadInput::Circle: return Has(sample, ps5pad::kCircle);
			case PadInput::Square: return Has(sample, ps5pad::kSquare);
			case PadInput::Triangle: return Has(sample, ps5pad::kTriangle);
			case PadInput::L1: return Has(sample, ps5pad::kL1);
			case PadInput::R1: return Has(sample, ps5pad::kR1);
			case PadInput::L2: return Has(sample, ps5pad::kL2);
			case PadInput::R2: return Has(sample, ps5pad::kR2);
			case PadInput::L3: return Has(sample, ps5pad::kL3);
			case PadInput::R3: return Has(sample, ps5pad::kR3);
			case PadInput::Create: return Has(sample, ps5pad::kCreate);
			case PadInput::Options: return Has(sample, ps5pad::kOptions);
			case PadInput::Up: return Has(sample, ps5pad::kUp);
			case PadInput::Down: return Has(sample, ps5pad::kDown);
			case PadInput::Left: return Has(sample, ps5pad::kLeft);
			case PadInput::Right: return Has(sample, ps5pad::kRight);
			case PadInput::LeftStickUp: return std::max(0.0f, sample.leftY);
			case PadInput::LeftStickDown: return std::max(0.0f, -sample.leftY);
			case PadInput::LeftStickLeft: return std::max(0.0f, -sample.leftX);
			case PadInput::LeftStickRight: return std::max(0.0f, sample.leftX);
			case PadInput::RightStickUp: return std::max(0.0f, sample.rightY);
			case PadInput::RightStickDown: return std::max(0.0f, -sample.rightY);
			case PadInput::RightStickLeft: return std::max(0.0f, -sample.rightX);
			case PadInput::RightStickRight: return std::max(0.0f, sample.rightX);
			case PadInput::None: break;
			}
			return 0.0f;
		}

		melonDS::u32 Mask(const Sample& sample)
		{
			melonDS::u32 mask = 0xFFF; // a bit set for each key let go
			for (int key = 0; key < 12; key++)
				if (Amount(sample, s_keys[key]) > 0.5f)
					mask &= ~(1u << key);
			for (int direction = 0; direction < 4; direction++)
				if (Amount(sample, s_circle[direction]) > s_threshold)
					mask &= ~(1u << (4 + direction));
			return mask;
		}
	}

	void Configure(const ps5settings::N3ds& settings)
	{
		std::lock_guard lock(s_mutex);
		s_micFree = true;
		for (int key = 0; key < 12; key++)
		{
			s_keys[key] = ps5azahar::MappedInput(settings, kKeys[key]);
			s_micFree = s_micFree && s_keys[key] != PadInput::R3;
		}
		for (int direction = 0; direction < 4; direction++)
		{
			s_circle[direction] = ps5azahar::MappedInput(settings, kCircle[direction]);
			s_micFree = s_micFree && s_circle[direction] != PadInput::R3;
		}
		// the deadzone (0 to 50%) as the stick's travel the D-pad waits for: 30% to 65%
		s_threshold = 0.3f + std::clamp(settings.deadzone, 0, 50) / 100.0f * 0.7f;
	}

	void Update(bool blocked, bool bottomShown)
	{
		ps5pad::Data data{};
		Sample sample;
		bool touch = false, fingerDown = false;
		float fingerX = 0, fingerY = 0;
		if (ps5pad::Read(0, data) && !(data.buttons & ps5pad::kIntercepted))
		{
			const ps5pad::Filtered filtered = ps5pad::FilterShortcuts(0, data.buttons);
			sample.buttons = filtered.buttons;
			touch = filtered.touch;
			sample.leftX = Stick(data.leftX);
			sample.leftY = -Stick(data.leftY);
			sample.rightX = Stick(data.rightX);
			sample.rightY = -Stick(data.rightY);
			const bool sticksMoved = std::abs(sample.leftX) > kStickRest || std::abs(sample.leftY) > kStickRest ||
				std::abs(sample.rightX) > kStickRest || std::abs(sample.rightY) > kStickRest;
			if (blocked)
			{
				// the shortcuts still seen (the menu's own closes it), nothing for the game; what is held
				// now stays the menu's until it is let go
				s_heldOver |= sample.buttons;
				s_sticksHeldOver |= sticksMoved;
				s_touchHeldOver |= touch;
				sample = Sample{};
				touch = false;
			}
			else
			{
				s_heldOver &= sample.buttons;
				sample.buttons &= ~s_heldOver;
				s_sticksHeldOver = s_sticksHeldOver && sticksMoved;
				if (s_sticksHeldOver)
					sample.leftX = sample.leftY = sample.rightX = sample.rightY = 0;
				s_touchHeldOver = s_touchHeldOver && touch;
				if (s_touchHeldOver)
					touch = false;
			}
			if (!blocked && data.touchCount > 0)
			{
				float width = 1, height = 1;
				ps5pad::TouchResolution(0, width, height);
				fingerDown = true;
				fingerX = std::clamp(data.touch[0].x / std::max(width, 1.0f), 0.0f, 1.0f);
				fingerY = std::clamp(data.touch[0].y / std::max(height, 1.0f), 0.0f, 1.0f);
			}
		}
		std::lock_guard lock(s_mutex);
		s_mask = Mask(sample);
		s_cursorVisible = fingerDown;
		if (fingerDown)
		{
			s_cursorX = fingerX;
			s_cursorY = fingerY;
		}
		// the click touches only a bottom screen that is shown
		s_touching = touch && bottomShown;
		s_blowing = s_micFree && Has(sample, ps5pad::kR3);
	}

	melonDS::u32 KeyMask()
	{
		std::lock_guard lock(s_mutex);
		return s_mask;
	}

	bool Touch(int& x, int& y)
	{
		std::lock_guard lock(s_mutex);
		x = std::clamp((int)(s_cursorX * 256.0f), 0, 255);
		y = std::clamp((int)(s_cursorY * 192.0f), 0, 191);
		return s_touching;
	}

	bool Blowing()
	{
		return s_blowing;
	}

	bool Cursor(float& x, float& y)
	{
		std::lock_guard lock(s_mutex);
		x = s_cursorX;
		y = s_cursorY;
		return s_cursorVisible;
	}
}
