// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the in-game menus' look, the Wii U's (ingame.cpp) and the 3DS's (ingame3ds.cpp): the
// launcher's (docs/UI-REDESIGN.md, 6.9 and 7), drawn with ImGui on the launcher's 1920x1080 layout
// scaled to the screen. Its colours are the launcher's tokens (port/ui/tokens.json), its type Lexend
// (tools/render-menu-fonts.py's three weights), its focus the launcher's double ring. In a game there
// is no glass: panels are ink at 92 %, with no blur (4.2, rule 11).

#pragma once

#include "lang.h"
#include "paths.h"
#include "../ui/text.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace ps5menu
{
	constexpr ImU32 Colour(uint32_t rgb, uint8_t alpha = 255)
	{
		return IM_COL32(rgb >> 16, (rgb >> 8) & 255, rgb & 255, alpha);
	}

	inline ImU32 Fade(ImU32 colour, float alpha)
	{
		const float a = ((colour >> IM_COL32_A_SHIFT) & 255) * std::clamp(alpha, 0.0f, 1.0f);
		return (colour & ~IM_COL32_A_MASK) | ((ImU32)(a + 0.5f) << IM_COL32_A_SHIFT);
	}

	// The characters the menus' fonts are made with (docs/UI-REDESIGN.md, 7.7): Latin-1 and the dashes,
	// quotes, bullet, ellipsis and angle quotes, then only those the language's words and the game's
	// name have besides, so a menu in English costs what it always did. Lexend has the Latin ones
	// (tools/render-menu-fonts.py), the console's font the rest (MenuFont). Kept: ImGui's atlases read
	// them as they are built.
	inline const ImWchar* GlyphRanges(const std::string& title)
	{
		static std::vector<ImWchar> ranges;
		ranges = {0x0020, 0x00ff, 0x2010, 0x2027, 0x2039, 0x203a};
		std::u32string extra = ps5lang::MenuCharacters();
		extra += ui::Decode(title);
		extra += ui::Upper(extra, ps5lang::Current() == "tr"); // the badges' capitals
		std::sort(extra.begin(), extra.end());
		extra.erase(std::unique(extra.begin(), extra.end()), extra.end());
		for (char32_t c : extra)
			if (c > 0xff && c <= 0xffff && !(c >= 0x2010 && c <= 0x2027) && c != 0x2039 && c != 0x203a)
				ranges.insert(ranges.end(), {(ImWchar)c, (ImWchar)c});
		ranges.push_back(0);
		return ranges.data();
	}

	// The console's font for what Lexend lacks (a Russian menu's Cyrillic, a Japanese title's kanji),
	// as the launcher chose it (ps5lang::GetMenuFont), read once and kept: ImGui's atlases read it as
	// they are built. Empty when the menus need none.
	inline const std::vector<unsigned char>& MergeFont(int& face)
	{
		static std::vector<unsigned char> data;
		static bool read = false;
		const ps5lang::MenuFont& font = ps5lang::GetMenuFont();
		face = font.face;
		if (!read && !font.path.empty())
		{
			read = true;
			std::ifstream in(font.path, std::ios::binary);
			data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		return data;
	}

	// The three weights, as the menus' files name them (assets/ui/fonts)
	enum class Weight
	{
		Regular = 1,
		Medium = 2,
		SemiBold = 3
	};
	inline const char* WeightFile(Weight weight)
	{
		switch (weight)
		{
		case Weight::Medium: return "Lexend-Medium.ttf";
		case Weight::SemiBold: return "Lexend-SemiBold.ttf";
		default: return "Lexend-Regular.ttf";
		}
	}

	// A weight's file, read once and kept: ImGui's atlases read it as they are built. Empty when it
	// is missing, and the menus fall back on the console's own font.
	inline const std::vector<unsigned char>& FontFile(Weight weight)
	{
		static std::vector<unsigned char> files[4];
		static bool read[4] = {};
		const int i = std::clamp((int)weight, 1, 3);
		if (!read[i])
		{
			read[i] = true;
			std::ifstream in(ps5paths::Assets() + "/ui/fonts/" + WeightFile(weight), std::ios::binary);
			files[i].assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		return files[i];
	}

	// A side's colours: the launcher's tokens, with the Wii U's sky or the 3DS's sand as the accent.
	// The names before ink are those the menus have always used.
	struct Palette
	{
		ImU32 title, text, copy, accent, kicker, line;
		ImU32 panel, panelEdge, row, rowEdge, focus, focusEdge, dim;
		ImU32 ink, faint, accentInk, bad, good;
	};
	constexpr Palette Side(uint32_t accent, uint32_t accentInk)
	{
		return {Colour(0xf5f7fb), Colour(0xf5f7fb), Colour(0xf5f7fb, 0xb3), Colour(accent), Colour(accent), Colour(0xffffff, 0x17),
			Colour(0x0a0f1b, 0xeb), Colour(0xffffff, 0x1a), Colour(0xffffff, 0x0e), Colour(0xffffff, 0x1a), Colour(0xffffff, 0x16),
			Colour(0xffffff), Colour(0x03050a, 0x9e), Colour(0x05070d), Colour(0xf5f7fb, 0x75), Colour(accentInk), Colour(0xff7272),
			Colour(0x3dd6a3)};
	}
	constexpr Palette kBlue = Side(0x5aa9ff, 0x071a33);
	constexpr Palette kGold = Side(0xf4b63f, 0x2a1a02);

	// The type styles the menus use (docs/UI-REDESIGN.md, 7.2), each a font made at its size
	struct Fonts
	{
		ImFont* heading; // 32 SemiBold: the game's name, a sheet's title
		ImFont* body;	 // 26 Medium: a row's label
		ImFont* label;	 // 24 Regular: values, tiles, hints
		ImFont* caption; // 20 Regular: help and facts
		ImFont* chip;	 // 18 SemiBold: badges, in capitals
	};
	constexpr float kHeading = 32, kBody = 26, kLabel = 24, kCaption = 20, kChip = 18;

	struct Canvas
	{
		ImDrawList* draw;
		float scale;
		ImVec2 origin;
		const Palette& colours = kBlue;

		ImVec2 At(float x, float y) const { return {origin.x + x * scale, origin.y + y * scale}; }

		float Width(ImFont* font, float size, const std::string& text) const
		{
			return font->CalcTextSizeA(size * scale, FLT_MAX, 0.0f, text.c_str()).x / scale;
		}

		// A sheet: ink at 92 % with a one-pixel light edge, the launcher's sheet radius
		void Panel(float x, float y, float width, float height) const
		{
			draw->AddRectFilled(At(x, y), At(x + width, y + height), colours.panel, 28 * scale);
			draw->AddRect(At(x, y), At(x + width, y + height), colours.panelEdge, 28 * scale, 0, std::max(1.0f, scale));
		}

		// The focused element's double ring: ink inside, white outside, so it reads on any picture,
		// breathing a little (±6 % over 2.4 s) and glowing in the side's colour
		void Ring(float x, float y, float width, float height, float radius, float time) const
		{
			const float breath = 0.94f + 0.06f * std::sin(time * 6.2831853f / 2.4f);
			for (int glow = 3; glow >= 1; glow--)
			{
				const float out = 4.0f + 5.0f * glow;
				draw->AddRect(At(x - out, y - out), At(x + width + out, y + height + out), Fade(colours.accent, 0.06f * breath),
					(radius + out) * scale, 0, 5 * scale);
			}
			draw->AddRect(At(x - 2, y - 2), At(x + width + 2, y + height + 2), Fade(colours.ink, 0.9f), (radius + 2) * scale, 0, 4 * scale);
			draw->AddRect(At(x - 5.5f, y - 5.5f), At(x + width + 5.5f, y + height + 5.5f), Fade(colours.focusEdge, breath),
				(radius + 5.5f) * scale, 0, 3.5f * scale);
		}

		// A card: glass at rest; brighter glass in the ring when focused
		void Row(float x, float y, float width, float height, bool focused, float time = 0.0f) const
		{
			draw->AddRectFilled(At(x, y), At(x + width, y + height), focused ? colours.focus : colours.row, 20 * scale);
			if (focused)
				Ring(x, y, width, height, 20, time);
			else
				draw->AddRect(At(x, y), At(x + width, y + height), colours.rowEdge, 20 * scale, 0, std::max(1.0f, scale));
		}

		void Text(ImFont* font, float size, float x, float y, ImU32 colour, const std::string& text, float wrap = 0.0f) const
		{
			draw->AddText(font, size * scale, At(x, y), colour, text.c_str(), nullptr, wrap * scale);
		}

		void TextRight(ImFont* font, float size, float right, float y, ImU32 colour, const std::string& text) const
		{
			Text(font, size, right - Width(font, size, text), y, colour, text);
		}

		void TextCentred(ImFont* font, float size, float centre, float y, ImU32 colour, const std::string& text) const
		{
			Text(font, size, centre - Width(font, size, text) / 2, y, colour, text);
		}

		// The size text fits a width at: its own, or down to a fifth smaller (a longer language's words)
		float FitSize(ImFont* font, float size, const std::string& text, float width) const
		{
			const float natural = Width(font, size, text);
			return natural <= width || natural <= 0 ? size : std::max(size * 0.8f, size * width / natural);
		}

		// Text in a width, as the launcher's: a little smaller when it needs to be, then cut with an
		// ellipsis, its middle where it would have been. Returns its width.
		enum class Align
		{
			Left,
			Centre,
			Right
		};
		float TextFit(ImFont* font, float size, float x, float y, ImU32 colour, const std::string& text, float width, Align align = Align::Left) const
		{
			const float fitted = FitSize(font, size, text, width);
			const std::string shown = Fit(font, fitted, text, width);
			const float w = Width(font, fitted, shown);
			const float at = align == Align::Centre ? x - w / 2 : align == Align::Right ? x - w : x;
			Text(font, fitted, at, y + (size - fitted) * 0.5f, colour, shown);
			return w;
		}

		// Text wrapped to a width on at most lines lines: a little smaller first, then cut
		void TextLines(ImFont* font, float size, float x, float y, ImU32 colour, std::string text, float width, int lines) const
		{
			auto height = [&](float s, const std::string& t) { return font->CalcTextSizeA(s * scale, FLT_MAX, width * scale, t.c_str()).y / scale; };
			float s = size;
			while (s > size * 0.8f && height(s, text) > s * lines + 0.5f)
				s -= 1;
			if (height(s, text) > s * lines + 0.5f)
			{
				// still too long: words go from its end (characters, where it has no spaces) until it
				// and an ellipsis fit
				while (!text.empty() && height(s, text + "…") > s * lines + 0.5f)
				{
					const size_t space = text.find_last_of(' ');
					if (space != std::string::npos && space > text.size() / 2)
						text.resize(space);
					else
					{
						text.pop_back();
						while (!text.empty() && ((unsigned char)text.back() & 0xc0) == 0x80)
							text.pop_back();
					}
				}
				while (!text.empty() && (text.back() == ' ' || text.back() == ',' || text.back() == '.' || text.back() == ':'))
					text.pop_back();
				text += "…";
			}
			Text(font, s, x, y, colour, text, width);
		}

		// Text that never overflows: cut at a word where it can be, ending in an ellipsis
		std::string Fit(ImFont* font, float size, const std::string& text, float width) const
		{
			if (Width(font, size, text) <= width)
				return text;
			std::string cut = text;
			while (!cut.empty() && Width(font, size, cut + "…") > width)
			{
				const size_t space = cut.find_last_of(' ');
				if (space != std::string::npos && space > cut.size() / 2)
					cut.resize(space);
				else
				{
					cut.pop_back();
					while (!cut.empty() && ((unsigned char)cut.back() & 0xc0) == 0x80)
						cut.pop_back(); // never half a character
				}
			}
			while (!cut.empty() && (cut.back() == ' ' || cut.back() == ',' || cut.back() == '.' || cut.back() == ':'))
				cut.pop_back();
			return cut + "…";
		}

		// Each character of UTF-8 text, as its own string (a letter spaced from the next)
		template<typename Each>
		static void Letters(const std::string& text, Each each)
		{
			for (size_t i = 0; i < text.size();)
			{
				const unsigned char lead = (unsigned char)text[i];
				size_t length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 1;
				length = std::min(length, text.size() - i);
				each(text.substr(i, length));
				i += length;
			}
		}

		// Capitals spaced out, as the launcher's overlines and badges have them. Returns the end.
		float Tracked(ImFont* font, float size, float x, float y, ImU32 colour, const std::string& text, float tracking = 2.0f) const
		{
			Letters(text, [&](const std::string& letter) {
				draw->AddText(font, size * scale, At(x, y), colour, letter.c_str());
				x += font->CalcTextSizeA(size * scale, FLT_MAX, 0.0f, letter.c_str()).x / scale + tracking;
			});
			return x - tracking;
		}

		// A badge: a small rounded chip, its words in capitals (the language's: Cyrillic, Greek too); a
		// square of the colour first when dot. Returns where the next one goes.
		float Chip(ImFont* font, float x, float y, const std::string& words, ImU32 colour, ImU32 fill, bool dot = false) const
		{
			const std::string text = ui::Upper(words, ps5lang::Current() == "tr");
			float width = 24 + (dot ? 22 : 0);
			Letters(text, [&](const std::string& letter) { width += font->CalcTextSizeA(kChip * scale, FLT_MAX, 0.0f, letter.c_str()).x / scale + 1.5f; });
			draw->AddRectFilled(At(x, y), At(x + width, y + 34), fill, 10 * scale);
			float textX = x + 12;
			if (dot)
			{
				draw->AddRectFilled(At(x + 12, y + 10), At(x + 26, y + 24), colour, 4 * scale);
				textX += 22;
			}
			Tracked(font, kChip, textX, y + 7, colour, text, 1.5f);
			return x + width + 10;
		}

		// A chevron centred at x, y: right (a category, or a setting's next value), down (an open
		// category) or left (a setting's previous value)
		enum class Point
		{
			Right,
			Down,
			Left
		};
		void Chevron(float x, float y, ImU32 colour, Point point = Point::Right) const
		{
			const float r = 6.0f;
			const float thick = 2.2f * scale;
			if (point == Point::Down)
			{
				draw->AddLine(At(x - r, y - r / 2), At(x, y + r / 2), colour, thick);
				draw->AddLine(At(x, y + r / 2), At(x + r, y - r / 2), colour, thick);
				return;
			}
			const float side = point == Point::Left ? -1.0f : 1.0f;
			draw->AddLine(At(x - side * r / 2, y - r), At(x + side * r / 2, y), colour, thick);
			draw->AddLine(At(x + side * r / 2, y), At(x - side * r / 2, y + r), colour, thick);
		}

		// A DualSense button's mark, in the launcher's line style, centred at x, y
		void Button(float x, float y, const char* button, ImU32 colour) const
		{
			const float thick = 2.2f * scale;
			const ImVec2 centre = At(x, y);
			const float r = 9 * scale;
			if (std::strcmp(button, "cross") == 0)
			{
				draw->AddLine({centre.x - r * 0.8f, centre.y - r * 0.8f}, {centre.x + r * 0.8f, centre.y + r * 0.8f}, colour, thick);
				draw->AddLine({centre.x - r * 0.8f, centre.y + r * 0.8f}, {centre.x + r * 0.8f, centre.y - r * 0.8f}, colour, thick);
			}
			else if (std::strcmp(button, "circle") == 0)
				draw->AddCircle(centre, r, colour, 0, thick);
			else if (std::strcmp(button, "leftright") == 0)
			{
				for (const float side : {-1.0f, 1.0f})
				{
					const ImVec2 tip{centre.x + side * (r + 3 * scale), centre.y};
					draw->AddLine(tip, {tip.x - side * 6 * scale, centre.y - 6 * scale}, colour, thick);
					draw->AddLine(tip, {tip.x - side * 6 * scale, centre.y + 6 * scale}, colour, thick);
				}
			}
			else if (std::strcmp(button, "touchpad") == 0)
				draw->AddRect({centre.x - r - 3 * scale, centre.y - r + 3 * scale}, {centre.x + r + 3 * scale, centre.y + r - 3 * scale}, colour,
					3 * scale, 0, thick);
			else if (std::strcmp(button, "triangle") == 0)
				draw->AddTriangle({centre.x, centre.y - r}, {centre.x + r, centre.y + r * 0.75f}, {centre.x - r, centre.y + r * 0.75f}, colour, thick);
			else if (std::strcmp(button, "square") == 0)
				draw->AddRect({centre.x - r * 0.8f, centre.y - r * 0.8f}, {centre.x + r * 0.8f, centre.y + r * 0.8f}, colour, 2 * scale, 0, thick);
			else if (std::strcmp(button, "options") == 0)
				for (const float line : {-1.0f, 0.0f, 1.0f}) // the Options button's three lines
					draw->AddLine({centre.x - r * 0.7f, centre.y + line * 5 * scale}, {centre.x + r * 0.7f, centre.y + line * 5 * scale}, colour, thick);
		}

		// A controller hint, as the launcher's: the button's mark and what it does. Returns where the
		// next one goes.
		float Hint(ImFont* font, float x, float y, const char* button, const std::string& label, float size = kLabel - 2) const
		{
			Button(x + 11, y + 14, button, colours.copy);
			Text(font, size, x + 30, y + 1 + (kLabel - 2 - size) * 0.5f, colours.copy, label);
			return x + 30 + Width(font, size, label) + 32 * size / (kLabel - 2);
		}

		// The size hints take to fit in a width together (a longer language's words), a quarter
		// smaller at the most
		float HintsSize(ImFont* font, const std::vector<std::pair<const char*, std::string>>& hints, float width) const
		{
			float total = 0;
			for (const auto& hint : hints)
				total += 30 + Width(font, kLabel - 2, hint.second) + 32;
			return total <= width ? kLabel - 2 : std::max((kLabel - 2) * 0.75f, (kLabel - 2) * width / total);
		}

		// A quick action's picture, drawn in lines around x, y: resume, save, load, screens, swap,
		// packs, amiibo
		void Icon(const std::string& name, float x, float y, ImU32 colour) const
		{
			const float s = scale, thick = 2.4f * s;
			const ImVec2 c = At(x, y);
			if (name == "resume")
				draw->AddTriangleFilled({c.x - 7 * s, c.y - 10 * s}, {c.x - 7 * s, c.y + 10 * s}, {c.x + 10 * s, c.y}, colour);
			else if (name == "save")
			{
				// a disk: its body with a cut corner, the label and the shutter
				const ImVec2 body[] = {{c.x - 12 * s, c.y - 12 * s}, {c.x + 7 * s, c.y - 12 * s}, {c.x + 12 * s, c.y - 7 * s},
					{c.x + 12 * s, c.y + 12 * s}, {c.x - 12 * s, c.y + 12 * s}};
				draw->AddPolyline(body, 5, colour, ImDrawFlags_Closed, thick);
				draw->AddRect({c.x - 6 * s, c.y - 12 * s}, {c.x + 5 * s, c.y - 5 * s}, colour, 0, 0, thick);
				draw->AddRect({c.x - 7 * s, c.y + 2 * s}, {c.x + 7 * s, c.y + 12 * s}, colour, 0, 0, thick);
			}
			else if (name == "load")
			{
				// an arrow down into a tray
				draw->AddLine({c.x, c.y - 13 * s}, {c.x, c.y + 4 * s}, colour, thick);
				draw->AddLine({c.x - 7 * s, c.y - 3 * s}, {c.x, c.y + 4 * s}, colour, thick);
				draw->AddLine({c.x + 7 * s, c.y - 3 * s}, {c.x, c.y + 4 * s}, colour, thick);
				const ImVec2 tray[] = {{c.x - 12 * s, c.y + 3 * s}, {c.x - 12 * s, c.y + 12 * s}, {c.x + 12 * s, c.y + 12 * s}, {c.x + 12 * s, c.y + 3 * s}};
				draw->AddPolyline(tray, 4, colour, 0, thick);
			}
			else if (name == "screens")
			{
				// the 3DS's two screens, the top one wider
				draw->AddRect({c.x - 12 * s, c.y - 13 * s}, {c.x + 12 * s, c.y - 1 * s}, colour, 2 * s, 0, thick);
				draw->AddRect({c.x - 8 * s, c.y + 3 * s}, {c.x + 8 * s, c.y + 13 * s}, colour, 2 * s, 0, thick);
			}
			else if (name == "swap")
			{
				// a TV and a GamePad, arrows between them
				draw->AddRect({c.x - 14 * s, c.y - 12 * s}, {c.x + 2 * s, c.y - 1 * s}, colour, 2 * s, 0, thick);
				draw->AddRect({c.x - 2 * s, c.y + 3 * s}, {c.x + 14 * s, c.y + 12 * s}, colour, 3 * s, 0, thick);
				draw->AddLine({c.x + 7 * s, c.y - 10 * s}, {c.x + 12 * s, c.y - 5 * s}, colour, thick);
				draw->AddLine({c.x + 12 * s, c.y - 5 * s}, {c.x + 12 * s, c.y - 10 * s}, colour, thick);
				draw->AddLine({c.x - 7 * s, c.y + 10 * s}, {c.x - 12 * s, c.y + 5 * s}, colour, thick);
				draw->AddLine({c.x - 12 * s, c.y + 5 * s}, {c.x - 12 * s, c.y + 10 * s}, colour, thick);
			}
			else if (name == "packs")
			{
				// three layers, the top one over the others
				for (const float layer : {6.0f, 0.0f, -6.0f})
				{
					const ImVec2 diamond[] = {{c.x, c.y + layer * s - 7 * s}, {c.x + 13 * s, c.y + layer * s}, {c.x, c.y + layer * s + 7 * s},
						{c.x - 13 * s, c.y + layer * s}};
					draw->AddConvexPolyFilled(diamond, 4, colours.ink);
					draw->AddPolyline(diamond, 4, colour, ImDrawFlags_Closed, thick);
				}
			}
			else if (name == "amiibo")
			{
				// a figure's head on its round base
				draw->AddCircle({c.x, c.y - 6 * s}, 6 * s, colour, 0, thick);
				draw->AddLine({c.x - 5 * s, c.y + 4 * s}, {c.x + 5 * s, c.y + 4 * s}, colour, thick);
				ImVec2 base[24];
				for (int i = 0; i < 24; i++)
				{
					const float angle = i * 6.2831853f / 24;
					base[i] = {c.x + 13 * s * std::cos(angle), c.y + 9 * s + 4.5f * s * std::sin(angle)};
				}
				draw->AddPolyline(base, 24, colour, ImDrawFlags_Closed, thick);
			}
		}

		// Hold to confirm: a ring that fills from the top over the hold
		void HoldRing(float x, float y, float radius, float progress, ImU32 colour) const
		{
			draw->AddCircle(At(x, y), radius * scale, Fade(colour, 0.25f), 0, 3 * scale);
			if (progress <= 0)
				return;
			draw->PathArcTo(At(x, y), radius * scale, -1.5707963f, -1.5707963f + 6.2831853f * std::min(progress, 1.0f), 32);
			draw->PathStroke(colour, 0, 3 * scale);
		}
	};

	// The touchpad's cursor
	inline void DrawCursor(ImDrawList* draw, ImVec2 at, bool pressed, float scale)
	{
		const float radius = 12.0f * scale;
		if (pressed)
			draw->AddCircleFilled(at, radius, IM_COL32(255, 255, 255, 170));
		draw->AddCircle(at, radius + 2.0f * scale, IM_COL32(0, 0, 0, 200), 0, 3.0f * scale);
		draw->AddCircle(at, radius, IM_COL32(255, 255, 255, 255), 0, 2.5f * scale);
	}
}
