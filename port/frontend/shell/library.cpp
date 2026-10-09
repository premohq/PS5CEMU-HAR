// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the Library (docs/UI-REDESIGN.md, 6.2): where every side opens, every game on it,
// found fast, the one played last in focus. Filters across the top with their counts, Sort (Square)
// and Search (Triangle); the covers on a shelf of equal rows, the focused one lifted with its name and
// facts under it; L2 and R2 jump to the previous or next letter (or year, or status); an index of the
// letters at the right. On the 3DS side the shelf's first tile is Artic Base. Before there is a game
// to show (a notice, Cemu starting, the first scan, no games yet) a card says why, with what to do
// (6.1: what Home showed).

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
		constexpr const char* kSorts[] = {"Recently played", "A to Z", "Release year", "How it runs"};
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
		return IsWiiU() ? 8 : 6;
	}

	int Shell::LibraryFilters() const
	{
		return IsWiiU() && m_status.coreReady ? 4 : 3;
	}

	int Shell::LibraryGame() const
	{
		const int at = m_libraryIndex - ArticTiles();
		const int game = at >= 0 && at < (int)m_shelf.size() ? m_shelf[at] : -1;
		return game < (int)m_games.size() ? game : -1; // a shelf of a side being left has none
	}

	bool Shell::LibraryState(std::string& kicker, std::string& title, std::string& text, std::vector<std::string>& buttons) const
	{
		buttons.clear();
		if (!Notice().empty())
		{
			kicker = "Setup";
			title = "Something needs a look";
			text = Notice();
			buttons = {"Setup check", "Diagnostics"};
			return true;
		}
		if (!CoreReady())
		{
			kicker = "Wii U";
			title = "Starting Cemu";
			text = "Its settings, graphic packs and controllers are loading, and then it looks for your games.";
			return true;
		}
		if (!m_games.empty())
			return false;
		if (m_scanning)
		{
			kicker = SystemName(m_side);
			title = "Looking for games…";
			text = "In " + ShortPath(GamesFolder(m_side), 60) + ".";
			return true;
		}
		const std::string& folder = GamesFolder(m_side);
		kicker = "Welcome";
		title = "Your games go here";
		text = Is3ds() ? "Put your 3DS games (.3ds, .cci, .cxi, .3dsx, decrypted) in " + folder +
				", or choose any folder the PS5 can read, such as one on a USB drive. Folders inside it are searched too." :
			IsDs() ? "Put your DS games (.nds) in " + folder +
				", or choose any folder the PS5 can read, such as one on a USB drive. Folders inside it are searched too." :
					 "Put your Wii U games (.wua, .wud, .wux, or folders with code, content and meta) in " + folder +
				", or choose any folder the PS5 can read, such as one on a USB drive.";
		buttons = {"Choose a folder", "Setup check"};
		if (Is3ds())
			buttons.insert(buttons.begin() + 1, "Play from your 3DS"); // Artic Base needs no games here
		return true;
	}

	void Shell::LibraryStateUpdate(const ui::Press& press)
	{
		std::string kicker, title, text;
		std::vector<std::string> buttons;
		LibraryState(kicker, title, text, buttons);
		const Button b = press.button;
		const int count = (int)buttons.size();
		if (b == Button::Up || b == Button::Circle)
			FocusBar();
		else if ((b == Button::Left || b == Button::Right) && count > 0)
		{
			m_stateButton = (m_stateButton + (b == Button::Right ? 1 : count - 1)) % count;
			m_feedback.Play(ui::Cue::Focus);
		}
		else if (b == Button::Cross)
		{
			if (count == 0)
			{
				m_feedback.Play(ui::Cue::Denied);
				return;
			}
			m_feedback.Play(ui::Cue::Select);
			const std::string& chosen = buttons[std::clamp(m_stateButton, 0, count - 1)];
			if (chosen == "Setup check")
				SetupOpen(false);
			else if (chosen == "Play from your 3DS")
			{
				m_pageFrom = ScreenId::Library;
				ArticOpen();
			}
			else if (chosen == "Diagnostics")
			{
				ShowTab(ScreenId::Settings);
				SettingsOpen("diagnostics");
			}
			else
			{
				m_pageFrom = ScreenId::Library;
				m_filesSide = m_side;
				FilesOpen(0);
			}
		}
	}

	void Shell::LibraryStateDraw(Canvas& canvas)
	{
		std::string kicker, title, text;
		std::vector<std::string> buttons;
		LibraryState(kicker, title, text, buttons);
		WantBackdrop(nullptr);
		m_stateButtons.clear();
		canvas.PushAlpha(Enter(1));
		const Box card{kSafeX, 300, 1100, 520};
		canvas.Text(Style(kOverlineStyle), card.x, card.y, kicker, Secondary());
		const ui::TextBlock titleBlock = canvas.Text(Style(kDisplayStyle), card.x, card.y + 36, title, kText, card.w, 2);
		const ui::TextBlock textBlock = canvas.Text(Style(kBodyStyle), card.x, card.y + 56 + titleBlock.height, text, Secondary(), 960, 5);
		float x = card.x;
		m_stateButton = std::clamp(m_stateButton, 0, std::max(0, (int)buttons.size() - 1));
		for (int i = 0; i < (int)buttons.size(); i++)
		{
			const Box b = PillButton(canvas, x, card.y + 96 + titleBlock.height + textBlock.height, buttons[i], i == 0, !m_onBar && m_stateButton == i);
			m_stateButtons.push_back(b);
			x += b.w + 20;
		}
		canvas.PopAlpha();
	}

	void Shell::LibraryRefresh()
	{
		const int side = SideIndex();
		int& filter = m_settings.ui.libraryFilter[side];
		if (filter >= LibraryFilters())
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
			if (filter == 3 && IsWiiU())
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
		// the focus stays on the game it was on (or goes to the one asked for: the one played last)
		const int offset = ArticTiles();
		if (m_libraryIndex < 0 && !m_shelf.empty())
			m_libraryIndex = offset; // the first game, unless the one asked for is found below
		m_libraryIndex = std::clamp(m_libraryIndex, 0, std::max(0, (int)m_shelf.size() + offset - 1));
		for (int i = 0; i < (int)m_shelf.size(); i++)
			if (m_games[m_shelf[i]].entry.game.titleId == m_libraryFocusTitle)
				m_libraryIndex = i + offset;
	}

	void Shell::LibraryUpdate(const ui::Press& press)
	{
		const Button b = press.button;
		{
			std::string kicker, title, text;
			std::vector<std::string> buttons;
			if (LibraryState(kicker, title, text, buttons))
			{
				LibraryStateUpdate(press);
				return;
			}
		}
		const int side = SideIndex();
		const int columns = LibraryColumns();
		const int offset = ArticTiles();
		const int total = (int)m_shelf.size() + offset;
		auto sortPicker = [this, side] {
			OpenPicker("Library", "Sort", std::vector<std::string>(std::begin(kSorts), std::end(kSorts)), m_settings.ui.librarySort[side],
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
			// the search let go, then up to the filters, then the bar: the Library is where a side opens
			if (!m_search.empty())
			{
				m_search.clear();
				LibraryRefresh();
				m_feedback.Play(ui::Cue::Back);
			}
			else if (m_libraryZone == 1)
			{
				m_libraryZone = 0;
				m_feedback.Play(ui::Cue::Back);
			}
			else
				FocusBar();
			return;
		}
		if (m_libraryZone == 0)
		{
			const int filters = LibraryFilters();
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
			if (at < ArticTiles())
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
		{
			std::string kicker, title, text;
			std::vector<std::string> buttons;
			if (LibraryState(kicker, title, text, buttons))
			{
				LibraryStateDraw(canvas);
				return;
			}
		}
		const int side = SideIndex();
		const bool wiiu = IsWiiU();
		const int offset = ArticTiles();
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
		const char* names[4] = {"All", "Recently added", "Favourites", "Graphic packs on"};
		const int filters = LibraryFilters();
		const int filter = m_settings.ui.libraryFilter[side];
		float x = kSafeX;
		const ui::TextStyle pillStyle = Style({26, ui::Weight::Medium, 1.0f});
		const ui::TextStyle countStyle = Style({20, ui::Weight::SemiBold, 1.0f, 0, false, true});
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
		if (!m_search.empty())
			Chip(canvas, x + 6, 143, "Search: " + m_search);
		if (m_scanning)
			canvas.Text(Style(kCaptionStyle), x + 12, 150, "Looking for new games…", Tertiary());
		// Sort and Search
		const ui::TextStyle action = Style({24, ui::Weight::Medium, 1.0f});
		const std::string sortLabel = std::string("Sort: ") + kSorts[std::clamp(m_settings.ui.librarySort[side], 0, 3)];
		float right = 1920 - kSafeX;
		const ui::TextBlock searchText = m_fonts.Layout(action, "Search");
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
				m_scanning ? "Looking for games…" : !m_search.empty() ? "Nothing matches “" + m_search + "”" : filter ? "None here yet" : "No games yet",
				kText);
			canvas.Text(Style(kBodyStyle), kSafeX, 390,
				!m_search.empty() ? "Circle clears the search." :
				filter == 2		  ? "Options on a game, then Favourite, puts it here." :
				filter			  ? "All shows every game." :
									"Settings > Game files says where they go.",
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
				canvas.Text(Style({26, ui::Weight::SemiBold, 1.2f}), box.x + 16, box.y + box.h * 0.22f, "Play from your 3DS", kText, box.w - 32, 2,
					ui::Align::Centre);
				canvas.Text(Style({19, ui::Weight::Regular, 1.3f}), box.x + 16, box.y + box.h * 0.6f, "Artic Base, over your network", Secondary(),
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
				canvas.Text(Style({26, ui::Weight::SemiBold, 1.2f}), box.x + 16, box.y + box.h * 0.22f, "Play from your 3DS", kText, box.w - 32, 2,
					ui::Align::Centre);
				canvas.Text(Style({19, ui::Weight::Regular, 1.3f}), box.x + 16, box.y + box.h * 0.6f, "Artic Base, over your network", Secondary(),
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
					fx += Chip(canvas, fx, under + name.height + 2, g.report->status, ps5compat::Kind(g.report->status), 34) + 10;
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
