// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the Library (docs/UI-REDESIGN.md, 6.2): every game on the side, found fast. Filters
// across the top with their counts, Sort (Square) and Search (Triangle); the covers on a shelf of
// equal rows, the focused one lifted with its name and facts under it; L2 and R2 jump to the previous
// or next letter (or year, or status); an index of the letters at the right. On the 3DS side the
// shelf's first tile is Artic Base.

#include "shell_internal.h"
#include "../actions.h"
#include "../../app/emulator.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace ps5shell
{
	using namespace ui::tokens;
	using namespace ps5actions;

	namespace
	{
		// tr: the Library's orders (Square)
		constexpr const char* kSorts[] = {TrMark("Recently played"), TrMark("A to Z"), TrMark("Release year"), TrMark("How it runs")};

		std::vector<std::string> SortNames()
		{
			std::vector<std::string> names;
			for (const char* sort : kSorts)
				names.push_back(Tr(sort));
			return names;
		}
		constexpr float kGridTop = 236;

		int StatusRank(const ps5compat::Report* report)
		{
			if (!report)
				return 4;
			const std::string kind = ps5compat::Kind(report->status);
			return report->status == "Playable" ? 0 : kind == "good" ? 0 : kind == "warn" ? 1 : report->status == "Crashes" ? 2 : 3;
		}
	}

	int Shell::LibraryColumns() const
	{
		return Is3ds() ? 6 : 8;
	}

	int Shell::LibraryGame() const
	{
		const int at = m_libraryIndex - (Is3ds() ? 1 : 0);
		return at >= 0 && at < (int)m_shelf.size() ? m_shelf[at] : -1;
	}

	void Shell::LibraryRefresh()
	{
		const int side = Is3ds() ? 1 : 0;
		int& filter = m_settings.ui.libraryFilter[side];
		if (filter == 3 && (Is3ds() || !m_status.coreReady))
			filter = 0;
		const int sort = std::clamp(m_settings.ui.librarySort[side], 0, 3);
		const int focused = LibraryGame();
		if (focused >= 0)
			m_libraryFocusTitle = m_games[focused].entry.game.titleId;
		const int64_t recent = std::time(nullptr) - 30 * 86400;
		const std::string search = Lower(m_search);
		m_shelf.clear();
		for (int i = 0; i < (int)m_games.size(); i++)
		{
			Game& game = m_games[i];
			if (filter == 1 && game.entry.added < recent)
				continue;
			if (filter == 2 && !game.entry.favourite)
				continue;
			if (filter == 3)
			{
				if (game.packsOn < 0)
					game.packsOn = ps5emu::EnabledGraphicPackCount(game.entry.game.titleId);
				if (game.packsOn == 0)
					continue;
			}
			if (!search.empty() && Lower(game.entry.game.name).find(search) == std::string::npos)
				continue;
			m_shelf.push_back(i);
		}
		std::stable_sort(m_shelf.begin(), m_shelf.end(), [&](int a, int b) {
			const Game& x = m_games[a];
			const Game& y = m_games[b];
			switch (sort)
			{
			case 0:
				if (x.entry.lastPlayed != y.entry.lastPlayed)
					return x.entry.lastPlayed > y.entry.lastPlayed;
				break;
			case 2:
				if (x.year != y.year)
					return (x.year ? x.year : -1) > (y.year ? y.year : -1);
				break;
			case 3:
				if (StatusRank(x.report) != StatusRank(y.report))
					return StatusRank(x.report) < StatusRank(y.report);
				break;
			default: break;
			}
			return x.sortName < y.sortName;
		});
		// the focus stays on the game it was on
		const int offset = Is3ds() ? 1 : 0;
		m_libraryIndex = std::clamp(m_libraryIndex, 0, std::max(0, (int)m_shelf.size() + offset - 1));
		for (int i = 0; i < (int)m_shelf.size(); i++)
			if (m_games[m_shelf[i]].entry.game.titleId == m_libraryFocusTitle)
				m_libraryIndex = i + offset;
	}

	void Shell::LibraryUpdate(const ui::Press& press)
	{
		const Button b = press.button;
		const int side = Is3ds() ? 1 : 0;
		const int columns = LibraryColumns();
		const int offset = Is3ds() ? 1 : 0;
		const int total = (int)m_shelf.size() + offset;
		auto sortPicker = [this, side] {
			OpenPicker(TrC("tab", "Library"), Tr("Sort"), SortNames(), m_settings.ui.librarySort[side],
				[this, side](int choice) {
					m_settings.ui.librarySort[side] = choice;
					SaveSettings();
					LibraryRefresh();
				});
		};
		if (b == Button::Square)
		{
			sortPicker();
			return;
		}
		if (b == Button::Triangle)
		{
			OpenKeyboard();
			return;
		}
		if (b == Button::Circle)
		{
			if (!m_search.empty())
			{
				m_search.clear();
				LibraryRefresh();
				m_feedback.Play(ui::Cue::Back);
				return;
			}
			ShowTab(ScreenId::Home);
			m_feedback.Play(ui::Cue::Back);
			return;
		}
		if (m_libraryZone == 0)
		{
			const int filters = Is3ds() || !m_status.coreReady ? 3 : 4;
			int& filter = m_settings.ui.libraryFilter[side];
			switch (b)
			{
			case Button::Left:
			case Button::Right:
			{
				const int next = filter + (b == Button::Right ? 1 : -1);
				if (next < 0 || next >= filters)
					m_feedback.Play(ui::Cue::Edge, press.repeat);
				else
				{
					filter = next;
					SaveSettings();
					m_libraryIndex = 0;
					m_libraryFocusTitle = 0;
					LibraryRefresh();
					m_feedback.Play(ui::Cue::Focus);
				}
				break;
			}
			case Button::Up: FocusBar(); break;
			case Button::Down:
			case Button::Cross:
				if (total > 0)
				{
					m_libraryZone = 1;
					m_feedback.Play(ui::Cue::Focus);
				}
				else
					m_feedback.Play(ui::Cue::Edge, press.repeat);
				break;
			default: break;
			}
			return;
		}
		if (total == 0)
		{
			if (b == Button::Up)
				m_libraryZone = 0;
			return;
		}
		int& at = m_libraryIndex;
		auto move = [&](int to) {
			to = std::clamp(to, 0, total - 1);
			if (to == at)
				m_feedback.Play(ui::Cue::Edge, press.repeat);
			else
			{
				at = to;
				m_feedback.Play(ui::Cue::Focus);
			}
		};
		switch (b)
		{
		case Button::Left:
			if (at % columns == 0)
				m_feedback.Play(ui::Cue::Edge, press.repeat);
			else
				move(at - 1);
			break;
		case Button::Right:
			if (at % columns == columns - 1)
				m_feedback.Play(ui::Cue::Edge, press.repeat);
			else
				move(at + 1);
			break;
		case Button::Up:
			if (at < columns)
			{
				m_libraryZone = 0;
				m_feedback.Play(ui::Cue::Focus);
			}
			else
				move(at - columns);
			break;
		case Button::Down:
			if (at + columns < total)
				move(at + columns);
			else if (at / columns < (total - 1) / columns)
				move(total - 1);
			else
				m_feedback.Play(ui::Cue::Edge, press.repeat);
			break;
		case Button::L2:
		case Button::R2:
		{
			// to the previous or next group: letter, year or status, as sorted
			const int sort = m_settings.ui.librarySort[side];
			auto group = [&](int index) -> int {
				if (index < offset)
					return -1;
				const Game& game = m_games[m_shelf[index - offset]];
				if (sort == 2)
					return game.year;
				if (sort == 3)
					return StatusRank(game.report);
				if (sort == 0)
					return (index - offset) / 10;
				const char c = game.sortName.empty() ? '#' : game.sortName[0];
				return std::isalpha((unsigned char)c) ? c : '#';
			};
			const int current = group(at);
			int to = at;
			if (b == Button::R2)
			{
				while (to < total - 1 && group(to) == current)
					to++;
			}
			else
			{
				while (to > 0 && group(to - 1) == current)
					to--;
				if (to == at && to > 0)
				{
					const int previous = group(to - 1);
					to--;
					while (to > 0 && group(to - 1) == previous)
						to--;
				}
			}
			move(to);
			break;
		}
		case Button::Cross:
		{
			if (Is3ds() && at == 0)
			{
				m_feedback.Play(ui::Cue::Select);
				m_pageFrom = ScreenId::Library;
				ArticOpen();
				break;
			}
			const int game = LibraryGame();
			if (game >= 0)
				Launch(game);
			break;
		}
		case Button::Options:
			if (LibraryGame() >= 0)
				OpenGameMenu(LibraryGame());
			break;
		default: break;
		}
		if (LibraryGame() >= 0)
			m_libraryFocusTitle = m_games[LibraryGame()].entry.game.titleId;
	}

	void Shell::LibraryDraw(Canvas& canvas)
	{
		DrawBar(canvas, true);
		const int side = Is3ds() ? 1 : 0;
		const bool wiiu = !Is3ds();
		const int offset = Is3ds() ? 1 : 0;
		const int columns = LibraryColumns();
		const float tileW = wiiu ? 186.0f : 216.0f, tileH = wiiu ? 260.0f : 200.0f, gap = wiiu ? 24.0f : 28.0f;
		const float pitch = tileH + 112;
		const int total = (int)m_shelf.size() + offset;

		// the filters, with their counts, and Sort and Search at the right
		canvas.PushAlpha(Enter(1));
		const int64_t recent = std::time(nullptr) - 30 * 86400;
		int counts[4] = {(int)m_games.size(), 0, 0, 0};
		for (Game& game : m_games)
		{
			counts[1] += game.entry.added >= recent;
			counts[2] += game.entry.favourite;
			counts[3] += game.packsOn > 0;
		}
		// tr: the Library's filters, each with its count after it
		const char* names[4] = {TrC("filter", "All"), Tr("Recently added"), Tr("Favourites"), Tr("Graphic packs on")};
		const int filters = Is3ds() || !m_status.coreReady ? 3 : 4;
		const int filter = m_settings.ui.libraryFilter[side];
		// Sort and Search at the right first: the filters have the rest of the row, their text a step
		// smaller at a time in a language whose words do not fit it
		const ui::TextStyle action = Style({24, ui::Weight::Medium, 1.0f});
		const std::string sortLabel = TrF("Sort: {0}", Tr(kSorts[std::clamp(m_settings.ui.librarySort[side], 0, 3)]));
		const float actionsWidth = m_fonts.Width(action, Tr("Search")) + 36 + 34 + m_fonts.Width(action, sortLabel) + 36;
		const float filtersRoom = 1920 - 2 * kSafeX - actionsWidth - 40 - (m_scanning || !m_search.empty() ? 260 : 0);
		ui::TextStyle pillStyle = Style({26, ui::Weight::Medium, 1.0f});
		ui::TextStyle countStyle = Style({20, ui::Weight::SemiBold, 1.0f, 0, false, true});
		auto filtersWidth = [&] {
			float width = 0;
			for (int i = 0; i < filters; i++)
				width += 26 + m_fonts.Width(pillStyle, names[i]) + 12 + m_fonts.Width(countStyle, std::to_string(counts[i])) + 26 + 14;
			return width;
		};
		for (int step = 0; step < 6 && filtersWidth() > filtersRoom; step++)
		{
			pillStyle.size -= 2;
			countStyle.size -= 1;
		}
		float x = kSafeX;
		for (int i = 0; i < filters; i++)
		{
			const bool on = i == filter;
			const std::string count = i == 3 && counts[3] == 0 ? "" : std::to_string(counts[i]);
			const float w = 26 + m_fonts.Width(pillStyle, names[i]) + (count.empty() ? 0 : 12 + m_fonts.Width(countStyle, count)) + 26;
			const Box pill{x, 136, w, 52};
			canvas.Rect(pill, 26, on ? ui::SetAlpha(Accent(), 0.22f) : Surface());
			canvas.Ring(pill, 26, 1.5f, on ? ui::SetAlpha(Accent(), 0.5f) : kGlassEdge);
			const ui::TextBlock text = m_fonts.Layout(pillStyle, names[i]);
			canvas.Text(text, x + 26, pill.CentreY() - text.height * 0.5f, on ? kText : Secondary());
			if (!count.empty())
			{
				const ui::TextBlock number = m_fonts.Layout(countStyle, count);
				canvas.Text(number, x + 26 + text.width + 12, pill.CentreY() - number.height * 0.5f, on ? Accent() : Tertiary());
			}
			if (!m_onBar && m_libraryZone == 0 && on)
				Focus(pill, 26);
			x += w + 14;
		}
		const float room = 1920 - kSafeX - actionsWidth - 20 - (x + 12);
		if (!m_search.empty())
			Chip(canvas, x + 6, 143, TrF("Search: {0}", m_search));
		if (m_scanning)
			canvas.Text(Style(kCaptionStyle), x + 12, 150, Tr("Looking for new games…"), Tertiary(), std::max(120.0f, room), 1);
		// Sort and Search
		float right = 1920 - kSafeX;
		const ui::TextBlock searchText = m_fonts.Layout(action, Tr("Search"));
		right -= searchText.width;
		canvas.Text(searchText, right, 162 - searchText.height * 0.5f, Secondary());
		canvas.Draw(Icon::Triangle, {right - 36, 148, 28, 28}, Secondary());
		right -= 36 + 34;
		const ui::TextBlock sortText = m_fonts.Layout(action, sortLabel);
		right -= sortText.width;
		canvas.Text(sortText, right, 162 - sortText.height * 0.5f, Secondary());
		canvas.Draw(Icon::Square, {right - 36, 148, 28, 28}, Secondary());
		canvas.PopAlpha();

		// the shelf
		if (total == 0)
		{
			WantBackdrop(nullptr);
			canvas.PushAlpha(Enter(2));
			canvas.Text(Style(kTitleStyle), kSafeX, 320,
				m_scanning			? std::string(Tr("Looking for games…")) :
				!m_search.empty()	? TrF("Nothing matches “{0}”", m_search) :
				filter				? std::string(Tr("None here yet")) :
									  std::string(Tr("No games yet")),
				kText, 1920 - 2 * kSafeX, 1);
			canvas.Text(Style(kBodyStyle), kSafeX, 390,
				!m_search.empty() ? Tr("Circle clears the search.") :
				filter == 2		  ? Tr("Options on a game, then Favourite, puts it here.") :
				filter			  ? Tr("All shows every game.") :
									Tr("Settings > Games and folders says where they go."),
				Secondary(), 900, 3);
			canvas.PopAlpha();
			return;
		}
		m_libraryIndex = std::clamp(m_libraryIndex, 0, total - 1);
		const int focusRow = m_libraryIndex / columns;
		m_libraryScroll.target = std::max(0, focusRow - 1) * pitch;
		m_libraryScroll.omega = m_settings.ui.reduceMotion ? 60.0f : kScrollOmega;
		m_libraryScroll.Update(m_dt);
		const float scroll = m_libraryScroll.value;
		const int game = LibraryGame();
		WantBackdrop(game >= 0 ? &m_games[game] : nullptr);
		canvas.PushAlpha(Enter(2));
		canvas.PushOffset(0, (1 - Enter(2)) * 30);
		canvas.PushClip({0, 210, 1920, 870});
		Box focusedBox{};
		for (int index = 0; index < total; index++)
		{
			const int row = index / columns, column = index % columns;
			const Box box{kSafeX + column * (tileW + gap), kGridTop + row * pitch - scroll, tileW, tileH};
			if (box.Bottom() < 200 || box.y > 1090)
				continue;
			const bool focused = !m_onBar && m_libraryZone == 1 && index == m_libraryIndex;
			if (focused)
			{
				focusedBox = box;
				continue; // drawn last, over its neighbours
			}
			if (index < offset)
			{
				// Artic Base: a game from your 3DS
				canvas.Rect(box, kRadiusCard, 0x0dffffff);
				canvas.Ring(box, kRadiusCard, 2, ui::SetAlpha(kN3ds, 0.45f));
				canvas.Text(Style({26, ui::Weight::SemiBold, 1.2f}), box.x + 16, box.y + box.h * 0.22f, Tr("Play from your 3DS"), kText, box.w - 32, 2,
					ui::Align::Centre);
				canvas.Text(Style({19, ui::Weight::Regular, 1.3f}), box.x + 16, box.y + box.h * 0.6f, Tr("Artic Base, over your network"), Secondary(),
					box.w - 32, 2, ui::Align::Centre);
			}
			else
				DrawCover(canvas, m_games[m_shelf[index - offset]], box, false, 0);
		}
		if (focusedBox.w > 0)
		{
			const float lift = m_settings.ui.reduceMotion ? 0.0f : 0.07f;
			if (m_libraryIndex < offset)
			{
				const Box box = focusedBox.Scaled(1 + lift);
				canvas.Rect(box, kRadiusCard, 0x1affffff);
				canvas.Text(Style({26, ui::Weight::SemiBold, 1.2f}), box.x + 16, box.y + box.h * 0.22f, Tr("Play from your 3DS"), kText, box.w - 32, 2,
					ui::Align::Centre);
				canvas.Text(Style({19, ui::Weight::Regular, 1.3f}), box.x + 16, box.y + box.h * 0.6f, Tr("Artic Base, over your network"), Secondary(),
					box.w - 32, 2, ui::Align::Centre);
				Focus(box, kRadiusCard, kN3ds);
			}
			else
			{
				Game& g = m_games[m_shelf[m_libraryIndex - offset]];
				DrawCover(canvas, g, focusedBox, true, lift);
				// its name and facts under it
				const float under = focusedBox.Bottom() + tileH * lift * 0.5f + 22;
				const float infoX = std::min(focusedBox.x, 1920 - kSafeX - 640);
				const ui::TextBlock name = canvas.Text(Style({28, ui::Weight::SemiBold, 1.15f}), infoX, under, g.entry.game.name, kText, 640, 1);
				float fx = infoX;
				const ui::TextBlock facts = canvas.Text(Style({22, ui::Weight::Regular, 1.2f}), fx, under + name.height + 6, Byline(g), Secondary(), 420, 1);
				fx += facts.width + 14;
				if (g.report)
					fx += Chip(canvas, fx, under + name.height + 2, TrC("status", g.report->status), ps5compat::Kind(g.report->status), 34) + 10;
				if (!Played(g).empty())
					Chip(canvas, fx, under + name.height + 2, Played(g), "", 34);
			}
		}
		canvas.PopClip();
		canvas.PopOffset();
		canvas.PopAlpha();

		// the index of letters at the right, for A to Z
		if (m_settings.ui.librarySort[side] == 1 && m_shelf.size() > 12)
		{
			std::vector<char> letters;
			for (int index : m_shelf)
			{
				const char c = m_games[index].sortName.empty() ? '#' : (char)std::toupper((unsigned char)m_games[index].sortName[0]);
				const char letter = std::isalpha((unsigned char)c) ? c : '#';
				if (letters.empty() || letters.back() != letter)
					if (std::find(letters.begin(), letters.end(), letter) == letters.end())
						letters.push_back(letter);
			}
			const char current = game >= 0 && !m_games[game].sortName.empty() && std::isalpha((unsigned char)m_games[game].sortName[0]) ?
				(char)std::toupper((unsigned char)m_games[game].sortName[0]) :
				'#';
			const float step = std::min(25.0f, 640.0f / letters.size());
			float y = 540 - letters.size() * step / 2;
			canvas.PushAlpha(Enter(3));
			for (char letter : letters)
			{
				const bool on = letter == current && game >= 0;
				if (on)
					canvas.Rect({1852 - 15, y - 2, 30, 30}, 15, Accent());
				canvas.Text(Style({16, ui::Weight::SemiBold, 1.0f}), 1852, y + 6, std::string(1, letter), on ? kInk1 : Tertiary(), 0, 0, ui::Align::Centre);
				y += step;
			}
			canvas.PopAlpha();
		}
	}
}
