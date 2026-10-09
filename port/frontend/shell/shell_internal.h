// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the new launcher's insides (shell.h). One Shell holds the state of every screen; each
// screen's file (home.cpp, library.cpp, hub.cpp, settings.cpp, pages.cpp, setup.cpp) has its
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
#include "../../ui/canvas.h"
#include "../../ui/feedback.h"
#include "../../ui/images.h"
#include "../../ui/input.h"
#include "../../ui/motion.h"
#include "../../ui/qr.h"
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
		Chooser,
		Setup,
		Home,
		Library,
		Hub,
		Settings,
		Packs,
		Player,
		Mapping,
		Files,
		Artic,
	};

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
		std::string help;		 // Triangle's, and the panel's
		bool on = false;		 // Toggle
		int index = 0;			 // Choice, Stepper, Segmented, Swatches: the one in use
		std::vector<std::string> options;
		float fraction = 0;		 // Slider
		bool dimmed = false;
		System side = System::WiiU; // a Link to a page of a side
		bool sided = false;		 // the row belongs to one side's runtime (needs that side open)
	};

	struct SettingsPage
	{
		std::string id, title, subtitle;
		int section; // 0 General, 1 the side's, 2 the other side's, 3 Help
		System side; // sections 1 and 2
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
		void DrawBar(Canvas& canvas, bool tabs);
		void DrawHints(Canvas& canvas, const std::vector<Hint>& hints);
		void DrawOverlays(Canvas& canvas);
		std::vector<Hint> Hints() const;
		double Now() const { return m_now; }
		float Dt() const { return m_dt; }

		// -- sides and games ----------------------------------------------------------------------
		bool Is3ds() const { return m_side == System::N3ds; }
		ps5catalog::System CatalogSide() const { return Is3ds() ? ps5catalog::System::N3ds : ps5catalog::System::WiiU; }
		ps5boxart::System BoxSide(System side) const { return side == System::N3ds ? ps5boxart::System::N3ds : ps5boxart::System::WiiU; }
		bool CoreReady() const { return Is3ds() || m_status.coreReady; }
		const std::string& Notice() const { return Is3ds() ? m_status.notice3ds : m_status.notice; }
		bool SystemScanning() const;
		uint32_t Accent() const { return Is3ds() ? ui::tokens::kN3ds : ui::tokens::kWiiu; }
		uint32_t AccentOf(System side) const { return side == System::N3ds ? ui::tokens::kN3ds : ui::tokens::kWiiu; }
		std::string& GamesFolder(System side) { return side == System::N3ds ? m_settings.n3ds.gamesFolder : m_settings.gamesFolder; }
		void OpenSide(System side, bool first);
		void SwitchSide();
		void LoadGames();
		void PollGames();
		void MakeGame(Game& game, const ps5catalog::Entry& entry);
		std::string IconOf(Game& game);
		std::string CoverOf(Game& game); // box art, else the icon (looked for at most one a frame)
		// The backdrop's picture (7.4): a Wii U game's boot screen (looked for at most one a frame,
		// sharing the icons' budget: each mounts the game), else the box art
		std::string BackdropOf(Game& game);
		const ui::Picture& Cover(Game& game, bool blurred = false);
		std::vector<int> RecentGames() const; // indices, newest first, at most 12
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

		// the side chooser: only with Start on: Ask each time (5.3); nothing else asks which side
		void ChooserUpdate(const ui::Press& press);
		void ChooserDraw(Canvas& canvas);
		void SetupOpen(bool first);
		void SetupUpdate(const ui::Press& press);
		void SetupDraw(Canvas& canvas);
		bool SetupNeeded() const; // a check that fails at start: the Setup check comes first
		void FinishStart();		  // the first Setup check done: the side, or the chooser when asked for
		void DrawBrandBackdrop(Canvas& canvas, float seam); // both sides' colours meeting at the seam

		void HomeUpdate(const ui::Press& press);
		void HomeDraw(Canvas& canvas);
		int HomeGame() const; // the game Home's row has the focus on, or -1

		void LibraryUpdate(const ui::Press& press);
		void LibraryDraw(Canvas& canvas);
		void LibraryRefresh(); // the filter and the sort again
		int LibraryGame() const;
		int LibraryColumns() const;

		void HubOpen(int game, ScreenId from);
		void HubUpdate(const ui::Press& press);
		void HubDraw(Canvas& canvas);

		void SettingsOpen(const std::string& page, bool onRows = false, int row = 0);
		void SettingsUpdate(const ui::Press& press, const ui::Actions& actions);
		void SettingsDraw(Canvas& canvas);
		std::vector<SettingsPage> SettingsPages() const;
		std::vector<Row> SettingRows(const std::string& page);
		std::string PanelText(const std::string& page) const;
		void ChangeSetting(const Row& row, int step, bool cross);
		void HoldDone(const std::string& id);
		std::string PacksStatus() const;
		static std::string AppUpdateStatus();
		void PollPacks();

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
		void FilesOpen(int mode);
		bool BrowseTo(const std::string& folder);
		void FilesUpdate(const ui::Press& press);
		void FilesDraw(Canvas& canvas);
		void PollInstall();
		bool IsFileEntry(int index) const;
		const ps5emu::InstallCandidate& Inspect(const std::string& folder);
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
		uint32_t m_boxArrivals = 0;
		bool m_iconLookedThisFrame = false;

		ScreenId m_screen = ScreenId::Home;
		ScreenId m_tab = ScreenId::Home;
		bool m_onBar = false;
		int m_barFocus = 1; // 0 the side switch, 1 to 3 the tabs
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

		// Home
		struct Card
		{
			std::string title, value, detail, action;
		};
		std::vector<Card> m_cards;
		uint64_t m_cardsFor = ~0ull;
		std::vector<Box> m_rowBoxes;
		std::vector<Box> m_homeButtons;
		int m_homeZone = 0; // 0 the row, 1 the buttons, 2 the cards
		int m_homeIndex = 0, m_homeButton = 0, m_homeCard = 0;
		std::vector<int> m_homeGames;
		std::vector<ui::Spring> m_rowX, m_rowH;

		// Library
		std::vector<int> m_shelf; // indices in m_games, filtered and sorted
		int m_libraryIndex = 0;	  // in m_shelf (and the Artic tile first on the 3DS side)
		int m_libraryZone = 1;	  // 0 the filters, 1 the shelf
		ui::Spring m_libraryScroll;
		std::string m_search;
		uint64_t m_libraryFocusTitle = 0;

		// each side keeps its focus across a switch (5.3): its Home and Library games, by title
		struct SideFocus
		{
			uint64_t homeTitle = 0, libraryTitle = 0;
		} m_sideFocus[2];

		// the hub
		int m_hubGame = -1;
		ScreenId m_hubFrom = ScreenId::Home;
		int m_hubAction = 0;
		bool m_hubAbout = false;
		ui::Spring m_hubScroll;
		float m_hubAboutHeight = 0;

		// Settings
		std::string m_page = "display";
		bool m_onRows = false;
		int m_settingRow = 0;
		ui::Spring m_railScroll;
		std::string m_diagnosticsDone[3];
		std::string m_packsShown;
		bool m_packsReloaded = false;
		bool m_holdFired = false;
		std::string m_message; // a row's outcome
		float m_holdProgress = 0; // a Hold row's, 0 to 1
		bool m_rescan[2] = {};	   // a side's folder changed while the other side was open
		void SetChoice(const std::string& id, int index);
		void DrawScreensPreview(Canvas& canvas, const Box& box);

		// pages
		ScreenId m_pageFrom = ScreenId::Settings; // where Files and Artic Base go back to
		ScreenId m_packsFrom = ScreenId::Home;
		bool m_mapping3ds = false;
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
		int m_articRow = 0, m_articOctet = 3;
		bool m_articEditing = false;
		std::array<int, 4> m_articOctets{192, 168, 1, 2};

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
		bool m_askSide = false; // Start on: Ask each time, at this start
		int m_chooserSide = 0;

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
