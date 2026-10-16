// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the Setup check. It answers "why doesn't it work?" in one place: storage, the recompilers, each side's games and keys,
// the 3DS's system files, the DS's BIOS and box art, each with its status, what it means and what to
// do, and a QR code to the guide's page about it. It shows on the first start and whenever storage or
// the recompilers fail at start, then the app opens on a side's Library; Settings > Diagnostics opens
// it at any time. Nothing asks which side (5.3): the app opens on the side last used.

#include "shell_internal.h"
#include "../actions.h"
#include "../../app/paths.h"
#include "../../app/updates.h"
#include "../../azahar/azahar.h"
#include "../../melonds/melonds.h"
#include "../../ps5/privilege.h"

#include <algorithm>
#include <cmath>
#include <sys/statvfs.h>

namespace ps5shell
{
	using namespace ui::tokens;
	using namespace ps5actions;

	namespace
	{
		constexpr float kListTop = 290, kListWidth = 1000, kRowHeight = 82, kRowGap = 8;
		constexpr float kAsideX = 1196, kAsideWidth = 628, kAsidePad = 36;
		constexpr const char* kSite = "github.com/premohq/PS5CEMU-HAR";
		constexpr const char* kRepository = "https://github.com/premohq/PS5CEMU-HAR";

		std::string FreeSpace(const char* path)
		{
			struct statvfs info{};
			if (statvfs(path, &info) != 0 || info.f_frsize == 0)
				return {};
			const double gigabytes = (double)info.f_bavail * (double)info.f_frsize / 1e9;
			// tr: free space on the PS5's storage, as in 412 GB free
			return TrF("{0} GB free", ps5lang::Decimal(gigabytes, gigabytes >= 10 ? 0 : 1));
		}

		// where a folder is, as a person names it: "USB drive 1 / 3DS"
		std::string Place(const std::string& folder)
		{
			for (const std::string& drive : ConnectedDrives())
				if (folder == drive || folder.starts_with(drive + "/"))
				{
					std::string place = DriveName(drive);
					for (char c : folder.substr(drive.size()))
						place += c == '/' ? std::string(" / ") : std::string(1, c);
					return place;
				}
			return folder;
		}

		// the covers fetched so far, both sides'
		int Covers()
		{
			int covers = 0;
			for (const char* side : {"wiiu", "3ds", "ds"})
			{
				bool ok = false;
				for (const std::string& name : ListEntries(std::string(ps5paths::kCovers) + "/boxart/" + side, false, ok))
					covers += Lower(name).ends_with(".tga");
			}
			return covers;
		}
	}

	// -- the Setup check ------------------------------------------------------------------------------

	bool Shell::SetupNeeded() const
	{
		const ps5privilege::Result& privileges = ps5privilege::Current();
		return !privileges.filesystem || (!privileges.jit && !privileges.executable);
	}

	void Shell::SetupOpen(bool first)
	{
		m_setupFirst = first;
		// the first start's first question is the language (the PS5's, unless another is chosen)
		m_setupOnLanguage = first && !m_settings.ui.setupDone;
		if (!first)
			m_setupFrom = m_screen == ScreenId::Settings ? ScreenId::Settings : ScreenId::Library;
		else if (!m_settings.ui.setupDone)
		{
			// shown once (and again whenever storage or the recompilers fail at start)
			m_settings.ui.setupDone = true;
			SaveSettings();
		}
		RunChecks();
		// the focus on the first that needs a look
		m_setupRow = 0;
		for (int i = 0; i < (int)m_checks.size(); i++)
			if (m_checks[i].state == 1)
			{
				m_setupRow = i;
				break;
			}
		m_onBar = false;
		Show(ScreenId::Setup);
	}

	void Shell::FinishStart()
	{
		m_setupFirst = false;
		OpenSide(m_side, true);
	}

	void Shell::RunChecks()
	{
		m_checks.clear();
		const ps5privilege::Result& privileges = ps5privilege::Current();
		const std::string henGuide = std::string(kRepository) + "/blob/HEAD/docs/HEN-SETUP.md";

		// where everything is kept
		Check storage{"storage", Tr("Storage")};
		storage.guide = henGuide;
		// tr: what the QR code opens: "Scan it for the HEN setup guide"
		storage.guideTitle = Tr("the HEN setup guide");
		if (privileges.filesystem)
		{
			const std::string free = FreeSpace(ps5paths::kRoot);
			storage.state = 0;
			storage.detail = Join({Tr("/data is reachable"), free}, " · ");
			storage.aboutTitle = Tr("Where everything is kept");
			storage.about = TrF("Settings, saves, caches and logs live in {0}, which your HEN opens to PS5CEMU-HAR: its app jailbreak, or "
								"elfldr with the helper that comes with the app.",
				ps5paths::kRoot);
		}
		else
		{
			storage.state = 1;
			storage.detail = Tr("/data is out of reach: nothing can be kept");
			storage.aboutTitle = Tr("Let PS5CEMU-HAR out of its sandbox");
			storage.about = Tr("Add PPSA99360 to your HEN's app jailbreak list (etaHEN: its app jailbreak list; OnionHEN: the end of "
							   "exact_title_ids in config.ini, with no comma after it), or have elfldr listening on port 9021. Then start "
							   "PS5CEMU-HAR again.");
		}
		m_checks.push_back(storage);

		// what runs the games' code
		Check recompilers{"recompilers", Tr("Recompilers")};
		recompilers.guide = henGuide;
		recompilers.guideTitle = Tr("the HEN setup guide");
		recompilers.aboutTitle = Tr("What runs the games' code");
		if (privileges.jit || privileges.executable)
		{
			recompilers.state = 0;
			recompilers.detail = privileges.jit ? Tr("JIT memory from your HEN: games run at full speed") :
												  Tr("Executable memory: games run at full speed, no HEN grant needed");
			recompilers.about = Tr("Cemu's and Azahar's recompilers turn the games' code into the PS5's as they run. They need memory they "
								   "can write code into and run it from: your HEN's JIT memory, or executable memory PS5CEMU-HAR makes itself.");
		}
		else
		{
			recompilers.state = 1;
			recompilers.detail = Tr("No JIT or executable memory: Wii U games run on the much slower interpreter");
			recompilers.about = Tr("Neither your HEN's JIT memory nor executable memory could be made, so Wii U games fall back to Cemu's "
								   "interpreter. Add PPSA99360 to your HEN's app jailbreak list, then start PS5CEMU-HAR again; Settings > "
								   "Diagnostics says what it got.");
		}
		m_checks.push_back(recompilers);

		// each side's games: as its last scan found them, else as its folder holds them
		for (System side : {System::WiiU, System::N3ds, System::Nds})
		{
			if (!SideShown(side))
				continue; // a side the bar does not offer (Settings > Launcher) has nothing to check
			const bool n3ds = side == System::N3ds, nds = side == System::Nds;
			const std::string& folder = GamesFolder(side);
			const int catalogued = (int)ps5catalog::Games((ps5catalog::System)SideIndex(side)).size();
			int count = std::max(catalogued, side == System::WiiU ? m_settings.gameCount : Handheld(side).gameCount);
			bool readable = true;
			if (count <= 0)
			{
				count = n3ds ? Count3dsGames(folder) : nds ? CountDsGames(folder) : CountGames(folder);
				readable = count >= 0;
			}
			// the side's own words, so each keeps its translation
			const char* gamesTitle = n3ds ? Tr("3DS games") : nds ? Tr("DS games") : Tr("Wii U games");
			Check games{std::string(ps5launcher::SideName(side)) + "-games", gamesTitle};
			games.action = Tr("Choose a folder");
			games.state = count > 0 ? 0 : 1;
			const std::string where = ShortPath(Place(folder), 44);
			// tr: {1} is a folder
			games.detail = count > 0 ? TrP(count, "{0} game in {1}", "{0} games in {1}", where) :
				readable		   ? TrF("None in {0} yet", where) :
									 TrF("{0} cannot be read", where);
			games.aboutTitle = n3ds ? Tr("Where your 3DS games go") : nds ? Tr("Where your DS games go") : Tr("Where your Wii U games go");
			games.about = n3ds ? TrF("Put them in {0}, or choose any folder the PS5 can read, such as one on a USB drive. Folders inside it are "
									 "searched too. CIA files are installed, from Settings > Game files on the 3DS side.",
									 folder) :
				nds			   ? TrF("Put them in {0}, or choose any folder the PS5 can read, such as one on a USB drive. Folders inside it are "
									 "searched too.",
									 folder) :
								 TrF("Put them in {0}, or choose any folder the PS5 can read, such as one on a USB drive. Updates and DLC go "
									 "in from Settings > Game files, or come with a .wua.",
									 folder);
			games.chips = n3ds ? std::vector<std::string>{".3ds", ".cci", ".cxi", ".3dsx", ".z3ds", ".cia"} :
				nds			   ? std::vector<std::string>{".nds", ".srl"} :
								 std::vector<std::string>{".wua", ".wud", ".wux", Tr("code · content · meta"), ".rpx"};
			games.guide = std::string(kRepository) + (n3ds ? "#nintendo-3ds" : nds ? "#nintendo-ds-melonds" : "#wii-u");
			games.guideTitle = Tr("the game files page");
			m_checks.push_back(games);
		}

		// the keys encrypted dumps need
		const bool discKeys = IsFile(std::string(ps5paths::kRoot) + "/keys.txt");
		Check wiiuKeys{"wiiu-keys", Tr("Wii U disc keys")};
		wiiuKeys.state = discKeys ? 0 : 1;
		wiiuKeys.detail = discKeys ? Tr("keys.txt found") : Tr("keys.txt is missing: only encrypted .wud and .wux need it");
		wiiuKeys.action = discKeys ? "" : Tr("How to");
		wiiuKeys.aboutTitle = Tr("Keys for encrypted discs");
		wiiuKeys.about = TrF("Encrypted .wud and .wux dumps need their disc keys in {0}/keys.txt, one a line. Decrypted dumps, .wua files and "
							 "folders with code, content and meta need none.",
			ps5paths::kRoot);
		wiiuKeys.guide = std::string(kRepository) + "#wii-u";
		wiiuKeys.guideTitle = Tr("the game files page");
		if (SideShown(System::WiiU))
			m_checks.push_back(wiiuKeys);
		// no 3DS keys to check: Azahar's own are built in (tools/build-azahar.sh), and it plays only
		// decrypted games whatever keys it has

		// the 3DS's own files, which only the Home Menu and a few games need. Azahar looks for them in its
		// own files, which only the 3DS side opens (asked from elsewhere, its logging would start and run
		// on beside a Wii U game): elsewhere, the check says what they are for
		ps5emu::Game homeMenu;
		const bool systemFiles =
			ps5azahar::Available() && m_prepared == System::N3ds && ps5azahar::HomeMenu(m_settings.n3ds.region, homeMenu);
		Check system{"3ds-system", Tr("3DS system files")};
		system.state = systemFiles ? 0 : 2;
		system.detail = systemFiles ? Tr("Artic Setup has run: the Home Menu can start") : Tr("Artic Setup copies them from your 3DS, for the Home Menu");
		system.action = systemFiles || !ps5azahar::Available() ? "" : Tr("Artic Base");
		system.aboutTitle = Tr("Your 3DS's own files");
		system.about = Tr("The 3DS Home Menu, and the few games that need the console's system files, run once Artic Setup has copied them "
						  "from your own 3DS over the network: start the Artic Setup Tool on the 3DS, then Artic Base here.");
		system.guide = std::string(kRepository) + "#nintendo-3ds-azahar";
		system.guideTitle = Tr("the 3DS page");
		if (SideShown(System::N3ds))
			m_checks.push_back(system);

		// a DS's own BIOS, which DS games run without (melonDS's FreeBIOS) but a few run better with
		const bool dsBios = ps5melonds::OwnBiosFound();
		Check nds{"ds-bios", "DS BIOS"};
		nds.state = dsBios ? 0 : 2;
		nds.detail = dsBios ? Tr("Your DS's BIOS found: DS games run on it") : Tr("melonDS's built-in BIOS runs DS games; your DS's is optional");
		nds.action = dsBios ? "" : Tr("How to");
		nds.aboutTitle = Tr("Your DS's own BIOS and firmware");
		nds.about = TrF("melonDS runs DS games on its own BIOS (FreeBIOS) and firmware, which most games are happy with. The few that "
								"are not run on your own DS's: bios7.bin, bios9.bin and firmware.bin, dumped from it, in {0}/bios. No BIOS "
								"comes with PS5CEMU-HAR.",
			ps5melonds::kRoot);
		nds.guide = std::string(kRepository) + "#nintendo-ds-melonds";
		nds.guideTitle = Tr("the DS page");
		if (SideShown(System::Nds))
			m_checks.push_back(nds);

		// covers
		Check boxArt{"boxart", Tr("Box art")};
		boxArt.guide = std::string(kRepository) + "#online";
		boxArt.guideTitle = Tr("what PS5CEMU-HAR fetches online");
		boxArt.aboutTitle = Tr("Covers from GameTDB");
		boxArt.about = Tr("The first time a game shows up, its cover comes from GameTDB (art.gametdb.com), by the ID on its box. Without a "
						  "connection the games' own icons are shown, and the next start tries again.");
		const int covers = Covers();
		if (!m_settings.boxArt)
		{
			boxArt.state = 2;
			boxArt.detail = Tr("Off: the games' icons are shown instead");
			boxArt.action = Tr("Turn on");
		}
		else if (ps5boxart::Answered() == 0)
		{
			boxArt.state = 1;
			boxArt.detail = Tr("GameTDB did not answer: the games' icons are shown instead");
		}
		else
		{
			boxArt.state = 0;
			boxArt.detail = ps5boxart::Answered() == 1 ? TrP(covers, "GameTDB is reachable · {0} cover", "GameTDB is reachable · {0} covers") :
				covers > 0								  ? TrP(covers, "On · {0} cover", "On · {0} covers") :
															std::string(Tr("On: covers come as your games show up"));
		}
		m_checks.push_back(boxArt);
	}

	void Shell::SetupUpdate(const ui::Press& press)
	{
		const int count = (int)m_checks.size();
		switch (press.button)
		{
		case Button::Up:
		case Button::Down:
		{
			const bool down = press.button == Button::Down;
			if (m_setupOnLanguage)
			{
				if (down && count > 0)
				{
					m_setupOnLanguage = false;
					m_setupRow = 0;
					m_feedback.Play(ui::Cue::Focus);
				}
				else
					m_feedback.Play(ui::Cue::Edge, press.repeat);
				break;
			}
			const int next = m_setupRow + (down ? 1 : -1);
			if (next < 0)
			{
				m_setupOnLanguage = true; // the language, above the checks
				m_feedback.Play(ui::Cue::Focus);
			}
			else if (next >= count)
				m_feedback.Play(ui::Cue::Edge, press.repeat);
			else
			{
				m_setupRow = next;
				m_feedback.Play(ui::Cue::Focus);
			}
			break;
		}
		case Button::Cross:
		{
			if (m_setupOnLanguage)
			{
				m_feedback.Play(ui::Cue::Select);
				OpenLanguagePicker(Tr("Setup"));
				break;
			}
			if (m_setupRow >= count || m_checks[m_setupRow].action.empty())
			{
				m_feedback.Play(ui::Cue::Denied);
				break;
			}
			const Check& check = m_checks[m_setupRow];
			m_feedback.Play(ui::Cue::Select);
			if (check.id == "wiiu-games" || check.id == "3ds-games" || check.id == "ds-games")
			{
				m_filesSide = ps5launcher::SideNamed(check.id.substr(0, check.id.find('-')));
				FilesOpen(0); // back here when it is done
			}
			else if (check.id == "3ds-system")
				ArticOpen();
			else if (check.id == "boxart")
			{
				m_settings.boxArt = true;
				ps5boxart::SetEnabled(true);
				SaveSettings();
				RunChecks();
			}
			else
				OpenHelp(check.aboutTitle, check.about);
			break;
		}
		case Button::Triangle:
			RunChecks();
			m_feedback.Play(ui::Cue::Select);
			Toast(Tr("Checked again"));
			break;
		case Button::Circle:
			m_feedback.Play(ui::Cue::Back);
			if (m_setupFirst)
				FinishStart();
			else if (m_setupFrom == ScreenId::Library)
				ShowTab(ScreenId::Library);
			else
				SettingsOpen("diagnostics", true);
			break;
		default: break;
		}
	}

	void Shell::SetupDraw(Canvas& canvas)
	{
		if (m_checks.empty())
			RunChecks();
		const int count = (int)m_checks.size();
		m_setupRow = std::clamp(m_setupRow, 0, std::max(0, count - 1));

		// what this is, and how far along
		canvas.PushAlpha(Enter(0));
		canvas.Text(Style(kOverlineStyle), kSafeX, 92, m_setupFirst ? Tr("Welcome to PS5CEMU-HAR") : Tr("Help"), Secondary(), 1100, 1);
		canvas.Text(Style({56, ui::Weight::Bold, 1.1f, -0.8f}), kSafeX, 122, Tr("Let’s check your setup"), kText, 1100, 1);
		const int ready = (int)std::count_if(m_checks.begin(), m_checks.end(), [](const Check& check) { return check.state != 1; });
		float x = kSafeX;
		for (int i = 0; i < count; i++, x += 50)
			canvas.Rect({x, 243, 40, 6}, 3, i < ready ? kText : Wash(0.14f));
		// tr: the Setup check's progress: how many checks are ready, and how many need a look
		const std::string progress = ready == count ? TrP(count, "The {0} check is ready.", "All {0} checks are ready.") :
			ready == 0								? TrP(count, "The {0} check needs a look.", "All {0} checks need a look.") :
													  TrP(ready, "{0} of {1} is ready.", "{0} of {1} are ready.", count) + " " +
														  TrP(count - ready, "{0} needs a look.", "{0} need a look.");
		canvas.Text(Style({20, ui::Weight::Medium, 1.0f}), x + 8, 236, progress, Secondary(), kListWidth + kSafeX - x - 8, 1);

		// the language, at the right: the PS5's own, until another is chosen
		{
			const ui::TextStyle style = Style({24, ui::Weight::Medium, 1.0f});
			const std::string name = m_settings.ui.language.empty() ? TrF("{0} · as the PS5", ps5lang::NameOf(ps5lang::Current())) :
																	  std::string(ps5lang::NameOf(ps5lang::Current()));
			const float textWidth = std::min(m_fonts.Width(style, name), 520.0f);
			const float width = 28 + 30 + 14 + textWidth + 14 + 24 + 26;
			const Box pill{1920 - kSafeX - width, 132, width, 64};
			const Box drawn = m_setupOnLanguage ? pill.Scaled(1.03f) : pill;
			canvas.Rect(drawn, drawn.h / 2, m_setupOnLanguage ? Surface2() : Surface());
			canvas.Ring(drawn, drawn.h / 2, 1.5f, kGlassEdge);
			canvas.Draw(Icon::Globe, {drawn.x + 28, drawn.CentreY() - 15, 30, 30}, kText);
			canvas.Text(style, drawn.x + 28 + 30 + 14, drawn.CentreY() - 13, name, kText, textWidth + 1, 1);
			canvas.Draw(Icon::ChevronDown, {drawn.Right() - 26 - 24, drawn.CentreY() - 12, 24, 24}, Secondary());
			if (m_setupOnLanguage)
				Focus(drawn, drawn.h / 2);
		}
		canvas.PopAlpha();

		// the checks: those past the screen's foot scroll with the focus
		canvas.PushAlpha(Enter(1));
		canvas.PushOffset(0, (1 - Enter(1)) * 24);
		constexpr float kListBottom = 1080 - 96;
		const float listHeight = count * (kRowHeight + kRowGap) - kRowGap;
		const float listScroll = std::clamp(m_setupRow * (kRowHeight + kRowGap) - (kListBottom - kListTop) * 0.5f, 0.0f,
			std::max(0.0f, listHeight - (kListBottom - kListTop)));
		canvas.PushClip({0, kListTop - 12, kSafeX + kListWidth + 24, kListBottom - kListTop + 24});
		for (int i = 0; i < count; i++)
		{
			const Check& check = m_checks[i];
			const Box row{kSafeX, kListTop + i * (kRowHeight + kRowGap) - listScroll, kListWidth, kRowHeight};
			const bool focused = i == m_setupRow && !m_setupOnLanguage;
			canvas.Rect(row, kRadiusCard, focused ? Surface2() : Surface());
			if (focused)
				canvas.Rect(row, kRadiusCard, Wash(0.05f));
			// its status
			const uint32_t colour = check.state == 0 ? kGood : check.state == 1 ? kWarn : Tertiary();
			const Box icon{row.x + 22, row.CentreY() - 22, 44, 44};
			canvas.Rect(icon, 22, check.state == 2 ? Wash(0.08f) : ui::SetAlpha(colour, 0.16f));
			canvas.Draw(check.state == 0 ? Icon::Check : check.state == 1 ? Icon::Warning : Icon::Dash, icon.Inset(10), colour);
			// what Cross does, on the right
			float right = row.Right() - 28;
			if (!check.action.empty())
			{
				const ui::TextBlock action = m_fonts.Layout(Style({21, ui::Weight::SemiBold, 1.0f}), check.action, 280, 1);
				right -= action.width;
				canvas.Text(action, right, row.CentreY() - action.height * 0.5f, focused ? kText : Secondary());
				if (focused)
				{
					canvas.Draw(Icon::Cross, {right - 36, row.CentreY() - 13, 26, 26}, kText);
					right -= 36;
				}
				right -= 24;
			}
			// what it is, and what was found
			const float tx = icon.Right() + 22;
			const float optionalWidth = check.state == 2 ? m_fonts.Width(Style({25, ui::Weight::Regular, 1.2f}), Tr("· optional")) + 10 : 0.0f;
			const ui::TextBlock title =
				canvas.Text(Style({25, ui::Weight::SemiBold, 1.2f}), tx, row.y + 11, check.title, kText, std::max(100.0f, right - tx - optionalWidth), 1);
			if (check.state == 2)
				canvas.Text(Style({25, ui::Weight::Regular, 1.2f}), tx + title.width + 10, row.y + 11, Tr("· optional"), Tertiary(),
					std::max(60.0f, right - tx - title.width - 10), 1);
			canvas.Text(Style({20, ui::Weight::Regular, 1.25f}), tx, row.y + 45, check.detail, Secondary(), std::max(100.0f, right - tx), 1);
			if (focused)
				Focus(row, kRadiusCard);
		}
		canvas.PopClip();
		canvas.PopOffset();
		canvas.PopAlpha();
		if (count == 0)
			return;

		// the focused check, beside the list: what it means, what to do, and the guide's page (the
		// language's has none)
		Check language{"language", Tr("Language")};
		language.aboutTitle = Tr("The menus' language");
		language.about = Tr("PS5CEMU-HAR speaks the language your PS5 is set to. Cross chooses another; Settings > Language "
							"changes it at any time.");
		const Check& check = m_setupOnLanguage ? language : m_checks[m_setupRow];
		const float inner = kAsideWidth - 2 * kAsidePad;
		const ui::TextBlock heading = m_fonts.Layout(Style({32, ui::Weight::SemiBold, 1.2f}), check.aboutTitle, inner, 2);
		const ui::TextBlock about = m_fonts.Layout(Style({21, ui::Weight::Regular, 1.55f}), check.about, inner, 9);
		float chipsHeight = 0;
		{
			float cx = 0;
			int lines = check.chips.empty() ? 0 : 1;
			for (const std::string& chip : check.chips)
			{
				const float width = m_fonts.Width(Style({20, ui::Weight::SemiBold, 1.0f}), chip) + 32;
				if (cx > 0 && cx + width > inner)
				{
					cx = 0;
					lines++;
				}
				cx += width + 8;
			}
			chipsHeight = lines ? lines * 38 + (lines - 1) * 8 + 18 : 0;
		}
		const float qr = check.guide.empty() ? 0.0f : 150.0f;
		const float height = kAsidePad + 18 + 10 + heading.height + 14 + about.height + chipsHeight + (qr > 0 ? 28 + 2 + 26 + qr : 0.0f) + kAsidePad;
		const Box aside{kAsideX, kListTop + 10, kAsideWidth, height};
		canvas.PushAlpha(Enter(2));
		canvas.PushOffset((1 - Enter(2)) * 30, 0);
		canvas.Rect(aside, kRadiusSheet, m_settings.ui.highContrast ? kInk2 : ui::SetAlpha(kInk1, 0.75f));
		canvas.Ring(aside, kRadiusSheet, 1.5f, kGlassEdge);
		float y = aside.y + kAsidePad;
		const float ax = aside.x + kAsidePad;
		canvas.Text(Style(kOverlineStyle), ax, y, check.title, Secondary(), inner, 1);
		y += 18 + 10;
		canvas.Text(heading, ax, y, kText);
		y += heading.height + 14;
		canvas.Text(about, ax, y, Secondary());
		y += about.height;
		if (!check.chips.empty())
		{
			y += 18;
			float cx = ax;
			for (const std::string& chip : check.chips)
			{
				const float width = m_fonts.Width(Style({20, ui::Weight::SemiBold, 1.0f}), chip) + 32;
				if (cx > ax && cx + width > ax + inner)
				{
					cx = ax;
					y += 38 + 8;
				}
				Chip(canvas, cx, y, chip);
				cx += width + 8;
			}
			y += 38;
		}
		if (qr <= 0)
		{
			canvas.PopOffset();
			canvas.PopAlpha();
			return;
		}
		y += 28;
		canvas.Rect({ax, y, inner, 1.5f}, 0, kLine);
		y += 2 + 26;
		auto code = m_qrCodes.find(check.guide);
		if (code == m_qrCodes.end())
			code = m_qrCodes.emplace(check.guide, ui::QrCode(check.guide)).first;
		ui::DrawQr(canvas, code->second, {ax, y, qr, qr}, 12);
		const float tx = ax + qr + 24, tw = aside.Right() - kAsidePad - tx;
		const ui::TextBlock guideTitle = m_fonts.Layout(Style({22, ui::Weight::SemiBold, 1.3f}), Tr("The guide, on your phone"), tw, 2);
		// tr: {0} is what the QR code opens ("the HEN setup guide"), {1} the site
		const ui::TextBlock guideText =
			m_fonts.Layout(Style({19, ui::Weight::Regular, 1.5f}), TrF("Scan it for {0}: {1}", check.guideTitle, kSite), tw, 4);
		const float ty = y + (qr - guideTitle.height - 6 - guideText.height) * 0.5f;
		canvas.Text(guideTitle, tx, ty, kText);
		canvas.Text(guideText, tx, ty + guideTitle.height + 6, Secondary());
		canvas.PopOffset();
		canvas.PopAlpha();
	}

	// -- what the Setup check stands on: the sides, meeting at the seam -----------------

	void Shell::DrawBrandBackdrop(Canvas& canvas, float seam)
	{
		const Box screen{0, 0, 1920, 1080};
		const bool themed = ui::CurrentMotif() != ui::Motif::Side;
		canvas.LinearGradient(screen, 0, themed ? kInk1 : 0xff160c08, kInk0, 0, 0, 0, 1080);
		// the Wii U's blue far behind the left, the 3DS's gold behind the right, as the banner has them, and
		// the DS's green rising from below the middle (the theme's colours for them)
		const uint32_t wiiu = themed ? kWiiu : 0xffdf9f3b;
		canvas.RadialGradient(screen, 0, ui::SetAlpha(wiiu, 0.30f), ui::SetAlpha(wiiu, 0), 0.18f * 1920, 0.40f * 1080, 605, 529);
		canvas.RadialGradient(screen, 0, ui::SetAlpha(kN3ds, 0.20f), ui::SetAlpha(kN3ds, 0), 0.88f * 1920, 0.60f * 1080, 605, 529);
		canvas.RadialGradient(screen, 0, ui::SetAlpha(kNds, 0.14f), ui::SetAlpha(kNds, 0), 0.5f * 1920, 1.05f * 1080, 700, 420);
		if (themed)
		{
			// a theme's own motif across the whole screen, no seam
			if (ui::CurrentMotif() == ui::Motif::Slashes)
				DrawSlashes(canvas, 0.8f);
			else if (ui::CurrentMotif() == ui::Motif::Grid)
				DrawGrid(canvas, 0.8f);
			else if (ui::CurrentMotif() == ui::Motif::Eden)
				DrawEden(canvas, 0.8f);
		}
		else
		{
			// bubbles on the one side of the seam, waves on the other
			canvas.PushClip({0, 0, seam, 1080});
			DrawBubbles(canvas, 0.6f);
			canvas.PopClip();
			canvas.PushClip({seam, 0, 1920 - seam, 1080});
			DrawWaves(canvas, 0.8f);
			canvas.PopClip();
			canvas.LinearGradient({seam - 1, 0, 2, 540}, 0, 0x00ffffff, 0x38ffffff, seam, 0, seam, 540);
			canvas.LinearGradient({seam - 1, 540, 2, 540}, 0, 0x38ffffff, 0x00ffffff, seam, 540, seam, 1080);
		}
		if (!m_settings.ui.highContrast)
			canvas.Grain(screen, 0x0cffffff);
		canvas.RadialGradient(screen, 0, 0x00000000, 0x8c000000, 960, 432, 1248, 1026, 0.55f);
	}
}
