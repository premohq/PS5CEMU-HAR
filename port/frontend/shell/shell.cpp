// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the new launcher (shell.h): its loop, the sides and their games, the bar along the top,
// the hints, the backdrop, the overlays and the launch. The screens are in their own files
// (shell_internal.h).

#include "shell_internal.h"
#include "../actions.h"
#include "../sound.h"
#include "../../app/boxart.h"
#include "../../app/forwarders.h"
#include "../../app/keyboard.h"
#include "../../app/mods.h"
#include "../../app/pack_updates.h"
#include "../../app/paths.h"
#include "../../app/updates.h"
#include "../../azahar/azahar.h"
#include "../../azahar/library.h"
#include "../../melonds/library.h"
#include "../../ps5/kernel.h"
#include "../../ps5/log.h"
#include "../../ps5/notify.h"
#include "../../ps5/pad.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace ps5shell
{
	using namespace ui::tokens;
	using namespace ps5actions;

	namespace
	{
		Host s_host;
		bool s_hostSet = false;
		std::string s_failure; // why the launcher could not start or go on (LastFailure)

		const Host& TheHost()
		{
			if (!s_hostSet)
			{
				s_host = DefaultHost();
				s_hostSet = true;
			}
			return s_host;
		}

		// the keyboard's pages, as the in-game keyboards have them (app/keyboard.h), so the app has one
		// keyboard: the Latin letters, the menus' language's own, the symbols, which L1 and R1 go
		// through; capitals with Shift (its key or Triangle). Under a page's rows, Shift, Space, Delete
		// and Done.
		const std::vector<ps5keyboard::Page>& KeyboardPages()
		{
			return ps5keyboard::Pages(ps5lang::Current());
		}
	}

	const std::string& LastFailure()
	{
		return s_failure;
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
			s_failure = error;
			return Outcome::Classic;
		}
		m_fonts.SetFallbackFolders(host.fontFolders);
		ApplyTheme();
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
			s_failure = "it cannot draw: " + error;
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

		// the side: the one a game came back from, else the one last used, else (a first start) the first
		// with games (Wii U, 3DS, DS), else the Wii U's. Nothing asks which side (5.3), unless Start on:
		// Ask each time is on.
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
			const int ndsGames = std::max<int>(m_settings.nds.gameCount, (int)ps5catalog::Games(ps5catalog::System::Nds).size());
			side = wiiuGames > 0 ? "wiiu" : n3dsGames > 0 ? "3ds" : ndsGames > 0 ? "ds" : "wiiu";
		}
		// why the last game did not start (main_ps5.cpp's): said once here, not a state that keeps every
		// game from starting
		std::string launchError;
		for (std::string* error : {&m_status.launchError, &m_status.launchError3ds, &m_status.launchErrorDs})
			if (!error->empty())
			{
				launchError = *error;
				error->clear();
			}
		// a home screen tile's game (--rom): its side, whichever was used last
		if (!m_status.autoLaunch.empty())
		{
			side = SideName(m_status.autoLaunchSide);
			ps5log::Line("[launcher] a home screen tile's game, on the {} side: {}", SideTitle(m_status.autoLaunchSide), m_status.autoLaunch);
		}
		m_side = ps5launcher::SideNamed(side);
		if (!SideShown(m_side) && m_status.autoLaunch.empty())
			m_side = FirstShownSide();
		// a version this console has not run before: What's new, once, before anything else, on a first
		// start too (a home screen tile's game starts at once instead, and What's new waits for the next
		// start); then a first start's look and feel to choose, or an update's new options
		const bool firstStart = !m_settings.ui.setupDone;
		const bool newVersion = m_settings.ui.lastVersion != PS5CEMU_VERSION;
		const bool updated = !firstStart && !returning && newVersion;
		const bool news = newVersion && !returning && m_status.autoLaunch.empty();
		if (newVersion && m_status.autoLaunch.empty())
		{
			ps5log::Line("[ui] version {} (last run: {})", PS5CEMU_VERSION, m_settings.ui.lastVersion.empty() ? "3.5.0 or earlier" : m_settings.ui.lastVersion);
			m_settings.ui.lastVersion = PS5CEMU_VERSION;
			SaveSettings();
		}
		const NewsThen then = firstStart ? NewsThen::Welcome : SetupNeeded() ? NewsThen::Setup : updated ? NewsThen::NewOptions : NewsThen::Side;
		if (news)
			WhatsNewOpen(then);
		else if (then == NewsThen::Welcome)
			WelcomeOpen(true); // then the Setup check, then the side
		else if (then == NewsThen::Setup)
			SetupOpen(true); // then the side (FinishStart)
		else if (then == NewsThen::NewOptions)
			WelcomeOpen(false); // then the side
		else
		{
			// the side's Library, on the game played last: after a game, that game (6.8)
			OpenSide(m_side, true);
			if (returning)
				Toast(Tr("Saved your place in the library"));
		}
		if (!launchError.empty())
			OpenHelp(Tr("The game could not start"), launchError);
		// Eden's opening: the PS5's launch picture handed over to the launcher, as the app starts (not
		// after a game, nor over what a failed one says)
		else if (SectionLayout() && !ClassicLayout() && !returning && m_screen == ScreenId::Home && m_status.autoLaunch.empty())
			EdenStartOpening();
		ps5log::Line("[ui] the new launcher is up in {:.0f} ms", (host.clock() - started) * 1000);

		while (!m_done && !m_failed)
			Frame();
		if (m_failed)
		{
			m_images->Stop();
			ps5sound::Stop();
			m_gfx.Stop();
			s_failure = "the GPU stopped drawing it";
			return Outcome::Restart;
		}
		// the background work stops behind the launch (4.2, rule 4), the screen kept drawn
		ps5boxart::Stop();
		ps5packs::Stop();
		ps5mods::Stop();
		ps5forwarders::Stop();
		ps5emu::StopFindingInstallables();
		ps5saves::Finish(); // a backup or restore running finishes first
		for (int waited = 0; SystemScanning() && !m_failed; waited++)
		{
			if (waited == 0)
				ps5log::Line("[launcher] waiting for the library's scan to finish");
			Frame();
		}
		ps5catalog::Save();
		Frame();
		// the in-game menus' font for what Lexend lacks: the console's, for this language's words, its
		// keyboard's keys and this game's name; none for a Latin language and title
		if (m_choice)
		{
			const ui::Fonts::FontFile font = m_fonts.FileFor(
				ps5lang::MenuCharacters() + ps5keyboard::Characters(ps5lang::Current()) + ui::Decode(m_choice->game.name));
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
		if (m_status.coreStarting)
			PollCore();
		PollGames();
		PollPacks();
		PollMods();
		PollTiles();
		PollAutoLaunch();
		if (m_installing)
			PollInstall();
		if (m_screen == ScreenId::Updates || m_updates.installing)
			PollUpdates();
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
			ps5log::Line("[ui] first frame: {} instances, {} draws", m_list.instances.size(), m_list.runs.size());
		// the PS5's splash goes once the launcher is drawn; with Eden's opening, once its picture (the
		// splash's own) is drawn under it, so the one becomes the other
		if (!m_splashHidden && (!m_openingOn || m_openingAt >= 0 || m_now - m_start > 1.5))
		{
			sceSystemServiceHideSplashScreen();
			m_splashHidden = true;
		}
		// the tour log's line, every 600 frames
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
		// Eden's own screens with another theme in use (it was changed): the Library
		if (!SectionLayout())
		{
			m_railFocus = false;
			if (m_screen == ScreenId::Home || m_screen == ScreenId::About)
				ShowTab(ScreenId::Library);
		}
		for (const ui::Press& press : actions.presses)
		{
			if (m_launch.active || m_capture.active)
				break;
			if (m_openingOn)
			{
				EdenOpeningUpdate(press);
				continue;
			}
			if (OverlayUpdate(press))
				continue;
			const bool tabbed = m_screen == ScreenId::Library || m_screen == ScreenId::Settings;
			if (SectionLayout() && EdenSectionIndex() >= 0)
			{
				if (EdenBarUpdate(press))
					continue;
			}
			else if (tabbed && BarUpdate(press))
				continue;
			switch (m_screen)
			{
			case ScreenId::WhatsNew: WhatsNewUpdate(press); break;
			case ScreenId::Welcome: WelcomeUpdate(press, actions); break;
			case ScreenId::Setup: SetupUpdate(press); break;
			case ScreenId::Library: LibraryUpdate(press); break;
			case ScreenId::Hub: HubUpdate(press); break;
			case ScreenId::Settings: SettingsUpdate(press, actions); break;
			case ScreenId::Packs: PacksUpdate(press); break;
			case ScreenId::Player: PlayerUpdate(press, actions); break;
			case ScreenId::Mapping: MappingUpdate(press); break;
			case ScreenId::Files: FilesUpdate(press); break;
			case ScreenId::Artic: ArticUpdate(press, actions); break;
			case ScreenId::Updates: UpdatesUpdate(press); break;
			case ScreenId::Shares: SharesUpdate(press, actions); break;
			case ScreenId::Home: EdenHomeUpdate(press); break;
			case ScreenId::About: EdenAboutUpdate(press); break;
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
			else if (m_screen == ScreenId::Shares)
				SharesUpdate(none, actions);
		}
	}

	void Shell::Draw(Canvas& canvas)
	{
		m_drawing = &canvas;
		m_edenLaunchFrom = {};
		DrawBackdrop(canvas);
		const float appear = m_launch.active ? 1.0f - ui::ease::CubicOut((float)((m_now - m_launch.at) / kLaunchSeconds)) * 0.85f : 1.0f;
		canvas.PushAlpha(appear);
		const bool rail = SectionLayout() && EdenSectionIndex() >= 0;
		switch (m_screen)
		{
		case ScreenId::WhatsNew: WhatsNewDraw(canvas); break;
		case ScreenId::Welcome: WelcomeDraw(canvas); break;
		case ScreenId::Setup: SetupDraw(canvas); break;
		case ScreenId::Library: LibraryDraw(canvas); break;
		case ScreenId::Hub: HubDraw(canvas); break;
		case ScreenId::Settings: SettingsDraw(canvas); break;
		case ScreenId::Packs: PacksDraw(canvas); break;
		case ScreenId::Player: PlayerDraw(canvas); break;
		case ScreenId::Mapping: MappingDraw(canvas); break;
		case ScreenId::Files: FilesDraw(canvas); break;
		case ScreenId::Artic: ArticDraw(canvas); break;
		case ScreenId::Updates: UpdatesDraw(canvas); break;
		case ScreenId::Shares: SharesDraw(canvas); break;
		case ScreenId::Home: EdenHomeDraw(canvas); break;
		case ScreenId::About: EdenAboutDraw(canvas); break;
		}
		if (rail)
			EdenDrawRail(canvas);
		const bool overlay = m_picker.open || m_help.open || m_menu.open || m_keyboard.open || m_update.open;
		if (overlay || m_openingOn)
			m_ringAsked = false; // the overlay's own focus is drawn with it; the ring comes after Eden's opening
		if (!m_launch.active)
			DrawFocusRing(canvas);
		if (!m_launch.active)
		{
			if (rail)
			{
				canvas.PushAlpha(Enter(0)); // with the section (and after Eden's opening)
				EdenDrawHints(canvas, Hints());
				canvas.PopAlpha();
			}
			else
				DrawHints(canvas, Hints());
		}
		canvas.PopAlpha();
		DrawOverlays(canvas);
		if (m_openingOn)
			EdenDrawOpening(canvas);
		if (TerminalLayout())
			TermDrawCrt(canvas); // the tube, over every screen of Terminal's
		m_drawing = nullptr;
	}

	// -- sides and games ------------------------------------------------------------------------

	bool Shell::SystemScanning() const
	{
		return Is3ds() ? ps5azahar::Scanning() : IsDs() ? ps5melonds::Scanning() : m_status.coreReady && ps5emu::Scanning();
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
			m_listed = false;
			if (!first || m_frames > 0)
			{
				// what is happening shows while the side starts (Cemu takes a few seconds)
				Show(SectionLayout() ? ScreenId::Home : ScreenId::Library);
				Frame();
			}
			m_prepare(side);
			m_prepared = side;
			// A and B chosen for every side before Cemu was up (Make it yours): the Wii U players' now
			if (side == System::WiiU && m_status.coreReady && m_settings.wiiuFaceLayout)
			{
				for (int player = 0; player < ps5pad::kMaxPlayers; player++)
					ps5emu::SetFaceLayout(player, m_settings.wiiuFaceLayout == 1 ? ps5emu::FaceLayout::Nintendo : ps5emu::FaceLayout::PlayStation);
				m_settings.wiiuFaceLayout = 0;
				SaveSettings();
			}
		}
		const bool changed = m_side != side || first;
		m_side = side;
		m_settings.ui.lastSide = SideName(side);
		SaveSettings();
		LoadGames();
		m_feedback.SetLight(Accent());
		if (changed)
			m_sideMix.Go(1, m_now, m_settings.ui.reduceMotion ? kReducedSeconds : kAmbientSeconds), m_sideMix.from = 0;
		// the Library, on the game this side had in focus, else the one played last (6.1)
		m_libraryZone = 1;
		m_oscarZone = 0; // Meyer Oscar's: on Recently played, the game played last first
		m_libraryIndex = -1; // no game in focus, so the refresh finds the one asked for
		m_libraryFocusTitle = m_sideFocus[SideIndex()] ? m_sideFocus[SideIndex()] : LastGame(side);
		m_search.clear();
		m_stateButton = 0;
		LibraryRefresh();
		// Eden opens on Home, the game to continue in front (6.17)
		m_homeFocus = 0;
		m_tab = SectionLayout() ? ScreenId::Home : ScreenId::Library;
		Show(m_tab);
	}

	System Shell::NextShownSide(System side) const
	{
		for (int i = 1; i <= 3; i++)
		{
			const System next = (System)((SideIndex(side) + i) % 3);
			if (SideShown(next))
				return next;
		}
		return side;
	}

	System Shell::FirstShownSide() const
	{
		for (System side : {System::WiiU, System::N3ds, System::Nds})
			if (SideShown(side))
				return side;
		return System::WiiU;
	}

	void Shell::SidesChanged()
	{
		if (SideShown(m_side))
			return;
		// the side open was hidden: the first one shown, opened where a side is (Settings stays Settings)
		if (m_prepared && m_screen != ScreenId::Welcome && m_screen != ScreenId::Setup && m_screen != ScreenId::WhatsNew)
			SwitchSide(FirstShownSide());
		else
			m_side = FirstShownSide();
	}

	void Shell::ApplyTheme()
	{
		const std::string folder = std::string(ps5paths::kRoot) + "/themes";
		std::error_code ec;
		std::filesystem::create_directories(folder, ec); // where your own go, there to be found over FTP
		m_themes = ui::AllThemes(folder);
		const ui::Theme theme = ui::FindTheme(m_settings.ui.theme, folder);
		ui::ApplyTheme(theme);
		ps5log::Line("[ui] theme: {}", theme.id.empty() ? std::string("Link Jack") : theme.id);
	}

	void Shell::SwitchSide(System side)
	{
		if (side == m_side)
			return;
		m_feedback.Play(ui::Cue::Select);
		TermLog(std::string("ssh ") + Lower(SideTitle(side))); // Terminal's log
		// where the side being left was, for coming back to it
		m_sideFocus[SideIndex()] = LibraryGame() >= 0 ? m_games[LibraryGame()].entry.game.titleId : 0;
		// the same tab (Eden: the same section) on the other side
		const ScreenId tab = m_screen == ScreenId::Settings						 ? ScreenId::Settings :
			SectionLayout() && (m_screen == ScreenId::Home || m_screen == ScreenId::About) ? m_screen :
																						  ScreenId::Library;
		OpenSide(side, false);
		m_railFocus = false;
		ShowTab(tab);
		if (tab == ScreenId::Settings)
		{
			// the same page where the side has it
			const auto pages = SettingsPages();
			if (std::none_of(pages.begin(), pages.end(), [this](const SettingsPage& page) { return page.id == m_page; }))
				m_page = pages.front().id;
		}
		// the focus on the side's games (or pages)
		m_onBar = false;
		m_barFocus = SideIndex();
	}

	void Shell::MakeGame(Game& game, const ps5catalog::Entry& entry)
	{
		game.entry = entry;
		game.boxArt = ps5boxart::Path(BoxOf(entry.game), entry.game.gameId);
		game.known = ps5gameinfo::Find(BoxOf(entry.game), entry.game.gameId, game.info);
		// the list names games in English: GameTDB's title finds one whose own name is in the menus'
		// language (7.7). It has no DS games yet.
		game.report = IsDs() ? nullptr : ps5compat::Find(Is3ds(), entry.game.name);
		if (!game.report && game.known && !IsDs())
			game.report = ps5compat::Find(Is3ds(), game.info.title);
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
			ps5catalog::Replace(CatalogSide(), Is3ds() ? ps5azahar::ListGames() : IsDs() ? ps5melonds::ListGames() : ps5emu::ListGames());
			ps5catalog::Migrate(CatalogSide(), Is3ds() ? m_settings.n3ds.recent : IsDs() ? m_settings.nds.recent : m_settings.recent);
			ps5catalog::Save();
		}
		// what is on screen keeps its place
		if (LibraryGame() >= 0)
			m_libraryFocusTitle = m_games[LibraryGame()].entry.game.titleId;
		m_games.clear();
		for (const ps5catalog::Entry& entry : ps5catalog::Games(CatalogSide()))
		{
			if (entry.game.nds != IsDs())
				continue; // a DS game the 3DS side listed before the DS had a side, until its scan replaces it
			m_games.emplace_back();
			MakeGame(m_games.back(), entry);
		}
		m_boxArrivals = ps5boxart::Arrivals();
		if (!m_scanning && CoreReady())
		{
			m_listed = true;
			ps5log::Line("[launcher] {} games", m_games.size());
			FetchBoxArt();
			RememberCount();
		}
		m_libraryIndex = -1;
		LibraryRefresh();
	}

	void Shell::PollCore()
	{
		bool ok = false;
		std::string error;
		if (!ps5emu::CoreStarted(ok, error))
			return;
		m_status.coreStarting = false;
		if (!ok)
		{
			m_status.notice = TrF("Cemu did not start: {0}", error);
			ps5log::Line("[launcher] {}", m_status.notice);
		}
		else
		{
			m_status.coreReady = true;
			ps5emu::LogMemory(); // Cemu's start, against the 3DS side's
			// A and B chosen for every side before Cemu was up (Make it yours): the Wii U players' now
			if (m_settings.wiiuFaceLayout)
			{
				for (int player = 0; player < ps5pad::kMaxPlayers; player++)
					ps5emu::SetFaceLayout(player, m_settings.wiiuFaceLayout == 1 ? ps5emu::FaceLayout::Nintendo : ps5emu::FaceLayout::PlayStation);
				m_settings.wiiuFaceLayout = 0;
				SaveSettings();
			}
			UseOwnAccount(); // One account for each PS5 user: whoever started the app plays on theirs
		}
		// the side's games, now from Cemu's own scan (the catalogue's were shown meanwhile)
		if (IsWiiU())
			LoadGames();
	}

	void Shell::PollGames()
	{
		if (m_scanning && !SystemScanning())
			LoadGames();
		if (ps5boxart::Arrivals() != m_boxArrivals)
		{
			m_boxArrivals = ps5boxart::Arrivals();
			// a cover downloaded again (Settings > Online) is read again; a new one is shown
			for (const std::string& arrived : ps5boxart::TakeArrived())
			{
				const size_t slash = arrived.find('/');
				const std::string id = slash == std::string::npos ? arrived : arrived.substr(slash + 1);
				for (Game& game : m_games)
					if (game.entry.game.gameId == id)
					{
						game.boxArt = ps5boxart::Path(BoxOf(game.entry.game), id);
						m_images->Forget(game.boxArt);
					}
			}
			for (Game& game : m_games)
				if (game.boxArt.empty())
					game.boxArt = ps5boxart::Path(BoxOf(game.entry.game), game.entry.game.gameId);
		}
	}

	std::string Shell::IconOf(Game& game)
	{
		if (!game.iconLooked && !m_iconLookedThisFrame)
		{
			// reading a Wii U game's icon mounts the game: one a frame
			game.iconLooked = true;
			m_iconLookedThisFrame = true;
			game.icon = Is3ds()				? ps5azahar::CoverPath(game.entry.game.titleId) :
				IsDs()						? ps5melonds::CoverPath(game.entry.game.titleId) :
				m_status.coreReady			? ps5emu::CoverPath(game.entry.game.titleId) :
											  std::string();
		}
		return game.icon;
	}

	std::string Shell::CoverOf(Game& game)
	{
		return !game.boxArt.empty() ? game.boxArt : IconOf(game);
	}

	std::string Shell::BackdropOf(Game& game)
	{
		if (IsWiiU() && !game.bootScreenLooked && !m_iconLookedThisFrame && m_status.coreReady)
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
		if (!IsWiiU())
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
		// the side's covers, from GameTDB's covers of its system
		std::vector<std::string> ids;
		for (const Game& game : m_games)
			if (!game.entry.game.gameId.empty())
				ids.push_back(game.entry.game.gameId);
		ps5boxart::Fetch(BoxSide(m_side), ids);
	}

	void Shell::RememberCount()
	{
		int& count = IsWiiU() ? m_settings.gameCount : Handheld(m_side).gameCount;
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
		ps5settings::AddRecent(LastGame(m_side), IsWiiU() ? m_settings.recent : Handheld(m_side).recent, game.entry.game.titleId);
		SaveSettings();
		ps5catalog::Played(CatalogSide(), game.entry.game.titleId, std::time(nullptr));
		game.entry.lastPlayed = std::time(nullptr);
		ps5catalog::Save();
		// from where the game was in focus (Eden's covers say where), else the middle
		Box from = SectionLayout() && m_edenLaunchFrom.w > 0 ? m_edenLaunchFrom : Box{};
		if (from.w <= 0)
			from = {760, 240, 400, 560};
		LaunchGame(game.entry.game, IsDs() ? Tr("Starting melonDS") : Is3ds() ? Tr("Starting Azahar") : Tr("Starting Cemu"), from);
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
		ps5log::Line("[launcher] {} chosen on the {} side", game.name, SideTitle(m_side));
		TermLog(fmt::format("run \"{}\"", game.name));
	}

	void Shell::Toast(const std::string& text)
	{
		m_toasts.push_back({text, m_now});
		if (m_toasts.size() > 3)
			m_toasts.erase(m_toasts.begin());
		m_feedback.Play(ui::Cue::Notify);
	}

	void Shell::AddTile(Game& game)
	{
		// the box art, else the icon: made square for the tile
		std::string cover = game.boxArt;
		if (cover.empty() || !IsFile(cover))
			cover = IconOf(game);
		ps5forwarders::Start({game.entry.game.name, game.entry.game.path.string(), cover});
		m_feedback.Play(ui::Cue::Select);
		Toast(TrF("Making {0}'s home screen tile…", game.entry.game.name));
	}

	void Shell::RemoveTile(Game& game)
	{
		ps5forwarders::StartRemove({game.entry.game.name, game.entry.game.path.string(), {}});
		m_feedback.Play(ui::Cue::Select);
		Toast(TrF("Removing {0}'s home screen tile…", game.entry.game.name));
	}

	void Shell::PollTiles()
	{
		const ps5forwarders::Status status = ps5forwarders::GetStatus();
		if (status.finished == m_tilesFinished)
			return;
		m_tilesFinished = status.finished;
		if (status.state == ps5forwarders::Status::State::Failed)
		{
			m_feedback.Play(ui::Cue::Denied);
			OpenHelp(status.removed ? Tr("The tile could not be removed") : Tr("The tile could not be made"), status.message);
			return;
		}
		if (status.removed)
		{
			m_feedback.Play(ui::Cue::Notify);
			OpenHelp(Tr("The tile is removed"),
				Tr("Its folder is gone from /data/homebrew. If its picture stays on the home screen, delete it there (Options, then "
				   "Delete), or restart ShadowMount Plus."));
			return;
		}
		// tr: {0} is the tile's folder, /data/homebrew/PPSA99xxx
		OpenHelp(status.replaced ? Tr("The tile is made again") : Tr("The tile is made"),
			TrF("It is in {0}. ShadowMount Plus puts it on the PS5's home screen as it does any homebrew: if it does not show, "
				"restart ShadowMount Plus or the console. The tile starts the game in PS5CEMU-HAR, and closing the game closes the app.",
				status.folder));
	}

	void Shell::PollAutoLaunch()
	{
		if (m_status.autoLaunch.empty() || m_launch.active || m_done)
			return;
		const std::string rom = m_status.autoLaunch;
		auto give = [&](const std::string& why) {
			m_status.autoLaunch.clear();
			ps5log::Line("[launcher] the tile's game did not start: {}", why);
			OpenHelp(Tr("The tile's game could not start"), why);
		};
		if (m_side != m_status.autoLaunchSide)
		{
			m_status.autoLaunch.clear(); // another side chosen meanwhile
			return;
		}
		if (!Notice().empty())
		{
			give(Notice());
			return;
		}
		if (!CoreReady())
			return; // Cemu still starting
		// by its path, or by its file's name in this side's library (the format's --rom allows either)
		const bool named = rom.find('/') == std::string::npos;
		for (int i = 0; i < (int)m_games.size(); i++)
		{
			const std::filesystem::path& path = m_games[i].entry.game.path;
			if (named ? path.filename().string() == rom : path.string() == rom)
			{
				m_status.autoLaunch.clear();
				ps5log::Line("[launcher] starting the tile's game: {}", m_games[i].entry.game.name);
				Launch(i);
				return;
			}
		}
		if (m_listed && !m_scanning && !SystemScanning())
			give(TrF("It is not in the library: {0}", rom));
	}

	// -- screens and the bar ------------------------------------------------------------------------

	void Shell::Show(ScreenId screen)
	{
		// Settings' dialog closing onto the screen it was over (6.5): that screen was there all along
		// (Eden's Settings are a section of their own: the screen arrives)
		const bool under = !SectionLayout() && m_screen == ScreenId::Settings && (screen == ScreenId::Library || screen == ScreenId::Hub);
		if (screen != m_screen)
			m_screenAt = under ? m_now - 10 : m_now;
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
		if (tab == ScreenId::Settings)
			m_onBar = false; // the dialog has the focus: the bar is under its dim
		Show(tab);
	}

	void Shell::NextTab(int direction)
	{
		// two tabs, Library and Settings: either way goes to the other
		(void)direction;
		ShowTab(m_tab == ScreenId::Library ? ScreenId::Settings : ScreenId::Library);
		m_barFocus = m_tab == ScreenId::Library ? 3 : 4;
		m_feedback.Play(ui::Cue::Focus);
	}

	void Shell::FocusBar()
	{
		if (SectionLayout() && EdenSectionIndex() >= 0)
		{
			// Eden's bar is its rail, on the section shown
			m_railFocus = true;
			m_railIndex = std::max(0, EdenSectionIndex());
			m_onBar = false;
			m_feedback.Play(ui::Cue::Focus);
			return;
		}
		m_onBar = true;
		m_barFocus = m_tab == ScreenId::Library ? 3 : 4;
		m_feedback.Play(ui::Cue::Focus);
	}

	// The bar: the three sides (m_barFocus 0 to 2), then the Library and Settings tabs (3 and 4)
	bool Shell::BarUpdate(const ui::Press& press)
	{
		if (InGameSettings())
			return false; // a game's settings have no tabs, nor sides
		if (press.button == Button::L1 || press.button == Button::R1)
		{
			NextTab(press.button == Button::R1 ? 1 : -1);
			return true;
		}
		if (press.button == Button::Touchpad)
		{
			if (SidesShown() > 1)
				SwitchSide(NextShownSide(m_side));
			else
				m_feedback.Play(ui::Cue::Denied);
			return true;
		}
		if (!m_onBar)
			return false;
		switch (press.button)
		{
		case Button::Left:
		case Button::Right:
		{
			const int step = press.button == Button::Right ? 1 : -1;
			int next = m_barFocus + step;
			while (next >= 0 && next < 3 && !SideShown((System)next)) // past the sides not shown
				next += step;
			if (next < 0 || next > 4)
			{
				m_feedback.Play(ui::Cue::Edge, press.repeat);
				break;
			}
			m_barFocus = next;
			// onto a tab opens it, as the PS5's bar does
			if (next == 3 && m_tab != ScreenId::Library)
				ShowTab(ScreenId::Library);
			else if (next == 4 && m_tab != ScreenId::Settings)
				ShowTab(ScreenId::Settings);
			m_feedback.Play(ui::Cue::Focus);
			break;
		}
		case Button::Cross:
			if (m_barFocus < 3 && m_barFocus != SideIndex())
				SwitchSide((System)m_barFocus);
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
			if (m_tab != ScreenId::Library)
			{
				ShowTab(ScreenId::Library);
				m_barFocus = 3;
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
		// each side's square with its console's glyph in it: the GamePad on blue, the 3DS on gold, the DS
		// on green (the app's own tiles, tools/render-icons.py), plain colour until the picture has loaded
		static const std::string kTiles[3] = {ps5paths::Assets() + "/ui/icons/ps5cemu-72.tga", ps5paths::Assets() + "/ui/icons/azahar-72.tga",
			ps5paths::Assets() + "/ui/icons/melonds-72.tga"};
		auto tile = [&](int side, const Box& box, float radius, uint32_t tint) {
			const ui::Picture& picture = m_images->Get(kTiles[side]);
			if (picture.texture)
				canvas.Image(picture.texture, box, radius, tint);
			else
				canvas.Rect(box, radius, ui::SetAlpha(AccentOf((System)side), ((tint >> 24) & 0xff) / 255.0f));
		};
		// the mark: the three sides' squares
		tile(0, {90, y + 2, 28, 28}, 7, 0xffffffff);
		tile(1, {110, y + 9, 28, 28}, 7, 0xebffffff);
		tile(2, {99, y + 26, 28, 28}, 7, 0xebffffff);
		// the side switch: Wii U | 3DS | DS, the DS to the right of the 3DS
		float x = 160;
		const ui::TextStyle sideStyle = Style({22, ui::Weight::SemiBold, 1.0f});
		constexpr float kTile = 34;
		float widths[3];
		float pillWidth = 8;
		int shown = 0;
		for (int i = 0; i < 3; i++)
		{
			widths[i] = 12 + kTile + 10 + m_fonts.Width(sideStyle, SideTitle((System)i)) + 20;
			if (SideShown((System)i))
				pillWidth += widths[i] + (shown++ ? 4 : 0);
		}
		const Box pill{x, y, pillWidth, h};
		canvas.Rect(pill, h / 2, Wash(0.06f));
		canvas.Ring(pill, h / 2, 1.5f, kGlassEdge);
		float sx = x + 4;
		for (int i = 0; i < 3; i++)
		{
			if (!SideShown((System)i))
				continue;
			const bool on = i == SideIndex();
			const uint32_t colour = AccentOf((System)i);
			const Box segment{sx, y + 5, widths[i], h - 10};
			if (on)
				canvas.Rect(segment, segment.h / 2, ui::SetAlpha(colour, 0.24f));
			tile(i, {sx + 12, segment.CentreY() - kTile / 2, kTile, kTile}, 9, on ? 0xffffffff : 0x73ffffff);
			const ui::TextBlock text = m_fonts.Layout(sideStyle, SideTitle((System)i));
			canvas.Text(text, sx + 12 + kTile + 10, segment.CentreY() - text.height * 0.5f, on ? kText : Tertiary());
			if (m_onBar && m_barFocus == i)
				Focus(segment, segment.h / 2);
			sx += widths[i] + 4;
		}
		x = pill.Right() + 26;
		if (tabs)
		{
			canvas.Draw(Icon::L1, {x, y + 12, 36, 32}, Tertiary());
			x += 36 + 14;
			// tr: the launcher's tabs
			const char* labels[2] = {TrC("tab", "Library"), TrC("tab", "Settings")};
			for (int i = 0; i < 2; i++)
			{
				const bool selected = (i == 0 && m_tab == ScreenId::Library) || (i == 1 && m_tab == ScreenId::Settings);
				const ui::TextStyle style = Style({26, selected ? ui::Weight::SemiBold : ui::Weight::Medium, 1.0f});
				const float w = m_fonts.Width(style, labels[i]) + 52;
				const Box tab{x, y, w, h};
				if (selected)
					canvas.Rect(tab, h / 2, ui::SetAlpha(kText, 0.94f));
				const ui::TextBlock text = m_fonts.Layout(style, labels[i]);
				canvas.Text(text, x + 26, tab.CentreY() - text.height * 0.5f, selected ? kInk1 : Secondary());
				if (m_onBar && m_barFocus == i + 3)
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
				canvas.Ring(pad, 7, 2, Wash(0.35f));
			const ui::TextBlock text = m_fonts.Layout({14, ui::Weight::Bold, 1.0f}, fmt::format("P{}", player + 1));
			canvas.Text(text, pad.CentreX() - text.width * 0.5f, pad.CentreY() - text.height * 0.5f, player == 0 ? kInk1 : Secondary());
			right -= 44;
		}
		canvas.PopAlpha();
	}

	std::vector<Hint> Shell::Hints() const
	{
		// the touchpad's next side; none when one side is shown (its hint goes: DrawHints leaves out an empty one)
		const std::string other = SidesShown() > 1 ? SystemName(NextShownSide(m_side)) : "";
		if (m_update.open)
			return {};
		// tr: the hints along the bottom: what a button does on this screen, a word or two
		if (m_keyboard.open)
			return {{Icon::Cross, Tr("Type")}, {Icon::Circle, Tr("Delete")}, {Icon::Triangle, Tr("Shift")}, {Icon::Options, Tr("Done")}};
		if (m_picker.open)
			return {{Icon::Cross, Tr("Choose")}, {Icon::Circle, Tr("Cancel")}};
		if (m_help.open)
			return {{Icon::Circle, Tr("Close")}};
		if (m_menu.open)
			return {{Icon::Cross, Tr("Choose")}, {Icon::Circle, Tr("Close")}};
		if (SectionLayout() && EdenSectionIndex() >= 0)
		{
			bool handled = false;
			std::vector<Hint> hints = EdenHints(handled);
			if (handled)
				return hints;
		}
		if (m_onBar && (m_screen == ScreenId::Library || m_screen == ScreenId::Settings) && !InGameSettings())
			return {{Icon::LeftRight, Tr("Move")}, {Icon::Cross, m_barFocus < 3 && m_barFocus != SideIndex() ? Tr("Switch") : Tr("Open")},
				{Icon::Touchpad, other}};
		switch (m_screen)
		{
		case ScreenId::WhatsNew:
			return {{Icon::Cross, Tr("Continue")}};
		case ScreenId::Welcome:
			return {{Icon::LeftRight, Tr("Change")}, {Icon::Triangle, Tr("More about it")},
				{Icon::Circle, m_welcomeThenSetup ? Tr("Continue to the Setup check") : m_welcomeFromSettings ? Tr("Back") : Tr("Continue to the Library")}};
		case ScreenId::Setup:
		{
			std::vector<Hint> hints;
			if (m_setupOnLanguage)
				hints.push_back({Icon::Cross, Tr("Change the language")});
			else if (m_setupRow < (int)m_checks.size() && !m_checks[m_setupRow].action.empty())
				hints.push_back({Icon::Cross, m_checks[m_setupRow].action});
			hints.push_back({Icon::Triangle, Tr("Check again")});
			hints.push_back({Icon::Circle, !m_setupFirst ? Tr("Back") : Tr("Continue to the Library")});
			return hints;
		}
		case ScreenId::Library:
		{
			std::string kicker, title, text;
			std::vector<std::string> buttons;
			if (LibraryState(kicker, title, text, buttons))
				return buttons.empty() ? std::vector<Hint>{{Icon::Touchpad, other}} : std::vector<Hint>{{Icon::Cross, Tr("Choose")}, {Icon::Touchpad, other}};
			if (RecentLayout())
			{
				if (m_oscarZone == 1 && m_libraryIndex < ArticTiles())
					return {{Icon::Cross, Tr("Open")}, {Icon::Touchpad, other}};
				// tr: Up and Down on Meyer Oscar's Library: between its two parts
				if (m_recent.empty()) // nothing played on this side yet: the Library alone
					return {{Icon::Cross, Tr("Play")}, {Icon::Options, Tr("Options")}, {Icon::R2, Tr("Display")}, {Icon::Touchpad, other}};
				return {{Icon::Cross, Tr("Play")}, {Icon::Options, Tr("Options")}, {Icon::UpDown, Tr("Recently played / Library")}, {Icon::Touchpad, other}};
			}
			if (m_libraryZone == 0)
				return {{Icon::LeftRight, Tr("Filters")}, {Icon::Square, Tr("Sort")}, {Icon::Triangle, Tr("Search")}, {Icon::Touchpad, other}};
			if (m_libraryIndex < ArticTiles())
				return {{Icon::Cross, Tr("Open")}, {Icon::Touchpad, other}};
			return {{Icon::Cross, Tr("Play")}, {Icon::Options, Tr("Options")}, {Icon::R2, Tr("Jump by letter")}, {Icon::Touchpad, other}};
		}
		case ScreenId::Hub: return {{Icon::Cross, m_hubAbout ? Tr("Back to the buttons") : Tr("Choose")}, {Icon::Options, Tr("Options")},
			{Icon::UpDown, Tr("Scroll")}, {Icon::Circle, Tr("Back")}};
		case ScreenId::Settings:
			if (InGameSettings())
			{
				if (!m_onRows)
					return {{Icon::UpDown, Tr("Pages")}, {Icon::Cross, Tr("Open")}, {Icon::Circle, Tr("Done")}};
				if (m_page == "gamesaves") // a game's saves: held to move one aside, no defaults
					return {{Icon::Cross, Tr("Choose")}, {Icon::Triangle, Tr("More about it")}, {Icon::Circle, Tr("Back")}};
				return {{Icon::LeftRight, Tr("Change")}, {Icon::Square, Tr("Default")}, {Icon::Triangle, Tr("More about it")}, {Icon::Circle, Tr("Back")}};
			}
			if (!m_onRows)
				return {{Icon::UpDown, Tr("Pages")}, {Icon::Cross, Tr("Open")}, {Icon::Circle, Tr("Close")}, {Icon::Touchpad, other}};
			return {{Icon::LeftRight, Tr("Change")}, {Icon::Triangle, Tr("More about it")}, {Icon::Circle, Tr("Back")}};
		case ScreenId::Packs:
			return m_presetsFocus ?
				std::vector<Hint>{{Icon::Cross, Tr("Choose")}, {Icon::LeftRight, Tr("Change")}, {Icon::Circle, Tr("Back to the packs")}} :
				std::vector<Hint>{{Icon::Cross, Tr("On / off")}, {Icon::LeftRight, Tr("Presets")}, {Icon::Circle, Tr("Back")}};
		case ScreenId::Player:
			return {{Icon::Cross, Tr("Choose")}, {Icon::LeftRight, Tr("Change")}, {Icon::Triangle, Tr("More about it")}, {Icon::Circle, Tr("Back")}};
		case ScreenId::Mapping:
			if (m_capture.active) // the Wii U takes the touchpad's click as a press; a handheld's cancels
				return m_mappingSide == System::WiiU ? std::vector<Hint>{} : std::vector<Hint>{{Icon::Touchpad, Tr("Cancel")}};
			return {{Icon::Cross, Tr("Assign")}, {Icon::Square, Tr("Clear")}, {Icon::Circle, Tr("Back")}};
		case ScreenId::Files:
			if (m_installing)
				return {{Icon::Circle, Tr("Cancel")}};
			return {{Icon::Cross, Tr("Open")}, {Icon::Triangle, m_filesMode == 0 ? Tr("Use this folder") : Tr("Install")}, {Icon::Circle, Tr("Back")}};
		case ScreenId::Artic:
			if (m_articEditing)
				return {{Icon::LeftRight, Tr("Number")}, {Icon::UpDown, Tr("Change")}, {Icon::Cross, Tr("Keep")}};
			return {{Icon::Cross, m_articRow == 0 ? Tr("Edit") : m_articRow == 1 ? Tr("Connect") : Tr("Hold to set up")}, {Icon::Circle, Tr("Back")}};
		case ScreenId::Updates: return UpdatesHints();
		case ScreenId::Shares: return SharesHints();
		case ScreenId::Home:
		case ScreenId::About: break; // Eden's (EdenHints)
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
			if (it->text.empty())
				continue; // a hint with nothing to say (the touchpad's, with one side shown)
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
		// the Setup check belongs to no side: the sides' colours, and the seam
		if (m_screen == ScreenId::Setup || m_screen == ScreenId::Welcome || m_screen == ScreenId::WhatsNew)
		{
			// What's new's seam between its second and third columns of cards, never through one
			DrawBrandBackdrop(canvas, m_screen == ScreenId::WhatsNew ? 1252.0f : 1140.0f);
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
		// a light theme keeps the game's colours faint: its ground stays light under dark text
		const bool light = ui::LightTheme();
		const uint32_t base = ui::Mix(Accent(), kInk0, 0.86f);
		uint32_t glow = m_ambient[0] ? m_ambient[0] : ui::Mix(Accent(), kInk0, 0.55f);
		uint32_t deep = m_ambient[1] ? m_ambient[1] : kInk0;
		if (light)
		{
			glow = ui::Mix(glow, kInk0, 0.7f);
			deep = ui::Mix(deep, kInk0, 0.9f);
		}
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
					canvas.ImageCover(p.texture, p.width, p.height, screen, 0, ui::SetAlpha(0xffffffff, alpha * (light ? 0.3f : 0.42f)));
			};
			picture(m_backdropOld, 1 - pictureMix);
			picture(m_backdrop, pictureMix);
		}
		// the motif: bubbles rising on the Wii U side, waves rolling on the 3DS side, pixels rising on the
		// DS side
		// (the theme's own instead, or none: theme.h)
		switch (ui::CurrentMotif())
		{
		case ui::Motif::Side:
			if (IsWiiU())
				DrawBubbles(canvas, 1);
			else if (Is3ds())
				DrawWaves(canvas, 1);
			else
				DrawPixels(canvas, 1);
			break;
		case ui::Motif::Slashes: DrawSlashes(canvas, 1); break;
		case ui::Motif::Grid: DrawGrid(canvas, 1); break;
		case ui::Motif::Eden: DrawEden(canvas, 1); break;
		case ui::Motif::None: break;
		}
		// scrims (Eden's where its rail is: 6.17), grain and a vignette
		if (SectionLayout() && EdenSectionIndex() >= 0)
			EdenDrawScrims(canvas);
		else
		{
			canvas.LinearGradient(screen, 0, ui::SetAlpha(kInk0, 0.7f), ui::SetAlpha(kInk0, 0), 0, 0, 1300, 0);
			canvas.LinearGradient(screen, 0, ui::SetAlpha(kInk0, 0), ui::SetAlpha(kInk0, 0.8f), 0, 640, 0, 1080);
		}
		if (!m_settings.ui.highContrast)
			canvas.Grain(screen, Wash(0.05f));
		canvas.RadialGradient(screen, 0, 0x00000000, light ? 0x1f000000 : 0x8c000000, 960, 432, 1248, 1026, 0.55f);
		// Eden's motes of light, over the shade so they show
		if (ui::CurrentMotif() == ui::Motif::Eden)
			DrawMotes(canvas, 1);
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

	void Shell::DrawPixels(Canvas& canvas, float alpha)
	{
		// the bubbles' places and speeds, as squares with rounded corners in melonDS's green, rising
		// straight, a third of them (fewer and larger than bubbles: a DS's pixels)
		const bool still = m_settings.ui.reduceMotion;
		for (size_t i = 0; i < m_bubbles.size(); i += 3)
		{
			Bubble& pixel = m_bubbles[i];
			if (!still)
			{
				pixel.y -= pixel.speed * 0.7f * m_dt;
				if (pixel.y < -pixel.radius * 2)
					pixel.y = 1080 + pixel.radius * 2;
			}
			const float size = 8 + pixel.radius * 0.9f;
			const Box square{pixel.x - size / 2, pixel.y - size / 2, size, size};
			canvas.Rect(square, size * 0.22f, ui::SetAlpha(kNds, std::min(1.0f, pixel.alpha * 1.6f) * 0.5f * alpha));
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

	// Phantom's motif: bands at an angle in the side's colour and white, sweeping slowly across, and a few
	// thin slashes, as a game's menu might cut across its screen
	void Shell::DrawSlashes(Canvas& canvas, float alpha)
	{
		const float t = m_settings.ui.reduceMotion ? 0.0f : (float)(m_now - m_start);
		auto band = [&](float x, float width, float slant, uint32_t colour) {
			// a parallelogram: its top edge slant pixels right of its bottom one
			canvas.Triangle(x, 1080, x + slant, 0, x + slant + width, 0, colour);
			canvas.Triangle(x, 1080, x + slant + width, 0, x + width, 1080, colour);
		};
		const uint32_t accent = Accent();
		const float sweep = std::fmod(t * 18.0f, 2600.0f);
		band(1180 - sweep * 0.25f, 420, 520, ui::SetAlpha(accent, 0.20f * alpha));
		band(1460 - sweep * 0.25f, 90, 520, ui::SetAlpha(0xffffffff, 0.07f * alpha));
		band(-600 + sweep, 140, 520, ui::SetAlpha(accent, 0.10f * alpha));
		for (int i = 0; i < 5; i++)
		{
			const float x = std::fmod(300.0f + i * 410.0f + t * (30.0f + i * 7.0f), 2400.0f) - 400.0f;
			band(x, 6 + (i % 2) * 6, 520, ui::SetAlpha(i % 2 ? accent : 0xffffffff, (i % 2 ? 0.30f : 0.10f) * alpha));
		}
	}

	// Terminal's motif: a fine grid in the side's colour, and a bright line scanning down it
	void Shell::DrawGrid(Canvas& canvas, float alpha)
	{
		const uint32_t accent = Accent();
		const uint32_t line = ui::SetAlpha(accent, 0.06f * alpha);
		for (float x = 0; x <= 1920; x += 48)
			canvas.Line(x, 0, x, 1080, 1, line);
		for (float y = 0; y <= 1080; y += 48)
			canvas.Line(0, y, 1920, y, 1, line);
		if (m_settings.ui.reduceMotion)
			return;
		const float y = std::fmod((float)(m_now - m_start) * 90.0f, 1240.0f) - 80.0f;
		canvas.LinearGradient({0, y - 80, 1920, 80}, 0, ui::SetAlpha(accent, 0), ui::SetAlpha(accent, 0.10f * alpha), 0, y - 80, 0, y);
		canvas.Line(0, y, 1920, y, 2, ui::SetAlpha(accent, 0.35f * alpha));
	}

	// Eden's motif, after ProsperoEden 1.001's Canopy (its backdrop's light, drift and motes): the side's
	// light drifting slowly where Canopy's sun is, and the side's own bubbles, waves or pixels faint behind
	// the glass; DrawMotes adds its motes of light over the shade
	void Shell::DrawEden(Canvas& canvas, float alpha)
	{
		const Box screen{0, 0, 1920, 1080};
		const bool still = m_settings.ui.reduceMotion;
		const float t = still ? 0.0f : (float)(m_now - m_start);
		const uint32_t light = ui::Mix(Accent(), 0xffffffff, 0.45f);
		// the light: at Canopy's sun (0.71, 0.39 of the screen), wandering a little
		const float lx = 1363 + std::sin(t * 0.05f) * 60, ly = 422 + std::cos(t * 0.04f) * 30;
		canvas.RadialGradient(screen, 0, ui::SetAlpha(light, 0.34f * alpha), ui::SetAlpha(light, 0), lx, ly, 960, 640);
		canvas.RadialGradient(screen, 0, ui::SetAlpha(0xffffffff, 0.12f * alpha), ui::SetAlpha(0xffffffff, 0), lx, ly, 280, 210);
		// the side's own, faint behind the glass
		if (IsWiiU())
			DrawBubbles(canvas, 0.45f * alpha);
		else if (Is3ds())
			DrawWaves(canvas, 0.5f * alpha);
		else
			DrawPixels(canvas, 0.45f * alpha);
	}

	// 26 motes of light rising, each from its own seed, 14 to 36 pixels a second, swaying and glowing on its
	// own, fading out near the top
	void Shell::DrawMotes(Canvas& canvas, float alpha)
	{
		const bool still = m_settings.ui.reduceMotion;
		const float t = still ? 0.0f : (float)(m_now - m_start);
		const uint32_t light = ui::Mix(Accent(), 0xffffffff, 0.45f);
		auto fraction = [](float x) { return x - std::floor(x); };
		for (int i = 0; i < 26; i++)
		{
			const float a = fraction(std::sin(i * 12.9898f) * 43758.5453f), b = fraction(std::sin(i * 78.233f) * 12543.21f);
			const float speed = 14 + b * 22, size = 3 + a * 5;
			const float y = 1120 - std::fmod(t * speed + b * 1200, 1240.0f);
			const float x = a * 1920 + std::sin(t * 0.6f + i) * 18;
			const float glow = (0.45f + 0.35f * std::sin(t * 1.3f + i * 1.7f)) * std::clamp(y / 540.0f, 0.0f, 1.0f) * alpha;
			// its light, fading out from it, then the mote
			const float reach = size * 4;
			canvas.RadialGradient({x - reach, y - reach, reach * 2, reach * 2}, 0, ui::SetAlpha(light, 0.4f * glow), ui::SetAlpha(light, 0), x, y, reach, reach);
			canvas.Rect({x - size / 2, y - size / 2, size, size}, size / 2, ui::SetAlpha(0xffffffff, 0.85f * glow));
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
		// a game with settings of its own says so
		const bool own = m_settings.games.count(ps5settings::GameKey(ps5launcher::SideName(m_side), g.entry.game.titleId)) > 0;
		m_menu.items.push_back({"settings", own ? Tr("Game settings: custom") : Tr("Game settings")});
		if (g.entry.game.titleId != 0)
			m_menu.items.push_back({"saves", Tr("Saves")}); // its own, by account (6.15)
		if (IsWiiU() && m_status.coreReady)
		{
			m_menu.items.push_back({"packs", Tr("Graphic packs")});
			m_menu.items.push_back({"mods", Tr("Add mods from USB")});
			m_menu.items.push_back({"updates", Tr("Updates and DLC")});
		}
		m_menu.items.push_back({"favourite", g.entry.favourite ? Tr("Not a favourite") : Tr("Favourite")});
		if (ps5forwarders::HasTile(g.entry.game.path.string()))
			m_menu.items.push_back({"untile", Tr("Remove from home screen")});
		else
			m_menu.items.push_back({"tile", Tr("Add to home screen")});
		m_feedback.Play(ui::Cue::Sheet);
	}

	void Shell::OpenNameKeyboard(const std::string& title, const std::string& text, size_t limit, std::function<void(const std::string&)> done)
	{
		OpenKeyboard();
		m_keyboard.naming = true;
		m_keyboard.title = title;
		m_keyboard.text = text;
		m_keyboard.limit = limit;
		m_keyboard.done = std::move(done);
	}

	void Shell::OpenTextKeyboard(const std::string& title, const std::string& text, size_t limit, bool secret,
		std::function<void(const std::string&)> done)
	{
		OpenNameKeyboard(title, text, limit, std::move(done));
		m_keyboard.raw = true;
		m_keyboard.secret = secret;
		m_keyboard.page = ps5keyboard::FirstPage(ps5lang::Current(), true); // an address, a user name, a password
	}

	void Shell::OpenKeyboard()
	{
		m_keyboard.naming = false;
		m_keyboard.raw = false;
		m_keyboard.secret = false;
		m_keyboard.shift = false;
		m_keyboard.page = ps5keyboard::FirstPage(ps5lang::Current(), false);
		m_keyboard.done = nullptr;
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
			const std::vector<ps5keyboard::Page>& pages = KeyboardPages();
			m_keyboard.page = std::clamp(m_keyboard.page, 0, (int)pages.size() - 1);
			const ps5keyboard::Page& page = pages[m_keyboard.page];
			// the page's rows, then Shift, Space, Delete and Done
			const int actionRow = (int)page.rows.size();
			auto rowLength = [&page, actionRow](int row) { return row < actionRow ? (int)page.rows[row].size() : 4; };
			// what was typed, its spaces at the ends left out, to the one who asked (who may open the
			// keyboard again for the next field: the call is made from a copy)
			auto finish = [this] {
				m_keyboard.open = false;
				if (m_keyboard.naming && m_keyboard.done)
				{
					const std::string& text = m_keyboard.text;
					const size_t first = text.find_first_not_of(' '), last = text.find_last_not_of(' ');
					const std::string typed = m_keyboard.raw ? text : first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
					auto done = std::move(m_keyboard.done);
					m_keyboard.done = nullptr;
					done(typed);
				}
			};
			switch (b)
			{
			case Button::R1:
			case Button::L1:
			{
				// the next page, or the one before; the focus where it was, on the keys Shift and the
				// others stay under
				const int count = (int)pages.size();
				m_keyboard.page = (m_keyboard.page + (b == Button::R1 ? 1 : count - 1)) % count;
				const ps5keyboard::Page& next = pages[m_keyboard.page];
				const int rows = (int)next.rows.size();
				m_keyboard.row = m_keyboard.row >= actionRow ? rows : std::min(m_keyboard.row, rows - 1);
				const int length = m_keyboard.row < rows ? (int)next.rows[m_keyboard.row].size() : 4;
				m_keyboard.column = std::min(m_keyboard.column, length - 1);
				m_feedback.Play(ui::Cue::Focus);
				break;
			}
			case Button::Up:
			case Button::Down:
			{
				const int row = m_keyboard.row + (b == Button::Down ? 1 : -1);
				if (row < 0 || row > actionRow)
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
			{
				std::string& text = m_keyboard.naming ? m_keyboard.text : m_search;
				// a name's limit is in characters, however many bytes they take
				auto fits = [this](const std::string& typed) { return !m_keyboard.naming || ps5keyboard::Length(typed) <= m_keyboard.limit; };
				const bool last = m_keyboard.row == actionRow;
				if (!last)
				{
					// a key types what it shows; some change the character before (a kana's marks, a
					// Korean syllable, a Vietnamese tone), and one that changes nothing says so
					const std::string typed =
						ps5keyboard::Typed(text, page, page.rows[m_keyboard.row][m_keyboard.column], m_keyboard.shift);
					if (typed == text || !fits(typed))
					{
						m_feedback.Play(ui::Cue::Denied);
						break;
					}
					text = typed;
				}
				else if (m_keyboard.column == 0)
					m_keyboard.shift = !m_keyboard.shift;
				else if (m_keyboard.column == 1)
				{
					if (!fits(text + ' '))
					{
						m_feedback.Play(ui::Cue::Denied);
						break;
					}
					text += ' ';
				}
				else if (m_keyboard.column == 2)
					text = ps5keyboard::Deleted(text);
				else
					finish();
				m_feedback.Play(ui::Cue::Select);
				if (!m_keyboard.naming)
					LibraryRefresh();
				break;
			}
			case Button::Square:
			{
				std::string& text = m_keyboard.naming ? m_keyboard.text : m_search;
				text = ps5keyboard::Deleted(text);
				m_feedback.Play(ui::Cue::Back);
				if (!m_keyboard.naming)
					LibraryRefresh();
				break;
			}
			case Button::Triangle:
				// Shift, as on the in-game keyboards: capitals and the second symbols until pressed again
				m_keyboard.shift = !m_keyboard.shift;
				m_feedback.Play(ui::Cue::Toggle);
				break;
			case Button::Options:
				// Done, as the hints have it: a name is kept
				finish();
				m_feedback.Play(ui::Cue::Select);
				break;
			case Button::Circle:
			{
				// deletes, as on the in-game keyboards; with nothing left, a fresh press closes the
				// keyboard (a name is left as it was), a held one stops at empty
				std::string& text = m_keyboard.naming ? m_keyboard.text : m_search;
				if (!text.empty())
				{
					text = ps5keyboard::Deleted(text);
					m_feedback.Play(ui::Cue::Back);
					if (!m_keyboard.naming)
						LibraryRefresh();
				}
				else if (!press.repeat)
				{
					m_keyboard.open = false;
					m_feedback.Play(ui::Cue::Back);
				}
				break;
			}
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
				else if (id == "updates") // that game's alone (6.12)
					UpdatesOpen(g.entry.game.titleId, m_screen);
				else if (id == "mods")
					AddMods(g.entry.game.titleId, g.entry.game.name);
				else if (id == "tile")
					AddTile(g);
				else if (id == "untile")
					RemoveTile(g);
				else if (id == "settings")
				{
					m_feedback.Play(ui::Cue::Select);
					GameSettingsOpen(game);
				}
				else if (id == "saves")
				{
					// its game settings, on their Saves page (6.15)
					m_feedback.Play(ui::Cue::Select);
					GameSettingsOpen(game);
					if (InGameSettings())
						m_page = "gamesaves";
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
			// wider, to 900, for a longer language's options (the one in use has a check and "In use" after it)
			float widest = 0;
			for (const std::string& option : m_picker.options)
				widest = std::max(widest, m_fonts.Width(Style(kBodyStyle), option));
			const float inUseWidth = m_fonts.Width(Style(kCaptionStyle), Tr("In use"));
			const float sheetWidth = std::clamp(widest + 40 + 20 + 20 + 56 + std::min(160.0f, inUseWidth) + 40, 620.0f, 900.0f);
			const Box sheet{1920 - kSafeX - sheetWidth, 540 - (visible * rowH + 170) / 2, sheetWidth, visible * rowH + 170};
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
				// the option, and on the one in use a check and "In use" (at most 160 wide) after it
				float labelWidth = row.w - 180;
				if (i == m_picker.active)
				{
					canvas.Draw(Icon::Check, {row.Right() - 46, row.CentreY() - 13, 26, 26}, Accent());
					const ui::TextBlock inUse = canvas.Text(Style(kCaptionStyle), row.Right() - 216, row.CentreY() - 13, Tr("In use"), Secondary(), 160, 1,
						ui::Align::Right);
					labelWidth = std::min(labelWidth, row.w - 20 - 56 - inUse.width - 20);
				}
				canvas.Text(Style(kBodyStyle), row.x + 20, row.CentreY() - 19, m_picker.options[i], focused ? kText : Secondary(), labelWidth, 1);
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
			static const std::map<std::string, Icon> kIcons = {{"play", Icon::Play}, {"hub", Icon::ChevronRight}, {"settings", Icon::Sparkle},
				{"saves", Icon::Save}, {"packs", Icon::Gear}, {"mods", Icon::Usb}, {"updates", Icon::Download}, {"favourite", Icon::Star},
				{"tile", Icon::Home}, {"untile", Icon::Home}};
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
			const std::vector<ps5keyboard::Page>& pages = KeyboardPages();
			const int pageIndex = std::clamp(m_keyboard.page, 0, (int)pages.size() - 1);
			const ps5keyboard::Page& page = pages[pageIndex];
			// as tall as the language's tallest page, its keys as wide as its longest row lets ten keys'
			// width hold them: the sheet stays as it is from page to page
			const float keyH = 56, gap = 10, tenKeys = 92 * 10 + gap * 9;
			const int tallest = ps5keyboard::MostRows(ps5lang::Current());
			const int longest = std::max(10, ps5keyboard::MostKeys(ps5lang::Current()));
			const float keyW = std::floor((tenKeys - gap * (longest - 1)) / longest);
			const float height = 86 + (tallest + 1) * (keyH + gap) + 4;
			const Box sheet{960 - 560, 1080 - 60 - height, 1120, height};
			canvas.PushAlpha(a);
			canvas.PushOffset(0, (1 - a) * 60);
			DrawSheet(canvas, sheet, a);
			std::string typed = m_keyboard.naming ? m_keyboard.text : m_search;
			if (m_keyboard.secret)
			{
				// a password: a dot for each character
				std::string dots;
				for (size_t i = 0; i < ps5keyboard::Length(typed); i++)
					dots += "•";
				typed = dots;
			}
			const std::string empty = m_keyboard.naming ? m_keyboard.title : std::string(Tr("Search"));
			canvas.Draw(m_keyboard.naming ? Icon::Sparkle : Icon::Search, {sheet.x + 36, sheet.y + 30, 30, 30}, Secondary());
			canvas.Text(Style(kHeadingStyle), sheet.x + 80, sheet.y + 26, typed.empty() ? empty : typed + "_", typed.empty() ? Tertiary() : kText,
				sheet.w - 120 - (m_keyboard.raw ? 260 : 0), 1);
			// the page R1 goes to, by the letters it has ("abc", "äöü", "абв", "#+=")
			canvas.Text(Style({22, ui::Weight::Medium, 1.0f}), sheet.Right() - 40 - 240, sheet.y + 34,
				TrF("L1 R1: {0}", pages[(pageIndex + 1) % pages.size()].label), Secondary(), 240, 1, ui::Align::Right);
			const int actionRow = (int)page.rows.size();
			for (int row = 0; row <= actionRow; row++)
			{
				const bool last = row == actionRow;
				const int length = last ? 4 : (int)page.rows[row].size();
				// the last row as wide as ten keys, under the tallest page's rows: Shift 2 keys, Space 4,
				// Delete 2, Done 2
				const float widths[4] = {92 * 2 + gap, 92 * 4 + gap * 3, 92 * 2 + gap, 92 * 2 + gap};
				float total = 0;
				for (int k = 0; k < length; k++)
					total += (last ? widths[k] : keyW) + (k ? gap : 0);
				float x = sheet.CentreX() - total / 2;
				const float y = sheet.y + 86 + (last ? tallest : row) * (keyH + gap);
				for (int k = 0; k < length; k++)
				{
					const float w = last ? widths[k] : keyW;
					const Box key{x, y, w, keyH};
					const bool focused = m_keyboard.row == row && m_keyboard.column == k;
					const bool lit = last && k == 0 && m_keyboard.shift; // Shift held on
					canvas.Rect(key, 12, focused ? kText : lit ? Accent() : Surface2());
					// tr: the on-screen keyboard's keys
					const std::string label = !last ? ps5keyboard::Label(page.rows[row][k], m_keyboard.shift) :
						k == 0 ? std::string(m_keyboard.shift ? Tr("Shift: on") : Tr("Shift")) : k == 1 ? Tr("Space") : k == 2 ? Tr("Delete") : Tr("Done");
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
				canvas.Rect(bar, 6, Wash(0.14f));
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

		// the launch (6.8): the cover to the centre, growing, the rest dimmed, and what is happening; Eden's
		// its own (6.17)
		if (m_launch.active && SectionLayout())
			EdenDrawLaunch(canvas);
		else if (m_launch.active)
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
