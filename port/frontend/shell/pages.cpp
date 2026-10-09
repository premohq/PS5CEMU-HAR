// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the pages kept from the classic launcher, rebuilt (docs/UI-REDESIGN.md, 6.6), each a
// list on the left and its detail on the right: a game's graphic packs (Cemu's folder tree, on and
// off, presets as dropdowns, "choosing one turns it on"), a player's controls and buttons (the
// mapping waits for a DualSense input), the folder browser for games and installs (drives first,
// what each side would find in a folder, the install's measured progress), and Artic Base.

#include "shell_internal.h"
#include "../actions.h"
#include "../../app/emulator.h"
#include "../../app/paths.h"
#include "../../azahar/azahar.h"
#include "../../azahar/controls.h"
#include "../../azahar/library.h"
#include "../../ps5/kernel.h"
#include "../../ps5/pad.h"

#include <algorithm>
#include <cmath>

namespace ps5shell
{
	using namespace ui::tokens;
	using namespace ps5actions;

	namespace
	{
		constexpr float kListTop = 240, kListBottom = 1000;
		constexpr float kListX = 96, kListWidth = 800;
		constexpr float kDetailX = 980, kDetailWidth = 844;
		constexpr double kCaptureSeconds = 6.0; // how long a mapping waits for a press

		enum PlayerRow
		{
			kRowType,
			kRowMotion,
			kRowRumble,
			kRowLeftDeadzone,
			kRowRightDeadzone,
			kRowButtons,
			kRowReset,
			kPlayerRows,
		};

		enum ArticRow
		{
			kRowArticAddress,
			kRowArticConnect,
			kRowArticSetupOld,
			kRowArticSetupNew,
			kArticRows,
		};
	}

	void Shell::ClosePage(ScreenId to)
	{
		m_feedback.Play(ui::Cue::Back);
		if (to == ScreenId::Settings)
		{
			m_tab = ScreenId::Settings;
			Show(ScreenId::Settings);
		}
		else if (to == ScreenId::Hub)
			Show(ScreenId::Hub);
		else if (to == ScreenId::Setup)
		{
			RunChecks();
			Show(ScreenId::Setup);
		}
		else
			ShowTab(to);
	}

	void Shell::ApplyCemuOptions()
	{
		ps5emu::ApplyOptions({m_settings.gamesFolder, m_settings.overlay, m_settings.volume, m_settings.upscaleFilter, m_settings.asyncShaders,
			m_settings.gamePadSpeaker});
	}

	void Shell::DrawPageHeader(Canvas& canvas, const std::string& kicker, const std::string& title, const std::string& copy)
	{
		canvas.PushAlpha(Enter(0));
		canvas.Text(Style(kOverlineStyle), kSafeX, 64, kicker, Secondary(), 1500, 1);
		canvas.Text(Style(kTitleStyle), kSafeX, 94, title, kText, 1500, 1);
		// on one line, a little smaller if need be; a longer language's on two smaller ones, in the same room
		const ui::TextStyle one = Style({24, ui::Weight::Regular, 1.3f});
		if (canvas.GetFonts().Width(one, copy) * one.minScale <= 1500)
			canvas.Text(one, kSafeX, 158, copy, Secondary(), 1500, 1);
		else
			canvas.Text(Style({20, ui::Weight::Regular, 1.12f}), kSafeX, 151, copy, Secondary(), 1500, 2);
		canvas.PopAlpha();
	}

	void Shell::DrawList(Canvas& canvas, const Box& box, int count, int selected, float& scroll, float rowHeight,
		const std::function<void(int, const Box&, bool)>& row)
	{
		const int visible = std::max(1, (int)(box.h / rowHeight));
		const float target = std::clamp((selected - visible / 2) * rowHeight, 0.0f, std::max(0.0f, count * rowHeight - box.h));
		// a spring per list would do; one shared, eased toward the target, is enough here
		scroll += (target - scroll) * std::min(1.0f, m_dt * (m_settings.ui.reduceMotion ? 60.0f : 14.0f));
		canvas.PushClip(box.Inset(-12, -8));
		for (int i = 0; i < count; i++)
		{
			const Box rowBox{box.x, box.y + i * rowHeight - scroll, box.w, rowHeight - 10};
			if (rowBox.Bottom() < box.y - 10 || rowBox.y > box.Bottom() + 10)
				continue;
			row(i, rowBox, i == selected);
		}
		canvas.PopClip();
		if (count * rowHeight > box.h)
		{
			// where in the list the rows shown are
			const float track = box.h, thumb = std::max(40.0f, track * box.h / (count * rowHeight));
			const float at = (track - thumb) * scroll / std::max(1.0f, count * rowHeight - box.h);
			canvas.Rect({box.Right() + 18, box.y, 4, track}, 2, 0x14ffffff);
			canvas.Rect({box.Right() + 18, box.y + at, 4, thumb}, 2, 0x66ffffff);
		}
	}

	// -- graphic packs -----------------------------------------------------------------------------

	void Shell::RefreshPacks()
	{
		const uint64_t title = m_games[m_packsGame].entry.game.titleId;
		m_packs = ps5emu::ListGraphicPacks(title);
		m_games[m_packsGame].packsOn = ps5emu::EnabledGraphicPackCount(title);
		m_cardsFor = ~0ull;
		const int selectedPack = m_packItem < (int)m_packItems.size() ? m_packItems[m_packItem].pack : -1;
		m_packItems.clear();
		std::vector<std::string> folders;
		for (int i = 0; i < (int)m_packs.size(); i++)
		{
			if (m_packs[i].folder.empty())
				m_packItems.push_back({i, {}});
			else if (std::find(folders.begin(), folders.end(), m_packs[i].folder) == folders.end())
				folders.push_back(m_packs[i].folder);
		}
		for (const auto& folder : folders)
		{
			m_packItems.push_back({-1, folder});
			for (int i = 0; i < (int)m_packs.size(); i++)
				if (m_packs[i].folder == folder)
					m_packItems.push_back({i, {}});
		}
		m_packItem = 0;
		for (int i = 0; i < (int)m_packItems.size() && selectedPack >= 0; i++)
			if (m_packItems[i].pack == selectedPack)
				m_packItem = i;
		SettlePackItem(1);
	}

	void Shell::SettlePackItem(int direction)
	{
		const int count = (int)m_packItems.size();
		for (int tries = 0; tries < count && m_packItems[m_packItem].pack < 0; tries++)
		{
			const int next = m_packItem + direction;
			if (next < 0 || next >= count)
				direction = -direction;
			else
				m_packItem = next;
		}
	}

	const ps5emu::GraphicPackInfo* Shell::SelectedPack() const
	{
		if (m_packItem >= (int)m_packItems.size() || m_packItems[m_packItem].pack < 0)
			return nullptr;
		return &m_packs[m_packItems[m_packItem].pack];
	}

	void Shell::PacksOpen(int game, ScreenId from)
	{
		if (game < 0 || game >= (int)m_games.size() || Is3ds() || !m_status.coreReady)
			return;
		m_packsGame = game;
		m_packsFrom = from;
		m_packItem = 0;
		m_packItems.clear();
		m_presetsFocus = false;
		m_presetSelected = 0;
		m_listScroll.Snap(0);
		RefreshPacks();
		Show(ScreenId::Packs);
	}

	void Shell::PacksUpdate(const ui::Press& press)
	{
		const Button b = press.button;
		const uint64_t title = m_games[m_packsGame].entry.game.titleId;
		if (b == Button::Circle)
		{
			if (m_presetsFocus)
			{
				m_presetsFocus = false;
				m_feedback.Play(ui::Cue::Back);
			}
			else
			{
				LibraryRefresh();
				ClosePage(m_packsFrom == ScreenId::Hub ? ScreenId::Hub : m_packsFrom);
			}
			return;
		}
		const auto* pack = SelectedPack();
		if (!pack)
			return;
		if (!m_presetsFocus)
		{
			const int count = (int)m_packItems.size();
			if (b == Button::Up || b == Button::Down || b == Button::L1 || b == Button::R1 || b == Button::L2 || b == Button::R2)
			{
				const int before = m_packItem;
				const int step = b == Button::Up ? -1 : b == Button::Down ? 1 : b == Button::L1 || b == Button::L2 ? -7 : 7;
				m_packItem = std::clamp(m_packItem + step, 0, count - 1);
				SettlePackItem(step < 0 ? -1 : 1);
				m_feedback.Play(m_packItem == before ? ui::Cue::Edge : ui::Cue::Focus, press.repeat);
			}
			else if (b == Button::Cross)
			{
				ps5emu::ToggleGraphicPack(title, m_packItems[m_packItem].pack);
				RefreshPacks();
				m_feedback.Play(ui::Cue::Toggle);
			}
			else if ((b == Button::Right || b == Button::Square) && !pack->choices.empty())
			{
				m_presetsFocus = true;
				m_presetSelected = 0;
				m_feedback.Play(ui::Cue::Focus);
			}
			return;
		}
		if (pack->choices.empty())
		{
			m_presetsFocus = false;
			return;
		}
		const int choices = (int)pack->choices.size();
		auto choose = [this, title](int choiceIndex, int presetIndex) {
			const auto* selected = SelectedPack();
			if (!selected || choiceIndex >= (int)selected->choices.size())
				return;
			const auto& choice = selected->choices[choiceIndex];
			if (presetIndex < 0 || presetIndex >= (int)choice.presets.size())
				return;
			ps5emu::SetGraphicPackPreset(title, m_packItems[m_packItem].pack, choice.category, choice.presets[presetIndex]);
			RefreshPacks();
		};
		switch (b)
		{
		case Button::Up:
		case Button::Down:
		{
			const int next = m_presetSelected + (b == Button::Down ? 1 : -1);
			if (next < 0 || next >= choices)
				m_feedback.Play(ui::Cue::Edge, press.repeat);
			else
			{
				m_presetSelected = next;
				m_feedback.Play(ui::Cue::Focus);
			}
			break;
		}
		case Button::Cross:
		{
			const auto& choice = pack->choices[std::min(m_presetSelected, choices - 1)];
			const int choiceIndex = m_presetSelected;
			OpenPicker(choice.category.empty() ? std::string(Tr("Preset")) : choice.category, pack->name, choice.presets, choice.active,
				[choose, choiceIndex](int preset) { choose(choiceIndex, preset); });
			break;
		}
		case Button::Left:
		case Button::Right:
		{
			const auto& choice = pack->choices[std::min(m_presetSelected, choices - 1)];
			const int presets = (int)choice.presets.size();
			if (presets > 0)
			{
				choose(m_presetSelected, (choice.active + (b == Button::Right ? 1 : presets - 1)) % presets);
				m_feedback.Play(ui::Cue::Focus);
			}
			break;
		}
		default: break;
		}
	}

	void Shell::PacksDraw(Canvas& canvas)
	{
		Game& game = m_games[m_packsGame];
		WantBackdrop(&game);
		DrawPageHeader(canvas, Tr("Graphic packs"), game.entry.game.name,
			Tr("Cross turns a pack on or off; Right goes to its presets. Choosing a preset turns its pack on. Changes apply at the next start."));
		canvas.PushAlpha(Enter(1));
		int packCount = 0, packNumber = 0;
		for (int i = 0; i < (int)m_packItems.size(); i++)
			if (m_packItems[i].pack >= 0)
			{
				packCount++;
				if (i <= m_packItem)
					packNumber++;
			}
		if (packCount == 0)
		{
			canvas.Text(Style(kHeadingStyle), kListX, kListTop, Tr("No graphic packs for this game"), kText, 1920 - 2 * kSafeX, 1);
			canvas.Text(Style(kBodyStyle), kListX, kListTop + 50, Tr("Settings > Online and updates gets the community's newest."), Secondary(), 800, 2);
			canvas.PopAlpha();
			return;
		}
		// tr: where the focus is in a list: the 3rd of 12
		canvas.Text(Style({20, ui::Weight::Medium, 1.0f, 0, false, true}), kListX + kListWidth, kListTop - 40,
			TrF("{0} of {1}", packNumber, packCount), Tertiary(), 0, 0, ui::Align::Right);
		DrawList(canvas, {kListX, kListTop, kListWidth, kListBottom - kListTop}, (int)m_packItems.size(), m_packItem, m_listScroll.value, 72,
			[&](int i, const Box& box, bool focused) {
				const PackItem& item = m_packItems[i];
				if (item.pack < 0)
				{
					canvas.Text(Style(kOverlineStyle), box.x + 8, box.y + 34, item.heading, Tertiary());
					return;
				}
				const auto& pack = m_packs[item.pack];
				canvas.Rect(box, 16, focused ? Surface2() : Surface());
				canvas.Text(Style(kBodyStyle), box.x + 24, box.CentreY() - 19, pack.name, kText, box.w - 160, 1);
				DrawToggle(canvas, box.Right() - 24 - 64, box.CentreY() - 18, pack.enabled, focused);
				if (focused && !m_presetsFocus)
					Focus(box, 16);
				else if (focused)
					canvas.Ring(box, 16, 2, ui::SetAlpha(Accent(), 0.6f));
			});
		canvas.PopAlpha();

		const auto* pack = SelectedPack();
		if (!pack)
			return;
		canvas.PushAlpha(Enter(2));
		Panel(canvas, {kDetailX, kListTop - 10, kDetailWidth, kListBottom - kListTop + 10}, kRadiusSheet);
		const float x = kDetailX + 40;
		float y = kListTop + 26;
		canvas.Text(Style(kOverlineStyle), x, y, pack->folder.empty() ? std::string(Tr("Graphic pack")) : Tr("Graphic pack") + std::string(" · ") + pack->folder,
			Secondary(), kDetailWidth - 80, 1);
		y += 34;
		const ui::TextBlock name = canvas.Text(Style(kHeadingStyle), x, y, pack->name, kText, kDetailWidth - 80, 2);
		y += name.height + 10;
		const ui::TextBlock description = canvas.Text(Style({22, ui::Weight::Regular, 1.45f}), x, y,
			pack->description.empty() ? std::string(Tr("This pack has no description.")) : pack->description, Secondary(), kDetailWidth - 80, 6);
		y += description.height + 30;
		const int choices = (int)pack->choices.size();
		canvas.Text(Style(kOverlineStyle), x, y, choices == 0 ? Tr("No presets") : pack->enabled ? Tr("Presets") : Tr("Presets · choosing one turns it on"),
			Tertiary(), kDetailWidth - 80, 1);
		y += 34;
		m_presetSelected = std::clamp(m_presetSelected, 0, std::max(0, choices - 1));
		DrawList(canvas, {x, y, kDetailWidth - 80, kListBottom - 20 - y}, choices, m_presetSelected, m_listScroll2.value, 84,
			[&](int i, const Box& box, bool focused) {
				const auto& choice = pack->choices[i];
				Row row;
				row.kind = Row::Kind::Choice;
				row.label = choice.category.empty() ? std::string(Tr("Preset")) : choice.category;
				row.value = choice.presets.empty() ? "" : choice.presets[std::clamp(choice.active, 0, (int)choice.presets.size() - 1)];
				DrawRow(canvas, row, box, focused && m_presetsFocus);
			});
		canvas.PopAlpha();
	}

	// -- a player's controls (the Wii U's) -------------------------------------------------------------

	void Shell::PlayerOpen(int player)
	{
		m_player = std::clamp(player, 0, ps5pad::kMaxPlayers - 1);
		m_playerRow = 0;
		m_holdProgress = 0;
		m_message.clear();
		Show(ScreenId::Player);
	}

	void Shell::PlayerUpdate(const ui::Press& press, const ui::Actions& actions)
	{
		if (m_playerRow == kRowReset)
		{
			const float seconds = m_settings.ui.holdMs / 1000.0f;
			m_holdProgress = actions.Held(Button::Cross) ? actions.HeldFor(Button::Cross) / seconds : 0.0f;
			if (m_holdProgress >= 1 && !m_holdFired)
			{
				m_holdFired = true;
				ps5emu::ResetControls(m_player);
				m_message = TrC("finished", "Done");
				m_feedback.Play(ui::Cue::Hold);
			}
			if (!actions.Held(Button::Cross))
				m_holdFired = false;
		}
		else
			m_holdProgress = 0;
		const Button b = press.button;
		if (b == Button::Count)
			return;
		const auto controls = ps5emu::GetPlayerControls(m_player);
		switch (b)
		{
		case Button::Circle: ClosePage(ScreenId::Settings); return;
		case Button::Up:
		case Button::Down:
		{
			const int next = m_playerRow + (b == Button::Down ? 1 : -1);
			if (next < 0 || next >= kPlayerRows)
				m_feedback.Play(ui::Cue::Edge, press.repeat);
			else
			{
				m_playerRow = next;
				m_message.clear();
				m_feedback.Play(ui::Cue::Focus);
			}
			return;
		}
		case Button::Triangle:
		{
			static const char* kHelp[kPlayerRows] = {
				TrMark("What the game sees in this player's hands.\n\nMost games want the Wii U GamePad for player 1: its screen is the second one "
					   "PS5CEMU-HAR shows, and the touchpad touches it. Others take Pro Controllers, or Wii Remotes for games such as New Super "
					   "Mario Bros. U.\nA Wii Remote points where a finger rests on the touchpad, or where you aim the DualSense.\nCemu has two "
					   "GamePads at most."),
				TrMark("The DualSense's gyroscope and accelerometer as the controller's own, for the games that aim or steer by tilting the "
					   "GamePad or the Wii Remote.\nThe Pro Controller and the Classic Controller have none."),
				TrMark("How strongly the DualSense rumbles when the game makes the controller vibrate.\nLeft and Right change it by 10%."),
				TrMark("How far the left stick moves before the game sees it. Raise it if a character drifts when you let go of the stick; "
					   "lower it for finer control."),
				TrMark("How far the right stick moves before the game sees it. Raise it if the camera drifts when you let go of the stick; "
					   "lower it for finer control."),
				TrMark("Which DualSense button is which of the controller's.\nA is on Circle and B on Cross by default, where the Wii U has them."),
				TrMark("The default buttons, vibration, motion and deadzones for this controller. Held to confirm."),
			};
			static const char* kNames[kPlayerRows] = {TrMark("Emulated controller"), TrMark("Motion controls"), TrMark("Vibration"),
				TrMark("Left stick deadzone"), TrMark("Right stick deadzone"), TrMark("Buttons"), TrMark("Reset to defaults")};
			OpenHelp(Tr(kNames[m_playerRow]), Tr(kHelp[m_playerRow]));
			return;
		}
		default: break;
		}
		const bool left = b == Button::Left, right = b == Button::Right, cross = b == Button::Cross;
		if (!left && !right && !cross)
			return;
		// the emulated controllers a player can have: Cemu has two GamePads at most
		auto types = [this]() {
			int otherGamePads = 0;
			for (int other = 0; other < ps5pad::kMaxPlayers; other++)
				if (other != m_player && ps5emu::GetPlayerControls(other).type == ps5emu::EmulatedType::GamePad)
					otherGamePads++;
			std::vector<ps5emu::EmulatedType> list;
			if (otherGamePads < 2)
				list.push_back(ps5emu::EmulatedType::GamePad);
			for (auto type : {ps5emu::EmulatedType::Pro, ps5emu::EmulatedType::Classic, ps5emu::EmulatedType::Wiimote, ps5emu::EmulatedType::Nunchuk})
				list.push_back(type);
			return list;
		};
		switch (m_playerRow)
		{
		case kRowType:
		{
			const auto list = types();
			int active = -1;
			for (int i = 0; i < (int)list.size(); i++)
				if (list[i] == controls.type)
					active = i;
			if (cross)
			{
				std::vector<std::string> names;
				for (auto type : list)
					names.push_back(TypeName(type));
				OpenPicker(Tr("Emulated controller"), TrF("Player {0}", m_player + 1), names, active,
					[this, list](int index) { ps5emu::SetEmulatedType(m_player, list[index]); });
				return;
			}
			const int count = (int)list.size();
			ps5emu::SetEmulatedType(m_player, list[((active < 0 ? 0 : active) + (right ? 1 : count - 1)) % count]);
			m_feedback.Play(ui::Cue::Focus);
			break;
		}
		case kRowMotion:
			if (controls.hasMotion)
			{
				ps5emu::SetMotion(m_player, !controls.motion);
				m_feedback.Play(ui::Cue::Toggle);
			}
			else
				m_feedback.Play(ui::Cue::Denied);
			break;
		case kRowRumble:
		{
			int rumble = controls.rumble + (left ? -10 : 10);
			if (cross && rumble > 100)
				rumble = 0;
			rumble = std::clamp(rumble, 0, 100);
			ps5emu::SetRumble(m_player, rumble);
			if (rumble > 0 && !m_settings.rumble)
			{
				// the launcher's own switch would keep the motors still
				m_settings.rumble = true;
				ps5pad::SetVibrationEnabled(true);
				SaveSettings();
			}
			m_feedback.Play(ui::Cue::Focus);
			break;
		}
		case kRowLeftDeadzone:
		case kRowRightDeadzone:
		{
			const bool leftStick = m_playerRow == kRowLeftDeadzone;
			int value = (leftStick ? controls.leftDeadzone : controls.rightDeadzone) + (left ? -5 : 5);
			if (cross && value > 50)
				value = 0;
			value = std::clamp(value, 0, 50);
			ps5emu::SetDeadzones(m_player, leftStick ? value : controls.leftDeadzone, leftStick ? controls.rightDeadzone : value);
			m_feedback.Play(ui::Cue::Focus);
			break;
		}
		case kRowButtons:
			if (cross)
			{
				m_feedback.Play(ui::Cue::Select);
				MappingOpen();
			}
			break;
		case kRowReset:
			if (cross)
				m_feedback.Play(ui::Cue::Denied);
			break;
		}
	}

	void Shell::PlayerDraw(Canvas& canvas)
	{
		const auto controls = ps5emu::GetPlayerControls(m_player);
		const auto mappings = ps5emu::ListMappings(m_player);
		const int mapped = (int)std::count_if(mappings.begin(), mappings.end(), [](const ps5emu::ButtonMapping& m) { return !m.input.empty(); });
		// tr: {0} is an emulated controller (Wii U GamePad)
		DrawPageHeader(canvas, "Wii U · " + std::string(Tr("Controllers")), TrF("Player {0}", m_player + 1),
			controls.connected ? TrF("{0}, on this player's DualSense", TypeName(controls.type)) :
								 TrF("{0}, on a DualSense that is not connected", TypeName(controls.type)));
		std::vector<Row> rows(kPlayerRows);
		rows[kRowType] = {Row::Kind::Choice, "", Tr("Emulated controller"), TypeName(controls.type), Tr("What the game sees in this player's hands.")};
		rows[kRowMotion] = {Row::Kind::Toggle, "", Tr("Motion controls"), controls.hasMotion ? (controls.motion ? Tr("On") : Tr("Off")) : Tr("None on this one"),
			controls.hasMotion ? Tr("The DualSense's motion as the controller's.") : Tr("The Pro and Classic Controllers have none.")};
		rows[kRowMotion].on = controls.motion;
		rows[kRowMotion].dimmed = !controls.hasMotion;
		if (!controls.hasMotion)
			rows[kRowMotion].kind = Row::Kind::Action;
		rows[kRowRumble] = {Row::Kind::Slider, "", Tr("Vibration"), controls.rumble ? ps5lang::Percent(controls.rumble) : std::string(Tr("Off")),
			Tr("How strongly the DualSense rumbles.")};
		rows[kRowRumble].fraction = controls.rumble / 100.0f;
		rows[kRowLeftDeadzone] = {Row::Kind::Slider, "", Tr("Left stick deadzone"), ps5lang::Percent(controls.leftDeadzone),
			Tr("How far the left stick moves before it counts.")};
		rows[kRowLeftDeadzone].fraction = controls.leftDeadzone / 50.0f;
		rows[kRowRightDeadzone] = {Row::Kind::Slider, "", Tr("Right stick deadzone"), ps5lang::Percent(controls.rightDeadzone),
			Tr("How far the right stick moves before it counts.")};
		rows[kRowRightDeadzone].fraction = controls.rightDeadzone / 50.0f;
		rows[kRowButtons] = {Row::Kind::Link, "", Tr("Buttons"), TrP(mapped, "{0} button set", "{0} buttons set"), Tr("Which DualSense button is which.")};
		rows[kRowReset] = {Row::Kind::Hold, "", Tr("Reset to defaults"), m_message.empty() ? std::string(Tr("Hold Cross")) : m_message,
			Tr("The default buttons, vibration, motion and deadzones.")};
		canvas.PushAlpha(Enter(1));
		for (int i = 0; i < kPlayerRows; i++)
		{
			const Box box{kListX, kListTop + i * 108.0f, kListWidth, 96};
			DrawRow(canvas, rows[i], box, i == m_playerRow);
			if (i == kRowReset && i == m_playerRow)
				HoldRing(canvas, box, m_holdProgress);
		}
		canvas.PopAlpha();
		// the DualSense's players, as they are connected
		canvas.PushAlpha(Enter(2));
		const Box panel{kDetailX, kListTop, kDetailWidth, 420};
		Panel(canvas, panel, kRadiusSheet);
		canvas.Text(Style(kOverlineStyle), panel.x + 36, panel.y + 32, Tr("Players"), Tertiary(), panel.w - 72, 1);
		for (int player = 0; player < ps5pad::kMaxPlayers; player++)
		{
			const auto other = ps5emu::GetPlayerControls(player);
			const float y = panel.y + 80 + player * 80;
			const Box pad{panel.x + 36, y, 56, 40};
			if (player == m_player)
				canvas.Rect(pad, 10, Accent());
			else
				canvas.Ring(pad, 10, 2, other.connected ? 0x80ffffff : 0x33ffffff);
			canvas.Text(Style({18, ui::Weight::Bold, 1.0f}), pad.CentreX(), pad.y + 10, fmt::format("P{}", player + 1),
				player == m_player ? kInk1 : Secondary(), 0, 0, ui::Align::Centre);
			canvas.Text(Style(kBodyStyle), panel.x + 116, y + 2, TypeName(other.type), other.connected ? kText : Tertiary(), 420, 1);
			canvas.Text(Style(kCaptionStyle), panel.x + 552, y + 8, other.connected ? Tr("Connected") : Tr("No DualSense"), Tertiary(), panel.w - 552 - 36, 1);
		}
		canvas.PopAlpha();
	}

	// -- a controller's buttons ------------------------------------------------------------------------

	void Shell::MappingOpen()
	{
		m_mapping3ds = m_screen == ScreenId::Settings;
		m_mapSelected = 0;
		m_capture = {};
		m_mapMessage.clear();
		m_listScroll.Snap(0);
		Show(ScreenId::Mapping);
	}

	std::vector<ps5emu::ButtonMapping> Shell::Mappings() const
	{
		return m_mapping3ds ? ps5azahar::ListMappings(m_settings.n3ds) : ps5emu::ListMappings(m_player);
	}

	void Shell::MappingUpdate(const ui::Press& press)
	{
		const int count = (int)Mappings().size();
		switch (press.button)
		{
		case Button::Circle:
			if (m_mapping3ds)
				ClosePage(ScreenId::Settings);
			else
			{
				m_feedback.Play(ui::Cue::Back);
				Show(ScreenId::Player);
			}
			break;
		case Button::Cross:
			if (count)
			{
				m_capture = {true, false, m_now + kCaptureSeconds};
				m_mapMessage.clear();
				m_feedback.Play(ui::Cue::Select);
			}
			break;
		case Button::Square:
			if (count)
			{
				if (m_mapping3ds)
				{
					ps5azahar::SetMapping(m_settings.n3ds, m_mapSelected, ps5emu::PadInput::None);
					SaveSettings();
				}
				else
					ps5emu::SetMapping(m_player, m_mapSelected, ps5emu::PadInput::None);
				m_mapMessage = Tr("Cleared: no DualSense button is this one now.");
				m_feedback.Play(ui::Cue::Back);
			}
			break;
		case Button::Up:
		case Button::Down:
		case Button::L2:
		case Button::R2:
		{
			const int step = press.button == Button::Up ? -1 : press.button == Button::Down ? 1 : press.button == Button::L2 ? -7 : 7;
			const int next = std::clamp(m_mapSelected + step, 0, std::max(0, count - 1));
			if (next == m_mapSelected)
				m_feedback.Play(ui::Cue::Edge, press.repeat);
			else
			{
				m_mapSelected = next;
				m_mapMessage.clear();
				m_feedback.Play(ui::Cue::Focus);
			}
			break;
		}
		default: break;
		}
	}

	// Waits for everything to be let go (the Cross that started it), then takes the next press. The
	// touchpad cancels.
	void Shell::PollCapture()
	{
		ps5pad::Data data{};
		const int player = m_mapping3ds ? 0 : m_player;
		if (!ps5pad::Read(player, data) && !ps5pad::Read(0, data))
			return;
		const bool idle = Pressed(data) == ps5emu::PadInput::None && !(data.buttons & ps5pad::kTouchPad) && data.l2 < 60 && data.r2 < 60;
		if (m_now > m_capture.until)
		{
			m_capture.active = false;
			m_mapMessage = Tr("No button was pressed, so it stays as it was.");
		}
		else if (!m_capture.released)
			m_capture.released = idle;
		else if (data.buttons & ps5pad::kTouchPad)
		{
			m_capture.active = false;
			m_mapMessage = Tr("Cancelled: it stays as it was.");
		}
		else if (const auto input = Pressed(data); input != ps5emu::PadInput::None)
		{
			m_capture.active = false;
			if (m_mapping3ds)
			{
				ps5azahar::SetMapping(m_settings.n3ds, m_mapSelected, input);
				SaveSettings();
			}
			else
				ps5emu::SetMapping(m_player, m_mapSelected, input);
			m_mapMessage = Tr("Done.");
			m_feedback.Play(ui::Cue::Select);
		}
	}

	void Shell::MappingDraw(Canvas& canvas)
	{
		const auto mappings = Mappings();
		const int count = (int)mappings.size();
		m_mapSelected = std::clamp(m_mapSelected, 0, std::max(0, count - 1));
		if (m_mapping3ds)
			DrawPageHeader(canvas, "Nintendo 3DS · " + std::string(Tr("Controls")), Tr("Buttons"), Tr("The DualSense's buttons for the 3DS's"));
		else
		{
			const auto controls = ps5emu::GetPlayerControls(m_player);
			// tr: {0} is the player's number, {1} an emulated controller (Wii U GamePad)
			DrawPageHeader(canvas, std::string("Wii U · ") + TypeName(controls.type), Tr("Buttons"),
				TrF("Player {0}: the DualSense's buttons for the {1}", m_player + 1, TypeName(controls.type)));
		}
		canvas.PushAlpha(Enter(1));
		canvas.Text(Style({20, ui::Weight::Medium, 1.0f, 0, false, true}), kListX + kListWidth, kListTop - 40,
			TrF("{0} of {1}", count ? m_mapSelected + 1 : 0, count), Tertiary(), 0, 0, ui::Align::Right);
		DrawList(canvas, {kListX, kListTop, kListWidth, kListBottom - kListTop}, count, m_mapSelected, m_listScroll.value, 72,
			[&](int i, const Box& box, bool focused) {
				canvas.Rect(box, 16, focused ? Surface2() : Surface());
				canvas.Text(Style(kBodyStyle), box.x + 24, box.CentreY() - 19, ButtonName(mappings[i].button), kText, box.w * 0.55f, 1);
				const bool unset = mappings[i].input.empty();
				canvas.Text(Style(kLabelStyle), box.Right() - 24 - box.w * 0.4f, box.CentreY() - 16,
					unset ? std::string(Tr("Not set")) : InputName(mappings[i].input), unset ? Tertiary() : Accent(), box.w * 0.4f, 1, ui::Align::Right);
				if (focused)
					Focus(box, 16);
			});
		canvas.PopAlpha();
		if (count == 0)
			return;
		const auto& mapping = mappings[m_mapSelected];
		canvas.PushAlpha(Enter(2));
		const Box panel{kDetailX, kListTop - 10, kDetailWidth, 520};
		Panel(canvas, panel, kRadiusSheet);
		canvas.Text(Style(kOverlineStyle), panel.x + 40, panel.y + 36, Tr("This button"), Tertiary(), panel.w - 80, 1);
		canvas.Text(Style(kTitleStyle), panel.x + 40, panel.y + 66, ButtonName(mapping.button), kText, panel.w - 80, 1);
		const Box input{panel.x + 40, panel.y + 140, panel.w - 80, 110};
		canvas.Rect(input, kRadiusCard, m_capture.active ? ui::SetAlpha(Accent(), 0.18f) : 0x12ffffff);
		if (m_capture.active)
			canvas.Ring(input, kRadiusCard, 2, Accent());
		canvas.Text(Style({40, ui::Weight::SemiBold, 1.0f}), input.x + 20, input.CentreY() - 20,
			m_capture.active ? std::string(Tr("Press a button")) : mapping.input.empty() ? std::string(Tr("Not set")) : InputName(mapping.input),
			m_capture.active ? kText : Accent(), input.w - 40, 1, ui::Align::Centre);
		std::string message;
		if (m_capture.active)
		{
			const int seconds = (int)std::max(0.0, m_capture.until - m_now) + 1;
			// tr: {0} is a button of the Wii U's or the 3DS's controller
			message = TrF("Press the DualSense button, trigger or stick direction for {0} now.", ButtonName(mapping.button)) + "\n" +
				TrP(seconds, "{0} second left. A touchpad click cancels.", "{0} seconds left. A touchpad click cancels.");
		}
		else
			message = (m_mapMessage.empty() ? "" : m_mapMessage + "\n\n") +
				Tr("Cross, then a DualSense button, trigger or stick direction: it becomes this one.\nSquare clears it.");
		ui::TextStyle body = Style({24, ui::Weight::Regular, 1.45f});
		body.tabular = true;
		canvas.Text(body, panel.x + 40, panel.y + 280, message, Secondary(), panel.w - 80, 7);
		canvas.PopAlpha();
	}

	// -- the folder browser: a side's games folder, and installs ---------------------------------------

	void Shell::FilesOpen(int mode)
	{
		m_filesMode = mode;
		if (mode == 1)
			m_filesSide = System::WiiU;
		else if (mode == 2)
			m_filesSide = System::N3ds;
		if (m_screen != ScreenId::Files && m_screen != ScreenId::Settings && m_screen != ScreenId::Setup)
			m_pageFrom = m_screen == ScreenId::Library ? ScreenId::Library : ScreenId::Home;
		else if (m_screen == ScreenId::Setup)
			m_pageFrom = ScreenId::Setup;
		const std::string& games = GamesFolder(m_filesSide);
		std::string start = mode != 0 && !m_installFolder.empty() ? m_installFolder : games.empty() ? ps5paths::kGames : games;
		while (!BrowseTo(start) && start != "/")
			start = ParentPath(start);
		m_filesMessage.clear();
		m_listScroll.Snap(0);
		Show(ScreenId::Files);
	}

	bool Shell::BrowseTo(const std::string& folder)
	{
		bool ok = false;
		auto folders = ListEntries(folder, true, ok);
		if (!ok)
			return false;
		m_browseFolder = folder;
		m_browseEntries.clear();
		if (folder != "/")
			m_browseEntries.push_back("..");
		// the drives plugged in, as shortcuts, except the one already open (and in /mnt, which lists them)
		if (folder != "/mnt")
			for (const std::string& drive : ConnectedDrives())
				if (folder != drive && !folder.starts_with(drive + "/"))
					m_browseEntries.push_back(drive);
		m_browseEntries.insert(m_browseEntries.end(), folders.begin(), folders.end());
		m_browseFiles = 0;
		if (m_filesMode == 2)
			for (const auto& file : ListEntries(folder, false, ok))
				if (Lower(file).ends_with(".cia"))
				{
					m_browseEntries.push_back(file);
					m_browseFiles++;
				}
		m_browseSelected = 0;
		m_folderCounts.clear();
		return true;
	}

	bool Shell::IsFileEntry(int index) const
	{
		return index >= (int)m_browseEntries.size() - m_browseFiles && index < (int)m_browseEntries.size();
	}

	const ps5emu::InstallCandidate& Shell::Inspect(const std::string& folder)
	{
		auto it = m_inspected.find(folder);
		if (it == m_inspected.end())
			it = m_inspected.emplace(folder, ps5emu::InspectInstall(folder)).first;
		return it->second;
	}

	void Shell::FilesUpdate(const ui::Press& press)
	{
		const Button b = press.button;
		const int count = (int)m_browseEntries.size();
		if (m_installing)
		{
			if (b == Button::Circle)
			{
				// Cemu's install puts back what it replaced; Azahar's writes in place, so the title's contents go
				if (m_filesMode == 2)
				{
					ps5azahar::CancelInstall();
					m_filesMessage = Tr("Cancelling: what it copied is removed…");
				}
				else
				{
					ps5emu::CancelInstall();
					m_filesMessage = Tr("Cancelling: what was installed before is put back…");
				}
				m_feedback.Play(ui::Cue::Back);
			}
			return;
		}
		switch (b)
		{
		case Button::Circle:
			ClosePage(m_pageFrom);
			if (m_pageFrom == ScreenId::Settings)
				SettingsOpen(m_filesMode == 0 ? "folders" : m_filesMode == 1 ? "wiiu-installs" : "3ds-installs", true);
			return;
		case Button::Up:
		case Button::Down:
		case Button::L1:
		case Button::R1:
		case Button::L2:
		case Button::R2:
		{
			const int step = b == Button::Up ? -1 : b == Button::Down ? 1 : b == Button::L1 || b == Button::L2 ? -7 : 7;
			const int next = std::clamp(m_browseSelected + step, 0, std::max(0, count - 1));
			if (next == m_browseSelected)
				m_feedback.Play(ui::Cue::Edge, press.repeat);
			else
			{
				m_browseSelected = next;
				m_feedback.Play(ui::Cue::Focus);
			}
			return;
		}
		case Button::Cross:
		{
			if (!count || IsFileEntry(m_browseSelected))
			{
				m_feedback.Play(ui::Cue::Denied);
				return;
			}
			const std::string entry = m_browseEntries[m_browseSelected];
			const std::string from = m_browseFolder;
			const bool up = entry == "..";
			if (!BrowseTo(up ? ParentPath(m_browseFolder) : JoinPath(m_browseFolder, entry)))
			{
				m_filesMessage = Tr("This folder cannot be opened.");
				m_feedback.Play(ui::Cue::Denied);
				return;
			}
			m_filesMessage.clear();
			m_listScroll.Snap(0);
			if (up) // the folder just left stays in view
			{
				const std::string left = from.substr(from.find_last_of('/') + 1);
				for (int i = 0; i < (int)m_browseEntries.size(); i++)
					if (m_browseEntries[i] == left)
						m_browseSelected = i;
			}
			m_feedback.Play(up ? ui::Cue::Back : ui::Cue::Select);
			return;
		}
		case Button::Triangle:
			break;
		default: return;
		}
		// Triangle: the folder shown is used, or what is chosen installed
		std::string error;
		m_installFolder = m_browseFolder;
		if (m_filesMode == 0)
		{
			GamesFolder(m_filesSide) = m_browseFolder;
			const bool saved = ps5settings::Save(m_settings);
			const bool here = m_filesSide == m_side;
			if (here && m_filesSide == System::N3ds)
			{
				ps5azahar::StartScan(m_browseFolder);
				m_scanning = true;
			}
			else if (here && m_status.coreReady)
			{
				ApplyCemuOptions();
				m_scanning = true;
			}
			else
				m_rescan[m_filesSide == System::N3ds ? 1 : 0] = true;
			m_filesMessage = !saved ? Tr("The folder could not be saved. Please try again.") :
				here			   ? Tr("Saved. PS5CEMU-HAR is looking for games there.") :
									 Tr("Saved. That side looks for games there when it opens.");
			m_feedback.Play(saved ? ui::Cue::Select : ui::Cue::Denied);
			if (saved && here)
				Toast(TrF("Looking for games in {0}", ShortPath(m_browseFolder, 40)));
		}
		else if (m_filesMode == 2)
		{
			if (!IsFileEntry(m_browseSelected))
				m_filesMessage = Tr("Choose a CIA file first.");
			else if (ps5azahar::StartInstall(JoinPath(m_browseFolder, m_browseEntries[m_browseSelected]), error))
			{
				m_installing = true;
				m_filesMessage.clear();
			}
			else
				m_filesMessage = error;
			m_feedback.Play(m_installing ? ui::Cue::Select : ui::Cue::Denied);
		}
		else
		{
			if (!m_status.coreReady)
				m_filesMessage = Tr("Cemu did not start, so nothing can be installed.");
			else if (ps5emu::StartInstall(m_browseFolder, error))
			{
				m_installing = true;
				m_filesMessage.clear();
			}
			else
				m_filesMessage = error;
			m_feedback.Play(m_installing ? ui::Cue::Select : ui::Cue::Denied);
		}
	}

	void Shell::PollInstall()
	{
		const auto status = m_filesMode == 2 ? ps5azahar::GetInstallStatus() : ps5emu::GetInstallStatus();
		using State = ps5emu::InstallStatus::State;
		if (status.state == State::Running)
		{
			m_installFraction = status.total ? (float)status.copied / (float)status.total : -1.0f;
			// tr: {0} is a percentage, {1} and {2} sizes (2.4 GB)
			m_installProgress = status.total ? TrF("Installing: {0}, {1} of {2}.\n\nCircle cancels.", ps5lang::Percent((int)(status.copied * 100 / status.total)),
												   Gigabytes(status.copied), Gigabytes(status.total)) :
											   std::string(Tr("Installing: counting the files…"));
			return;
		}
		m_installing = false;
		m_installProgress.clear();
		m_installFraction = -1;
		m_inspected.clear();
		if (status.state == State::Done)
		{
			m_filesMessage = Tr("Installed. The library has it with its game.");
			if (m_filesSide == m_side)
			{
				if (Is3ds())
					ps5azahar::StartScan(m_settings.n3ds.gamesFolder);
				else
					ps5emu::Rescan();
				m_scanning = true;
			}
			Toast(Tr("Installed"));
		}
		else if (status.state == State::Cancelled && m_filesMode == 2)
			m_filesMessage = Tr("Cancelled. What it copied was removed, with any earlier copy of the same title: install it again to play it. "
								"Its saves are kept.");
		else if (status.state == State::Cancelled)
			m_filesMessage = Tr("Cancelled. What was installed before is as it was.");
		else
			m_filesMessage = TrF("It could not be installed: {0}", status.message);
	}

	void Shell::FilesDraw(Canvas& canvas)
	{
		const bool install = m_filesMode == 1, cia = m_filesMode == 2;
		const bool n3ds = m_filesSide == System::N3ds;
		DrawPageHeader(canvas,
			install || cia ? (n3ds ? "Nintendo 3DS · " : "Wii U · ") + std::string(Tr("Install")) : n3ds ? Tr("3DS games") : Tr("Wii U games"),
			cia ? Tr("Install CIA files") : install ? Tr("Install updates and DLC") : Tr("Choose a folder"),
			cia		? Tr("Choose a CIA file: a game, an update or DLC") :
			install ? Tr("Choose a folder with an update, DLC or game (code, content and meta)") :
			n3ds	? Tr("Choose the folder that holds your 3DS games") :
					  Tr("Choose the folder that holds your Wii U games"));
		const int count = (int)m_browseEntries.size();
		canvas.PushAlpha(Enter(1));
		canvas.Text(Style(kOverlineStyle), kListX, kListTop - 40, ShortPath(m_browseFolder, 56), Secondary(), kListWidth - 140, 1);
		canvas.Text(Style({20, ui::Weight::Medium, 1.0f, 0, false, true}), kListX + kListWidth, kListTop - 40,
			TrF("{0} of {1}", count ? m_browseSelected + 1 : 0, count), Tertiary(), 0, 0, ui::Align::Right);
		if (count == 0)
			canvas.Text(Style(kBodyStyle), kListX, kListTop + 10, Tr("This folder is empty."), Secondary(), kListWidth, 2);
		DrawList(canvas, {kListX, kListTop, kListWidth, kListBottom - kListTop}, count, m_browseSelected, m_listScroll.value, 72,
			[&](int i, const Box& box, bool focused) {
				const std::string& entry = m_browseEntries[i];
				const bool up = entry == "..", drive = entry.starts_with('/'), file = IsFileEntry(i);
				canvas.Rect(box, 16, focused ? Surface2() : Surface());
				canvas.Draw(up ? Icon::Back : drive ? Icon::Drive : file ? Icon::Square : Icon::Folder, {box.x + 20, box.CentreY() - 16, 32, 32},
					drive ? Accent() : Secondary());
				canvas.Text(Style(kBodyStyle), box.x + 70, box.CentreY() - 19, up ? std::string(Tr("Parent folder")) : drive ? DriveName(entry) : entry, kText,
					box.w - 260, 1);
				std::string meta = file ? "CIA" : drive ? Tr("Drive") : "";
				if (install && !up && !drive)
				{
					const auto& candidate = Inspect(JoinPath(m_browseFolder, entry));
					if (candidate.kind != ps5emu::InstallCandidate::Kind::None)
						meta = KindName(candidate.kind);
				}
				if (!meta.empty())
					canvas.Text(Style(kCaptionStyle), box.Right() - 24 - 170, box.CentreY() - 13, meta, Tertiary(), 170, 1, ui::Align::Right);
				if (focused)
					Focus(box, 16);
			});
		canvas.PopAlpha();

		// what the side would find in the folder shown (Triangle uses it), or what is to be installed
		std::pair<std::string, std::string> lines[3];
		bool ready[3]{};
		std::string message = m_filesMessage, current;
		if (cia)
		{
			const bool file = IsFileEntry(m_browseSelected);
			const auto title = file ? ps5azahar::Inspect(JoinPath(m_browseFolder, m_browseEntries[m_browseSelected])) : ps5azahar::Title{};
			current = !file ? ShortPath(m_browseFolder, 40) : title.name.empty() ? m_browseEntries[m_browseSelected] : title.name;
			lines[0] = {TrC("kind", "Type"), !file ? Tr("A folder") : title.titleId ? CiaKind(title.titleId) : "CIA"};
			lines[1] = {Tr("Title ID"), file && title.titleId ? Hex(title.titleId) : "-"};
			lines[2] = {Tr("Version"), file && title.titleId ? fmt::format("v{}", title.version) : "-"};
			ready[0] = ready[1] = ready[2] = file;
			if (message.empty())
				message = file ? Tr("Triangle installs it into the 3DS's storage: an update or DLC goes with its game, and a game joins the library.") :
								 Tr("Choose a CIA file to install: a game, an update or DLC.");
		}
		else if (!install)
		{
			current = ShortPath(m_browseFolder, 40);
			auto it = m_folderCounts.find(m_browseFolder);
			if (it == m_folderCounts.end())
			{
				const int games = n3ds ? Count3dsGames(m_browseFolder) : CountGames(m_browseFolder);
				it = m_folderCounts.emplace(m_browseFolder, games < 0 ? "-1" : std::to_string(games)).first;
			}
			const int games = std::atoi(it->second.c_str());
			lines[0] = {Tr("Games here"), games < 0 ? std::string(Tr("Cannot be read")) : TrP(games, "{0} game", "{0} games")};
			if (n3ds)
			{
				const bool keys = IsFile(std::string(ps5azahar::kRoot) + "/sysdata/aes_keys.txt");
				lines[1] = {"aes_keys.txt", keys ? Tr("Found") : Tr("Missing (only encrypted dumps need it)")};
				ready[1] = keys;
			}
			else
			{
				const bool keys = IsFile(std::string(ps5paths::kRoot) + "/keys.txt");
				lines[1] = {"keys.txt", keys ? Tr("Found in /data/ps5cemu") : Tr("Missing (only .wud/.wux need it)")};
				ready[1] = keys;
			}
			lines[2] = {Tr("In use"), ShortPath(GamesFolder(m_filesSide), 40)};
			ready[0] = games > 0;
			ready[2] = GamesFolder(m_filesSide) == m_browseFolder;
			if (message.empty())
				message = n3ds ? Tr("Games can be .3ds or .cci, .cxi, .cia or .3dsx, decrypted, here or in the folders in it. Triangle uses the folder shown.") :
								 Tr("Games can be .wua, .wud, .wux, or folders with code, content and meta. Triangle uses the folder shown.");
		}
		else
		{
			const auto& candidate = Inspect(m_browseFolder);
			const bool valid = candidate.kind != ps5emu::InstallCandidate::Kind::None;
			current = valid && !candidate.name.empty() ? candidate.name : ShortPath(m_browseFolder, 40);
			lines[0] = {Tr("Type"), valid ? KindName(candidate.kind) : Tr("Nothing to install")};
			lines[1] = {Tr("Title ID"), valid ? Hex(candidate.titleId) : "-"};
			// tr: a title's version to install ({0}), and the one installed now ({1})
			lines[2] = {Tr("Version"), !valid ? std::string("-") : candidate.installedVersion < 0 ? TrF("v{0}, not installed yet", candidate.version) :
																									TrF("v{0}, v{1} installed now", candidate.version, candidate.installedVersion)};
			ready[0] = ready[1] = valid;
			ready[2] = valid && candidate.installedVersion < (int)candidate.version;
			if (message.empty())
				message = valid ? Tr("Triangle installs it into the Wii U's storage (mlc01). Updates and DLC in the game files folder work as they are, too.") :
								  candidate.note;
		}
		if (m_installing && !m_installProgress.empty() && m_filesMessage.empty())
			message = m_installProgress;
		canvas.PushAlpha(Enter(2));
		const Box panel{kDetailX, kListTop - 10, kDetailWidth, kListBottom - kListTop + 10};
		Panel(canvas, panel, kRadiusSheet);
		float y = panel.y + 36;
		canvas.Text(Style(kOverlineStyle), panel.x + 40, y, install || cia ? Tr("To install") : Tr("This folder"), Tertiary(), panel.w - 80, 1);
		y += 34;
		const ui::TextBlock name = canvas.Text(Style(kHeadingStyle), panel.x + 40, y, current, kText, panel.w - 80, 2);
		y += name.height + 26;
		for (int i = 0; i < 3; i++)
		{
			if (lines[i].first.empty())
				continue;
			canvas.Draw(ready[i] ? Icon::Check : Icon::Dash, {panel.x + 40, y + 2, 26, 26}, ready[i] ? kGood : Tertiary());
			canvas.Text(Style(kCaptionStyle), panel.x + 80, y, lines[i].first, Tertiary(), panel.w - 120, 1);
			canvas.Text(Style(kBodyStyle), panel.x + 80, y + 26, lines[i].second, kText, panel.w - 120, 1);
			y += 82;
		}
		y += 10;
		if (m_installing && m_installFraction >= 0)
		{
			const Box bar{panel.x + 40, y, panel.w - 80, 12};
			canvas.Rect(bar, 6, 0x24ffffff);
			canvas.Rect({bar.x, bar.y, bar.w * m_installFraction, bar.h}, 6, Accent());
			y += 36;
		}
		ui::TextStyle body = Style({23, ui::Weight::Regular, 1.45f});
		body.tabular = true;
		canvas.Text(body, panel.x + 40, y, message, Secondary(), panel.w - 80, 8);
		canvas.PopAlpha();
	}

	// -- Artic Base: a game from a 3DS on the network ----------------------------------------------------

	void Shell::ArticOpen()
	{
		m_articRow = 0;
		m_articEditing = false;
		m_holdProgress = 0;
		if (!ParseAddress(m_settings.n3ds.articAddress, m_articOctets))
			m_articOctets = OwnAddress();
		if (m_screen != ScreenId::Settings)
			m_pageFrom = m_screen == ScreenId::Library ? ScreenId::Library : ScreenId::Home;
		Show(ScreenId::Artic);
	}

	std::string Shell::ArticAddress() const
	{
		return fmt::format("{}.{}.{}.{}", m_articOctets[0], m_articOctets[1], m_articOctets[2], m_articOctets[3]);
	}

	void Shell::ArticUpdate(const ui::Press& press, const ui::Actions& actions)
	{
		auto launch = [this](const char* scheme, const char* what) {
			m_settings.n3ds.articAddress = ArticAddress();
			SaveSettings();
			ps5emu::Game game;
			game.name = fmt::format("{} ({})", what, ArticAddress());
			game.path = std::string(scheme) + ArticAddress();
			game.format = "ARTIC";
			// tr: {0} is a 3DS's network address
			LaunchGame(game, TrF("Connecting to {0}", ArticAddress()), {760, 240, 400, 400});
		};
		// Artic Setup writes the 3DS's own data into Azahar's: held to confirm
		if (!m_articEditing && (m_articRow == kRowArticSetupOld || m_articRow == kRowArticSetupNew))
		{
			const float seconds = m_settings.ui.holdMs / 1000.0f;
			m_holdProgress = actions.Held(Button::Cross) ? actions.HeldFor(Button::Cross) / seconds : 0.0f;
			if (m_holdProgress >= 1 && !m_holdFired)
			{
				m_holdFired = true;
				m_feedback.Play(ui::Cue::Hold);
				launch(m_articRow == kRowArticSetupOld ? ps5azahar::kArticSetupOld : ps5azahar::kArticSetupNew, "Artic Setup Tool");
				return;
			}
			if (!actions.Held(Button::Cross))
				m_holdFired = false;
		}
		else
			m_holdProgress = 0;
		const Button b = press.button;
		if (b == Button::Count)
			return;
		if (m_articEditing)
		{
			switch (b)
			{
			case Button::Left: m_articOctet = (m_articOctet + 3) % 4; break;
			case Button::Right: m_articOctet = (m_articOctet + 1) % 4; break;
			case Button::Up:
			case Button::Down:
			case Button::L1:
			case Button::R1:
			{
				const int step = b == Button::Up ? 1 : b == Button::Down ? -1 : b == Button::R1 ? 10 : -10;
				int& octet = m_articOctets[m_articOctet];
				octet = (octet + step + 256) % 256;
				break;
			}
			case Button::Cross:
			case Button::Circle:
				m_articEditing = false;
				m_settings.n3ds.articAddress = ArticAddress();
				SaveSettings();
				break;
			default: return;
			}
			m_feedback.Play(b == Button::Cross || b == Button::Circle ? ui::Cue::Select : ui::Cue::Focus);
			return;
		}
		switch (b)
		{
		case Button::Circle:
			ClosePage(m_pageFrom);
			if (m_pageFrom == ScreenId::Settings)
				SettingsOpen("3ds-artic", true);
			return;
		case Button::Up:
		case Button::Down:
		{
			const int next = m_articRow + (b == Button::Down ? 1 : -1);
			if (next < 0 || next >= kArticRows)
				m_feedback.Play(ui::Cue::Edge, press.repeat);
			else
			{
				m_articRow = next;
				m_feedback.Play(ui::Cue::Focus);
			}
			return;
		}
		case Button::Cross:
			if (m_articRow == kRowArticAddress)
			{
				m_articEditing = true;
				m_articOctet = 3; // the last number is the one that differs on a home network
				m_feedback.Play(ui::Cue::Select);
			}
			else if (m_articRow == kRowArticConnect)
			{
				if (!Is3ds())
				{
					OpenSide(System::N3ds, false);
					m_pageFrom = ScreenId::Home;
				}
				launch(ps5azahar::kArticBase, "Artic Base");
			}
			else
				m_feedback.Play(ui::Cue::Denied); // held, not pressed
			return;
		default: return;
		}
	}

	void Shell::ArticDraw(Canvas& canvas)
	{
		DrawPageHeader(canvas, "Nintendo 3DS", "Artic Base", Tr("Play a game from your 3DS over your network, or set Azahar up from it"));
		std::string address = ArticAddress();
		if (m_articEditing)
		{
			address.clear();
			for (int i = 0; i < 4; i++)
				address += (i ? " . " : "") + (i == m_articOctet ? fmt::format("[{}]", m_articOctets[i]) : std::to_string(m_articOctets[i]));
		}
		struct Item
		{
			const char* name;
			std::string value;
			Row::Kind kind;
			const char* help;
		};
		const Item items[kArticRows] = {
			{Tr("3DS address"), address, Row::Kind::Action,
				Tr("The address Artic Base shows on the 3DS's screen when it is ready.\nCross edits it: Left and Right choose a number, Up and "
				   "Down change it by 1, L1 and R1 by 10, and Cross again keeps it.\nThe 3DS and the PS5 must be on the same network.")},
			{Tr("Connect and play"), "", Row::Kind::Action,
				Tr("On the 3DS, start the Artic Base app (homebrew, from Azahar's team) and choose a game in it: the cartridge or an installed "
				   "one. Then connect from here: the game plays on the PS5 from the 3DS, and its saves stay on the 3DS.\nIt is only as smooth as "
				   "the network: a strong Wi-Fi signal for the 3DS and a wired PS5 help.")},
			{Tr("Set up from an Old 3DS"), Tr("Hold Cross"), Row::Kind::Hold,
				Tr("With the Artic Setup Tool app running on an Old 3DS or 2DS: copies its system files and its own data (system settings, "
				   "friend code, Mii and eShop data) into Azahar, so games that need the 3DS's system applets or files start, and the Home Menu "
				   "can (Settings > System and Home Menu).\nThat data is your console's: do not share Azahar's folder afterwards.")},
			{Tr("Set up from a New 3DS"), Tr("Hold Cross"), Row::Kind::Hold, Tr("As above, from a New 3DS or New 2DS running the Artic Setup Tool app.")},
		};
		canvas.PushAlpha(Enter(1));
		for (int i = 0; i < kArticRows; i++)
		{
			Row row;
			row.kind = items[i].kind;
			row.label = items[i].name;
			row.value = items[i].value;
			const Box box{kListX, kListTop + i * 108.0f, kListWidth, 96};
			DrawRow(canvas, row, box, i == m_articRow);
			if (i == m_articRow && items[i].kind == Row::Kind::Hold)
				HoldRing(canvas, box, m_holdProgress);
		}
		canvas.PopAlpha();
		canvas.PushAlpha(Enter(2));
		const ui::TextBlock help = m_fonts.Layout(Style({24, ui::Weight::Regular, 1.45f}), items[m_articRow].help, kDetailWidth - 80, 14);
		const Box panel{kDetailX, kListTop - 10, kDetailWidth, help.height + 130};
		Panel(canvas, panel, kRadiusSheet);
		canvas.Text(Style(kOverlineStyle), panel.x + 40, panel.y + 36, items[m_articRow].name, Tertiary(), panel.w - 80, 1);
		canvas.Text(help, panel.x + 40, panel.y + 76, Secondary());
		canvas.PopAlpha();
	}
}
