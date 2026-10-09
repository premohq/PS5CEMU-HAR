// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the in-game menus' sheet, the Wii U's (ingame.cpp) and the 3DS's (ingame3ds.cpp), as the
// launcher's Quick Menu (docs/UI-REDESIGN.md, 6.9). One sheet down the left of the screen over the
// game, dimmed more on the left than on the right: the game's cover, name and badges at its top, a
// row of quick actions, then the list. A row is a setting, an action, or a category that opens in
// place to show its own rows (one open at a time), its current value beside it so most visits need
// no opening; the list scrolls when an open category makes it longer than the sheet. Under the list,
// the focused row's help; at the bottom, the controller hints.
//
// The menu reads the controller itself, as the launcher does: Up and Down move (held, they repeat),
// from the list's ends onto the quick actions; Left and Right move along the quick actions or change
// a setting; Cross chooses, or opens and closes a category (Right opens it, Left closes it); Circle
// closes the open category, else the menu. What cannot be undone (leaving the game, loading a state)
// is held: a ring fills over 600 ms. The focus is the launcher's double ring, gliding between rows.

#pragma once

#include "menu_canvas.h"

#include "../ps5/pad.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace ps5menu
{
	struct Row
	{
		std::string id;
		std::string label, value;
		bool setting = false;	// Left and Right change it
		std::string help;		// its first paragraph shows under the list
		std::vector<Row> rows;	// a category: the rows it opens to
		bool apart = false;		// set apart from the rows above it, under a line (leaving the game)
		bool hold = false;		// Cross held to confirm, not pressed
		float slider = -1.0f;	// 0 to 1: drawn as a slider beside the value
	};

	// A quick action: a large tile above the list. Its id is a row's (a category's opens it) or an
	// action's of its own.
	struct Tile
	{
		std::string id, label;
		std::string icon;	 // menu_canvas.h's Icon
		std::string caption; // small, under the label
		bool hold = false;
		std::string help; // under the list while it is focused (else its row's)
	};

	// What the player did this frame: a row chosen with Cross, or a setting changed with Left or Right
	struct Action
	{
		std::string id;
		int change = 0;		 // -1 or 1
		bool chosen = false; // Cross
	};

	// The top of the sheet: the game
	struct Header
	{
		ImTextureID cover = nullptr;
		float coverWidth = 0, coverHeight = 0; // the picture's own size, for its shape
		std::string system;					   // the badge: WII U, 3DS
		std::string status;					   // the second: RUNNING, PAUSED
		std::string title, details;
	};

	class SideMenu
	{
	public:
		// The sheet's place on the launcher's 1920x1080 layout
		static constexpr float kX = 48, kY = 48, kWidth = 640, kHeight = 984;
		static constexpr float kInsetX = 84, kInnerWidth = 568; // the content's column
		static constexpr float kTilesTop = 224, kTileHeight = 120, kTileGap = 12;
		static constexpr float kRowHeight = 56, kPitch = 62, kApart = 22;
		static constexpr float kListBottom = kY + kHeight - 150;
		static constexpr uint64_t kHoldUs = 600000;

		// The menu opened again: its first quick action in focus, every category closed, and the
		// buttons held now (the shortcut that opened it) not counted until they are let go.
		void Reset()
		{
			m_focus.clear();
			m_open.clear();
			m_onTiles = true;
			m_tile = 0;
			m_scroll = 0;
			m_held = ~0u;
			m_holding.clear();
			m_holdProgress = 0;
			m_ringValid = false;
		}

		// The open category's id (empty: none), and the focused row's or quick action's
		const std::string& Open() const { return m_open; }
		const std::string& Focus() const { return m_onTiles ? m_tileId : m_focus; }
		void SetOpen(const std::string& id, const std::string& focus)
		{
			m_open = id;
			m_focus = focus;
			m_onTiles = false;
		}

		// One frame's buttons (ps5pad's, with the left stick's directions as the D-pad's), at nowUs.
		// What was chosen or changed comes back; close says Circle (or Options) asks for the menu to
		// close.
		Action Update(const std::vector<Row>& rows, const std::vector<Tile>& tiles, uint32_t buttons, uint64_t nowUs, bool& close)
		{
			close = false;
			Action action;
			const uint32_t pressed = buttons & ~m_held;
			m_held = buttons;
			if (buttons & ps5pad::kTouchPad)
			{
				m_holding.clear(); // a shortcut's chord, not the menu's
				m_holdProgress = 0;
				return action;
			}
			auto repeat = [&](uint32_t mask, int slot) {
				if (pressed & mask)
				{
					m_repeatAt[slot] = nowUs + 380000;
					return true;
				}
				if ((buttons & mask) && nowUs >= m_repeatAt[slot])
				{
					m_repeatAt[slot] = nowUs + 90000;
					return true;
				}
				return false;
			};
			const bool up = repeat(ps5pad::kUp, 0), down = repeat(ps5pad::kDown, 1);
			const bool left = repeat(ps5pad::kLeft, 2), right = repeat(ps5pad::kRight, 3);

			std::vector<Shown> shown = Visible(rows);
			m_tilesShown = !tiles.empty();
			if (tiles.empty())
				m_onTiles = false;
			else if (shown.empty())
				m_onTiles = true;
			m_tile = tiles.empty() ? 0 : std::clamp(m_tile, 0, (int)tiles.size() - 1);
			int at = Focused(shown);
			if (up || down)
			{
				const int last = (int)shown.size() - 1;
				if (m_onTiles)
				{
					m_onTiles = shown.empty();
					at = down ? 0 : last;
				}
				else if (!tiles.empty() && ((up && at == 0) || (down && at == last)))
					m_onTiles = true;
				else if (!shown.empty())
					at = (at + (down ? 1 : last)) % (last + 1);
				if (!m_onTiles)
					m_focus = shown[at].row->id;
			}

			if (pressed & ps5pad::kCircle)
			{
				if (!m_open.empty())
				{
					m_focus = m_open; // back to the category's own row, closed
					m_open.clear();
					m_onTiles = false;
				}
				else
					close = true;
			}
			else if (pressed & ps5pad::kOptions)
				close = true;
			else if (m_onTiles)
			{
				if (left || right)
					m_tile = std::clamp(m_tile + (right ? 1 : -1), 0, (int)tiles.size() - 1);
				const Tile& tile = tiles[m_tile];
				if (tile.hold)
					action = Hold(tile.id, pressed, buttons, nowUs);
				else if (pressed & ps5pad::kCross)
				{
					const auto category = std::find_if(rows.begin(), rows.end(), [&](const Row& row) { return row.id == tile.id && !row.rows.empty(); });
					if (category != rows.end())
					{
						// a category's quick action: it opens, on its first row
						m_open = category->id;
						m_focus = category->rows.front().id;
						m_onTiles = false;
					}
					else
						action = {tile.id, 1, true};
				}
			}
			else if (!shown.empty())
			{
				const Shown& focused = shown[at];
				const Row& row = *focused.row;
				const bool category = !row.rows.empty();
				if (category && ((pressed & ps5pad::kCross) || right || left))
				{
					const bool opening = m_open != row.id && !left;
					m_open = opening ? row.id : std::string();
				}
				else if (left && focused.depth > 0 && !row.setting)
				{
					m_focus = m_open; // Left on a row in a category: back to the category, closed
					m_open.clear();
				}
				else if (row.hold)
					action = Hold(row.id, pressed, buttons, nowUs);
				else if (pressed & ps5pad::kCross)
					action = {row.id, 1, true};
				else if ((left || right) && row.setting)
					action = {row.id, left ? -1 : 1, false};
			}
			if (m_onTiles && !tiles.empty())
				m_tileId = tiles[m_tile].id;
			const std::string focus = Focus();
			if (!m_holding.empty() && m_holding != focus)
			{
				m_holding.clear(); // the focus moved away: the hold starts over
				m_holdProgress = 0;
			}
			KeepInView(Visible(rows));
			return action;
		}

		void Draw(const Canvas& canvas, const Fonts& fonts, const Header& header, const std::vector<Row>& rows, const std::vector<Tile>& tiles,
			const std::vector<std::pair<const char*, std::string>>& hints) const
		{
			const Palette& c = canvas.colours;
			ImDrawList* draw = canvas.draw;
			const float s = canvas.scale;
			const ImGuiIO& io = ImGui::GetIO();
			const float time = (float)ImGui::GetTime();
			const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);
			// the game, dimmed by one quad: more on the left, under the sheet, than on the right
			const ImU32 heavy = Colour(0x05070d, 0xd0), light = Colour(0x05070d, 0x58);
			draw->AddRectFilledMultiColor({0, 0}, io.DisplaySize, heavy, light, light, heavy);
			canvas.Panel(kX, kY, kWidth, kHeight);

			// the game: its cover (or a place for it), its name, its badges and facts
			constexpr float kCoverX = kInsetX, kCoverY = 84, kCoverBox = 112;
			float coverWidth = kCoverBox * 0.75f, coverHeight = kCoverBox;
			if (header.cover && header.coverWidth > 0 && header.coverHeight > 0)
			{
				const float fit = std::min(kCoverBox / header.coverWidth, kCoverBox / header.coverHeight);
				coverWidth = header.coverWidth * fit;
				coverHeight = header.coverHeight * fit;
				const ImVec2 a = canvas.At(kCoverX, kCoverY), b = canvas.At(kCoverX + coverWidth, kCoverY + coverHeight);
				draw->AddRectFilled({a.x, a.y + 10 * s}, {b.x, b.y + 14 * s}, IM_COL32(0, 0, 0, 80), 14 * s); // its shadow
				draw->AddImageRounded(header.cover, a, b, {0, 0}, {1, 1}, IM_COL32_WHITE, 14 * s);
				draw->AddRect(a, b, c.rowEdge, 14 * s, 0, std::max(1.0f, s));
			}
			else
			{
				draw->AddRectFilled(canvas.At(kCoverX, kCoverY), canvas.At(kCoverX + coverWidth, kCoverY + coverHeight), c.row, 14 * s);
				draw->AddRect(canvas.At(kCoverX, kCoverY), canvas.At(kCoverX + coverWidth, kCoverY + coverHeight), c.rowEdge, 14 * s, 0, s);
			}
			const float textX = kCoverX + coverWidth + 24, textWidth = kX + kWidth - 36 - textX;
			canvas.TextFit(fonts.heading, kHeading, textX, kCoverY + 2, c.title, header.title, textWidth);
			float chipX = textX;
			if (!header.system.empty())
				chipX = canvas.Chip(fonts.chip, chipX, kCoverY + 50, header.system, c.accent, Fade(c.accent, 0.16f), true);
			if (!header.status.empty())
				chipX = canvas.Chip(fonts.chip, chipX, kCoverY + 50, header.status, c.copy, c.focus);
			canvas.TextFit(fonts.caption, kCaption, textX, kCoverY + 96 - 6, c.faint, header.details, textWidth);

			// the ring's place this frame: where the focus is, reached on a spring
			float ringX = 0, ringY = 0, ringWidth = 0, ringHeight = 0;

			// the quick actions
			const float listTop = tiles.empty() ? kTilesTop + 8 : kTilesTop + kTileHeight + 24;
			if (!tiles.empty())
			{
				const float width = (kInnerWidth - kTileGap * (tiles.size() - 1)) / tiles.size();
				for (size_t i = 0; i < tiles.size(); i++)
				{
					const Tile& tile = tiles[i];
					const float x = kInsetX + i * (width + kTileGap), y = kTilesTop;
					const bool focused = m_onTiles && (int)i == m_tile;
					draw->AddRectFilled(canvas.At(x, y), canvas.At(x + width, y + kTileHeight), focused ? c.focus : c.row, 20 * s);
					if (!focused)
						draw->AddRect(canvas.At(x, y), canvas.At(x + width, y + kTileHeight), c.rowEdge, 20 * s, 0, std::max(1.0f, s));
					const ImU32 ink = focused ? c.title : c.copy;
					canvas.Icon(tile.icon, x + width / 2, y + (tile.caption.empty() ? 42 : 36), ink);
					canvas.TextFit(fonts.label, kLabel - 2, x + width / 2, y + (tile.caption.empty() ? 72 : 62), ink, tile.label, width - 16, Canvas::Align::Centre);
					if (!tile.caption.empty())
						canvas.TextFit(fonts.caption, kCaption - 2, x + width / 2, y + 90, c.faint, tile.caption, width - 16, Canvas::Align::Centre);
					if (focused)
					{
						ringX = x, ringY = y, ringWidth = width, ringHeight = kTileHeight;
						if (tile.hold && m_holdProgress > 0)
							draw->AddRectFilled(canvas.At(x + 14, y + kTileHeight - 10), canvas.At(x + 14 + (width - 28) * std::min(m_holdProgress, 1.0f),
								y + kTileHeight - 6), c.accent, 2 * s);
					}
				}
			}

			// the rows, scrolled so the focused one is in view
			const std::vector<Shown> shown = Visible(rows);
			const int at = Focused(shown);
			const float room = kListBottom - listTop;
			m_scrollShown += (m_scroll - m_scrollShown) * (m_ringValid ? 1.0f - std::exp(-16.0f * dt) : 1.0f);
			draw->PushClipRect(canvas.At(kX, listTop - 12), canvas.At(kX + kWidth, kListBottom + 12), true);
			for (int i = 0; i < (int)shown.size(); i++)
			{
				const Row& row = *shown[i].row;
				const float y = listTop + shown[i].y - m_scrollShown;
				if (y + kRowHeight < listTop - 12 || y > kListBottom + 12)
					continue;
				if (row.apart)
					draw->AddLine(canvas.At(kInsetX + 8, y - kApart / 2 - 3), canvas.At(kInsetX + kInnerWidth - 8, y - kApart / 2 - 3), c.line, std::max(1.0f, s));
				const bool focused = !m_onTiles && i == at;
				const bool child = shown[i].depth > 0;
				const float indent = child ? 24.0f : 0.0f;
				const float x = kInsetX + indent, width = kInnerWidth - indent;
				if (focused)
				{
					draw->AddRectFilled(canvas.At(x, y), canvas.At(x + width, y + kRowHeight), c.focus, 20 * s);
					ringX = x, ringY = y, ringWidth = width, ringHeight = kRowHeight;
				}
				else if (child)
					draw->AddRectFilled(canvas.At(x, y), canvas.At(x + width, y + kRowHeight), c.row, 20 * s);
				ImFont* labelFont = child ? fonts.label : fonts.body;
				const float labelSize = child ? kLabel : kBody;
				const ImU32 labelColour = row.apart ? c.bad : child && !focused ? c.copy : c.text;
				const bool category = !row.rows.empty();
				float right = x + width - 20;
				if (category)
				{
					canvas.Chevron(right - 4, y + kRowHeight / 2, c.faint, m_open == row.id ? Canvas::Point::Down : Canvas::Point::Right);
					right -= 28;
				}
				// what goes right of the label: a hold's ring, a slider, the value
				float valueLeft = right;
				if (row.hold && focused)
				{
					const float ringRadius = 11;
					canvas.HoldRing(right - ringRadius, y + kRowHeight / 2, ringRadius, m_holdProgress, row.apart ? c.bad : c.accent);
					right -= ringRadius * 2 + 12;
					canvas.Button(right - 9, y + kRowHeight / 2, "cross", c.copy);
					right -= 28;
					// tr: beside a row held to confirm: hold Cross
					const char* hold = ps5lang::Tr("Hold");
					valueLeft = right - canvas.TextFit(fonts.label, kLabel, right, y + 14, c.copy, hold, 140, Canvas::Align::Right);
				}
				else if (!row.value.empty())
				{
					const bool arrows = focused && row.setting;
					if (arrows)
					{
						canvas.Chevron(right - 4, y + kRowHeight / 2, c.copy);
						right -= 22;
					}
					const float labelRoom = canvas.Width(labelFont, labelSize, row.label) + 36;
					const float sliderWidth = row.slider >= 0 ? 150 : 0;
					// the value has what the label leaves it, and at least a third of the row
					const float valueRoom = std::max(width * 0.34f, right - x - 20 - labelRoom - sliderWidth);
					valueLeft = right - canvas.TextFit(fonts.label, kLabel, right, y + 14, row.slider >= 0 ? c.text : c.copy, row.value, valueRoom,
											Canvas::Align::Right);
					if (row.slider >= 0)
					{
						// the slider: its track, filled in the side's colour to the knob
						const float trackRight = valueLeft - 18, trackLeft = trackRight - 132, middle = y + kRowHeight / 2;
						const float knob = trackLeft + (trackRight - trackLeft) * std::clamp(row.slider, 0.0f, 1.0f);
						draw->AddRectFilled(canvas.At(trackLeft, middle - 3), canvas.At(trackRight, middle + 3), c.focus, 3 * s);
						draw->AddRectFilled(canvas.At(trackLeft, middle - 3), canvas.At(knob, middle + 3), c.accent, 3 * s);
						draw->AddCircleFilled(canvas.At(knob, middle), 9 * s, c.title);
						valueLeft = trackLeft - 12;
					}
					if (arrows)
					{
						canvas.Chevron(valueLeft - 14, y + kRowHeight / 2, c.copy, Canvas::Point::Left);
						valueLeft -= 26;
					}
				}
				canvas.TextFit(labelFont, labelSize, x + 20, y + (child ? 14.0f : 12.0f), labelColour, row.label, std::max(80.0f, valueLeft - x - 40));
			}
			draw->PopClipRect();
			// a scroll bar, when the list is longer than its place
			const float total = shown.empty() ? 0 : shown.back().y + kRowHeight;
			if (total > room)
			{
				const float barTop = listTop + room * m_scrollShown / total, barHeight = room * room / total;
				draw->AddRectFilled(canvas.At(kX + kWidth - 16, listTop), canvas.At(kX + kWidth - 12, kListBottom), c.row, 2 * s);
				draw->AddRectFilled(canvas.At(kX + kWidth - 16, barTop), canvas.At(kX + kWidth - 12, barTop + barHeight), c.faint, 2 * s);
			}

			// the ring, gliding to the focus
			if (ringWidth > 0)
			{
				const float k = m_ringValid ? 1.0f - std::exp(-20.0f * dt) : 1.0f;
				m_ring[0] += (ringX - m_ring[0]) * k;
				m_ring[1] += (ringY - m_ring[1]) * k;
				m_ring[2] += (ringWidth - m_ring[2]) * k;
				m_ring[3] += (ringHeight - m_ring[3]) * k;
				m_ringValid = true;
				canvas.Ring(m_ring[0], m_ring[1], m_ring[2], m_ring[3], 20, time);
			}

			// the focused row's help: its first paragraph, on two lines at most
			std::string help;
			if (!m_onTiles && !shown.empty())
				help = shown[at].row->help;
			else if (m_onTiles && !tiles.empty())
			{
				const Tile& tile = tiles[std::clamp(m_tile, 0, (int)tiles.size() - 1)];
				help = tile.help;
				for (const Row& row : rows)
				{
					if (row.id == tile.id && help.empty())
						help = row.help;
					for (const Row& child : row.rows)
						if (child.id == tile.id && help.empty())
							help = child.help;
				}
				if (help.empty() && tile.hold)
					help = ps5lang::Tr("Hold Cross until the ring fills.");
			}
			if (!help.empty())
			{
				const std::string first = help.substr(0, help.find('\n'));
				// two lines under the list: a little smaller, then cut, never clipped mid-line
				canvas.TextLines(fonts.caption, kCaption, kInsetX + 4, kListBottom + 18, c.faint, first, kInnerWidth - 8, 2);
			}
			float x = kInsetX + 4;
			const float hintSize = canvas.HintsSize(fonts.label, hints, kInnerWidth - 8);
			for (const auto& [button, label] : hints)
				x = canvas.Hint(fonts.label, x, kY + kHeight - 62, button, label, hintSize);
		}

	private:
		struct Shown
		{
			const Row* row;
			int depth;
			float y; // from the list's top
		};

		// Cross held on a row or quick action that asks for it: what it does once the ring is full
		Action Hold(const std::string& id, uint32_t pressed, uint32_t buttons, uint64_t nowUs)
		{
			if (pressed & ps5pad::kCross)
			{
				m_holding = id;
				m_holdStart = nowUs;
			}
			if (m_holding != id || !(buttons & ps5pad::kCross))
			{
				m_holding.clear();
				m_holdProgress = 0;
				return {};
			}
			m_holdProgress = (float)(nowUs - m_holdStart) / kHoldUs;
			if (m_holdProgress < 1.0f)
				return {};
			m_holding.clear(); // once: Cross is let go and pressed again before it counts again
			m_holdProgress = 0;
			return {id, 1, true};
		}

		std::vector<Shown> Visible(const std::vector<Row>& rows) const
		{
			std::vector<Shown> shown;
			float y = 0;
			for (const Row& row : rows)
			{
				if (row.apart && !shown.empty())
					y += kApart;
				shown.push_back({&row, 0, y});
				y += kPitch;
				if (!row.rows.empty() && m_open == row.id)
					for (const Row& child : row.rows)
					{
						shown.push_back({&child, 1, y});
						y += kPitch;
					}
			}
			return shown;
		}

		int Focused(const std::vector<Shown>& shown) const
		{
			for (int i = 0; i < (int)shown.size(); i++)
				if (shown[i].row->id == m_focus)
					return i;
			return 0;
		}

		void KeepInView(const std::vector<Shown>& shown)
		{
			if (shown.empty())
				return;
			const int at = Focused(shown);
			m_focus = shown[at].row->id;
			if (m_onTiles)
				return;
			const float listTop = m_tilesShown ? kTilesTop + kTileHeight + 24 : kTilesTop + 8;
			const float room = kListBottom - listTop, total = shown.back().y + kRowHeight;
			// the focused row, and the one after it where there is one, in view
			const float top = shown[at].y - (at > 0 ? kPitch * 0.5f : 0.0f);
			const float bottom = shown[at].y + kRowHeight + (at + 1 < (int)shown.size() ? kPitch * 0.5f : 0.0f);
			if (top < m_scroll)
				m_scroll = top;
			if (bottom > m_scroll + room)
				m_scroll = bottom - room;
			m_scroll = std::clamp(m_scroll, 0.0f, std::max(0.0f, total - room));
		}

		std::string m_focus;  // the focused row's id
		std::string m_open;	  // the open category's id
		bool m_onTiles = true; // the focus is on the quick actions
		int m_tile = 0;
		std::string m_tileId;
		bool m_tilesShown = true;
		float m_scroll = 0;
		uint32_t m_held = ~0u;
		std::array<uint64_t, 4> m_repeatAt{};
		std::string m_holding; // the row or quick action Cross is held on
		uint64_t m_holdStart = 0;
		float m_holdProgress = 0;
		// what moves on a spring as it is drawn
		mutable float m_scrollShown = 0;
		mutable float m_ring[4] = {};
		mutable bool m_ringValid = false;
	};
}
