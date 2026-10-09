// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: Home (docs/UI-REDESIGN.md, 6.1): back into a game in one press. The side's recent games
// in a row, the focused one larger at the left, then All games; under it the focused game's preview
// (its name, facts and status, Play, Game hub and the Game menu) and glance cards into its state.

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
		constexpr float kRowTop = 124, kGap = 26;
		constexpr float kPreviewTop = 452;
	}

	void Shell::WantBackdrop(Game* game)
	{
		// the game's own picture: a Wii U game's boot screen once read, else its box art
		const std::string path = game ? BackdropOf(*game) : std::string();
		if (path == m_backdropWanted && (!game || game->entry.ambient[0] == m_wantedAmbient[0]))
			return;
		if (path != m_backdropWanted)
			m_wantedAt = m_now;
		m_backdropWanted = path;
		m_wantedAmbient[0] = game ? game->entry.ambient[0] : 0;
		m_wantedAmbient[1] = game ? game->entry.ambient[1] : 0;
		if (game && path == m_backdrop)
		{
			// the picture's colours arrived after it: they glide in now
			for (int i = 0; i < 2; i++)
			{
				m_ambientFrom[i] = m_ambient[i];
				m_ambientTo[i] = m_wantedAmbient[i];
			}
			m_ambientMix.Snap(0);
			m_ambientMix.Go(1, m_now, kAmbientSeconds, ui::ease::InOut);
		}
	}

	int Shell::HomeGame() const
	{
		return m_homeIndex >= 0 && m_homeIndex < (int)m_homeGames.size() ? m_homeGames[m_homeIndex] : -1;
	}

	void Shell::HomeUpdate(const ui::Press& press)
	{
		const Button b = press.button;
		const int count = (int)m_homeGames.size() + 1; // and All games
		const int game = HomeGame();
		const bool empty = m_homeGames.empty() || !Notice().empty() || !CoreReady();
		if (empty)
		{
			// the state's own buttons only
			const int buttons = (int)m_homeButtons.size();
			if (b == Button::Up)
				FocusBar();
			else if ((b == Button::Left || b == Button::Right) && buttons > 0)
			{
				m_homeButton = (m_homeButton + (b == Button::Right ? 1 : buttons - 1)) % buttons;
				m_feedback.Play(ui::Cue::Focus);
			}
			else if (b == Button::Cross)
			{
				m_feedback.Play(ui::Cue::Select);
				if (!Notice().empty())
				{
					if (m_homeButton == 0)
						SetupOpen(false);
					else
						SettingsOpen("diagnostics");
				}
				else if (!CoreReady())
					m_feedback.Play(ui::Cue::Denied);
				else if (m_homeButton == 0)
				{
					m_pageFrom = ScreenId::Home;
					m_filesSide = m_side;
					FilesOpen(0);
				}
				else
					SetupOpen(false);
			}
			return;
		}
		m_homeIndex = std::clamp(m_homeIndex, 0, count - 1);
		switch (m_homeZone)
		{
		case 0: // the row
			switch (b)
			{
			case Button::Left:
			case Button::Right:
			{
				const int next = m_homeIndex + (b == Button::Right ? 1 : -1);
				if (next < 0 || next >= count)
					m_feedback.Play(ui::Cue::Edge, press.repeat);
				else
				{
					m_homeIndex = next;
					m_feedback.Play(ui::Cue::Focus);
				}
				break;
			}
			case Button::Up: FocusBar(); break;
			case Button::Down:
				m_homeZone = 1;
				m_homeButton = 0;
				m_feedback.Play(ui::Cue::Focus);
				break;
			case Button::Cross:
				if (game >= 0)
				{
					m_launch.from = m_homeIndex < (int)m_rowBoxes.size() ? m_rowBoxes[m_homeIndex] : Box{};
					Launch(game);
				}
				else
				{
					m_feedback.Play(ui::Cue::Select);
					ShowTab(ScreenId::Library);
				}
				break;
			case Button::Options:
				if (game >= 0)
					OpenGameMenu(game);
				break;
			default: break;
			}
			break;
		case 1: // the buttons
		{
			const int buttons = (int)m_homeButtons.size();
			switch (b)
			{
			case Button::Left:
			case Button::Right:
			{
				const int next = m_homeButton + (b == Button::Right ? 1 : -1);
				if (next < 0 || next >= buttons)
					m_feedback.Play(ui::Cue::Edge, press.repeat);
				else
				{
					m_homeButton = next;
					m_feedback.Play(ui::Cue::Focus);
				}
				break;
			}
			case Button::Up:
			case Button::Circle:
				m_homeZone = 0;
				m_feedback.Play(b == Button::Circle ? ui::Cue::Back : ui::Cue::Focus);
				break;
			case Button::Down:
				if (!m_cards.empty() && game >= 0)
				{
					m_homeZone = 2;
					m_homeCard = std::min(m_homeCard, (int)m_cards.size() - 1);
					m_feedback.Play(ui::Cue::Focus);
				}
				else
					m_feedback.Play(ui::Cue::Edge, press.repeat);
				break;
			case Button::Cross:
				if (game < 0)
				{
					m_feedback.Play(ui::Cue::Select);
					ShowTab(ScreenId::Library);
				}
				else if (m_homeButton == 0)
				{
					m_launch.from = m_homeIndex < (int)m_rowBoxes.size() ? m_rowBoxes[m_homeIndex] : Box{};
					Launch(game);
				}
				else if (m_homeButton == 1)
				{
					m_feedback.Play(ui::Cue::Select);
					HubOpen(game, ScreenId::Home);
				}
				else
					OpenGameMenu(game);
				break;
			case Button::Options:
				if (game >= 0)
					OpenGameMenu(game);
				break;
			default: break;
			}
			break;
		}
		case 2: // the cards
			switch (b)
			{
			case Button::Left:
			case Button::Right:
			{
				const int next = m_homeCard + (b == Button::Right ? 1 : -1);
				if (next < 0 || next >= (int)m_cards.size())
					m_feedback.Play(ui::Cue::Edge, press.repeat);
				else
				{
					m_homeCard = next;
					m_feedback.Play(ui::Cue::Focus);
				}
				break;
			}
			case Button::Up:
				m_homeZone = 1;
				m_feedback.Play(ui::Cue::Focus);
				break;
			case Button::Circle:
				m_homeZone = 0;
				m_feedback.Play(ui::Cue::Back);
				break;
			case Button::Cross:
			{
				if (game < 0 || m_homeCard >= (int)m_cards.size())
					break;
				m_feedback.Play(ui::Cue::Select);
				const std::string& action = m_cards[m_homeCard].action;
				if (action == "packs")
					PacksOpen(game, ScreenId::Home);
				else if (action == "controllers")
					SettingsOpen("wiiu-controllers", true);
				else if (action == "installs")
					SettingsOpen(Is3ds() ? "3ds-installs" : "wiiu-installs", true);
				else
					HubOpen(game, ScreenId::Home);
				break;
			}
			case Button::Options:
				if (game >= 0)
					OpenGameMenu(game);
				break;
			default: break;
			}
			break;
		}
	}

	void Shell::HomeDraw(Canvas& canvas)
	{
		DrawBar(canvas, true);
		const bool wiiu = !Is3ds();
		const ui::TextStyle display = Style(kDisplayStyle);
		const float previewWidth = 1000;
		m_homeButtons.clear();

		// the states before there is a game to show
		const bool notice = !Notice().empty();
		if (notice || !CoreReady() || m_homeGames.empty())
		{
			WantBackdrop(nullptr);
			canvas.PushAlpha(Enter(1));
			std::string kicker, title, text;
			std::vector<std::string> buttons;
			if (notice)
			{
				kicker = Tr("Setup");
				title = Tr("Something needs a look");
				text = Notice();
				buttons = {Tr("Setup check"), Tr("Diagnostics")};
			}
			else if (!CoreReady())
			{
				kicker = "Wii U";
				title = Tr("Starting Cemu");
				text = Tr("Its settings, graphic packs and controllers are loading, and then it looks for your games.");
			}
			else if (m_scanning)
			{
				kicker = Is3ds() ? "Nintendo 3DS" : "Wii U";
				title = Tr("Looking for games…");
				// tr: {0} is a folder
				text = TrF("In {0}.", ShortPath(GamesFolder(m_side), 60));
			}
			else
			{
				kicker = Tr("Welcome");
				title = Tr("Your games go here");
				// tr: {0} is the games folder
				text = Is3ds() ? TrF("Put your 3DS games (.3ds, .cci, .cxi, .3dsx, decrypted) in {0}, or choose any folder the PS5 can read, such as "
									 "one on a USB drive. Folders inside it are searched too.",
									 GamesFolder(m_side)) :
								 TrF("Put your Wii U games (.wua, .wud, .wux, or folders with code, content and meta) in {0}, or choose any folder "
									 "the PS5 can read, such as one on a USB drive.",
									 GamesFolder(m_side));
				buttons = {Tr("Choose a folder"), Tr("Setup check")};
			}
			const Box card{kSafeX, 300, 1100, 520};
			canvas.Text(Style(kOverlineStyle), card.x, card.y, kicker, Secondary());
			const ui::TextBlock titleBlock = canvas.Text(display, card.x, card.y + 36, title, kText, card.w, 2);
			const ui::TextBlock textBlock = canvas.Text(Style(kBodyStyle), card.x, card.y + 56 + titleBlock.height, text, Secondary(), 960, 5);
			float x = card.x;
			m_homeButton = std::clamp(m_homeButton, 0, std::max(0, (int)buttons.size() - 1));
			for (int i = 0; i < (int)buttons.size(); i++)
			{
				const Box b = PillButton(canvas, x, card.y + 96 + titleBlock.height + textBlock.height, buttons[i], i == 0,
					!m_onBar && m_homeButton == i);
				m_homeButtons.push_back(b);
				x += b.w + 20;
			}
			canvas.PopAlpha();
			return;
		}

		// the row: the focused cover larger, at the row's start, the others after it
		const int count = (int)m_homeGames.size() + 1;
		m_homeIndex = std::clamp(m_homeIndex, 0, count - 1);
		const float aspect = wiiu ? 0.714f : 1.1f;
		const float big = wiiu ? 280.0f : 236.0f, small = wiiu ? 182.0f : 150.0f;
		if ((int)m_rowX.size() != count)
		{
			m_rowX.assign(count, {});
			m_rowH.assign(count, {});
			for (int i = 0; i < count; i++)
			{
				m_rowX[i].Snap(-1e4f);
				m_rowH[i].Snap(small);
			}
		}
		std::vector<float> targetX(count), targetH(count);
		for (int i = 0; i < count; i++)
			targetH[i] = i == m_homeIndex ? big : small;
		targetX[m_homeIndex] = kSafeX;
		for (int i = m_homeIndex + 1; i < count; i++)
			targetX[i] = targetX[i - 1] + targetH[i - 1] * aspect + kGap;
		for (int i = m_homeIndex - 1; i >= 0; i--)
			targetX[i] = targetX[i + 1] - targetH[i] * aspect - kGap;
		const float omega = m_settings.ui.reduceMotion ? 60.0f : kScrollOmega;
		m_rowBoxes.assign(count, {});
		canvas.PushAlpha(Enter(1));
		canvas.PushOffset(0, (1 - Enter(1)) * 24);
		for (int i = 0; i < count; i++)
		{
			if (m_rowX[i].value < -9000)
				m_rowX[i].Snap(targetX[i]);
			m_rowX[i].target = targetX[i];
			m_rowH[i].target = targetH[i];
			m_rowX[i].omega = m_rowH[i].omega = omega;
			m_rowX[i].Update(m_dt);
			m_rowH[i].Update(m_dt);
			const Box box{m_rowX[i].value, kRowTop, m_rowH[i].value * aspect, m_rowH[i].value};
			m_rowBoxes[i] = box;
			if (box.Right() < -40 || box.x > 1960)
				continue;
			const bool focused = !m_onBar && m_homeZone == 0 && i == m_homeIndex;
			if (i < count - 1)
				DrawCover(canvas, m_games[m_homeGames[i]], box, focused, 0);
			else
			{
				// All games, to the Library
				canvas.Rect(box, kRadiusCard, 0x0affffff);
				canvas.Ring(box, kRadiusCard, 2, 0x26ffffff);
				canvas.Draw(Icon::ChevronRight, {box.CentreX() - 18, box.y + box.h * 0.28f, 36, 36}, Secondary());
				// as many lines as the tile has room for under its chevron
				const ui::TextStyle allStyle = Style({26, ui::Weight::Medium, 1.2f});
				const int lines = std::max(1, (int)((box.h * 0.46f - 8) / (allStyle.size * allStyle.lineHeight)));
				canvas.Text(allStyle, box.x + 10, box.y + box.h * 0.52f, TrP((long long)m_games.size(), "All {0} game", "All {0} games"), Secondary(),
					box.w - 20, lines, ui::Align::Centre);
				if (focused)
					Focus(box, kRadiusCard);
			}
		}
		canvas.PopOffset();
		canvas.PopAlpha();

		// the focused game's preview
		const int index = HomeGame();
		Game* game = index >= 0 ? &m_games[index] : nullptr;
		WantBackdrop(game);
		canvas.PushAlpha(Enter(2));
		canvas.PushOffset(0, (1 - Enter(2)) * 24);
		float y = kPreviewTop;
		if (game)
		{
			const float badge = Badge(canvas, kSafeX, y, m_side);
			const std::string when = LastPlayedWords(game->entry.lastPlayed);
			// tr: {0} says when: "today", "2 days ago"
			const std::string kicker = when.empty() ? std::string(Tr("In your library")) :
				m_homeIndex == 0				  ? TrF("Continue · last played {0}", when) :
													TrF("Last played {0}", when);
			canvas.Text(Style({18, ui::Weight::SemiBold, 1.9f, 3.5f, true}), kSafeX + badge + 18, y, kicker, Secondary(), previewWidth - badge - 18, 1);
			y += 52;
			ui::TextStyle title = display;
			title.size = m_fonts.Fit(display, game->entry.game.name, previewWidth, 2, display.size - 16, 8);
			const ui::TextBlock name = canvas.Text(title, kSafeX, y, game->entry.game.name, kText, previewWidth, 2);
			y += std::max(name.height, title.size * 1.04f) + 16;
			// its facts and status
			float x = kSafeX;
			std::string facts = Byline(*game);
			const std::string played = Played(*game);
			if (!played.empty())
				facts = Join({facts, played}, "  ·  ");
			const ui::TextStyle factStyle = Style({30, ui::Weight::Regular, 1.2f});
			const ui::TextBlock factBlock = canvas.Text(factStyle, x, y, facts, Secondary(), previewWidth - 240, 1);
			x += factBlock.width + 20;
			if (game->report)
				Chip(canvas, x, y + 2, TrC("status", game->report->status), ps5compat::Kind(game->report->status), 38);
		}
		else
		{
			canvas.Text(Style(kOverlineStyle), kSafeX, y, Tr("Your library"), Secondary(), previewWidth, 1);
			y += 52;
			canvas.Text(display, kSafeX, y, TrP((long long)m_games.size(), "All {0} game", "All {0} games"), kText, previewWidth, 1);
			y += display.size * 1.04f + 16;
			canvas.Text(Style({30, ui::Weight::Regular, 1.2f}), kSafeX, y, m_scanning ? Tr("Looking for new games…") : Tr("A to Z, by year, by how they run"),
				Secondary(), previewWidth, 1);
		}
		// the buttons
		const float buttonsTop = 724;
		const bool buttonsFocused = !m_onBar && m_homeZone == 1;
		if (game)
		{
			const int buttons = 3;
			m_homeButton = std::clamp(m_homeButton, 0, buttons - 1);
			Box b = PillButton(canvas, kSafeX, buttonsTop, Tr("Play"), true, buttonsFocused && m_homeButton == 0, Icon::Play, true, 72, 420);
			m_homeButtons.push_back(b);
			b = PillButton(canvas, b.Right() + 20, buttonsTop, Tr("Game hub"), false, buttonsFocused && m_homeButton == 1, Icon::Play, false, 72, 420);
			m_homeButtons.push_back(b);
			m_homeButtons.push_back(IconButton(canvas, b.Right() + 20, buttonsTop, Icon::More, buttonsFocused && m_homeButton == 2));
		}
		else
		{
			m_homeButton = 0;
			m_homeButtons.push_back(PillButton(canvas, kSafeX, buttonsTop, Tr("Open the library"), true, buttonsFocused, Icon::Play, false, 72, previewWidth));
		}
		canvas.PopOffset();
		canvas.PopAlpha();

		// the glance cards: shortcuts into the game's state, each only when it has something to say
		if (game && m_cardsFor != game->entry.game.titleId)
		{
			m_cardsFor = game->entry.game.titleId;
			m_cards.clear();
			const auto& g = game->entry.game;
			if (wiiu && m_status.coreReady)
			{
				const auto packs = ps5emu::ListGraphicPacks(g.titleId);
				int on = 0;
				std::string names;
				for (const auto& pack : packs)
					if (pack.enabled)
					{
						if (on < 3)
							names += (names.empty() ? "" : " · ") + pack.name;
						on++;
					}
				if (on > 3)
					names += fmt::format(" · +{}", on - 3);
				// tr: a graphic packs card: how many are on, and how many the game has
				if (!packs.empty())
					m_cards.push_back({Tr("Graphic packs"), on ? TrP(on, "{0} on", "{0} on") : std::string(Tr("None on")),
						on ? names : TrP((long long)packs.size(), "{0} pack for this game", "{0} packs for this game"), "packs"});
				const auto p1 = ps5emu::GetPlayerControls(0);
				std::string others;
				for (int player = 1; player < 4; player++)
				{
					const auto controls = ps5emu::GetPlayerControls(player);
					if (controls.connected)
						// tr: an emulated controller ({0}) on a player (P2: player 2)
						others += (others.empty() ? "" : " · ") + TrF("{0} on P{1}", TypeName(controls.type), player + 1);
				}
				m_cards.push_back({Tr("Controllers"), TrF("{0} on P{1}", TypeName(p1.type), 1), others.empty() ? std::string(Tr("One player")) : others,
					"controllers"});
			}
			if (game->report)
				m_cards.push_back({Tr("How it runs"), TrC("status", game->report->status),
					game->report->notes.empty() ? std::string(Tr("From the compatibility list")) : TrC("compatibility note", game->report->notes), "hub"});
			if (wiiu)
				m_cards.push_back({Tr("Updates and DLC"), Join({g.hasUpdate ? fmt::format("v{}", g.version) : std::string(Tr("No update")), g.dlcCount ? Tr("DLC") : ""}, " · "),
					g.format, "installs"});
			else
				m_cards.push_back({Tr("This dump"), g.format, g.publisher.empty() ? Hex(g.titleId) : g.publisher, "hub"});
		}
		if (game && !m_cards.empty())
		{
			canvas.PushAlpha(Enter(3));
			canvas.PushOffset(0, (1 - Enter(3)) * 24);
			const float width = 360, height = 128, top = 848;
			m_homeCard = std::clamp(m_homeCard, 0, (int)m_cards.size() - 1);
			for (int i = 0; i < (int)m_cards.size() && i < 4; i++)
			{
				const Box card{kSafeX + i * (width + 20), top, width, height};
				const bool focused = !m_onBar && m_homeZone == 2 && i == m_homeCard;
				Panel(canvas, card, kRadiusCard);
				if (focused)
				{
					canvas.Rect(card, kRadiusCard, 0x0dffffff);
					Focus(card, kRadiusCard);
				}
				canvas.Text(Style(kOverlineStyle), card.x + 26, card.y + 24, m_cards[i].title, Tertiary(), card.w - 52, 1);
				canvas.Text(Style({28, ui::Weight::SemiBold, 1.1f}), card.x + 26, card.y + 52, m_cards[i].value, kText, card.w - 52, 1);
				canvas.Text(Style({21, ui::Weight::Regular, 1.2f}), card.x + 26, card.y + 90, m_cards[i].detail, Secondary(), card.w - 52, 1);
			}
			canvas.PopOffset();
			canvas.PopAlpha();
		}
	}
}
