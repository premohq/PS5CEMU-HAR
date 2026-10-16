// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the new launcher's insides (shell.h). One Shell holds the state of every screen; each
// screen's file (library.cpp, hub.cpp, settings.cpp, pages.cpp, setup.cpp) has its
// Update, which takes the frame's buttons, and its Draw, which records the frame; shell.cpp has the
// loop, the bar, the hints, the backdrop and the overlays (the dropdown, a setting's help, the Game
// menu, the keyboard, the update sheet, toasts and the launch). widgets.cpp draws the parts they
// share, as the design's tokens say.

#pragma once

#include "../shell.h"
#include "../../app/boxart.h"
#include "../../app/catalog.h"
#include "../../app/compatibility.h"
#include "../../app/gameinfo.h"
#include "../../app/lang.h"
#include "../../app/netfs.h"
#include "../../app/saves.h"
#include "../../ui/canvas.h"
#include "../../ui/feedback.h"
#include "../../ui/images.h"
#include "../../ui/input.h"
#include "../../ui/motion.h"
#include "../../ui/qr.h"
#include "../../ui/theme.h"
#include "../../ui/tokens.h"

#include <fmt/format.h>

#include <array>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps5shell
{
	using System = ps5launcher::System;
	using ui::Box;
	using ui::Button;
	using ui::Canvas;
	using ui::Icon;
	using ps5lang::Tr;
	using ps5lang::TrC;
	using ps5lang::TrF;
	using ps5lang::TrFC;
	using ps5lang::TrMark;
	using ps5lang::TrMarkC;
	using ps5lang::TrP;

	enum class ScreenId
	{
		WhatsNew, // what a version brings, once: on the first start and after an update (whatsnew.cpp)
		Welcome, // Make it yours: a first start's look and feel, or a new version's new options
		Setup,
		Library,
		Hub,
		Settings,
		Packs,
		Player,
		Mapping,
		Files,
		Artic,
		Updates, // Updates and DLC (6.12)
		Shares,	 // Network shares (6.14)
		Home,	 // Eden's (eden.cpp, 6.17): the game to continue, its cover, the recent games
		About,	 // Eden's: who made what, the version, the support card
	};

	struct ShareJob; // shares.cpp: a test or a look around the network, on a thread of its own

	// A game as the shell shows it: the catalogue's entry, and what was looked up for it
	struct Game
	{
		ps5catalog::Entry entry;
		std::string boxArt;		  // GameTDB's cover, when fetched
		std::string icon;		  // the game's own icon, once looked for
		bool iconLooked = false;
		std::string bootScreen;	  // Wii U: its boot screen (bootTvTex), the backdrop, once looked for
		bool bootScreenLooked = false;
		const ps5compat::Report* report = nullptr;
		ps5gameinfo::Info info;
		bool known = false;		  // GameTDB knows it
		int year = 0;
		std::string sortName;	  // lower case, "the" moved out of the way
		int packsOn = -1;		  // Wii U: graphic packs on, once asked
	};

	struct Hint
	{
		Icon icon;
		std::string text;
	};

	// A row of Settings, or of a page's list
	struct Row
	{
		enum class Kind
		{
			Toggle,	   // on or off
			Choice,	   // one of options: Left and Right step, Cross opens the dropdown
			Slider,	   // 0 to 100 in steps
			Stepper,   // one of options, shown as pips
			Segmented, // two or three options side by side
			Swatches,  // the borders
			Action,	   // Cross does it; the value says what came of it
			Hold,	   // Cross held does it (hold to confirm)
			Link,	   // Cross opens a page
		} kind = Kind::Action;
		std::string id;
		std::string label, value;
		std::string description; // one line, under it while focused
		std::string help;		 // Triangle's
		bool on = false;		 // Toggle
		int index = 0;			 // Choice, Stepper, Segmented, Swatches: the one in use
		std::vector<std::string> options;
		float fraction = 0;		 // Slider
		bool dimmed = false;
		bool needsCore = false;	 // a Wii U row Cemu's core must be up for
		std::string setting;	 // a game can have its own of it: its name in ps5cemu.json (game settings)
		bool fromSide = false;	 // game settings: the side's value, the game has none of its own
	};

	struct SettingsPage
	{
		std::string id, title, subtitle;
		Icon icon;
	};

	class Shell
	{
	public:
		Shell(ps5settings::Launcher& settings, ps5launcher::Status& status, const std::function<void(System)>& prepare);
		~Shell();
		Outcome Run(std::optional<ps5launcher::Choice>& choice);

		// -- the frame ------------------------------------------------------------------------
		void Frame();
		void Update(const ui::Actions& actions);
		void Draw(Canvas& canvas);
		void DrawBackdrop(Canvas& canvas);
		void DrawBubbles(Canvas& canvas, float alpha); // the Wii U side's motif, rising
		void DrawWaves(Canvas& canvas, float alpha);   // the 3DS side's, rolling
		void DrawPixels(Canvas& canvas, float alpha);  // the DS side's, rounded pixels rising
		void DrawBar(Canvas& canvas, bool tabs);
		void DrawHints(Canvas& canvas, const std::vector<Hint>& hints);
		void DrawOverlays(Canvas& canvas);
		std::vector<Hint> Hints() const;
		double Now() const { return m_now; }
		float Dt() const { return m_dt; }

		// -- sides and games ----------------------------------------------------------------------
		bool IsWiiU() const { return m_side == System::WiiU; }
		bool Is3ds() const { return m_side == System::N3ds; }
		bool IsDs() const { return m_side == System::Nds; }
		static int SideIndex(System side) { return (int)side; } // 0 Wii U, 1 3DS, 2 DS: the bar's order
		int SideIndex() const { return SideIndex(m_side); }
		static System NextSide(System side) { return (System)((SideIndex(side) + 1) % 3); }
		// the sides the bar offers (Settings > Launcher, ui.sides): at least one
		bool SideShown(System side) const { return m_settings.ui.sides & (1 << SideIndex(side)); }
		int SidesShown() const { return (m_settings.ui.sides & 1) + (m_settings.ui.sides >> 1 & 1) + (m_settings.ui.sides >> 2 & 1); }
		System NextShownSide(System side) const;
		System FirstShownSide() const;
		void SidesChanged(); // a side hidden while open: the first one shown instead
		static const char* SideTitle(System side) { return ps5launcher::SideTitle(side); }
		static const char* SystemName(System side) { return side == System::N3ds ? "Nintendo 3DS" : side == System::Nds ? "Nintendo DS" : "Wii U"; }
		static const char* EmulatorName(System side) { return side == System::N3ds ? "Azahar" : side == System::Nds ? "melonDS" : "Cemu"; }
		ps5catalog::System CatalogSide() const { return (ps5catalog::System)SideIndex(); }
		static ps5boxart::System BoxSide(System side)
		{
			return side == System::N3ds ? ps5boxart::System::N3ds : side == System::Nds ? ps5boxart::System::Nds : ps5boxart::System::WiiU;
		}
		ps5boxart::System BoxOf(const ps5emu::Game&) const { return BoxSide(m_side); }
		bool CoreReady() const { return !IsWiiU() || m_status.coreReady; }
		const std::string& Notice() const { return Is3ds() ? m_status.notice3ds : IsDs() ? m_status.noticeDs : m_status.notice; }
		bool SystemScanning() const;
		uint32_t Accent() const { return AccentOf(m_side); }
		static uint32_t AccentOf(System side) { return side == System::N3ds ? ui::tokens::kN3ds : side == System::Nds ? ui::tokens::kNds : ui::tokens::kWiiu; }
		std::string& GamesFolder(System side)
		{
			return side == System::N3ds ? m_settings.n3ds.gamesFolder : side == System::Nds ? m_settings.nds.gamesFolder : m_settings.gamesFolder;
		}
		const std::string& GamesFolder(System side) const
		{
			return side == System::N3ds ? m_settings.n3ds.gamesFolder : side == System::Nds ? m_settings.nds.gamesFolder : m_settings.gamesFolder;
		}
		// a handheld side's settings (the 3DS's or the DS's: the same shape)
		ps5settings::N3ds& Handheld(System side) { return side == System::Nds ? m_settings.nds : m_settings.n3ds; }
		uint64_t& LastGame(System side) { return side == System::N3ds ? m_settings.n3ds.lastGame : side == System::Nds ? m_settings.nds.lastGame : m_settings.lastGame; }
		void OpenSide(System side, bool first);
		void SwitchSide(System side);
		void LoadGames();
		void PollGames();
		void PollCore(); // Cemu's library, loading beside the launcher (ps5emu::StartCore): ready, or why not
		void MakeGame(Game& game, const ps5catalog::Entry& entry);
		std::string IconOf(Game& game);
		std::string CoverOf(Game& game); // box art, else the icon (looked for at most one a frame)
		// The backdrop's picture (7.4): a Wii U game's boot screen (looked for at most one a frame,
		// sharing the icons' budget: each mounts the game), else the box art
		std::string BackdropOf(Game& game);
		const ui::Picture& Cover(Game& game, bool blurred = false);
		int FindGame(uint64_t titleId) const;
		std::string Byline(const Game& game) const;
		std::string Played(const Game& game) const;
		std::string LastPlayedWords(int64_t when) const;
		void SaveSettings();
		// the language Settings names (or the PS5's) read again, and everything that keeps words said again
		void ApplyLanguage();
		// the PS5's own language first, then every language in its own words; index: the one in use
		std::vector<std::string> LanguageOptions(int& index) const;
		void OpenLanguagePicker(const std::string& kicker);
		void SetLanguage(int index); // LanguageOptions' index: the menus in that language, at once
		void FetchBoxArt();
		void RememberCount();
		void Launch(int index);
		void LaunchGame(const ps5emu::Game& game, const std::string& caption, const Box& from);
		void Toast(const std::string& text);

		// -- shared drawing (widgets.cpp) ----------------------------------------------------------
		ui::TextStyle Style(ui::TextStyle style) const; // larger text, when asked for
		uint32_t Secondary() const;	 // text at 70 %, or full in high contrast
		uint32_t Tertiary() const;
		uint32_t Surface() const;	 // glass, or opaque in high contrast
		uint32_t Surface2() const;
		// the theme's text colour, faint: white over a dark theme, ink over a light one (for a track, a
		// hairline, a chip's ground); what is drawn on a picture stays white instead
		uint32_t Wash(float alpha) const;
		void Focus(const Box& box, float radius, uint32_t glow = 0);
		void DrawFocusRing(Canvas& canvas);
		void Panel(Canvas& canvas, const Box& box, float radius);
		// maxWidth: the button's widest, its label smaller (then cut) in a language that needs more (0: as wide as the label)
		Box PillButton(Canvas& canvas, float x, float y, const std::string& label, bool primary, bool focused, Icon icon = Icon::Play, bool withIcon = false,
			float height = 72, float maxWidth = 0);
		Box IconButton(Canvas& canvas, float x, float y, Icon icon, bool focused, float size = 72);
		float Chip(Canvas& canvas, float x, float y, const std::string& text, const char* kind = "", float height = 38);
		float Badge(Canvas& canvas, float x, float y, System side);
		void DrawCover(Canvas& canvas, Game& game, const Box& box, bool focused, float lift);
		void DrawGameName(Canvas& canvas, Game& game, const Box& box);
		void DrawRow(Canvas& canvas, const Row& row, const Box& box, bool focused);
		void DrawToggle(Canvas& canvas, float x, float y, bool on, bool focused);
		void HoldRing(Canvas& canvas, const Box& box, float progress);
		float Enter(int part) const; // a screen's part, arriving: 0 to 1, 40 ms apart
		void DrawSheet(Canvas& canvas, const Box& box, float appear);
		void Scrim(Canvas& canvas, float alpha);

		// -- screens ---------------------------------------------------------------------------
		void Show(ScreenId screen);
		void ShowTab(ScreenId tab);
		void NextTab(int direction);
		bool BarUpdate(const ui::Press& press);
		void FocusBar();

		// Make it yours: first, a first start's look and feel, else what this
		// version brings that the last did not have
		void WelcomeOpen(bool first);
		void WelcomeUpdate(const ui::Press& press, const ui::Actions& actions);
		void WelcomeDraw(Canvas& canvas);
		std::vector<Row> WelcomeRows();
		void WelcomeDone();
		// What's new (whatsnew.cpp): once a version, before anything else; then what was next
		enum class NewsThen
		{
			Side,
			Welcome,	 // a first start's Make it yours, then the Setup check
			Setup,
			NewOptions, // an update's Make it yours: its new options
		};
		void WhatsNewOpen(NewsThen then);
		void WhatsNewUpdate(const ui::Press& press);
		void WhatsNewDraw(Canvas& canvas);
		void WhatsNewDone();
		// the theme Settings names (ui.theme), made the kit's; the themes the list offers
		void ApplyTheme();
		std::vector<ui::Theme> m_themes;
		void DrawSlashes(Canvas& canvas, float alpha); // Phantom's motif
		void DrawGrid(Canvas& canvas, float alpha);	   // Terminal's
		void DrawEden(Canvas& canvas, float alpha);	   // Eden's (ProsperoEden 1.001's light), under the scrims
		void DrawMotes(Canvas& canvas, float alpha);   // and its motes of light rising, over them

		// -- Eden (eden.cpp): ProsperoEden 1.001's launcher, in each side's colours.
		// A rail of sections at the left (Home, Library, Settings, About; L1 and R1 between them, the sides
		// under them), Home around the game to continue, the Library's covers beside the focused game's
		// details, Settings as a board of tiles whose pages (and a game's settings) are pages of three parts,
		// the launch picture opening into it, and a launch of its own. Every other page is the one every
		// theme has.
		bool EdenLayout() const { return ui::CurrentLayout() == ui::Layout::Eden; }
		// the launchers of sections (Home, Library, Settings, About) beside a rail: Eden, and the experiments
		// Phantom and Terminal (experiments.cpp), which share its navigation and draw every part their own way
		bool PhantomLayout() const { return ui::CurrentLayout() == ui::Layout::Phantom; }
		bool TerminalLayout() const { return ui::CurrentLayout() == ui::Layout::Terminal; }
		// Classic (classic.cpp, 6.20): 2.0.0 B's launcher over the same sections, its header's words for the rail
		bool ClassicLayout() const { return ui::CurrentLayout() == ui::Layout::Classic; }
		bool SectionLayout() const { return EdenLayout() || PhantomLayout() || TerminalLayout() || ClassicLayout(); }
		int SectionColumns() const; // the Library's columns: Eden's grid, the experiments' one-column list

		// -- Phantom (experiments.cpp, 6.18): after a heist game's menus. Black, white and the side's loud
		// colour; slanted name plates; the Library as a list of names beside the focused cover on a burst
		void PhantomPlate(Canvas& canvas, const Box& box, float skew, uint32_t colour); // a parallelogram
		void PhantomBurst(Canvas& canvas, float cx, float cy, float radius, uint32_t colour, float turn); // a spiked star
		float PhantomTag(Canvas& canvas, float x, float y, const std::string& text, uint32_t ground, uint32_t ink, float size = 20);
		void PhantomBackdrop(Canvas& canvas);
		void PhantomDrawRail(Canvas& canvas);
		void PhantomDrawHints(Canvas& canvas, const std::vector<Hint>& hints);
		void PhantomHomeDraw(Canvas& canvas);
		void PhantomLibraryDraw(Canvas& canvas);
		void PhantomSettingsDraw(Canvas& canvas);
		void PhantomAboutDraw(Canvas& canvas);
		void PhantomDrawLaunch(Canvas& canvas);
		void PhantomDrawOpening(Canvas& canvas);
		void PhantomStateDraw(Canvas& canvas, float y); // LibraryState's card, Phantom's way
		void TermStateDraw(Canvas& canvas, const Box& panel);
		Box TermButton(Canvas& canvas, float x, float y, Icon icon, const std::string& label, bool focused, bool primary);
		// -- Terminal (experiments.cpp, 6.19): after a hacker's console. Panels with a title on their edge, a
		// status line, the sections as a file tree, the Library as a table, a log of what was done, scanlines
		void TermPanel(Canvas& canvas, const Box& box, const std::string& title);
		void TermStatus(Canvas& canvas);
		void TermLog(const std::string& line); // a line of the log (what was done), as a command
		void TermBackdrop(Canvas& canvas);
		void TermDrawRail(Canvas& canvas);
		void TermDrawHints(Canvas& canvas, const std::vector<Hint>& hints);
		void TermHomeDraw(Canvas& canvas);
		void TermLibraryDraw(Canvas& canvas);
		void TermSettingsDraw(Canvas& canvas);
		void TermAboutDraw(Canvas& canvas);
		void TermDrawLaunch(Canvas& canvas);
		void TermDrawOpening(Canvas& canvas);
		void TermDrawCrt(Canvas& canvas); // scanlines, a rolling band, the tube's vignette: over everything
		// -- Classic (classic.cpp, 6.20): the launcher of 2.0.0 B
		void ClassicPanel(Canvas& canvas, const Box& box, float radius);
		Box ClassicButton(Canvas& canvas, const Box& box, const std::string& label, bool focused, bool primary);
		void ClassicTitle(Canvas& canvas, const std::string& title, const std::string& copy);
		void ClassicStateDraw(Canvas& canvas, const Box& panel);
		void ClassicBackdrop(Canvas& canvas);
		void ClassicDrawRail(Canvas& canvas);
		void ClassicDrawHints(Canvas& canvas, const std::vector<Hint>& hints);
		void ClassicHomeDraw(Canvas& canvas);
		void ClassicLibraryDraw(Canvas& canvas);
		void ClassicSettingsDraw(Canvas& canvas);
		void ClassicAboutDraw(Canvas& canvas);
		void ClassicDrawLaunch(Canvas& canvas);
		void ClassicDrawOpening(Canvas& canvas);
		std::vector<std::string> m_termLog;
		double m_termLogAt = 0;
		int EdenSectionIndex() const;  // the rail's section the screen is (0 Home, 1 Library, 2 Settings, 3 About), else -1
		bool EdenOnBoard() const;	   // where L1 and R1 go between the sections and Left reaches the rail
		void EdenGo(int section);
		bool EdenBarUpdate(const ui::Press& press);
		std::vector<System> RailSides() const; // the sides shown, as the rail lists them under the sections
		void SideTile(Canvas& canvas, System side, const Box& box, float radius, uint32_t tint); // a side's square, its console's glyph in it
		void EdenDrawRail(Canvas& canvas);
		float EdenDrawClock(Canvas& canvas); // the clock and the controllers at the top right; where they begin
		void EdenDrawTitle(Canvas& canvas, const std::string& title, const std::string& copy);
		void EdenDrawHints(Canvas& canvas, const std::vector<Hint>& hints);
		std::vector<Hint> EdenHints(bool& handled) const;
		void EdenDrawScrims(Canvas& canvas);
		void EdenArrive(Canvas& canvas, int part, float rise); // a part of the screen arriving (pushes an alpha and an offset)
		void EdenLeave(Canvas& canvas);
		int EdenContinueGame() const; // the side's game to continue (the one played last), else -1
		std::vector<int> EdenRecentGames(int continuing) const; // up to four played before it
		int OwnSettings(const Game& game) const; // how many settings the game has of its own
		std::string EdenRunsWith(Game& game);	 // its side, emulator, graphic packs and own settings, in a line
		void EdenHomeUpdate(const ui::Press& press);
		void EdenHomeDraw(Canvas& canvas);
		void EdenStateDraw(Canvas& canvas, float x, float y, float width); // LibraryState's card, beside the rail
		void EdenLibraryUpdate(const ui::Press& press);
		void EdenLibraryDraw(Canvas& canvas);
		void EdenDetailDraw(Canvas& canvas, int game);
		std::vector<SettingsPage> EdenBoardPages() const; // Settings' pages but About (the rail's)
		void EdenBoardUpdate(const ui::Press& press);
		void EdenSettingsDraw(Canvas& canvas);
		void EdenBoardDraw(Canvas& canvas);
		void EdenPageDraw(Canvas& canvas); // a page of Settings, or a game's settings, in three parts
		std::string EdenSummary(const std::string& page); // a page's values in a few words, one page refreshed a frame
		void EdenAboutUpdate(const ui::Press& press);
		void EdenAboutDraw(Canvas& canvas);
		void EdenDrawLaunch(Canvas& canvas);
		void EdenStartOpening();
		bool EdenOpeningUpdate(const ui::Press& press); // a press skips it
		void EdenDrawOpening(Canvas& canvas);
		bool m_railFocus = false;
		int m_railIndex = 0;	   // the sections, then the sides
		ui::Spring m_railOpen;	   // 0 its icons, 1 open with their names (the focus on it)
		ui::Spring m_railLit[4];   // the section shown
		ui::Spring m_edenShade;	   // 0 Home's shade (the artwork clear at the right), 1 an even one
		int m_edenDirection = 1;   // the way a section arrives from: 1 below the last, -1 above
		int m_homeFocus = 0;	   // Play, Game settings, the recent games (2 to 5), View all games (6)
		ui::Spring m_edenScroll;   // the Library's covers
		std::map<std::string, std::string> m_edenSummaries; // the pages' values, by page id ("game:" a game's)
		std::string m_edenSummaryOwner;						// whose they are: the side, and the game
		size_t m_edenSummaryNext = 0;
		Box m_edenLaunchFrom;	   // what Cross would start, where it is drawn this frame: the launch flies from there
		int m_edenDetailGame = -2; // the game the Library's details show, and since when (they fade in)
		double m_edenDetailAt = 0;
		bool m_openingOn = false; // the launch picture opening into the launcher
		double m_openingAt = -1;  // when it began: once its picture is in, the splash let go of
		bool m_splashHidden = false;

		void SetupOpen(bool first);
		void SetupUpdate(const ui::Press& press);
		void SetupDraw(Canvas& canvas);
		bool SetupNeeded() const; // a check that fails at start: the Setup check comes first
		void FinishStart();		  // the first Setup check done: the side's Library
		void DrawBrandBackdrop(Canvas& canvas, float seam); // both sides' colours meeting at the seam

		void LibraryUpdate(const ui::Press& press);
		void LibraryDraw(Canvas& canvas);
		// Meyer Oscar's Library (6.11): Recently played, a row of covers, then the Library, every game as
		// a grid or a list
		bool RecentLayout() const { return ui::CurrentLayout() == ui::Layout::Recent; }
		void RecentRefresh();
		void RecentUpdate(const ui::Press& press);
		void RecentDraw(Canvas& canvas);
		Box CoverBox(Game& game, float x, float bottom, float height, float maxWidth); // the whole box art at its own shape
		void LibraryRefresh(); // the filter and the sort again
		int LibraryGame() const;
		int LibraryColumns() const;
		int LibraryFilters() const; // All, Recently added, Favourites, and Graphic packs on (Wii U)
		int ArticTiles() const { return Is3ds() ? 1 : 0; } // the 3DS side's shelf starts with Artic Base
		// what the Library shows in place of its shelf: a notice, Cemu starting, the scan, no games
		// (6.1, 6.2), its buttons in m_stateButtons; false when the shelf shows
		bool LibraryState(std::string& kicker, std::string& title, std::string& text, std::vector<std::string>& buttons) const;
		void LibraryStateUpdate(const ui::Press& press);
		void LibraryStateDraw(Canvas& canvas);

		void HubOpen(int game, ScreenId from);
		void HubUpdate(const ui::Press& press);
		void HubDraw(Canvas& canvas);

		void SettingsOpen(const std::string& page, bool onRows = false, int row = 0);
		void SettingsUpdate(const ui::Press& press, const ui::Actions& actions);
		void SettingsDraw(Canvas& canvas);
		Box SettingsDialog() const { return {200, 136, 1520, 820}; } // over the Library, on every side and theme (6.5)
		std::vector<SettingsPage> SettingsPages() const; // the side's (or the game's, in game settings)
		std::vector<Row> SettingRows(const std::string& page);
		std::vector<Row> SideRows(const std::string& page, const ps5settings::Launcher& settings);
		std::string PanelText(const std::string& page) const;
		void ChangeSetting(const Row& row, int step, bool cross);
		// a row's value changed: in the settings given (the side's, or a game's view of them)
		bool ApplyRow(ps5settings::Launcher& settings, const Row& row, int step, bool cross);
		void SetRowIndex(ps5settings::Launcher& settings, const std::string& id, int index);
		void ChooseIndex(const Row& row, int index); // a dropdown's choice
		void DrawRemoteScreen(Canvas& canvas, const Box& card); // the remote touch screen's card
		void DrawSupport(Canvas& canvas, const Box& card);		 // About's: the developer's PayPal.me, as a QR code
		void SettingsChanged(const std::string& id); // the side's settings changed: saved, and what they change at once
		void KeepGameSetting(const std::string& setting, const ps5settings::Launcher& view);
		// game settings (6.5): the same pages, scoped to one game
		void GameSettingsOpen(int game);
		void GameSettingsClose();
		bool InGameSettings() const { return m_gameTitle != 0; }
		void ResetGameSetting(const Row& row);
		void HoldDone(const std::string& id);
		std::string PacksStatus() const;
		static std::string AppUpdateStatus();
		void PollPacks();
		// Wii U mods from the USB drives (app/mods.h): titleId and name, the game they were added from
		void AddMods(uint64_t titleId, const std::string& name);
		std::string ModsStatus() const;
		void PollMods();
		// a game's home screen tile (app/forwarders.h)
		void AddTile(Game& game);
		void RemoveTile(Game& game);
		void PollTiles();
		// a home screen tile's game, started once its side has it (Status::autoLaunch)
		void PollAutoLaunch();

		void PacksOpen(int game, ScreenId from);
		void PacksUpdate(const ui::Press& press);
		void PacksDraw(Canvas& canvas);
		void RefreshPacks();
		void SettlePackItem(int direction);
		const ps5emu::GraphicPackInfo* SelectedPack() const;
		void PlayerOpen(int player);
		void PlayerUpdate(const ui::Press& press, const ui::Actions& actions);
		void PlayerDraw(Canvas& canvas);
		void MappingOpen();
		void MappingUpdate(const ui::Press& press);
		void MappingDraw(Canvas& canvas);
		void PollCapture();
		std::vector<ps5emu::ButtonMapping> Mappings() const;
		void SetHandheldMapping(int index, ps5emu::PadInput input);
		void FilesOpen(int mode);
		bool BrowseTo(const std::string& folder);
		// the folder browser's tabs (6.6): the PS5's own storage ("/data"), then each drive plugged in
		std::vector<std::string> BrowseTabs() const;
		int BrowseTab() const; // the one the folder shown is on
		std::map<std::string, std::string> m_tabFolders; // each tab's folder, kept as L1 and R1 go between them
		void FilesUpdate(const ui::Press& press);
		void FilesDraw(Canvas& canvas);
		void PollInstall();
		bool IsFileEntry(int index) const;
		const ps5emu::InstallCandidate& Inspect(const std::string& folder);
		// Updates and DLC (installs.cpp, 6.12): what the drives have to install, by game; game: a base
		// title ID for that game's alone, 0 for every game's
		void UpdatesOpen(uint64_t game, ScreenId from);
		void UpdatesUpdate(const ui::Press& press);
		void UpdatesDraw(Canvas& canvas);
		void PollUpdates();
		void UpdatesSort();
		void UpdatesInstallNext();
		std::vector<Hint> UpdatesHints() const;
		// Network shares (shares.cpp, 6.14): the list, and the sheet that adds or changes one
		void SharesOpen(ScreenId from);
		void SharesEdit(int index); // -1: a new one
		void SharesScan();			// the computers on the network that share files
		void SharesFind();			// the computer's shared folders
		void SharesPoll();
		void SharesSave();
		void SharesRemove();
		std::vector<Row> ShareRows();
		void SharesUpdate(const ui::Press& press, const ui::Actions& actions);
		void SharesDraw(Canvas& canvas);
		std::vector<Hint> SharesHints() const;
		struct Shares
		{
			std::vector<ps5netfs::Share> list;
			std::vector<std::string> status; // each share's last test, in words
			int selected = 0;				 // in list, and Add a share after it
			ScreenId from = ScreenId::Settings;
			bool editing = false;			 // the sheet is open
			int index = -1;					 // the share it changes; -1 a new one
			ps5netfs::Share share;			 // as the sheet has it
			int row = 0;
			std::vector<ps5netfs::Found> found; // the computers that share files
			int foundIndex = 0;
			std::vector<std::string> names; // the computer's shared folders
			int nameIndex = 0;
			std::shared_ptr<ShareJob> scan, find, test;
			bool saving = false; // Save: the test first, then kept
			std::string message;
			bool messageGood = false;
		} m_shares;
		void ArticOpen();
		void ArticUpdate(const ui::Press& press, const ui::Actions& actions);
		void ArticDraw(Canvas& canvas);
		std::string ArticAddress() const;
		void DrawPageHeader(Canvas& canvas, const std::string& kicker, const std::string& title, const std::string& copy);
		void DrawList(Canvas& canvas, const Box& box, int count, int selected, float& scroll, float rowHeight,
			const std::function<void(int, const Box&, bool)>& row);

		// -- overlays ---------------------------------------------------------------------------
		void OpenPicker(const std::string& kicker, const std::string& title, std::vector<std::string> options, int active,
			std::function<void(int)> choose);
		void OpenHelp(const std::string& title, const std::string& text);
		void OpenGameMenu(int game);
		void OpenKeyboard();
		void OpenNameKeyboard(const std::string& title, const std::string& text, size_t limit, std::function<void(const std::string&)> done);
		// text as it is typed: no capitals put in, with capitals and symbols on R1 and L1; secret shows dots
		void OpenTextKeyboard(const std::string& title, const std::string& text, size_t limit, bool secret,
			std::function<void(const std::string&)> done);
		bool OverlayUpdate(const ui::Press& press);

		// -- state ----------------------------------------------------------------------------
		ps5settings::Launcher& m_settings;
		ps5launcher::Status& m_status;
		std::function<void(System)> m_prepare;
		std::optional<System> m_prepared;
		System m_side = System::WiiU;

		ui::Gfx m_gfx;
		ui::Fonts m_fonts;
		uint32_t m_atlasHeight = 0; // as Gfx has it
		std::unique_ptr<ui::Images> m_images;
		ui::Input m_input;
		ui::Feedback m_feedback;
		ui::DrawList m_list;
		double m_start = 0, m_now = 0, m_lastFrame = 0;
		float m_dt = 0;
		uint64_t m_frames = 0;
		bool m_failed = false;	 // the device was lost
		bool m_videoOut = false; // VideoOut was taken (a failure from here needs a fresh process)

		std::vector<Game> m_games;
		bool m_scanning = false;
		bool m_listed = false; // the side's scan has finished: m_games is all it found
		uint32_t m_boxArrivals = 0;
		bool m_iconLookedThisFrame = false;

		ScreenId m_screen = ScreenId::Library;
		ScreenId m_tab = ScreenId::Library;
		bool m_onBar = false;
		int m_barFocus = 1; // 0 the side switch, 1 Library, 2 Settings
		double m_screenAt = -10;  // when the screen was entered (its parts arrive)
		bool m_done = false;	  // a game is chosen and the launch shown
		std::optional<ps5launcher::Choice> m_choice;

		// the focus ring, gliding (x, y, w, h, radius) and what it was asked to be this frame
		ui::Spring m_ring[5];
		bool m_ringShown = false, m_ringAsked = false;
		Box m_ringBox;
		float m_ringRadius = 0;
		uint32_t m_ringGlow = 0;
		float m_ringAlpha = 0;

		// the backdrop: the focused game's colours and picture, changed once the focus rests
		uint32_t m_ambient[2] = {};
		uint32_t m_ambientFrom[2] = {}, m_ambientTo[2] = {};
		ui::Tween m_ambientMix;
		std::string m_backdrop, m_backdropOld;
		ui::Tween m_backdropMix;
		std::string m_backdropWanted;
		uint32_t m_wantedAmbient[2] = {};
		double m_wantedAt = 0;
		struct Bubble
		{
			float x, y, radius, speed, drift, alpha;
		};
		std::vector<Bubble> m_bubbles;
		float m_wavePhase[3] = {};
		ui::Tween m_sideMix; // switching sides: the backdrop cross-fades

		// what the backdrop is asked to show (the focused game's picture and colours)
		void WantBackdrop(Game* game);

		// a screen drawn behind another (the Library under Settings' dialog): it asks for no focus and
		// does not arrive again
		bool m_behind = false;
		// the canvas being drawn (Draw), so the focus ring goes where the focused box is drawn, offsets
		// (a page scrolled, a screen sliding in) included
		Canvas* m_drawing = nullptr;

		// the Library's state card (no games, a notice): its buttons, and the one in focus
		std::vector<Box> m_stateButtons;
		int m_stateButton = 0;

		// Library
		std::vector<int> m_shelf; // indices in m_games, filtered and sorted
		int m_libraryIndex = 0;	  // in m_shelf (and the Artic tile first on the 3DS side)
		int m_libraryZone = 1;	  // 0 the filters, 1 the shelf
		ui::Spring m_libraryScroll;
		std::string m_search;
		uint64_t m_libraryFocusTitle = 0;

		// Meyer Oscar's Library: the recent games (indices in m_games, the last played first), the focus
		// in the row (m_oscarZone 0) or in the library under it (1, m_libraryIndex in m_shelf)
		std::vector<int> m_recent;
		int m_recentIndex = 0;
		int m_oscarZone = 0;
		ui::Spring m_recentScroll;

		// each side keeps its focus across a switch (5.3): its Library game, by title
		uint64_t m_sideFocus[3] = {};

		// the hub
		int m_hubGame = -1;
		ScreenId m_hubFrom = ScreenId::Library;
		int m_hubAction = 0;
		bool m_hubAbout = false;
		ui::Spring m_hubScroll;
		float m_hubAboutHeight = 0;

		// Settings
		std::string m_page = "graphics";
		bool m_onRows = false;
		int m_settingRow = 0;
		ui::Spring m_railScroll;
		std::string m_diagnosticsDone[3];
		std::string m_packsShown;
		bool m_packsReloaded = false;
		uint32_t m_modsFinished = 0;  // the mods' runs answered (ps5mods::Status::finished)
		double m_modsToastAt = 0;	  // when a copy's progress was last said
		uint32_t m_tilesFinished = 0; // the tiles' requests answered (ps5forwarders::Status::finished)
		bool m_holdFired = false;
		std::string m_message; // a row's outcome
		std::map<std::string, int> m_holdChoice; // a held row with choices (Left and Right): the one chosen
		std::vector<ps5saves::Backup> m_backups;  // Accounts and saves: those on the drives, looked for now and then
		double m_backupsAt = -100;
		std::string m_savesMessage;			   // what the last backup or restore came to
		int m_savesFinished = 0;			   // the saves' runs seen ending (ps5saves::Status::finished)
		void UseOwnAccount();				   // One account for each PS5 user: the user who started the app's
		float m_holdProgress = 0; // a Hold row's, 0 to 1
		bool m_rescan[3] = {};	   // a side's folder changed while another side was open
		// game settings: the game's title ID (0: the side's settings), its name, what the side's
		// settings look like with its own over them, and where Circle goes back to
		uint64_t m_gameTitle = 0;
		std::string m_gameName;
		ps5settings::Launcher m_gameView;
		ScreenId m_gameFrom = ScreenId::Library;
		std::string m_sidePage = "graphics"; // the side's page, kept while game settings are open
		// a game's saves (6.15), its game settings' Saves page: the game, its saves and the backups that
		// hold some of them, looked for now and then
		ps5saves::GameRef m_gameRef;
		std::vector<ps5saves::GameSave> m_gameSaves;
		std::vector<ps5saves::Backup> m_gameBackups;
		double m_gameSavesAt = -100;
		std::vector<Row> GameSaveRows();
		std::string SaveLabel(const ps5saves::GameSave& save) const;

		// pages
		ScreenId m_pageFrom = ScreenId::Settings; // where Files and Artic Base go back to
		ScreenId m_packsFrom = ScreenId::Library;
		System m_mappingSide = System::N3ds; // a handheld side's buttons
		void ClosePage(ScreenId to);
		void ApplyCemuOptions();
		int m_packsGame = 0;
		std::vector<ps5emu::GraphicPackInfo> m_packs;
		struct PackItem
		{
			int pack = -1;
			std::string heading;
		};
		std::vector<PackItem> m_packItems;
		int m_packItem = 0;
		bool m_presetsFocus = false;
		int m_presetSelected = 0;
		ui::Spring m_listScroll, m_listScroll2;
		int m_player = 0, m_playerRow = 0;
		int m_mapSelected = 0;
		struct Capture
		{
			bool active = false, released = false;
			double until = 0;
		} m_capture;
		std::string m_mapMessage;
		int m_filesMode = 0; // 0 a games folder, 1 Cemu's install, 2 a CIA
		System m_filesSide = System::WiiU;
		std::string m_browseFolder;
		std::vector<std::string> m_browseEntries;
		int m_browseFiles = 0, m_browseSelected = 0;
		std::string m_filesMessage, m_installFolder, m_installProgress;
		float m_installFraction = -1;
		std::map<std::string, ps5emu::InstallCandidate> m_inspected;
		bool m_installing = false;
		struct Updates
		{
			uint64_t only = 0; // a game's base title ID: its own alone
			ScreenId from = ScreenId::Settings;
			std::vector<ps5emu::Installable> found;
			std::vector<int> rows; // found's, by game
			int selected = 0;
			float scroll = 0;
			bool looking = false;
			std::string where;			   // the folder being looked in
			std::vector<std::string> queue; // folders to install, the first installing
			int queued = 0, finished = 0;  // the run: how many asked for, how many done
			uint64_t runBytes = 0, runDone = 0; // the run's bytes, and those of the installs done
			bool installing = false, stopping = false;
			uint64_t lastCopied = 0;
			double lastAt = 0;
			float speed = 0; // bytes a second, over the last few seconds
			std::string message;
		} m_updates;
		int m_articRow = 0, m_articOctet = 3;
		bool m_articEditing = false;
		std::array<int, 4> m_articOctets{192, 168, 1, 2};

		// Make it yours
		bool m_welcomeFirst = false; // a first start's (else what is new)
		int m_welcomeRow = 0;
		bool m_welcomeThenSetup = false; // the Setup check follows (a first start)
		bool m_welcomeFromSettings = false; // opened from Settings > Launcher: back there when done
		float m_welcomeScroll = 0;
		NewsThen m_newsThen = NewsThen::Side; // what follows What's new

		// Setup check
		bool m_setupFirst = false; // as the app starts, before a side opens
		bool m_setupOnLanguage = false; // the focus on the language, above the checks
		int m_setupRow = 0;
		ScreenId m_setupFrom = ScreenId::Settings; // where Circle goes back to, when not at the start
		struct Check
		{
			std::string id, title, detail, action; // action: what Cross does (empty: nothing)
			int state; // 0 ready, 1 needs a look, 2 optional
			std::string about, aboutTitle;		  // the panel beside the list: what it means, what to do
			std::vector<std::string> chips;		  // what goes there (a games folder's formats)
			std::string guide, guideTitle;		  // the guide's page for it, as the QR code, and what it is
		};
		std::vector<Check> m_checks;
		void RunChecks();
		std::unordered_map<std::string, ui::QrCode> m_qrCodes; // made once each

		// overlays
		struct Picker
		{
			bool open = false;
			std::string kicker, title;
			std::vector<std::string> options;
			int active = -1, selected = 0;
			std::function<void(int)> choose;
			ui::Spring scroll;
			double at = 0;
		} m_picker;
		struct HelpSheet
		{
			bool open = false;
			std::string title, text;
			double at = 0;
		} m_help;
		struct GameMenu
		{
			bool open = false;
			int game = -1;
			int selected = 0;
			std::vector<std::pair<std::string, std::string>> items; // id, label
			double at = 0;
		} m_menu;
		struct Keyboard
		{
			bool open = false;
			int row = 1, column = 0;
			double at = 0;
			// naming something (an account) instead of the Library's search: its text, what the sheet
			// calls it, how long it may be, and what Done does with it (Circle leaves it as it was)
			bool naming = false;
			std::string text, title;
			size_t limit = 0;
			std::function<void(const std::string&)> done;
			// text typed as it is (a user name, a password, a computer's name): its spaces at the ends
			// kept; secret: shown as dots
			bool raw = false, secret = false;
			// the keys shown: capitals (Shift, its key or Triangle), and which of the language's pages (L1,
			// R1: app/keyboard.h), as on the in-game keyboards
			bool shift = false;
			int page = 0;
		} m_keyboard;
		struct UpdateSheet
		{
			bool open = false;
			int choice = 0;
			double at = 0;
		} m_update;
		struct ToastItem
		{
			std::string text;
			double at;
		};
		std::vector<ToastItem> m_toasts;
		struct LaunchState
		{
			bool active = false;
			double at = 0;
			Box from;
			std::string caption, title;
			float aspect = 0.714f;
			int game = -1; // in m_games, drawn as its cover is; -1: a launch of its own (the Home Menu, Artic Base)
		} m_launch;

		// facts and pictures looked up once
		std::unordered_map<std::string, std::string> m_folderCounts;
	};
}
