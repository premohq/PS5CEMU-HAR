// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the new launcher (shell.h): its loop, the sides and their games, the bar along the top,
// the hints, the backdrop, the overlays and the launch. The screens are in their own files
// (shell_internal.h).

#include "shell_internal.h"
#include "../actions.h"
#include "../sound.h"
#include "../../app/boxart.h"
#include "../../app/pack_updates.h"
#include "../../app/paths.h"
#include "../../app/updates.h"
#include "../../azahar/azahar.h"
#include "../../azahar/library.h"
#include "../../ps5/kernel.h"
#include "../../ps5/log.h"
#include "../../ps5/notify.h"
#include "../../ps5/pad.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ps5shell
{
	using namespace ui::tokens;
	using namespace ps5actions;

	namespace
	{
		Host s_host;
		bool s_hostSet = false;

		const Host& TheHost()
		{
			if (!s_hostSet)
			{
				s_host = DefaultHost();
				s_hostSet = true;
			}
			return s_host;
		}

		const char* SideName(System side)
		{
			return side == System::N3ds ? "3ds" : "wiiu";
		}

		// the keyboard's keys (Search)
		const char* const kKeyRows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL'", "ZXCVBNM-:.", nullptr};
		constexpr int kKeyboardRows = 5; // the last: space, delete, done
	}

	void SetHost(Host host)
	{
		s_host = std::move(host);
		s_hostSet = true;
	}

	Outcome Run(ps5settings::Launcher& settings, ps5launcher::Status& status, const std::function<void(System)>& prepare,
		std::optional<ps5launcher::Choice>& choice)
	{
		Shell shell(settings, status, prepare);
		return shell.Run(choice);
	}

	Shell::Shell(ps5settings::Launcher& settings, ps5launcher::Status& status, const std::function<void(System)>& prepare)
		: m_settings(settings), m_status(status), m_prepare(prepare)
	{
	}

	Shell::~Shell()
	{
		if (m_images)
			m_images->Stop();
		m_gfx.Stop();
	}

	// -- the loop -----------------------------------------------------------------------------------

	Outcome Shell::Run(std::optional<ps5launcher::Choice>& choice)
	{
		const Host& host = TheHost();
		ui::SetLog([](const std::string& line) { ps5log::Write(line); });
		std::string error;
		const double started = host.clock();
		m_fonts.SetLanguage(ps5lang::Current()); // before the atlas: a CJK language's has more room
		if (!m_fonts.Load(host.assets + "/fonts/lexend.sdf", error))
		{
			ps5log::Line("[ui] {}", error);
			return Outcome::Classic;
		}
		m_fonts.SetFallbackFolders(host.fontFolders);
		ui::Target target = host.target();
		const auto createSurface = target.createSurface;
		if (createSurface)
			target.createSurface = [this, createSurface](VkInstance instance, std::string& why) {
				m_videoOut = true;
				return createSurface(instance, why);
			};
		if (!m_gfx.Start(host.getInstanceProcAddr, target, error))
		{
			ps5log::Line("[ui] the new launcher cannot draw: {}", error);
			return m_videoOut ? Outcome::Restart : Outcome::Classic;
		}
		m_gfx.SetAtlas(m_fonts.AtlasWidth(), m_fonts.AtlasHeight(), m_fonts.Atlas());
		m_atlasHeight = m_fonts.AtlasHeight();
		uint32_t first, rows;
		m_fonts.TakeChanged(first, rows);
		m_images = std::make_unique<ui::Images>(m_gfx);
		ps5sound::Start(m_settings.music, m_settings.musicVolume, m_settings.menuSounds);
		ps5boxart::SetEnabled(m_settings.boxArt);
		ps5catalog::Load();
		m_start = m_now = m_lastFrame = host.clock();
		m_sideMix.Snap(1); // the backdrop shows from the first frame; switching sides fades it in again

		// the bubbles, everywhere at first
		uint32_t seed = 0x5735;
		auto random = [&seed] {
			seed ^= seed << 13;
			seed ^= seed >> 17;
			seed ^= seed << 5;
			return (seed >> 8) / 16777216.0f;
		};
		for (int i = 0; i < 90; i++)
		{
			const float radius = 3 + random() * 34;
			m_bubbles.push_back({random() * 1920, random() * 1080, radius, 14 + 38 * (0.5f * random() + 0.5f * radius / 37), (random() * 2 - 1) * 7,
				0.04f + random() * 0.12f});
		}

		// the side: the one a game came back from, else the one last used, else (a first start) the one
		// with games, else the Wii U's. Nothing asks which side (5.3), unless Start on: Ask each time is on.
		const bool returning = !m_settings.side.empty();
		std::string side = returning ? m_settings.side : m_settings.ui.lastSide;
		if (returning)
		{
			m_settings.side.clear(); // a fresh start next time
			SaveSettings();
		}
		if (side.empty())
		{
			const int wiiuGames = std::max<int>(m_settings.gameCount, (int)ps5catalog::Games(ps5catalog::System::WiiU).size());
			const int n3dsGames = std::max<int>(m_settings.n3ds.gameCount, (int)ps5catalog::Games(ps5catalog::System::N3ds).size());
			side = n3dsGames > 0 && wiiuGames <= 0 ? "3ds" : "wiiu";
		}
		// why the last game did not start (main_ps5.cpp's): said once here, not a state that keeps every
		// game from starting
		std::string launchError;
		for (std::string* error : {&m_status.launchError, &m_status.launchError3ds})
			if (!error->empty())
			{
				launchError = *error;
				error->clear();
			}
		m_askSide = !returning && m_settings.ui.startOn == "ask";
		m_side = side == "3ds" ? System::N3ds : System::WiiU;
		m_chooserSide = m_side == System::N3ds ? 1 : 0;
		if (!m_settings.ui.setupDone || SetupNeeded())
			SetupOpen(true); // then the side (FinishStart)
		else if (m_askSide)
			Show(ScreenId::Chooser);
		else
		{
			OpenSide(m_side, true);
			if (returning)
			{
				// back on the game just played, its hub, as the library was left (6.8)
				const uint64_t played = Is3ds() ? m_settings.n3ds.lastGame : m_settings.lastGame;
				const int game = played ? FindGame(played) : -1;
				if (game >= 0)
				{
					for (int i = 0; i < (int)m_homeGames.size(); i++)
						if (m_homeGames[i] == game)
							m_homeIndex = i;
					HubOpen(game, ScreenId::Home);
				}
				Toast(Tr("Saved your place in the library"));
			}
		}
		if (!launchError.empty())
			OpenHelp(Tr("The game could not start"), launchError);
		ps5log::Line("[ui] the new launcher is up in {:.0f} ms", (host.clock() - started) * 1000);

		while (!m_done && !m_failed)
			Frame();
		if (m_failed)
		{
			m_images->Stop();
			ps5sound::Stop();
			m_gfx.Stop();
			return Outcome::Restart;
		}
		// the background work stops behind the launch (4.2, rule 4), the screen kept drawn
		ps5boxart::Stop();
		ps5packs::Stop();
		for (int waited = 0; SystemScanning() && !m_failed; waited++)
		{
			if (waited == 0)
				ps5log::Line("[launcher] waiting for the library's scan to finish");
			Frame();
		}
		ps5catalog::Save();
		Frame();
		// the in-game menus' font for what Lexend lacks: the console's, for this language's words and this
		// game's name (docs/UI-REDESIGN.md, 7.7); none for a Latin language and title
		if (m_choice)
		{
			const ui::Fonts::FontFile font = m_fonts.FileFor(ps5lang::MenuCharacters() + ui::Decode(m_choice->game.name));
			ps5lang::SetMenuFont({font.path, font.face});
			if (!font.path.empty())
				ps5log::Line("[ui] the in-game menu's second font: {} (face {})", font.path, font.face);
		}
		ps5sound::Stop();
		m_feedback.Stop();
		m_images->Stop();
		const uint64_t memory = m_gfx.MemoryBytes();
		m_gfx.Stop();
		ps5log::Line("[ui] the new launcher is gone ({} frames, {} MiB of GPU memory freed)", m_frames, memory >> 20);
		choice = m_choice;
		return Outcome::Chosen;
	}

	void Shell::Frame()
	{
		const Host& host = TheHost();
		m_now = host.clock();
		m_dt = (float)std::clamp(m_now - m_lastFrame, 0.0, 0.05);
		const double interval = m_now - m_lastFrame;
		m_lastFrame = m_now;
		m_iconLookedThisFrame = false;
		if (++m_frames % 120 == 0)
			ps5pad::Rescan(); // controllers joining or leaving, about every two seconds

		ui::Actions actions = m_input.Poll(m_now);
		if (m_capture.active)
		{
			PollCapture();
			actions = {};
			if (!m_capture.active)
				m_input.Reset(); // what was pressed for the mapping is not the menu's
		}
		m_feedback.Update(m_now);
		Update(actions);
		PollGames();
		PollPacks();
		if (m_installing)
			PollInstall();
		const bool prompting = ps5update::Prompting();
		if (prompting != m_update.open)
		{
			m_update.open = prompting;
			m_update.at = m_now;
			m_update.choice = 0;
		}

		m_images->SetUploads(!m_launch.active);
		m_images->Update();
		m_list.Clear();
		Canvas canvas(m_list, m_fonts);
		Draw(canvas);
		// the glyphs this frame rendered, uploaded with it; the atlas made again when they made it grow
		if (m_fonts.AtlasHeight() != m_atlasHeight)
		{
			m_atlasHeight = m_fonts.AtlasHeight();
			m_gfx.SetAtlas(m_fonts.AtlasWidth(), m_fonts.AtlasHeight(), m_fonts.Atlas());
		}
		uint32_t first, rows;
		if (m_fonts.TakeChanged(first, rows))
			m_gfx.AtlasChanged(first, rows);
		if (!m_gfx.Frame(m_list, (float)(m_now - m_start)))
		{
			ps5log::Line("[ui] the GPU stopped drawing the launcher");
			m_failed = true;
		}
		if (m_frames == 1)
		{
			sceSystemServiceHideSplashScreen();
			ps5log::Line("[ui] first frame: {} instances, {} draws", m_list.instances.size(), m_list.runs.size());
		}
		// the tour log's line (docs/UI-REDESIGN.md, 9.9), every 600 frames
		static double s_worst = 0, s_sum = 0;
		static int s_count = 0;
		if (m_frames > 1)
		{
			s_worst = std::max(s_worst, interval);
			s_sum += interval;
			s_count++;
		}
		if (s_count == 600)
		{
			ps5log::Line("[ui] frames={} avg={:.2f} ms max={:.2f} ms draws={} instances={} gpu={} MiB", m_frames, s_sum / s_count * 1000,
				s_worst * 1000, m_list.runs.size(), m_list.instances.size(), m_gfx.MemoryBytes() >> 20);
			s_worst = s_sum = 0;
			s_count = 0;
		}
		if (host.afterFrame)
			host.afterFrame();
	}

	void Shell::Update(const ui::Actions& actions)
	{
		if (m_launch.active)
		{
			if (m_now - m_launch.at > kLaunchSeconds + 0.25)
				m_done = true;
			return;
		}
		for (const ui::Press& press : actions.presses)
		{
			if (m_launch.active || m_capture.active)
				break;
			if (OverlayUpdate(press))
				continue;
			const bool tabbed = m_screen == ScreenId::Home || m_screen == ScreenId::Library || m_screen == ScreenId::Settings;
			if (tabbed && BarUpdate(press))
				continue;
			switch (m_screen)
			{
			case ScreenId::Chooser: ChooserUpdate(press); break;
			case ScreenId::Setup: SetupUpdate(press); break;
			case ScreenId::Home: HomeUpdate(press); break;
			case ScreenId::Library: LibraryUpdate(press); break;
			case ScreenId::Hub: HubUpdate(press); break;
			case ScreenId::Settings: SettingsUpdate(press, actions); break;
			case ScreenId::Packs: PacksUpdate(press); break;
			case ScreenId::Player: PlayerUpdate(press, actions); break;
			case ScreenId::Mapping: MappingUpdate(press); break;
			case ScreenId::Files: FilesUpdate(press); break;
			case ScreenId::Artic: ArticUpdate(press, actions); break;
			}
		}
		// what is held to confirm goes on without presses
		if (actions.presses.empty() && !m_picker.open && !m_help.open && !m_menu.open && !m_keyboard.open && !m_update.open)
		{
			const ui::Press none{Button::Count, false};
			if (m_screen == ScreenId::Settings)
				SettingsUpdate(none, actions);
			else if (m_screen == ScreenId::Player)
				PlayerUpdate(none, actions);
			else if (m_screen == ScreenId::Artic)
				ArticUpdate(none, actions);
		}
	}

	void Shell::Draw(Canvas& canvas)
	{
		DrawBackdrop(canvas);
		const float appear = m_launch.active ? 1.0f - ui::ease::CubicOut((float)((m_now - m_launch.at) / kLaunchSeconds)) * 0.85f : 1.0f;
		canvas.PushAlpha(appear);
		switch (m_screen)
		{
		case ScreenId::Chooser: ChooserDraw(canvas); break;
		case ScreenId::Setup: SetupDraw(canvas); break;
		case ScreenId::Home: HomeDraw(canvas); break;
		case ScreenId::Library: LibraryDraw(canvas); break;
		case ScreenId::Hub: HubDraw(canvas); break;
		case ScreenId::Settings: SettingsDraw(canvas); break;
		case ScreenId::Packs: PacksDraw(canvas); break;
		case ScreenId::Player: PlayerDraw(canvas); break;
		case ScreenId::Mapping: MappingDraw(canvas); break;
		case ScreenId::Files: FilesDraw(canvas); break;
		case ScreenId::Artic: ArticDraw(canvas); break;
		}
		const bool overlay = m_picker.open || m_help.open || m_menu.open || m_keyboard.open || m_update.open;
		if (overlay)
			m_ringAsked = false; // the overlay's own focus is drawn with it
		if (!m_launch.active)
			DrawFocusRing(canvas);
		if (!m_launch.active)
			DrawHints(canvas, Hints());
		canvas.PopAlpha();
		DrawOverlays(canvas);
	}

	// -- sides and games ------------------------------------------------------------------------

	bool Shell::SystemScanning() const
	{
		return Is3ds() ? ps5azahar::Scanning() : m_status.coreReady && ps5emu::Scanning();
	}

	void Shell::OpenSide(System side, bool first)
	{
		if (m_prepared != side)
		{
			if (m_prepared)
			{
				// the side being left finishes looking for games first
				const System leaving = m_side;
				m_side = *m_prepared;
				while (SystemScanning() && !m_failed)
					Frame();
				m_side = leaving;
			}
			m_side = side;
			m_games.clear();
			if (!first || m_frames > 0)
			{
				// what is happening shows while the side starts (Cemu takes a few seconds)
				Show(ScreenId::Home);
				Frame();
			}
			m_prepare(side);
			m_prepared = side;
		}
		const bool changed = m_side != side || first;
		m_side = side;
		m_settings.ui.lastSide = SideName(side);
		SaveSettings();
		LoadGames();
		m_feedback.SetLight(Accent());
		if (changed)
			m_sideMix.Go(1, m_now, m_settings.ui.reduceMotion ? kReducedSeconds : kAmbientSeconds), m_sideMix.from = 0;
		m_homeZone = 0;
		m_homeIndex = 0;
		m_libraryIndex = 0;
		m_rowX.clear();
		m_rowH.clear();
		LibraryRefresh();
		m_tab = ScreenId::Home;
		Show(ScreenId::Home);
	}

	void Shell::SwitchSide()
	{
		m_feedback.Play(ui::Cue::Select);
		// where the side being left was, for coming back to it
		SideFocus& leaving = m_sideFocus[Is3ds() ? 1 : 0];
		leaving.homeTitle = HomeGame() >= 0 ? m_games[HomeGame()].entry.game.titleId : 0;
		leaving.libraryTitle = LibraryGame() >= 0 ? m_games[LibraryGame()].entry.game.titleId : 0;
		const System other = Is3ds() ? System::WiiU : System::N3ds;
		const ScreenId tab = m_screen == ScreenId::Library ? ScreenId::Library : ScreenId::Home;
		OpenSide(other, false);
		// the other side as it was left: the same games in focus
		const SideFocus& back = m_sideFocus[Is3ds() ? 1 : 0];
		for (int i = 0; i < (int)m_homeGames.size(); i++)
			if (back.homeTitle && m_games[m_homeGames[i]].entry.game.titleId == back.homeTitle)
				m_homeIndex = i;
		m_libraryIndex = -1; // no game in focus, so the refresh finds the remembered one
		m_libraryFocusTitle = back.libraryTitle;
		LibraryRefresh();
		ShowTab(tab);
		// the focus on the side's games: left on the bar's switch, nothing on the screen had it, and
		// Cross there switched straight back
		m_onBar = false;
		m_barFocus = tab == ScreenId::Library ? 2 : 1;
	}

	void Shell::MakeGame(Game& game, const ps5catalog::Entry& entry)
	{
		game.entry = entry;
		game.boxArt = ps5boxart::Path(BoxSide(m_side), entry.game.gameId);
		game.report = ps5compat::Find(Is3ds(), entry.game.name);
		game.known = ps5gameinfo::Find(BoxSide(m_side), entry.game.gameId, game.info);
		game.year = game.known && game.info.released.size() >= 4 ? std::atoi(game.info.released.substr(0, 4).c_str()) : 0;
		std::string name = Lower(entry.game.name);
		if (name.rfind("the ", 0) == 0)
			name = name.substr(4);
		game.sortName = name;
	}

	void Shell::LoadGames()
	{
		m_scanning = CoreReady() && SystemScanning();
		if (CoreReady() && !m_scanning)
		{
			ps5catalog::Replace(CatalogSide(), Is3ds() ? ps5azahar::ListGames() : ps5emu::ListGames());
			ps5catalog::Migrate(CatalogSide(), Is3ds() ? m_settings.n3ds.recent : m_settings.recent);
			ps5catalog::Save();
		}
		// what is on screen keeps its place
		const uint64_t focused = HomeGame() >= 0 ? m_games[HomeGame()].entry.game.titleId : 0;
		m_games.clear();
		for (const ps5catalog::Entry& entry : ps5catalog::Games(CatalogSide()))
		{
			m_games.emplace_back();
			MakeGame(m_games.back(), entry);
		}
		m_boxArrivals = ps5boxart::Arrivals();
		if (!m_scanning && CoreReady())
		{
			ps5log::Line("[launcher] {} games", m_games.size());
			FetchBoxArt();
			RememberCount();
		}
		m_homeGames = RecentGames();
		if (focused)
			for (int i = 0; i < (int)m_homeGames.size(); i++)
				if (m_games[m_homeGames[i]].entry.game.titleId == focused)
					m_homeIndex = i;
		LibraryRefresh();
	}

	void Shell::PollGames()
	{
		if (m_scanning && !SystemScanning())
			LoadGames();
		if (ps5boxart::Arrivals() != m_boxArrivals)
		{
			m_boxArrivals = ps5boxart::Arrivals();
			for (Game& game : m_games)
				if (game.boxArt.empty())
					game.boxArt = ps5boxart::Path(BoxSide(m_side), game.entry.game.gameId);
		}
	}

	std::string Shell::IconOf(Game& game)
	{
		if (!game.iconLooked && !m_iconLookedThisFrame)
		{
			// reading a Wii U game's icon mounts the game: one a frame
			game.iconLooked = true;
			m_iconLookedThisFrame = true;
			game.icon = Is3ds() ? ps5azahar::CoverPath(game.entry.game.titleId) :
				m_status.coreReady ? ps5emu::CoverPath(game.entry.game.titleId) : std::string();
		}
		return game.icon;
	}

	std::string Shell::CoverOf(Game& game)
	{
		return !game.boxArt.empty() ? game.boxArt : IconOf(game);
	}

	std::string Shell::BackdropOf(Game& game)
	{
		if (!Is3ds() && !game.bootScreenLooked && !m_iconLookedThisFrame && m_status.coreReady)
		{
			game.bootScreenLooked = true;
			m_iconLookedThisFrame = true;
			game.bootScreen = ps5emu::BootScreenPath(game.entry.game.titleId);
		}
		return !game.bootScreen.empty() ? game.bootScreen : game.boxArt;
	}

	const ui::Picture& Shell::Cover(Game& game, bool blurred)
	{
		const ui::Picture& picture = m_images->Get(game.boxArt, blurred);
		if (picture.ambient[0] && picture.ambient[0] != game.entry.ambient[0])
		{
			game.entry.ambient[0] = picture.ambient[0];
			game.entry.ambient[1] = picture.ambient[1];
			ps5catalog::SetAmbient(CatalogSide(), game.entry.game.titleId, picture.ambient);
		}
		return picture;
	}

	std::vector<int> Shell::RecentGames() const
	{
		std::vector<int> order;
		for (int i = 0; i < (int)m_games.size(); i++)
			if (m_games[i].entry.lastPlayed > 0)
				order.push_back(i);
		std::sort(order.begin(), order.end(), [this](int a, int b) { return m_games[a].entry.lastPlayed > m_games[b].entry.lastPlayed; });
		// none played yet: the library's first games
		for (int i = 0; i < (int)m_games.size() && order.size() < 12; i++)
			if (std::find(order.begin(), order.end(), i) == order.end() && order.size() < 4)
				order.push_back(i);
		if (order.size() > 12)
			order.resize(12);
		return order;
	}

	int Shell::FindGame(uint64_t titleId) const
	{
		for (int i = 0; i < (int)m_games.size(); i++)
			if (m_games[i].entry.game.titleId == titleId)
				return i;
		return -1;
	}

	std::string Shell::Byline(const Game& game) const
	{
		const auto& g = game.entry.game;
		if (game.known && (!game.info.publisher.empty() || !game.info.released.empty()))
			return Join({game.info.publisher, ps5gameinfo::Year(game.info.released)}, "  ·  ");
		if (Is3ds())
			return Join({g.publisher, g.format}, "  ·  ");
		return Join({fmt::format("v{}", g.version), g.dlcCount ? Tr("DLC") : "", g.format}, "  ·  ");
	}

	std::string Shell::Played(const Game& game) const
	{
		const uint32_t minutes = game.entry.minutesPlayed;
		if (minutes == 0)
			return {};
		if (minutes < 60)
			return TrP(minutes, "{0} min played", "{0} min played");
		return TrP(minutes / 60, "{0} h played", "{0} h played");
	}

	std::string Shell::LastPlayedWords(int64_t when) const
	{
		if (when <= 0)
			return {};
		const int64_t now = std::time(nullptr);
		std::tm today{}, then{};
		const std::time_t t0 = now, t1 = when;
		if (!localtime_r(&t0, &today) || !localtime_r(&t1, &then))
			return {};
		const int64_t days = (now - when) / 86400;
		// tr: how long ago a game was last played, as Home's kicker says it ("Last played {0}")
		if (today.tm_yday == then.tm_yday && today.tm_year == then.tm_year)
			return Tr("today");
		if (days <= 1)
			return Tr("yesterday");
		if (days < 7)
			return TrP(days, "{0} day ago", "{0} days ago");
		if (days < 60)
			return TrP(days / 7, "{0} week ago", "{0} weeks ago");
		return TrP(days / 30, "{0} month ago", "{0} months ago");
	}

	void Shell::ApplyLanguage()
	{
		ps5lang::Load(m_settings.ui.language);
		m_fonts.SetLanguage(ps5lang::Current());
		m_cardsFor = ~0ull; // the glance cards, said again
		if (m_screen == ScreenId::Setup)
			RunChecks();
	}

	void Shell::SaveSettings()
	{
		if (!ps5settings::Save(m_settings))
			ps5log::Line("[launcher] could not save {}", ps5paths::kLauncherSettings);
	}

	void Shell::FetchBoxArt()
	{
		std::vector<std::string> ids;
		for (const Game& game : m_games)
			if (!game.entry.game.gameId.empty())
				ids.push_back(game.entry.game.gameId);
		ps5boxart::Fetch(BoxSide(m_side), ids);
	}

	void Shell::RememberCount()
	{
		int& count = Is3ds() ? m_settings.n3ds.gameCount : m_settings.gameCount;
		if (count == (int)m_games.size())
			return;
		count = (int)m_games.size();
		SaveSettings();
	}

	void Shell::Launch(int index)
	{
		if (index < 0 || index >= (int)m_games.size())
			return;
		if (!CoreReady() || !Notice().empty())
		{
			m_feedback.Play(ui::Cue::Denied);
			Toast(Notice().empty() ? Tr("Cemu is still starting") : Notice());
			return;
		}
		Game& game = m_games[index];
		ps5settings::AddRecent(Is3ds() ? m_settings.n3ds.lastGame : m_settings.lastGame, Is3ds() ? m_settings.n3ds.recent : m_settings.recent,
			game.entry.game.titleId);
		SaveSettings();
		ps5catalog::Played(CatalogSide(), game.entry.game.titleId, std::time(nullptr));
		game.entry.lastPlayed = std::time(nullptr);
		ps5catalog::Save();
		Box from = m_launch.from;
		if (from.w <= 0)
			from = {760, 240, 400, 560};
		LaunchGame(game.entry.game, Is3ds() ? Tr("Starting Azahar") : Tr("Starting Cemu"), from);
		m_launch.game = index;
	}

	void Shell::LaunchGame(const ps5emu::Game& game, const std::string& caption, const Box& from)
	{
		m_launch.active = true;
		m_launch.at = m_now;
		m_launch.from = from;
		m_launch.caption = caption;
		m_launch.title = game.name;
		m_launch.aspect = from.h > 0 ? from.w / from.h : 0.714f;
		m_launch.game = -1;
		m_choice = ps5launcher::Choice{m_side, game};
		m_feedback.Play(ui::Cue::Launch);
		ps5log::Line("[launcher] {} chosen on the {} side", game.name, Is3ds() ? "3DS" : "Wii U");
	}

	void Shell::Toast(const std::string& text)
	{
		m_toasts.push_back({text, m_now});
		if (m_toasts.size() > 3)
			m_toasts.erase(m_toasts.begin());
		m_feedback.Play(ui::Cue::Notify);
	}

	// -- screens and the bar ------------------------------------------------------------------------

	void Shell::Show(ScreenId screen)
	{
		if (screen != m_screen)
			m_screenAt = m_now;
		m_screen = screen;
	}

	void Shell::ShowTab(ScreenId tab)
	{
		m_tab = tab;
		if (tab == ScreenId::Library)
			LibraryRefresh();
		if (tab == ScreenId::Settings && m_screen != ScreenId::Settings)
		{
			m_onRows = false;
			m_settingRow = 0;
		}
		Show(tab);
	}

	void Shell::NextTab(int direction)
	{
		static constexpr ScreenId kTabs[] = {ScreenId::Home, ScreenId::Library, ScreenId::Settings};
		int at = 0;
		for (int i = 0; i < 3; i++)
			if (kTabs[i] == m_tab)
				at = i;
		at = (at + direction + 3) % 3;
		ShowTab(kTabs[at]);
		m_barFocus = at + 1;
		m_feedback.Play(ui::Cue::Focus);
	}

	void Shell::FocusBar()
	{
		m_onBar = true;
		m_barFocus = m_tab == ScreenId::Home ? 1 : m_tab == ScreenId::Library ? 2 : 3;
		m_feedback.Play(ui::Cue::Focus);
	}

	bool Shell::BarUpdate(const ui::Press& press)
	{
		if (press.button == Button::L1 || press.button == Button::R1)
		{
			NextTab(press.button == Button::R1 ? 1 : -1);
			return true;
		}
		if (press.button == Button::Touchpad && (m_screen == ScreenId::Home || m_screen == ScreenId::Library))
		{
			SwitchSide();
			return true;
		}
		if (!m_onBar)
			return false;
		switch (press.button)
		{
		case Button::Left:
			if (m_barFocus == 0)
				SwitchSide();
			else if (m_barFocus == 1)
			{
				m_barFocus = 0;
				m_feedback.Play(ui::Cue::Focus);
			}
			else
				NextTab(-1);
			break;
		case Button::Right:
			if (m_barFocus == 0)
			{
				m_barFocus = 1;
				m_feedback.Play(ui::Cue::Focus);
			}
			else if (m_barFocus < 3)
				NextTab(1);
			else
				m_feedback.Play(ui::Cue::Edge, press.repeat);
			break;
		case Button::Cross:
			if (m_barFocus == 0)
				SwitchSide();
			else
			{
				m_onBar = false;
				m_feedback.Play(ui::Cue::Select);
			}
			break;
		case Button::Down:
			m_onBar = false;
			m_feedback.Play(ui::Cue::Focus);
			break;
		case Button::Circle:
			if (m_tab != ScreenId::Home)
			{
				ShowTab(ScreenId::Home);
				m_barFocus = 1;
				m_feedback.Play(ui::Cue::Back);
			}
			break;
		case Button::Up: m_feedback.Play(ui::Cue::Edge, press.repeat); break;
		default: break;
		}
		return true;
	}

	void Shell::DrawBar(Canvas& canvas, bool tabs)
	{
		const float y = kBarTop, h = kBarHeight;
		const float appear = Enter(0);
		canvas.PushAlpha(appear);
		// each side's square with its console's glyph in it: the GamePad on blue, the 3DS on gold (the
		// app's own tiles, tools/render-icons.py), plain colour until the picture has loaded
		static const std::string kTiles[2] = {ps5paths::Assets() + "/ui/icons/ps5cemu-72.tga", ps5paths::Assets() + "/ui/icons/azahar-72.tga"};
		auto tile = [&](int side, const Box& box, float radius, uint32_t tint) {
			const ui::Picture& picture = m_images->Get(kTiles[side]);
			if (picture.texture)
				canvas.Image(picture.texture, box, radius, tint);
			else
				canvas.Rect(box, radius, ui::SetAlpha(side ? kN3ds : kWiiu, ((tint >> 24) & 0xff) / 255.0f));
		};
		// the mark: the two sides' squares
		tile(0, {92, y + 4, 32, 32}, 8, 0xffffffff);
		tile(1, {108, y + 20, 32, 32}, 8, 0xebffffff);
		// the side switch
		float x = 160;
		const ui::TextStyle sideStyle = Style({22, ui::Weight::SemiBold, 1.0f});
		const char* names[2] = {"Wii U", "3DS"};
		constexpr float kTile = 34;
		float widths[2];
		for (int i = 0; i < 2; i++)
			widths[i] = 12 + kTile + 10 + m_fonts.Width(sideStyle, names[i]) + 20;
		const Box pill{x, y, 8 + widths[0] + 4 + widths[1], h};
		canvas.Rect(pill, h / 2, 0x0fffffff);
		canvas.Ring(pill, h / 2, 1.5f, kGlassEdge);
		float sx = x + 4;
		for (int i = 0; i < 2; i++)
		{
			const bool on = (i == 1) == Is3ds();
			const uint32_t colour = i == 1 ? kN3ds : kWiiu;
			const Box segment{sx, y + 5, widths[i], h - 10};
			if (on)
				canvas.Rect(segment, segment.h / 2, ui::SetAlpha(colour, 0.24f));
			tile(i, {sx + 12, segment.CentreY() - kTile / 2, kTile, kTile}, 9, on ? 0xffffffff : 0x73ffffff);
			const ui::TextBlock text = m_fonts.Layout(sideStyle, names[i]);
			canvas.Text(text, sx + 12 + kTile + 10, segment.CentreY() - text.height * 0.5f, on ? kText : Tertiary());
			if (m_onBar && m_barFocus == 0 && on)
				Focus(segment, segment.h / 2);
			sx += widths[i] + 4;
		}
		x = pill.Right() + 26;
		if (tabs)
		{
			canvas.Draw(Icon::L1, {x, y + 12, 36, 32}, Tertiary());
			x += 36 + 14;
			// tr: the launcher's three tabs
			const char* labels[3] = {TrC("tab", "Home"), TrC("tab", "Library"), TrC("tab", "Settings")};
			for (int i = 0; i < 3; i++)
			{
				const bool selected = (i == 0 && m_tab == ScreenId::Home) || (i == 1 && m_tab == ScreenId::Library) || (i == 2 && m_tab == ScreenId::Settings);
				const ui::TextStyle style = Style({26, selected ? ui::Weight::SemiBold : ui::Weight::Medium, 1.0f});
				const float w = m_fonts.Width(style, labels[i]) + 52;
				const Box tab{x, y, w, h};
				if (selected)
					canvas.Rect(tab, h / 2, 0xf0ffffff);
				const ui::TextBlock text = m_fonts.Layout(style, labels[i]);
				canvas.Text(text, x + 26, tab.CentreY() - text.height * 0.5f, selected ? kInk1 : Secondary());
				if (m_onBar && m_barFocus == i + 1)
					Focus(tab, h / 2);
				x += w + 6;
			}
			x += 8;
			canvas.Draw(Icon::R1, {x, y + 12, 36, 32}, Tertiary());
		}
		// the controllers and the clock, at the right
		char clock[16] = "";
		const std::time_t now = std::time(nullptr);
		std::tm local{};
		if (localtime_r(&now, &local))
			std::strftime(clock, sizeof(clock), "%H:%M", &local);
		const ui::TextStyle clockStyle = Style({24, ui::Weight::Medium, 1.0f, 0, false, true});
		const ui::TextBlock clockText = m_fonts.Layout(clockStyle, clock);
		float right = 1920 - kSafeX;
		canvas.Text(clockText, right - clockText.width, y + h / 2 - clockText.height * 0.5f, Secondary());
		right -= clockText.width + 28;
		for (int player = ps5pad::kMaxPlayers - 1; player >= 0; player--)
		{
			if (!ps5pad::IsConnected(player))
				continue;
			const Box pad{right - 36, y + h / 2 - 13, 36, 26};
			if (player == 0)
				canvas.Rect(pad, 7, kText);
			else
				canvas.Ring(pad, 7, 2, 0x59ffffff);
			const ui::TextBlock text = m_fonts.Layout({14, ui::Weight::Bold, 1.0f}, fmt::format("P{}", player + 1));
			canvas.Text(text, pad.CentreX() - text.width * 0.5f, pad.CentreY() - text.height * 0.5f, player == 0 ? kInk1 : Secondary());
			right -= 44;
		}
		canvas.PopAlpha();
	}

	std::vector<Hint> Shell::Hints() const
	{
		const std::string other = Is3ds() ? "Wii U" : "Nintendo 3DS";
		if (m_update.open)
			return {};
		// tr: the hints along the bottom: what a button does on this screen, a word or two
		if (m_keyboard.open)
			return {{Icon::Cross, Tr("Type")}, {Icon::Square, Tr("Delete")}, {Icon::Options, Tr("Done")}, {Icon::Circle, Tr("Close")}};
		if (m_picker.open)
			return {{Icon::Cross, Tr("Choose")}, {Icon::Circle, Tr("Cancel")}};
		if (m_help.open)
			return {{Icon::Circle, Tr("Close")}};
		if (m_menu.open)
			return {{Icon::Cross, Tr("Choose")}, {Icon::Circle, Tr("Close")}};
		if (m_onBar && (m_screen == ScreenId::Home || m_screen == ScreenId::Library || m_screen == ScreenId::Settings))
			return {{Icon::LeftRight, Tr("Tabs")}, {Icon::Cross, m_barFocus == 0 ? Tr("Switch") : Tr("Open")}, {Icon::Touchpad, other}};
		switch (m_screen)
		{
		case ScreenId::Chooser: return {{Icon::LeftRight, Tr("Choose")}, {Icon::Cross, Tr("Start")}};
		case ScreenId::Setup:
		{
			std::vector<Hint> hints;
			if (m_setupOnLanguage)
				hints.push_back({Icon::Cross, Tr("Change the language")});
			else if (m_setupRow < (int)m_checks.size() && !m_checks[m_setupRow].action.empty())
				hints.push_back({Icon::Cross, m_checks[m_setupRow].action});
			hints.push_back({Icon::Triangle, Tr("Check again")});
			hints.push_back({Icon::Circle, !m_setupFirst ? Tr("Back") : m_askSide ? Tr("Continue") : Tr("Continue to Home")});
			return hints;
		}
		case ScreenId::Home:
			if (m_homeGames.empty())
				return {{Icon::Cross, Tr("Choose")}, {Icon::Touchpad, other}};
			return {{Icon::Cross, m_homeZone == 0 ? (m_homeIndex < (int)m_homeGames.size() ? Tr("Play") : Tr("Open")) : Tr("Choose")},
				{Icon::Options, Tr("Options")}, {Icon::UpDown, m_homeZone == 0 ? Tr("Game hub") : Tr("Move")}, {Icon::Touchpad, other}};
		case ScreenId::Library:
			if (m_libraryZone == 0)
				return {{Icon::LeftRight, Tr("Filters")}, {Icon::Square, Tr("Sort")}, {Icon::Triangle, Tr("Search")}, {Icon::Touchpad, other}};
			return {{Icon::Cross, Tr("Play")}, {Icon::Options, Tr("Options")}, {Icon::R2, Tr("Jump by letter")}, {Icon::Touchpad, other}};
		case ScreenId::Hub: return {{Icon::Cross, m_hubAbout ? Tr("Back to the buttons") : Tr("Choose")}, {Icon::Options, Tr("Options")},
			{Icon::UpDown, Tr("Scroll")}, {Icon::Circle, Tr("Back")}};
		case ScreenId::Settings:
			if (!m_onRows)
				return {{Icon::UpDown, Tr("Pages")}, {Icon::Cross, Tr("Open")}, {Icon::Circle, TrC("tab", "Home")}};
			return {{Icon::LeftRight, Tr("Change")}, {Icon::Triangle, Tr("More about it")}, {Icon::Circle, Tr("Back")}};
		case ScreenId::Packs:
			return m_presetsFocus ?
				std::vector<Hint>{{Icon::Cross, Tr("Choose")}, {Icon::LeftRight, Tr("Change")}, {Icon::Circle, Tr("Back to the packs")}} :
				std::vector<Hint>{{Icon::Cross, Tr("On / off")}, {Icon::LeftRight, Tr("Presets")}, {Icon::Circle, Tr("Back")}};
		case ScreenId::Player:
			return {{Icon::Cross, Tr("Choose")}, {Icon::LeftRight, Tr("Change")}, {Icon::Triangle, Tr("More about it")}, {Icon::Circle, Tr("Back")}};
		case ScreenId::Mapping:
			if (m_capture.active)
				return {{Icon::Touchpad, Tr("Cancel")}};
			return {{Icon::Cross, Tr("Assign")}, {Icon::Square, Tr("Clear")}, {Icon::Circle, Tr("Back")}};
		case ScreenId::Files:
			if (m_installing)
				return {{Icon::Circle, Tr("Cancel")}};
			return {{Icon::Cross, Tr("Open")}, {Icon::Triangle, m_filesMode == 0 ? Tr("Use this folder") : Tr("Install")}, {Icon::Circle, Tr("Back")}};
		case ScreenId::Artic:
			if (m_articEditing)
				return {{Icon::LeftRight, Tr("Number")}, {Icon::UpDown, Tr("Change")}, {Icon::Cross, Tr("Keep")}};
			return {{Icon::Cross, m_articRow == 0 ? Tr("Edit") : m_articRow == 1 ? Tr("Connect") : Tr("Hold to set up")}, {Icon::Circle, Tr("Back")}};
		}
		return {};
	}

	void Shell::DrawHints(Canvas& canvas, const std::vector<Hint>& hints)
	{
		const ui::TextStyle style = Style({22, ui::Weight::Medium, 1.0f});
		float x = 1920 - kSafeX;
		const float y = 1080 - kHintsBottom - 30;
		for (auto it = hints.rbegin(); it != hints.rend(); ++it)
		{
			const ui::TextBlock text = m_fonts.Layout(style, it->text);
			x -= text.width;
			canvas.Text(text, x, y + 15 - text.height * 0.5f, Secondary());
			x -= 10 + 30;
			if (it->icon == Icon::R2)
			{
				canvas.Draw(Icon::R2, {x, y, 30, 30}, Secondary());
				x -= 34;
				canvas.Draw(Icon::L2, {x, y, 30, 30}, Secondary());
			}
			else
				canvas.Draw(it->icon, {x, y, 30, 30}, Secondary());
			x -= 34;
		}
	}

	// -- the backdrop (7.4): the focused game's colours and picture, the side's motif, grain ----------

	void Shell::DrawBackdrop(Canvas& canvas)
	{
		// the Setup check and the chooser belong to neither side: both sides' colours, and the seam
		if (m_screen == ScreenId::Setup || m_screen == ScreenId::Chooser)
		{
			DrawBrandBackdrop(canvas, m_screen == ScreenId::Setup ? 1140.0f : 960.0f);
			return;
		}
		// the colours glide to the focused game's
		const float mix = m_ambientMix.Value(m_now);
		for (int i = 0; i < 2; i++)
			m_ambient[i] = ui::Mix(m_ambientFrom[i], m_ambientTo[i], mix);
		// the picture changes once the focus has rested
		if (m_backdropWanted != m_backdrop && m_now - m_wantedAt > kRestSeconds)
		{
			m_backdropOld = m_backdrop;
			m_backdrop = m_backdropWanted;
			m_backdropMix.Snap(0);
			m_backdropMix.Go(1, m_now, m_settings.ui.reduceMotion ? kReducedSeconds : kAmbientSeconds, ui::ease::InOut);
			for (int i = 0; i < 2; i++)
			{
				m_ambientFrom[i] = m_ambient[i];
				m_ambientTo[i] = m_wantedAmbient[i];
			}
			m_ambientMix.Snap(0);
			m_ambientMix.Go(1, m_now, m_settings.ui.reduceMotion ? kReducedSeconds : kAmbientSeconds, ui::ease::InOut);
		}
		const float side = m_sideMix.Value(m_now);
		const Box screen{0, 0, 1920, 1080};
		canvas.PushAlpha(side);
		const uint32_t base = ui::Mix(Accent(), kInk0, 0.86f);
		const uint32_t glow = m_ambient[0] ? m_ambient[0] : ui::Mix(Accent(), kInk0, 0.55f);
		const uint32_t deep = m_ambient[1] ? m_ambient[1] : kInk0;
		canvas.LinearGradient(screen, 0, base, deep | 0xff000000, 0, 0, 0, 1080);
		canvas.RadialGradient(screen, 0, ui::SetAlpha(glow, 0.75f), ui::SetAlpha(glow, 0), 1380, 260, 1250, 900);
		// the focused game's picture, softened
		if (m_settings.ui.gamePictures && !m_settings.ui.highContrast)
		{
			const float pictureMix = m_backdropMix.Value(m_now);
			auto picture = [&](const std::string& path, float alpha) {
				if (path.empty() || alpha <= 0)
					return;
				const ui::Picture& p = m_images->Get(path, true);
				if (p.texture)
					canvas.ImageCover(p.texture, p.width, p.height, screen, 0, ui::SetAlpha(0xffffffff, alpha * 0.42f));
			};
			picture(m_backdropOld, 1 - pictureMix);
			picture(m_backdrop, pictureMix);
		}
		// the motif: bubbles rising on the Wii U side, waves rolling on the 3DS side
		if (!Is3ds())
			DrawBubbles(canvas, 1);
		else
			DrawWaves(canvas, 1);
		// scrims, grain and a vignette
		canvas.LinearGradient(screen, 0, 0xb305070d, 0x0005070d, 0, 0, 1300, 0);
		canvas.LinearGradient(screen, 0, 0x0005070d, 0xcc05070d, 0, 640, 0, 1080);
		if (!m_settings.ui.highContrast)
			canvas.Grain(screen, 0x0cffffff);
		canvas.RadialGradient(screen, 0, 0x00000000, 0x8c000000, 960, 432, 1248, 1026, 0.55f);
		canvas.PopAlpha();
	}

	void Shell::DrawBubbles(Canvas& canvas, float alpha)
	{
		const bool still = m_settings.ui.reduceMotion;
		for (Bubble& bubble : m_bubbles)
		{
			if (!still)
			{
				bubble.y -= bubble.speed * m_dt;
				bubble.x += bubble.drift * m_dt;
				if (bubble.y < -bubble.radius)
					bubble.y = 1080 + bubble.radius;
			}
			const Box disc{bubble.x - bubble.radius, bubble.y - bubble.radius, bubble.radius * 2, bubble.radius * 2};
			canvas.Rect(disc, bubble.radius, ui::SetAlpha(0xfff4e9de, bubble.alpha * 0.5f * alpha));
		}
	}

	void Shell::DrawWaves(Canvas& canvas, float alpha)
	{
		static constexpr float kTop[3] = {580, 670, 770}, kWave[3] = {1280, 960, 720}, kAmplitude[3] = {36, 28, 22}, kSpeed[3] = {24, -36, 50};
		static constexpr uint32_t kColour[3] = {0xff8ce4ff, 0xffa6ecff, 0xffc4f4ff};
		for (int layer = 0; layer < 3; layer++)
		{
			if (!m_settings.ui.reduceMotion)
				m_wavePhase[layer] -= kSpeed[layer] * m_dt / kWave[layer] * 6.2831853f;
			canvas.Wave({0, kTop[layer] - 4, 1920, 1080 - kTop[layer] + 4}, kTop[layer] + kAmplitude[layer], kAmplitude[layer],
				kWave[layer] / 6.2831853f, m_wavePhase[layer], ui::SetAlpha(kColour[layer], (0.05f + 0.015f * layer) * alpha));
		}
	}

	// -- overlays: the dropdown, help, the Game menu, the keyboard, the update sheet, toasts, the launch --

	void Shell::OpenPicker(const std::string& kicker, const std::string& title, std::vector<std::string> options, int active,
		std::function<void(int)> choose)
	{
		if (options.empty())
			return;
		m_picker.open = true;
		m_picker.kicker = kicker;
		m_picker.title = title;
		m_picker.options = std::move(options);
		m_picker.active = active;
		m_picker.selected = std::max(active, 0);
		m_picker.choose = std::move(choose);
		m_picker.scroll.Snap(0);
		m_picker.at = m_now;
		m_feedback.Play(ui::Cue::Sheet);
	}

	void Shell::OpenHelp(const std::string& title, const std::string& text)
	{
		m_help = {true, title, text, m_now};
		m_feedback.Play(ui::Cue::Sheet);
	}

	void Shell::OpenGameMenu(int game)
	{
		if (game < 0 || game >= (int)m_games.size())
			return;
		m_menu.open = true;
		m_menu.game = game;
		m_menu.selected = 0;
		m_menu.at = m_now;
		const Game& g = m_games[game];
		m_menu.items = {{"play", Tr("Play")}};
		if (m_screen != ScreenId::Hub)
			m_menu.items.push_back({"hub", Tr("Game hub")});
		if (!Is3ds() && m_status.coreReady)
			m_menu.items.push_back({"packs", Tr("Graphic packs")});
		m_menu.items.push_back({"favourite", g.entry.favourite ? Tr("Not a favourite") : Tr("Favourite")});
		m_menu.items.push_back({"where", Tr("Show where it is")});
		m_feedback.Play(ui::Cue::Sheet);
	}

	void Shell::OpenKeyboard()
	{
		m_keyboard.open = true;
		m_keyboard.row = 1;
		m_keyboard.column = 0;
		m_keyboard.at = m_now;
		m_feedback.Play(ui::Cue::Sheet);
	}

	bool Shell::OverlayUpdate(const ui::Press& press)
	{
		const Button b = press.button;
		if (m_update.open)
		{
			const auto status = ps5update::GetStatus();
			using State = ps5update::Status::State;
			const int choices = status.state == State::Available ? 2 : status.state == State::Ready || status.state == State::Failed ? 1 : 0;
			if ((b == Button::Left || b == Button::Right) && choices > 1)
			{
				m_update.choice = 1 - m_update.choice;
				m_feedback.Play(ui::Cue::Focus);
			}
			else if (b == Button::Circle)
			{
				ps5update::Dismiss();
				m_feedback.Play(ui::Cue::Back);
			}
			else if (b == Button::Cross && choices > 0)
			{
				// Available: Update now, Later; Ready: Restart now; Failed: OK
				if (status.state == State::Available && m_update.choice == 0)
					ps5update::Install();
				else if (status.state == State::Ready)
					ps5update::Restart();
				else
					ps5update::Dismiss();
				m_update.choice = 0;
				m_feedback.Play(ui::Cue::Select);
			}
			return true;
		}
		if (m_picker.open)
		{
			const int count = (int)m_picker.options.size();
			if (b == Button::Circle)
			{
				m_picker.open = false;
				m_feedback.Play(ui::Cue::Back);
			}
			else if (b == Button::Cross)
			{
				m_picker.open = false;
				m_feedback.Play(ui::Cue::Select);
				auto choose = m_picker.choose;
				choose(m_picker.selected);
			}
			else if (b == Button::Up || b == Button::Down)
			{
				const int next = m_picker.selected + (b == Button::Down ? 1 : -1);
				if (next < 0 || next >= count)
					m_feedback.Play(ui::Cue::Edge, press.repeat);
				else
				{
					m_picker.selected = next;
					m_feedback.Play(ui::Cue::Focus);
				}
			}
			else if (b == Button::L2 || b == Button::R2 || b == Button::L1 || b == Button::R1)
			{
				m_picker.selected = std::clamp(m_picker.selected + (b == Button::R2 || b == Button::R1 ? 6 : -6), 0, count - 1);
				m_feedback.Play(ui::Cue::Focus);
			}
			return true;
		}
		if (m_help.open)
		{
			if (b == Button::Circle || b == Button::Cross || b == Button::Triangle)
			{
				m_help.open = false;
				m_feedback.Play(ui::Cue::Back);
			}
			return true;
		}
		if (m_keyboard.open)
		{
			auto rowLength = [](int row) { return row < 4 ? (int)std::strlen(kKeyRows[row]) : 3; };
			switch (b)
			{
			case Button::Up:
			case Button::Down:
			{
				const int row = m_keyboard.row + (b == Button::Down ? 1 : -1);
				if (row < 0 || row >= kKeyboardRows)
					m_feedback.Play(ui::Cue::Edge, press.repeat);
				else
				{
					m_keyboard.row = row;
					m_keyboard.column = std::min(m_keyboard.column, rowLength(row) - 1);
					m_feedback.Play(ui::Cue::Focus);
				}
				break;
			}
			case Button::Left:
			case Button::Right:
			{
				const int length = rowLength(m_keyboard.row);
				m_keyboard.column = (m_keyboard.column + (b == Button::Right ? 1 : length - 1)) % length;
				m_feedback.Play(ui::Cue::Focus);
				break;
			}
			case Button::Cross:
				if (m_keyboard.row < 4)
					m_search += (char)std::tolower((unsigned char)kKeyRows[m_keyboard.row][m_keyboard.column]);
				else if (m_keyboard.column == 0)
					m_search += ' ';
				else if (m_keyboard.column == 1 && !m_search.empty())
					m_search.pop_back();
				else if (m_keyboard.column == 2)
					m_keyboard.open = false;
				m_feedback.Play(ui::Cue::Select);
				LibraryRefresh();
				break;
			case Button::Square:
				if (!m_search.empty())
					m_search.pop_back();
				m_feedback.Play(ui::Cue::Back);
				LibraryRefresh();
				break;
			case Button::Triangle:
				m_search += ' ';
				m_feedback.Play(ui::Cue::Select);
				LibraryRefresh();
				break;
			case Button::Options:
			case Button::Circle:
				m_keyboard.open = false;
				m_feedback.Play(ui::Cue::Back);
				break;
			default: break;
			}
			return true;
		}
		if (m_menu.open)
		{
			const int count = (int)m_menu.items.size();
			if (b == Button::Circle || b == Button::Options)
			{
				m_menu.open = false;
				m_feedback.Play(ui::Cue::Back);
			}
			else if (b == Button::Up || b == Button::Down)
			{
				const int next = m_menu.selected + (b == Button::Down ? 1 : -1);
				if (next < 0 || next >= count)
					m_feedback.Play(ui::Cue::Edge, press.repeat);
				else
				{
					m_menu.selected = next;
					m_feedback.Play(ui::Cue::Focus);
				}
			}
			else if (b == Button::Cross)
			{
				m_menu.open = false;
				const std::string id = m_menu.items[m_menu.selected].first;
				const int game = m_menu.game;
				Game& g = m_games[game];
				if (id == "play")
					Launch(game);
				else if (id == "hub")
				{
					m_feedback.Play(ui::Cue::Select);
					HubOpen(game, m_screen);
				}
				else if (id == "packs")
				{
					m_feedback.Play(ui::Cue::Select);
					PacksOpen(game, m_screen);
				}
				else if (id == "favourite")
				{
					g.entry.favourite = !g.entry.favourite;
					ps5catalog::SetFavourite(CatalogSide(), g.entry.game.titleId, g.entry.favourite);
					ps5catalog::Save();
					m_feedback.Play(ui::Cue::Toggle);
					Toast(g.entry.favourite ? Tr("Added to Favourites") : Tr("Taken out of Favourites"));
					LibraryRefresh();
				}
				else if (id == "where")
				{
					const std::string path = g.entry.game.path.string();
					std::string drive = Tr("The PS5's storage");
					for (const std::string& d : ConnectedDrives())
						if (path.starts_with(d))
							drive = DriveName(d);
					OpenHelp(g.entry.game.name, path + "\n\n" + drive + "\n\n" + TrF("Title ID {0}", Hex(g.entry.game.titleId)) + "  ·  " + g.entry.game.format);
				}
			}
			return true;
		}
		return false;
	}

	void Shell::DrawOverlays(Canvas& canvas)
	{
		// toasts, top right
		std::erase_if(m_toasts, [this](const ToastItem& toast) { return m_now - toast.at > 3.6; });
		float ty = 124;
		for (const ToastItem& toast : m_toasts)
		{
			const float age = (float)(m_now - toast.at);
			const float alpha = std::min({1.0f, age / 0.2f, (3.6f - age) / 0.4f});
			const ui::TextStyle style = Style(kLabelStyle);
			const ui::TextBlock text = m_fonts.Layout(style, toast.text, 760, 2);
			const Box box{1920 - kSafeX - text.width - 56, ty, text.width + 56, text.height + 32};
			canvas.PushAlpha(std::max(0.0f, alpha));
			canvas.PushOffset(0, (1 - std::min(1.0f, age / 0.25f)) * -12);
			DrawSheet(canvas, box, alpha);
			canvas.Rect({box.x + 18, box.CentreY() - 5, 10, 10}, 5, Accent());
			canvas.Text(text, box.x + 38, box.y + 16, kText);
			canvas.PopOffset();
			canvas.PopAlpha();
			ty += box.h + 12;
		}

		auto appear = [this](double at) { return m_settings.ui.reduceMotion ? std::min(1.0f, (float)((m_now - at) / kReducedSeconds)) :
																			ui::ease::CubicOut((float)((m_now - at) / 0.3)); };
		if (m_picker.open)
		{
			const float a = appear(m_picker.at);
			Scrim(canvas, 0.7f * a);
			const int count = (int)m_picker.options.size(), visible = std::min(count, 8);
			const float rowH = 64;
			const Box sheet{1920 - kSafeX - 620, 540 - (visible * rowH + 170) / 2, 620, visible * rowH + 170};
			canvas.PushAlpha(a);
			canvas.PushOffset((1 - a) * 40, 0);
			DrawSheet(canvas, sheet, a);
			canvas.Text(Style(kOverlineStyle), sheet.x + 36, sheet.y + 34, m_picker.kicker, Secondary());
			canvas.Text(Style(kHeadingStyle), sheet.x + 36, sheet.y + 60, m_picker.title, kText, sheet.w - 72, 1);
			const int first = std::clamp(m_picker.selected - visible / 2, 0, std::max(0, count - visible));
			m_picker.scroll.target = (float)first;
			m_picker.scroll.omega = kScrollOmega;
			m_picker.scroll.Update(m_dt);
			const Box list{sheet.x + 20, sheet.y + 124, sheet.w - 40, visible * rowH};
			canvas.PushClip(list);
			for (int i = 0; i < count; i++)
			{
				const Box row{list.x, list.y + (i - m_picker.scroll.value) * rowH, list.w, rowH - 6};
				if (row.Bottom() < list.y || row.y > list.Bottom())
					continue;
				const bool focused = i == m_picker.selected;
				if (focused)
					canvas.Rect(row, 14, Surface2());
				canvas.Text(Style(kBodyStyle), row.x + 20, row.CentreY() - 19, m_picker.options[i], focused ? kText : Secondary(), row.w - 180, 1);
				if (i == m_picker.active)
				{
					canvas.Draw(Icon::Check, {row.Right() - 46, row.CentreY() - 13, 26, 26}, Accent());
					canvas.Text(Style(kCaptionStyle), row.Right() - 56, row.CentreY() - 13, Tr("In use"), Secondary(), 120, 1, ui::Align::Right);
				}
			}
			canvas.PopClip();
			canvas.PopOffset();
			canvas.PopAlpha();
		}
		if (m_help.open)
		{
			const float a = appear(m_help.at);
			Scrim(canvas, 0.75f * a);
			const ui::TextBlock body = m_fonts.Layout(Style(kBodyStyle), m_help.text, 820, 14);
			const ui::TextBlock title = m_fonts.Layout(Style(kHeadingStyle), m_help.title, 820, 2);
			const float h = 48 + title.height + 20 + body.height + 48;
			const Box sheet{960 - 450, 540 - h / 2, 900, h};
			canvas.PushAlpha(a);
			canvas.PushOffset(0, (1 - a) * 30);
			DrawSheet(canvas, sheet, a);
			canvas.Text(title, sheet.x + 40, sheet.y + 48, kText);
			canvas.Text(body, sheet.x + 40, sheet.y + 48 + title.height + 20, Secondary());
			canvas.PopOffset();
			canvas.PopAlpha();
		}
		if (m_menu.open && m_menu.game >= 0 && m_menu.game < (int)m_games.size())
		{
			const float a = appear(m_menu.at);
			Scrim(canvas, 0.6f * a);
			const int count = (int)m_menu.items.size();
			const float rowH = 66;
			const Box sheet{1920 - kSafeX - 560, 150, 560, 150 + count * rowH};
			canvas.PushAlpha(a);
			canvas.PushOffset((1 - a) * 40, 0);
			DrawSheet(canvas, sheet, a);
			Game& game = m_games[m_menu.game];
			Badge(canvas, sheet.x + 36, sheet.y + 32, m_side);
			canvas.Text(Style(kHeadingStyle), sheet.x + 36, sheet.y + 78, game.entry.game.name, kText, sheet.w - 72, 1);
			static const std::map<std::string, Icon> kIcons = {{"play", Icon::Play}, {"hub", Icon::ChevronRight}, {"packs", Icon::Gear},
				{"favourite", Icon::Star}, {"where", Icon::Folder}};
			for (int i = 0; i < count; i++)
			{
				const Box row{sheet.x + 20, sheet.y + 136 + i * rowH, sheet.w - 40, rowH - 6};
				const bool focused = i == m_menu.selected;
				if (focused)
					canvas.Rect(row, 14, Surface2());
				canvas.Draw(kIcons.at(m_menu.items[i].first), {row.x + 18, row.CentreY() - 14, 28, 28}, focused ? kText : Secondary());
				canvas.Text(Style(kBodyStyle), row.x + 62, row.CentreY() - 19, m_menu.items[i].second, focused ? kText : Secondary());
				if (focused)
				{
					canvas.Ring(row.Inset(-4), 18, 4, kFocus);
				}
			}
			canvas.PopOffset();
			canvas.PopAlpha();
		}
		if (m_keyboard.open)
		{
			const float a = appear(m_keyboard.at);
			const Box sheet{960 - 560, 1080 - 60 - 420, 1120, 420};
			canvas.PushAlpha(a);
			canvas.PushOffset(0, (1 - a) * 60);
			DrawSheet(canvas, sheet, a);
			canvas.Draw(Icon::Search, {sheet.x + 36, sheet.y + 30, 30, 30}, Secondary());
			canvas.Text(Style(kHeadingStyle), sheet.x + 80, sheet.y + 26, m_search.empty() ? Tr("Search") : m_search + "_", m_search.empty() ? Tertiary() : kText,
				sheet.w - 120, 1);
			const float keyW = 92, keyH = 56, gap = 10;
			for (int row = 0; row < kKeyboardRows; row++)
			{
				const int length = row < 4 ? (int)std::strlen(kKeyRows[row]) : 3;
				const float widths[3] = {keyW * 4 + gap * 3, keyW * 2 + gap, keyW * 2 + gap};
				float total = 0;
				for (int k = 0; k < length; k++)
					total += (row < 4 ? keyW : widths[k]) + (k ? gap : 0);
				float x = sheet.CentreX() - total / 2;
				const float y = sheet.y + 86 + row * (keyH + gap);
				for (int k = 0; k < length; k++)
				{
					const float w = row < 4 ? keyW : widths[k];
					const Box key{x, y, w, keyH};
					const bool focused = m_keyboard.row == row && m_keyboard.column == k;
					canvas.Rect(key, 12, focused ? kText : Surface2());
					// tr: the on-screen keyboard's keys
					const std::string label = row < 4 ? std::string(1, kKeyRows[row][k]) : k == 0 ? Tr("Space") : k == 1 ? Tr("Delete") : Tr("Done");
					canvas.Text(Style({26, ui::Weight::Medium, 1.0f}), key.x + 8, key.CentreY() - 15, label, focused ? kInk1 : kText, key.w - 16, 1,
						ui::Align::Centre);
					x += w + gap;
				}
			}
			canvas.PopOffset();
			canvas.PopAlpha();
		}
		if (m_update.open)
		{
			const float a = appear(m_update.at);
			Scrim(canvas, 0.8f * a);
			using State = ps5update::Status::State;
			const auto status = ps5update::GetStatus();
			const std::string next = ps5update::Readable(status.latest), mine = ps5update::Readable(PS5CEMU_VERSION);
			std::string title, text;
			float done = -1;
			std::vector<std::string> choices;
			switch (status.state)
			{
			case State::Available:
				title = TrF("PS5CEMU-HAR {0} is out", next);
				text = TrF("This is {0}. Download and install it now? Your games, saves and settings stay as they are.", mine);
				choices = {Tr("Update now"), Tr("Later")};
				break;
			case State::Downloading:
				title = TrF("Updating to {0}", next);
				text = status.total ? TrF("Downloading: {0} of {1} MB", status.received >> 20, status.total >> 20) : Tr("Downloading…");
				done = status.total ? (float)status.received / (float)status.total : 0.0f;
				break;
			case State::Verifying:
				title = TrF("Updating to {0}", next);
				text = Tr("Checking the download");
				done = 1;
				break;
			case State::Installing:
				title = TrF("Updating to {0}", next);
				text = Tr("Installing");
				done = 1;
				break;
			case State::Ready:
				title = TrF("{0} is installed", next);
				text = Tr("PS5CEMU-HAR starts again to finish the update.");
				choices = {Tr("Restart now")};
				break;
			case State::Failed:
				title = Tr("The update did not finish");
				text = status.message + "\n" + TrF("PS5CEMU-HAR is still {0}.", mine);
				choices = {Tr("OK")};
				break;
			default: title = Tr("Checking for updates"); break;
			}
			const Box sheet{960 - 520, 300, 1040, 440};
			canvas.PushAlpha(a);
			canvas.PushOffset(0, (1 - a) * 40);
			DrawSheet(canvas, sheet, a);
			canvas.Text(Style(kOverlineStyle), sheet.x + 48, sheet.y + 44, Tr("PS5CEMU-HAR update"), Secondary(), sheet.w - 96, 1);
			canvas.Text(Style(kTitleStyle), sheet.x + 48, sheet.y + 72, title, kText, sheet.w - 96, 1);
			ui::TextStyle body = Style(kBodyStyle);
			body.tabular = true;
			canvas.Text(body, sheet.x + 48, sheet.y + 140, text, Secondary(), sheet.w - 96, 3);
			if (done >= 0)
			{
				const Box bar{sheet.x + 48, sheet.y + 260, sheet.w - 96, 12};
				canvas.Rect(bar, 6, 0x24ffffff);
				canvas.Rect({bar.x, bar.y, bar.w * std::clamp(done, 0.0f, 1.0f), bar.h}, 6, Accent());
			}
			m_update.choice = std::min<int>(m_update.choice, std::max<int>(0, (int)choices.size() - 1));
			float x = sheet.x + 48;
			for (int i = 0; i < (int)choices.size(); i++)
			{
				const Box b = PillButton(canvas, x, sheet.Bottom() - 48 - 72, choices[i], i == 0, i == m_update.choice);
				x += b.w + 20;
			}
			DrawFocusRing(canvas);
			if (!choices.empty())
				DrawHints(canvas, choices.size() > 1 ? std::vector<Hint>{{Icon::LeftRight, Tr("Choose")}, {Icon::Cross, Tr("Select")}, {Icon::Circle, Tr("Later")}} :
													   std::vector<Hint>{{Icon::Cross, status.state == State::Failed ? Tr("OK") : Tr("Restart")}});
			canvas.PopOffset();
			canvas.PopAlpha();
		}

		// the launch (6.8): the cover to the centre, growing, the rest dimmed, and what is happening
		if (m_launch.active)
		{
			const float t = ui::ease::QuintOut((float)((m_now - m_launch.at) / kLaunchSeconds));
			const float height = 560, width = height * std::clamp(m_launch.aspect, 0.5f, 1.4f);
			const Box to{960 - width / 2, 170, width, height};
			const Box from = m_launch.from;
			const Box at{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, from.w + (to.w - from.w) * t, from.h + (to.h - from.h) * t};
			canvas.Shadow(at, kRadiusCover, 30, 70, 0xa0000000);
			if (m_launch.game >= 0 && m_launch.game < (int)m_games.size())
				DrawCover(canvas, m_games[m_launch.game], at, false, 0); // its box art, or its card with the icon kept small
			else
				canvas.LinearGradient(at, kRadiusCover, ui::Mix(Accent(), kInk1, 0.4f), kInk1, at.x, at.y, at.x, at.Bottom());
			canvas.Ring(at, kRadiusCover, 1, 0x1fffffff);
			canvas.PushAlpha(t);
			canvas.Text(Style(kTitleStyle), 960 - 700, 770, m_launch.title, kText, 1400, 1, ui::Align::Centre);
			canvas.Text(Style(kBodyStyle), 960 - 700, 836, m_launch.caption, Secondary(), 1400, 1, ui::Align::Centre);
			canvas.PopAlpha();
		}
	}
}
