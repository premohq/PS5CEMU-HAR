// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: a game's hub (docs/UI-REDESIGN.md, 6.3): everything about one game. Its cover large,
// its system, IDs and title, chips for what is known for sure (status, version, DLC, format, play
// time), Play, its graphic packs and the Game menu; GameTDB's description and facts; how it runs.
// L1 and R1 go to the games either side of it, as the Library orders them.

#include "shell_internal.h"
#include "../actions.h"
#include "../../app/emulator.h"

#include <algorithm>
#include <cmath>

namespace ps5shell
{
	using namespace ui::tokens;
	using namespace ps5actions;

	namespace
	{
		constexpr float kAboutTop = 590, kAboutHeight = 250, kAboutWidth = 640;
	}

	void Shell::HubOpen(int game, ScreenId from)
	{
		if (game < 0 || game >= (int)m_games.size())
			return;
		m_hubGame = game;
		if (from != ScreenId::Hub)
			m_hubFrom = ScreenId::Library;
		m_hubAction = 0;
		m_hubAbout = false;
		m_hubScroll.Snap(0);
		m_onBar = false;
		Show(ScreenId::Hub);
		m_screenAt = m_now;
	}

	void Shell::HubUpdate(const ui::Press& press)
	{
		if (m_hubGame < 0 || m_hubGame >= (int)m_games.size())
		{
			ShowTab(m_hubFrom);
			return;
		}
		const Button b = press.button;
		// Play, Graphic packs (the Wii U's), Game settings, the Game menu
		const bool packs = IsWiiU() && m_status.coreReady;
		const int actions = packs ? 4 : 3;
		switch (b)
		{
		case Button::Circle:
			if (m_hubAbout)
			{
				m_hubAbout = false;
				m_feedback.Play(ui::Cue::Back);
				break;
			}
			m_feedback.Play(ui::Cue::Back);
			if (m_hubFrom == ScreenId::Library)
			{
				m_libraryFocusTitle = m_games[m_hubGame].entry.game.titleId;
				LibraryRefresh();
			}
			ShowTab(m_hubFrom);
			break;
		case Button::Left:
		case Button::Right:
		{
			if (m_hubAbout)
				break;
			const int next = m_hubAction + (b == Button::Right ? 1 : -1);
			if (next < 0 || next >= actions)
				m_feedback.Play(ui::Cue::Edge, press.repeat);
			else
			{
				m_hubAction = next;
				m_feedback.Play(ui::Cue::Focus);
			}
			break;
		}
		case Button::Down:
			if (!m_hubAbout)
			{
				if (m_hubAboutHeight > kAboutHeight)
				{
					m_hubAbout = true;
					m_feedback.Play(ui::Cue::Focus);
				}
				else
					m_feedback.Play(ui::Cue::Edge, press.repeat);
				break;
			}
			m_hubScroll.target = std::min(m_hubScroll.target + 108, std::max(0.0f, m_hubAboutHeight - kAboutHeight));
			break;
		case Button::Up:
			if (m_hubAbout)
			{
				if (m_hubScroll.target <= 0)
				{
					m_hubAbout = false;
					m_feedback.Play(ui::Cue::Focus);
				}
				else
					m_hubScroll.target = std::max(0.0f, m_hubScroll.target - 108);
			}
			else
				m_feedback.Play(ui::Cue::Edge, press.repeat);
			break;
		case Button::L1:
		case Button::R1:
		{
			// the game before or after, as the Library has them
			std::vector<int> order = m_shelf;
			if (order.empty())
				for (int i = 0; i < (int)m_games.size(); i++)
					order.push_back(i);
			auto it = std::find(order.begin(), order.end(), m_hubGame);
			int at = it == order.end() ? 0 : (int)(it - order.begin());
			at = (at + (b == Button::R1 ? 1 : (int)order.size() - 1)) % (int)order.size();
			HubOpen(order[at], m_hubFrom);
			m_feedback.Play(ui::Cue::Focus);
			break;
		}
		case Button::Cross:
			if (m_hubAbout)
			{
				m_hubAbout = false;
				m_feedback.Play(ui::Cue::Focus);
			}
			else if (m_hubAction == 0)
				Launch(m_hubGame);
			else if (packs && m_hubAction == 1)
			{
				m_feedback.Play(ui::Cue::Select);
				PacksOpen(m_hubGame, ScreenId::Hub);
			}
			else if (m_hubAction == actions - 2)
			{
				m_feedback.Play(ui::Cue::Select);
				GameSettingsOpen(m_hubGame);
			}
			else
				OpenGameMenu(m_hubGame);
			break;
		case Button::Options: OpenGameMenu(m_hubGame); break;
		default: break;
		}
	}

	void Shell::HubDraw(Canvas& canvas)
	{
		if (m_hubGame < 0 || m_hubGame >= (int)m_games.size())
			return;
		Game& game = m_games[m_hubGame];
		const auto& g = game.entry.game;
		WantBackdrop(&game);

		// the games either side, named at the top
		std::vector<int> order = m_shelf;
		if (order.empty())
			for (int i = 0; i < (int)m_games.size(); i++)
				order.push_back(i);
		if (order.size() > 1)
		{
			auto it = std::find(order.begin(), order.end(), m_hubGame);
			const int at = it == order.end() ? 0 : (int)(it - order.begin());
			const Game& before = m_games[order[(at + (int)order.size() - 1) % order.size()]];
			const Game& after = m_games[order[(at + 1) % order.size()]];
			canvas.PushAlpha(Enter(0));
			const ui::TextStyle side = Style({24, ui::Weight::Medium, 1.0f});
			canvas.Draw(Icon::L1, {kSafeX, 52, 34, 30}, Tertiary());
			canvas.Text(side, kSafeX + 46, 54, before.entry.game.name, Tertiary(), 560, 1);
			canvas.Draw(Icon::R1, {1920 - kSafeX - 34, 52, 34, 30}, Tertiary());
			const ui::TextBlock afterName = m_fonts.Layout(side, after.entry.game.name, 560, 1);
			canvas.Text(afterName, 1920 - kSafeX - 46 - afterName.width, 54, Tertiary());
			canvas.PopAlpha();
		}

		// the cover
		const bool wiiu = IsWiiU();
		const Box cover{kSafeX, 150, 420, wiiu ? 588.0f : 382.0f};
		canvas.PushAlpha(Enter(1));
		canvas.PushOffset((1 - Enter(1)) * -30, 0);
		DrawCover(canvas, game, cover, false, 0);
		canvas.PopOffset();
		canvas.PopAlpha();

		// its kicker, title and chips
		const float x = 596;
		canvas.PushAlpha(Enter(2));
		canvas.PushOffset(0, (1 - Enter(2)) * 24);
		float kx = x + Badge(canvas, x, 150, m_side) + 18;
		// a DS game has no title ID of its own (the one it is given stays out of sight)
		const std::string kicker = Join({g.gameId, game.known ? game.info.region : "", IsDs() ? "" : Hex(g.titleId)}, " · ");
		canvas.Text(Style({18, ui::Weight::SemiBold, 1.9f, 3.5f, true}), kx, 150, kicker, Secondary(), 1920 - kSafeX - kx, 1);
		ui::TextStyle display = Style(kDisplayStyle);
		display.size = m_fonts.Fit(display, g.name, 1228, 2, display.size - 16, 8);
		const ui::TextBlock title = canvas.Text(display, x, 196, g.name, kText, 1228, 2);
		float cy = 196 + std::max(title.height, display.size * 1.04f) + 18;
		float cx = x;
		if (game.report)
			cx += Chip(canvas, cx, cy, game.report->status, ps5compat::Kind(game.report->status)) + 10;
		if (wiiu)
		{
			cx += Chip(canvas, cx, cy, g.hasUpdate ? fmt::format("Update v{}", g.version) : fmt::format("v{}", g.version)) + 10;
			if (g.dlcCount)
				cx += Chip(canvas, cx, cy, "DLC") + 10;
		}
		else if (!g.publisher.empty() && !game.known)
			cx += Chip(canvas, cx, cy, g.publisher) + 10;
		if (!g.format.empty())
			cx += Chip(canvas, cx, cy, g.format) + 10;
		if (!Played(game).empty())
			cx += Chip(canvas, cx, cy, Played(game)) + 10;
		if (game.entry.favourite)
			Chip(canvas, cx, cy, "Favourite");

		// the actions
		const bool packs = wiiu && m_status.coreReady;
		const int actions = packs ? 4 : 3;
		m_hubAction = std::clamp(m_hubAction, 0, actions - 1);
		const float ay = cy + 72;
		Box b = PillButton(canvas, x, ay, "Play", true, !m_hubAbout && m_hubAction == 0, Icon::Play, true);
		float bx = b.Right() + 20;
		if (packs)
		{
			if (game.packsOn < 0)
				game.packsOn = ps5emu::EnabledGraphicPackCount(g.titleId);
			b = PillButton(canvas, bx, ay, game.packsOn ? fmt::format("Graphic packs · {} on", game.packsOn) : "Graphic packs", false,
				!m_hubAbout && m_hubAction == 1);
			bx = b.Right() + 20;
		}
		const bool own = m_settings.games.count(ps5settings::GameKey(ps5launcher::SideName(m_side), g.titleId)) > 0;
		b = PillButton(canvas, bx, ay, own ? "Game settings · its own" : "Game settings", false, !m_hubAbout && m_hubAction == actions - 2);
		bx = b.Right() + 20;
		IconButton(canvas, bx, ay, Icon::More, !m_hubAbout && m_hubAction == actions - 1);
		canvas.PopOffset();
		canvas.PopAlpha();

		// About: GameTDB's description, which Up and Down scroll
		canvas.PushAlpha(Enter(3));
		canvas.PushOffset(0, (1 - Enter(3)) * 24);
		const float aboutTop = std::max(kAboutTop, ay + 72 + 52);
		canvas.Text(Style(kOverlineStyle), x, aboutTop - 34, "About", Secondary());
		std::string synopsis = game.known ? game.info.synopsis : std::string();
		if (synopsis.empty())
			synopsis = g.gameId.empty() ? "GameTDB knows a game by the ID on its box, which this dump does not have." :
										  "GameTDB has no description of this game yet.";
		const ui::TextStyle body = Style({26, ui::Weight::Regular, 1.46f});
		const ui::TextBlock about = m_fonts.Layout(body, synopsis, kAboutWidth);
		m_hubAboutHeight = about.height;
		const float aboutHeight = std::min(kAboutHeight, 1080 - 120 - aboutTop);
		m_hubScroll.omega = kScrollOmega;
		m_hubScroll.Update(m_dt);
		const Box aboutBox{x - 16, aboutTop - 8, kAboutWidth + 32, aboutHeight + 16};
		canvas.PushClip(aboutBox);
		canvas.Text(about, x, aboutTop - m_hubScroll.value, Secondary());
		canvas.PopClip();
		if (m_hubAbout)
			Focus(aboutBox, 16);

		// the facts, and how it runs
		const float fx = 1284;
		std::vector<std::pair<const char*, std::string>> facts;
		if (game.known)
		{
			facts = {{"Developer", game.info.developer}, {"Released", ps5gameinfo::ReleaseDate(game.info.released)},
				{"Genre", ps5gameinfo::Genres(game.info.genre, 2)}, {"Rating", game.info.rating}, {"Publisher", game.info.publisher},
				{"Players", game.info.players > 0 ? std::to_string(game.info.players) : ""}};
			std::erase_if(facts, [](const auto& fact) { return fact.second.empty(); });
		}
		if (facts.empty() && !IsDs())
			facts.push_back({"Title ID", Hex(g.titleId)});
		for (int i = 0; i < (int)facts.size() && i < 4; i++)
		{
			const float fy = aboutTop - 32 + (i / 2) * 82;
			const float fxi = fx + (i % 2) * 284;
			canvas.Text(Style({20, ui::Weight::Medium, 1.2f}), fxi, fy, facts[i].first, Tertiary());
			canvas.Text(Style({26, ui::Weight::Medium, 1.2f}), fxi, fy + 28, facts[i].second, kText, 270, 1);
		}
		if (game.report)
		{
			const std::string kind = ps5compat::Kind(game.report->status);
			const uint32_t colour = kind == "good" ? kGood : kind == "warn" ? kWarn : kind == "bad" ? kBad : kText;
			const Box card{fx, aboutTop + 154, 540, 172};
			canvas.Rect(card, kRadiusCard, ui::SetAlpha(colour, 0.07f));
			canvas.Ring(card, kRadiusCard, 1.5f, ui::SetAlpha(colour, 0.25f));
			canvas.Draw(kind == "good" ? Icon::Check : Icon::Warning, {card.x + 24, card.y + 22, 28, 28}, colour);
			const std::string heading = kind == "good" ? "Runs well on PS5" : game.report->status;
			canvas.Text(Style({26, ui::Weight::SemiBold, 1.2f}), card.x + 62, card.y + 22, heading, ui::Mix(colour, 0xffffffff, 0.3f), card.w - 86, 1);
			const std::string notes = game.report->notes.empty() ? "The compatibility list has it as " + Lower(game.report->status) + "." :
																   game.report->notes;
			canvas.Text(Style({21, ui::Weight::Regular, 1.4f}), card.x + 24, card.y + 66, notes, Secondary(), card.w - 48, 3);
		}
		canvas.PopOffset();
		canvas.PopAlpha();
	}
}
