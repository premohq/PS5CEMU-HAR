// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the new launcher's parts (shell_internal.h): buttons, chips, badges, covers, a
// setting's row and its controls, sheets, and the focus ring that glides between them
// (docs/UI-REDESIGN.md, 7.3 and 7.5).

#include "shell_internal.h"

#include <algorithm>
#include <cmath>

namespace ps5shell
{
	using namespace ui::tokens;

	namespace
	{
		// a status's colours: its chip's background and text (the mockups' .chip.good and so on)
		uint32_t KindColour(const std::string& kind)
		{
			return kind == "good" ? kGood : kind == "warn" ? kWarn : kind == "bad" ? kBad : kText;
		}

		uint32_t Lighter(uint32_t colour, float t)
		{
			return ui::Mix(colour, 0xffffffff, t);
		}
	}

	ui::TextStyle Shell::Style(ui::TextStyle style) const
	{
		if (m_settings.ui.largerText)
			style.size = std::round(style.size * 1.15f);
		return style;
	}

	uint32_t Shell::Secondary() const
	{
		return m_settings.ui.highContrast ? kText : kText2;
	}

	uint32_t Shell::Tertiary() const
	{
		return m_settings.ui.highContrast ? kText2 : kText3;
	}

	uint32_t Shell::Surface() const
	{
		return m_settings.ui.highContrast ? kInk2 : kGlass;
	}

	uint32_t Shell::Surface2() const
	{
		return m_settings.ui.highContrast ? ui::Mix(kInk2, 0xffffffff, 0.06f) : kGlass2;
	}

	void Shell::Focus(const Box& box, float radius, uint32_t glow)
	{
		m_ringAsked = true;
		m_ringBox = box;
		m_ringRadius = radius;
		m_ringGlow = glow;
	}

	void Shell::DrawFocusRing(Canvas& canvas)
	{
		const bool asked = m_ringAsked;
		m_ringAsked = false;
		const float omega = m_settings.ui.reduceMotion ? 60.0f : kFocusOmega;
		if (asked)
		{
			const float target[5] = {m_ringBox.x, m_ringBox.y, m_ringBox.w, m_ringBox.h, m_ringRadius};
			for (int i = 0; i < 5; i++)
			{
				if (!m_ringShown)
					m_ring[i].Snap(target[i]);
				m_ring[i].target = target[i];
				m_ring[i].omega = omega;
			}
			m_ringShown = true;
		}
		m_ringAlpha = std::clamp(m_ringAlpha + (asked ? 1 : -1) * m_dt * 8.0f, 0.0f, 1.0f);
		if (m_ringAlpha <= 0)
		{
			m_ringShown = false;
			return;
		}
		for (auto& spring : m_ring)
			spring.Update(m_dt);
		const Box box{m_ring[0].value, m_ring[1].value, m_ring[2].value, m_ring[3].value};
		const float radius = m_ring[4].value;
		canvas.PushAlpha(m_ringAlpha);
		// the glow in the element's own colour, breathing (±6 % over 2.4 s) while the focus rests
		if (m_ringGlow >> 24)
		{
			const float breath = m_settings.ui.reduceMotion ? 0.0f : 0.06f * std::sin((float)(m_now * 2 * 3.14159265 / kBreathSeconds));
			canvas.Glow(box, radius, 46, ui::WithAlpha(m_ringGlow, 0.42f + breath));
		}
		const float width = m_settings.ui.highContrast ? 6.0f : 4.0f;
		canvas.Ring(box.Inset(-width), radius + width, width, kInk0);
		canvas.Ring(box.Inset(-2 * width), radius + 2 * width, width, kFocus);
		canvas.PopAlpha();
	}

	void Shell::Panel(Canvas& canvas, const Box& box, float radius)
	{
		canvas.Rect(box, radius, Surface());
		canvas.Ring(box, radius, 1.5f, kGlassEdge);
	}

	Box Shell::PillButton(Canvas& canvas, float x, float y, const std::string& label, bool primary, bool focused, Icon icon, bool withIcon, float height,
		float maxWidth)
	{
		ui::TextStyle style = Style({28, ui::Weight::SemiBold, 1.0f});
		const float pad = height * 0.55f;
		const float room = maxWidth > 0 ? maxWidth - pad * 2 - (withIcon ? 28 + 14 : 0) : 0.0f;
		// a label wider than the button may be: a size or two smaller, then cut
		if (room > 0)
			style.size = m_fonts.Fit(style, label, room, 1, style.size * 0.8f, 1);
		const float text = room > 0 ? std::min(m_fonts.Width(style, label), room) : m_fonts.Width(style, label);
		const float width = pad * 2 + text + (withIcon ? 28 + 14 : 0);
		Box box{x, y, width, height};
		const Box drawn = focused ? box.Scaled(1.04f) : box;
		if (focused)
			canvas.Shadow(drawn, height / 2, 18, 30, 0x73000000);
		if (primary)
			canvas.Rect(drawn, height / 2, kText);
		else
		{
			canvas.Rect(drawn, height / 2, Surface2());
			canvas.Ring(drawn, height / 2, 2, kGlassEdge);
		}
		const uint32_t ink = primary ? kInk1 : kText;
		float at = drawn.x + (drawn.w - (width - pad * 2)) * 0.5f;
		if (withIcon)
		{
			canvas.Draw(icon, {at, drawn.CentreY() - 14, 28, 28}, ink);
			at += 28 + 14;
		}
		const ui::TextBlock block = m_fonts.Layout(style, label, room > 0 ? room + 0.5f : 0.0f, 1);
		canvas.Text(block, at, drawn.CentreY() - block.height * 0.5f, ink);
		if (focused)
			Focus(drawn, drawn.h / 2);
		return box;
	}

	Box Shell::IconButton(Canvas& canvas, float x, float y, Icon icon, bool focused, float size)
	{
		Box box{x, y, size, size};
		const Box drawn = focused ? box.Scaled(1.04f) : box;
		canvas.Rect(drawn, size / 2, Surface2());
		canvas.Ring(drawn, size / 2, 2, kGlassEdge);
		canvas.Draw(icon, drawn.Inset(size * 0.3f), kText);
		if (focused)
			Focus(drawn, size / 2);
		return box;
	}

	float Shell::Chip(Canvas& canvas, float x, float y, const std::string& text, const char* kind, float height)
	{
		if (text.empty())
			return 0;
		const std::string k = kind ? kind : "";
		const ui::TextStyle style = Style({20, ui::Weight::SemiBold, 1.0f});
		const float dot = k.empty() ? 0.0f : 9 + 8;
		const float width = 16 + dot + m_fonts.Width(style, text) + 16;
		const Box box{x, y, width, height};
		const uint32_t colour = KindColour(k);
		canvas.Rect(box, kRadiusChip, k.empty() ? 0x17ffffff : ui::SetAlpha(colour, 0.16f));
		if (!k.empty())
			canvas.Rect({x + 16, box.CentreY() - 4.5f, 9, 9}, 4.5f, colour);
		const ui::TextBlock block = m_fonts.Layout(style, text);
		canvas.Text(block, x + 16 + dot, box.CentreY() - block.height * 0.5f, k.empty() ? kText : Lighter(colour, 0.45f));
		return width;
	}

	float Shell::Badge(Canvas& canvas, float x, float y, System side)
	{
		const ui::TextStyle style = Style({17, ui::Weight::Bold, 1.0f, 1.5f, true});
		const char* text = side == System::N3ds ? "3DS" : "Wii U";
		const float width = 10 + 18 + 8 + m_fonts.Width(style, text) + 14;
		const uint32_t colour = AccentOf(side);
		const Box box{x, y, width, 34};
		canvas.Rect(box, 10, ui::SetAlpha(colour, 0.16f));
		canvas.Rect({x + 10, y + 8, 18, 18}, 5, colour);
		const ui::TextBlock block = m_fonts.Layout(style, text);
		canvas.Text(block, x + 36, box.CentreY() - block.height * 0.5f, Lighter(colour, 0.5f));
		return width;
	}

	void Shell::DrawGameName(Canvas& canvas, Game& game, const Box& box)
	{
		const ui::TextStyle style{std::clamp(box.w * 0.11f, 13.0f, 30.0f), ui::Weight::Bold, 1.08f};
		const ui::TextBlock block = m_fonts.Layout(style, game.entry.game.name, box.w * 0.82f, 3);
		canvas.Text(block, box.x + box.w * 0.09f, box.Bottom() - box.w * 0.09f - block.height, kText);
	}

	void Shell::DrawCover(Canvas& canvas, Game& game, const Box& box, bool focused, float lift)
	{
		const Box drawn = box.Scaled(1.0f + lift);
		const ui::Picture& picture = Cover(game);
		uint32_t glow = game.entry.ambient[0] ? game.entry.ambient[0] : picture.ambient[0];
		if (!glow)
			glow = Accent();
		glow = ui::Mix(glow, 0xffffffff, 0.25f) | 0xff000000;
		canvas.Shadow(drawn, kRadiusCover, focused ? 26.0f : 12.0f, focused ? 60.0f : 30.0f, focused ? 0x99000000 : 0x73000000);
		if (picture.texture)
			canvas.ImageCover(picture.texture, picture.width, picture.height, drawn, kRadiusCover, 0xffffffff, 0.0f);
		else
		{
			// no box art: a card in the game's colours (or the side's), the side's band, its icon and name
			const uint32_t top = game.entry.ambient[0] ? ui::Mix(game.entry.ambient[0], 0xffffffff, 0.1f) : ui::Mix(Accent(), kInk1, 0.55f);
			const uint32_t bottom = game.entry.ambient[1] ? game.entry.ambient[1] : kInk1;
			canvas.LinearGradient(drawn, kRadiusCover, top | 0xff000000, bottom | 0xff000000, drawn.x, drawn.y, drawn.x, drawn.Bottom());
			canvas.PushClip(drawn, kRadiusCover);
			const float band = drawn.h * 0.09f;
			canvas.Rect({drawn.x, drawn.y, drawn.w, band}, 0, Is3ds() ? 0xfff4f4f4 : 0xffe08923);
			const ui::TextStyle bandStyle{std::max(9.0f, band * 0.5f), ui::Weight::Bold, 1.0f, 1.0f, true};
			const ui::TextBlock bandText = m_fonts.Layout(bandStyle, Is3ds() ? "Nintendo 3DS" : "Wii U");
			canvas.Text(bandText, drawn.x + drawn.w * 0.08f, drawn.y + (band - bandText.height) * 0.5f, Is3ds() ? 0xff2222cc : 0xe6ffffff);
			const ui::Picture& icon = m_images->Get(IconOf(game));
			if (icon.texture)
			{
				const float size = std::min(drawn.w, drawn.h) * 0.42f;
				canvas.Image(icon.texture, {drawn.CentreX() - size / 2, drawn.y + band + drawn.h * 0.12f, size, size}, size * 0.12f);
			}
			canvas.PopClip();
			DrawGameName(canvas, game, drawn);
		}
		// light from above, and a hairline edge
		canvas.LinearGradient(drawn, kRadiusCover, 0x29ffffff, 0x00ffffff, drawn.x, drawn.y, drawn.x + drawn.w * 0.35f, drawn.y + drawn.h * 0.38f);
		canvas.Ring(drawn, kRadiusCover, 1, 0x14ffffff);
		if (focused)
			Focus(drawn, kRadiusCover, glow);
	}

	void Shell::DrawToggle(Canvas& canvas, float x, float y, bool on, bool focused)
	{
		const Box track{x, y, 64, 36};
		canvas.Rect(track, 18, on ? Accent() : 0x2effffff);
		const float thumb = on ? x + 64 - 4 - 28 : x + 4;
		canvas.Rect({thumb, y + 4, 28, 28}, 14, on ? 0xffffffff : (focused ? 0xffd8dde6 : 0xffb4bac6));
	}

	void Shell::DrawRow(Canvas& canvas, const Row& row, const Box& box, bool focused)
	{
		canvas.PushAlpha(row.dimmed ? 0.5f : 1.0f);
		canvas.Rect(box, kRadiusCard, focused ? Surface2() : Surface());
		canvas.Ring(box, kRadiusCard, 1.5f, kGlassEdge);
		const ui::TextStyle label = Style(kBodyStyle);
		const ui::TextStyle caption = Style(kCaptionStyle);
		const ui::TextStyle value = Style(kLabelStyle);
		const bool described = focused && !row.description.empty();
		const float inner = box.w - 56;
		const float right = box.Right() - 28;

		// the control on the right first, then the label in what it leaves (a third of the row at the
		// least): a longer language's words make the control's text smaller, then cut it, never let it
		// run into the label (7.2)
		const float labelNatural = m_fonts.Width(label, row.label);
		const float labelLeast = std::min(labelNatural, inner * 0.34f);
		// fixed: the control's part that is not its value's text; valueMost: how wide that text may be
		float fixed = 0, valueMost = 0, control = 0;
		ui::TextStyle segment = Style({22, ui::Weight::SemiBold, 1.0f});
		float segmentShare = 0; // each segment's widest, when even the smaller text does not fit
		static constexpr float kPip = 14, kPipGap = 6, kTrack = 220, kSwatch = 46;
		switch (row.kind)
		{
		case Row::Kind::Toggle: fixed = 64; break;
		case Row::Kind::Slider: fixed = kTrack + 20, valueMost = 140; break;
		case Row::Kind::Stepper: fixed = row.options.size() * (kPip + kPipGap) - kPipGap + 16, valueMost = inner * 0.4f; break;
		case Row::Kind::Swatches: fixed = row.options.size() * (kSwatch + 8) + 20, valueMost = inner * 0.3f; break;
		case Row::Kind::Segmented:
		{
			const float room = inner - labelLeast - 24;
			auto total = [&] {
				float width = 8;
				for (const auto& option : row.options)
					width += m_fonts.Width(segment, option) + 40;
				return width;
			};
			while (total() > room && segment.size > 16)
				segment.size -= 1;
			if (total() > room)
				segmentShare = (room - 8) / std::max<size_t>(1, row.options.size()) - 40;
			fixed = std::min(total(), room);
			break;
		}
		case Row::Kind::Choice: fixed = focused ? 34 + 34 : 0, valueMost = inner * 0.42f; break;
		case Row::Kind::Action:
		case Row::Kind::Hold:
		case Row::Kind::Link: fixed = row.kind == Row::Kind::Link ? 40 : 0, valueMost = row.value.empty() ? 0.0f : inner * 0.45f; break;
		}
		control = fixed + std::min(m_fonts.Width(value, row.value), valueMost);
		const float labelWidth = std::max(inner * 0.3f, inner - control - 24);
		const ui::TextBlock labelBlock = m_fonts.Layout(label, row.label, labelWidth, 1);
		const float labelY = described ? box.y + 16 : box.CentreY() - labelBlock.height * 0.5f;
		canvas.Text(labelBlock, box.x + 28, labelY, kText);
		if (described)
			canvas.Text(caption, box.x + 28, labelY + labelBlock.height + 4, row.description, Secondary(), inner, 1);
		// the control, on the right
		const float centre = described ? labelY + labelBlock.height * 0.5f : box.CentreY();
		// the value's text: as wide as it may be, and no wider than the label leaves it
		const float valueRoom = std::max(60.0f, std::min(valueMost, inner - labelBlock.width - 24 - fixed));
		switch (row.kind)
		{
		case Row::Kind::Toggle: DrawToggle(canvas, right - 64, centre - 18, row.on, focused); break;
		case Row::Kind::Slider:
		{
			const float x = right - kTrack;
			canvas.Rect({x, centre - 3, kTrack, 6}, 3, 0x2effffff);
			canvas.Rect({x, centre - 3, kTrack * row.fraction, 6}, 3, Accent());
			canvas.Rect({x + kTrack * row.fraction - 11, centre - 11, 22, 22}, 11, 0xffffffff);
			const ui::TextBlock text = m_fonts.Layout(value, row.value, valueRoom, 1);
			canvas.Text(text, x - 20 - text.width, centre - text.height * 0.5f, focused ? kText : Secondary());
			break;
		}
		case Row::Kind::Stepper:
		{
			const int count = (int)row.options.size();
			float x = right - count * (kPip + kPipGap) + kPipGap;
			for (int i = 0; i < count; i++, x += kPip + kPipGap)
				canvas.Rect({x, centre - (i == row.index ? 9.0f : 5.0f), kPip, i == row.index ? 18.0f : 10.0f}, 4,
					i <= row.index ? (i == row.index ? Accent() : ui::SetAlpha(Accent(), 0.45f)) : 0x2effffff);
			const ui::TextBlock text = m_fonts.Layout(value, row.value, valueRoom, 1);
			canvas.Text(text, right - count * (kPip + kPipGap) - 16 - text.width, centre - text.height * 0.5f, focused ? kText : Secondary());
			break;
		}
		case Row::Kind::Segmented:
		{
			auto segmentWidth = [&](const std::string& option) {
				const float natural = m_fonts.Width(segment, option);
				return (segmentShare > 0 ? std::min(natural, segmentShare) : natural) + 40;
			};
			float width = 8;
			for (const auto& option : row.options)
				width += segmentWidth(option);
			float x = right - width;
			canvas.Rect({x, centre - 26, width, 52}, 14, 0x12ffffff);
			x += 4;
			for (int i = 0; i < (int)row.options.size(); i++)
			{
				const float w = segmentWidth(row.options[i]);
				if (i == row.index)
					canvas.Rect({x, centre - 22, w, 44}, 11, kText);
				const ui::TextBlock text = m_fonts.Layout(segment, row.options[i], w - 40 + 0.5f, 1);
				canvas.Text(text, x + 20, centre - text.height * 0.5f, i == row.index ? kInk1 : Secondary());
				x += w;
			}
			break;
		}
		case Row::Kind::Swatches:
		{
			static const uint32_t kSwatches[][2] = {{0xff1b1714, 0xff1b1714}, {0xff5c2a1b, 0xff8a4a2e}, {0xff2a9de0, 0xff47c4f4},
				{0xffc8b44f, 0xffd06a9a}, {0xffb0d5e8, 0xff90bbd8}, {0xffffa95a, 0xff3fb6f4}};
			const ui::TextBlock text = m_fonts.Layout(value, row.value, valueRoom, 1);
			float x = right - text.width;
			canvas.Text(text, x, centre - text.height * 0.5f, focused ? kText : Secondary());
			x -= 20;
			const int count = (int)row.options.size();
			for (int i = count - 1; i >= 0; i--)
			{
				x -= kSwatch;
				const Box swatch{x, centre - 16, kSwatch, 32};
				const uint32_t* colours = kSwatches[std::min(i, 5)];
				canvas.LinearGradient(swatch, 8, colours[0], colours[1], swatch.x, swatch.y, swatch.Right(), swatch.y);
				canvas.Ring(swatch, 8, i == row.index ? 3.0f : 1.5f, i == row.index ? Accent() : 0x33ffffff);
				x -= 8;
			}
			break;
		}
		case Row::Kind::Choice:
		{
			const ui::TextBlock text = m_fonts.Layout(value, row.value, valueRoom, 1);
			float x = right - (focused ? 34 : 0) - text.width;
			if (focused)
			{
				canvas.Draw(Icon::ChevronRight, {right - 26, centre - 13, 26, 26}, Accent());
				canvas.Draw(Icon::ChevronLeft, {x - 34, centre - 13, 26, 26}, Accent());
			}
			canvas.Text(text, x, centre - text.height * 0.5f, focused ? kText : Secondary());
			break;
		}
		case Row::Kind::Action:
		case Row::Kind::Hold:
		case Row::Kind::Link:
		{
			float x = right;
			if (row.kind == Row::Kind::Link)
			{
				canvas.Draw(Icon::ChevronRight, {right - 26, centre - 13, 26, 26}, focused ? kText : Secondary());
				x -= 40;
			}
			if (!row.value.empty())
			{
				const ui::TextBlock text = m_fonts.Layout(value, row.value, valueRoom, 1);
				canvas.Text(text, x - text.width, centre - text.height * 0.5f, focused ? kText : Secondary());
			}
			break;
		}
		}
		canvas.PopAlpha();
		if (focused)
			Focus(box, kRadiusCard);
	}

	void Shell::HoldRing(Canvas& canvas, const Box& box, float progress)
	{
		if (progress <= 0)
			return;
		canvas.PushClip(box, kRadiusCard);
		canvas.Rect({box.x, box.y, box.w * std::min(progress, 1.0f), box.h}, 0, ui::SetAlpha(Accent(), 0.28f));
		canvas.PopClip();
	}

	float Shell::Enter(int part) const
	{
		const float duration = m_settings.ui.reduceMotion ? kReducedSeconds : kScreenSeconds;
		const float t = (float)((m_now - m_screenAt - part * kStaggerSeconds) / duration);
		return m_settings.ui.reduceMotion ? std::clamp(t, 0.0f, 1.0f) : ui::ease::QuintOut(t);
	}

	void Shell::DrawSheet(Canvas& canvas, const Box& box, float appear)
	{
		canvas.Shadow(box, kRadiusSheet, 24, 60, ui::WithAlpha(0x99000000, appear));
		canvas.Rect(box, kRadiusSheet, m_settings.ui.highContrast ? kInk2 : 0xf51b0f0a);
		canvas.Ring(box, kRadiusSheet, 1.5f, kGlassEdge);
	}

	void Shell::Scrim(Canvas& canvas, float alpha)
	{
		canvas.Rect({0, 0, 1920, 1080}, 0, ui::WithAlpha(kScrim, alpha));
	}
}
