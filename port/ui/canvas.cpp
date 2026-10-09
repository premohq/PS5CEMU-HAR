// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's UI kit: drawing a frame (canvas.h).

#include "canvas.h"

#include <algorithm>
#include <cmath>

namespace ui
{
	uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
	{
		return r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24;
	}

	uint32_t WithAlpha(uint32_t colour, float alpha)
	{
		const float a = (colour >> 24) * std::clamp(alpha, 0.0f, 1.0f);
		return (colour & 0xffffff) | (uint32_t)std::lround(a) << 24;
	}

	uint32_t SetAlpha(uint32_t colour, float alpha)
	{
		return (colour & 0xffffff) | (uint32_t)std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255) << 24;
	}

	uint32_t Mix(uint32_t a, uint32_t b, float t)
	{
		t = std::clamp(t, 0.0f, 1.0f);
		uint32_t out = 0;
		for (int shift = 0; shift < 32; shift += 8)
		{
			const float x = (a >> shift & 255) * (1 - t) + (b >> shift & 255) * t;
			out |= (uint32_t)std::lround(x) << shift;
		}
		return out;
	}

	float Luminance(uint32_t colour)
	{
		auto linear = [](float c) {
			c /= 255.0f;
			return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
		};
		return 0.2126f * linear(colour & 255) + 0.7152f * linear(colour >> 8 & 255) + 0.0722f * linear(colour >> 16 & 255);
	}

	Instance Canvas::Make(const Box& quad, const Box& shape, float radius, Kind kind, uint32_t colour)
	{
		Instance instance{};
		instance.rect[0] = quad.x + m_dx;
		instance.rect[1] = quad.y + m_dy;
		instance.rect[2] = quad.w;
		instance.rect[3] = quad.h;
		instance.box[0] = shape.x + m_dx;
		instance.box[1] = shape.y + m_dy;
		instance.box[2] = shape.w;
		instance.box[3] = shape.h;
		instance.shape[0] = radius;
		instance.shape[3] = (float)kind;
		instance.colour0 = instance.colour1 = WithAlpha(colour, Alpha());
		return instance;
	}

	void Canvas::Add(Instance instance, TextureId texture)
	{
		if ((instance.colour0 >> 24) == 0 && (instance.colour1 >> 24) == 0)
			return;
		float left = 0, top = 0, right = 1920, bottom = 1080;
		if (!m_clips.empty())
		{
			const Clip& clip = m_clips.back();
			left = clip.box.x, top = clip.box.y, right = clip.box.Right(), bottom = clip.box.Bottom();
			instance.clip[0] = clip.box.x;
			instance.clip[1] = clip.box.y;
			instance.clip[2] = std::max(clip.box.w, 0.01f);
			instance.clip[3] = std::max(clip.box.h, 0.01f);
			instance.extra[0] = clip.radius;
		}
		// nothing that is all outside what can show
		if (instance.rect[0] >= right || instance.rect[1] >= bottom || instance.rect[0] + instance.rect[2] <= left ||
			instance.rect[1] + instance.rect[3] <= top || instance.rect[2] <= 0 || instance.rect[3] <= 0)
			return;
		m_list.Add(instance, texture);
	}

	void Canvas::Rect(const Box& box, float radius, uint32_t colour)
	{
		Add(Make(box, box, radius, kFill, colour));
	}

	void Canvas::LinearGradient(const Box& box, float radius, uint32_t from, uint32_t to, float x0, float y0, float x1, float y1, float start)
	{
		Instance instance = Make(box, box, radius, kFill, from);
		instance.colour1 = WithAlpha(to, Alpha());
		instance.gradient[0] = x0 + m_dx;
		instance.gradient[1] = y0 + m_dy;
		instance.gradient[2] = x1 + m_dx;
		instance.gradient[3] = y1 + m_dy;
		instance.extra[1] = 1;
		instance.extra[3] = start;
		Add(instance);
	}

	void Canvas::RadialGradient(const Box& box, float radius, uint32_t from, uint32_t to, float cx, float cy, float rx, float ry, float start)
	{
		Instance instance = Make(box, box, radius, kFill, from);
		instance.colour1 = WithAlpha(to, Alpha());
		instance.gradient[0] = cx + m_dx;
		instance.gradient[1] = cy + m_dy;
		instance.gradient[2] = rx;
		instance.gradient[3] = ry;
		instance.extra[1] = 2;
		instance.extra[3] = start;
		Add(instance);
	}

	void Canvas::Ring(const Box& box, float radius, float width, uint32_t colour)
	{
		Instance instance = Make(box, box, radius, kRing, colour);
		instance.shape[1] = width;
		Add(instance);
	}

	void Canvas::Shadow(const Box& box, float radius, float offsetY, float softness, uint32_t colour)
	{
		const Box shape = box.Offset(0, offsetY);
		const Box quad = shape.Inset(-softness * 1.2f);
		Instance instance = Make(quad, shape, radius, kShadow, colour);
		instance.shape[2] = softness;
		Add(instance);
	}

	void Canvas::Glow(const Box& box, float radius, float softness, uint32_t colour)
	{
		Instance instance = Make(box.Inset(-softness * 1.2f), box, radius, kShadow, colour);
		instance.shape[1] = 1; // outside the box only
		instance.shape[2] = softness;
		Add(instance);
	}

	void Canvas::Image(TextureId texture, const Box& box, float radius, uint32_t tint, float u0, float v0, float u1, float v1)
	{
		Instance instance = Make(box, box, radius, kImage, tint);
		instance.uv[0] = u0;
		instance.uv[1] = v0;
		instance.uv[2] = u1;
		instance.uv[3] = v1;
		Add(instance, texture);
	}

	void Canvas::ImageCover(TextureId texture, int width, int height, const Box& box, float radius, uint32_t tint, float alignY)
	{
		if (width <= 0 || height <= 0 || box.w <= 0 || box.h <= 0)
			return;
		const float picture = (float)width / height, shape = box.w / box.h;
		float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
		if (picture > shape)
		{
			const float used = shape / picture;
			u0 = (1 - used) * 0.5f;
			u1 = u0 + used;
		}
		else
		{
			const float used = picture / shape;
			v0 = (1 - used) * alignY;
			v1 = v0 + used;
		}
		Image(texture, box, radius, tint, u0, v0, u1, v1);
	}

	void Canvas::Line(float x0, float y0, float x1, float y1, float width, uint32_t colour)
	{
		const float pad = width * 0.5f + 2;
		const Box quad{std::min(x0, x1) - pad, std::min(y0, y1) - pad, std::fabs(x1 - x0) + 2 * pad, std::fabs(y1 - y0) + 2 * pad};
		Instance instance = Make(quad, quad, 0, kLine, colour);
		instance.shape[1] = width;
		instance.gradient[0] = x0 + m_dx;
		instance.gradient[1] = y0 + m_dy;
		instance.gradient[2] = x1 + m_dx;
		instance.gradient[3] = y1 + m_dy;
		Add(instance);
	}

	void Canvas::Triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t colour, float outline, float round)
	{
		const float pad = round + outline + 2;
		const float left = std::min({x0, x1, x2}) - pad, top = std::min({y0, y1, y2}) - pad;
		const Box quad{left, top, std::max({x0, x1, x2}) + pad - left, std::max({y0, y1, y2}) + pad - top};
		Instance instance = Make(quad, quad, round, kTriangle, colour);
		instance.shape[1] = outline;
		instance.gradient[0] = x0 + m_dx;
		instance.gradient[1] = y0 + m_dy;
		instance.gradient[2] = x1 + m_dx;
		instance.gradient[3] = y1 + m_dy;
		instance.uv[0] = x2 + m_dx;
		instance.uv[1] = y2 + m_dy;
		Add(instance);
	}

	void Canvas::Wave(const Box& box, float base, float amplitude, float wavelength, float phase, uint32_t colour)
	{
		Instance instance = Make(box, box, 0, kWave, colour);
		instance.gradient[0] = base + m_dy;
		instance.gradient[1] = amplitude;
		instance.gradient[2] = wavelength;
		instance.gradient[3] = phase - (m_dx / std::max(wavelength, 1e-3f));
		Add(instance);
	}

	void Canvas::Grain(const Box& box, uint32_t colour)
	{
		Add(Make(box, box, 0, kGrain, colour));
	}

	void Canvas::Text(const TextBlock& block, float x, float y, uint32_t colour, Align align, float alignWidth)
	{
		for (const TextBlock::Line& line : block.lines)
		{
			float dx = 0;
			if (align == Align::Centre)
				dx = (alignWidth - line.width) * 0.5f;
			else if (align == Align::Right)
				dx = alignWidth - line.width;
			for (uint32_t i = line.first; i < line.first + line.count; i++)
			{
				const PlacedGlyph& glyph = block.glyphs[i];
				const Box quad{x + dx + glyph.x, y + glyph.y, glyph.width, glyph.height};
				Instance instance = Make(quad, quad, 0, kGlyph, colour);
				instance.uv[0] = glyph.u0;
				instance.uv[1] = glyph.v0;
				instance.uv[2] = glyph.u1;
				instance.uv[3] = glyph.v1;
				instance.extra[2] = glyph.weight;
				Add(instance);
			}
		}
	}

	TextBlock Canvas::Text(const TextStyle& style, float x, float y, std::string_view utf8, uint32_t colour, float maxWidth, int maxLines, Align align)
	{
		TextBlock block = m_fonts.Layout(style, utf8, maxWidth, maxLines);
		if (maxWidth > 0) // each line placed in the width
			Text(block, x, y, colour, align, maxWidth);
		else // x is where the line starts, its middle or its end
			Text(block, align == Align::Left ? x : align == Align::Centre ? x - block.width * 0.5f : x - block.width, y, colour);
		return block;
	}

	void Canvas::Draw(Icon icon, const Box& box, uint32_t colour)
	{
		const float s = std::min(box.w, box.h);
		const float ox = box.x + (box.w - s) * 0.5f, oy = box.y + (box.h - s) * 0.5f;
		auto X = [&](float f) { return ox + f * s; };
		auto Y = [&](float f) { return oy + f * s; };
		const float stroke = std::max(1.5f, s * 0.09f);
		auto line = [&](float x0, float y0, float x1, float y1, float width = 0) { Line(X(x0), Y(y0), X(x1), Y(y1), width > 0 ? width * s : stroke, colour); };
		auto dot = [&](float x, float y, float r) { Rect({X(x - r), Y(y - r), 2 * r * s, 2 * r * s}, r * s, colour); };
		auto shoulder = [&](const char* label) {
			const Box outline{X(0.02f), Y(0.18f), s * 0.96f, s * 0.64f};
			Ring(outline, s * 0.16f, std::max(1.5f, s * 0.07f), colour);
			TextStyle style{s * 0.34f, Weight::Bold, 1.0f};
			const TextBlock block = m_fonts.Layout(style, label);
			Text(block, outline.CentreX() - block.width * 0.5f, outline.CentreY() - block.height * 0.5f, colour);
		};
		switch (icon)
		{
		case Icon::Cross:
			line(0.24f, 0.24f, 0.76f, 0.76f);
			line(0.76f, 0.24f, 0.24f, 0.76f);
			break;
		case Icon::Circle: Ring({X(0.17f), Y(0.17f), s * 0.66f, s * 0.66f}, s, stroke, colour); break;
		case Icon::Square: Ring({X(0.2f), Y(0.2f), s * 0.6f, s * 0.6f}, s * 0.06f, stroke, colour); break;
		case Icon::Triangle: Triangle(X(0.5f), Y(0.18f), X(0.86f), Y(0.78f), X(0.14f), Y(0.78f), colour, stroke, s * 0.02f); break;
		case Icon::Options:
			for (float y : {0.3f, 0.5f, 0.7f})
				line(0.2f, y, 0.8f, y, 0.075f);
			break;
		case Icon::Touchpad: Ring({X(0.08f), Y(0.28f), s * 0.84f, s * 0.44f}, s * 0.1f, stroke * 0.85f, colour); break;
		case Icon::L1: shoulder("L1"); break;
		case Icon::R1: shoulder("R1"); break;
		case Icon::L2: shoulder("L2"); break;
		case Icon::R2: shoulder("R2"); break;
		case Icon::UpDown:
			line(0.5f, 0.16f, 0.5f, 0.84f);
			line(0.32f, 0.34f, 0.5f, 0.16f);
			line(0.68f, 0.34f, 0.5f, 0.16f);
			line(0.32f, 0.66f, 0.5f, 0.84f);
			line(0.68f, 0.66f, 0.5f, 0.84f);
			break;
		case Icon::LeftRight:
			line(0.12f, 0.5f, 0.88f, 0.5f);
			line(0.3f, 0.32f, 0.12f, 0.5f);
			line(0.3f, 0.68f, 0.12f, 0.5f);
			line(0.7f, 0.32f, 0.88f, 0.5f);
			line(0.7f, 0.68f, 0.88f, 0.5f);
			break;
		case Icon::Play: Triangle(X(0.3f), Y(0.2f), X(0.82f), Y(0.5f), X(0.3f), Y(0.8f), colour, 0, s * 0.04f); break;
		case Icon::Check:
			line(0.2f, 0.52f, 0.42f, 0.74f);
			line(0.42f, 0.74f, 0.82f, 0.28f);
			break;
		case Icon::Warning:
			Triangle(X(0.5f), Y(0.12f), X(0.92f), Y(0.86f), X(0.08f), Y(0.86f), colour, stroke * 0.9f, s * 0.03f);
			line(0.5f, 0.38f, 0.5f, 0.6f, 0.08f);
			dot(0.5f, 0.73f, 0.045f);
			break;
		case Icon::Dash: line(0.28f, 0.5f, 0.72f, 0.5f); break;
		case Icon::ChevronLeft:
			line(0.62f, 0.2f, 0.36f, 0.5f);
			line(0.36f, 0.5f, 0.62f, 0.8f);
			break;
		case Icon::ChevronRight:
			line(0.38f, 0.2f, 0.64f, 0.5f);
			line(0.64f, 0.5f, 0.38f, 0.8f);
			break;
		case Icon::ChevronDown:
			line(0.2f, 0.38f, 0.5f, 0.64f);
			line(0.5f, 0.64f, 0.8f, 0.38f);
			break;
		case Icon::More:
			for (float x : {0.26f, 0.5f, 0.74f})
				dot(x, 0.5f, 0.07f);
			break;
		case Icon::Gear:
			Ring({X(0.33f), Y(0.33f), s * 0.34f, s * 0.34f}, s, stroke * 0.85f, colour);
			for (int i = 0; i < 8; i++)
			{
				const float a = i * 3.14159265f / 4;
				line(0.5f + std::cos(a) * 0.27f, 0.5f + std::sin(a) * 0.27f, 0.5f + std::cos(a) * 0.4f, 0.5f + std::sin(a) * 0.4f, 0.075f);
			}
			break;
		case Icon::Star:
		{
			float px[10], py[10];
			for (int i = 0; i < 10; i++)
			{
				const float a = -3.14159265f / 2 + i * 3.14159265f / 5;
				const float r = i % 2 ? 0.19f : 0.44f;
				px[i] = X(0.5f + std::cos(a) * r);
				py[i] = Y(0.53f + std::sin(a) * r);
			}
			for (int i = 0; i < 10; i += 2)
				Triangle(px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], px[(i + 9) % 10], py[(i + 9) % 10], colour, 0, s * 0.01f);
			Triangle(px[1], py[1], px[5], py[5], px[9], py[9], colour, 0, s * 0.012f);
			Triangle(px[1], py[1], px[3], py[3], px[5], py[5], colour, 0, s * 0.012f);
			Triangle(px[5], py[5], px[7], py[7], px[9], py[9], colour, 0, s * 0.012f);
			break;
		}
		case Icon::Folder:
			Rect({X(0.12f), Y(0.24f), s * 0.34f, s * 0.16f}, s * 0.05f, colour);
			Rect({X(0.12f), Y(0.32f), s * 0.76f, s * 0.46f}, s * 0.07f, colour);
			break;
		case Icon::Drive:
			Ring({X(0.12f), Y(0.3f), s * 0.76f, s * 0.4f}, s * 0.08f, stroke * 0.85f, colour);
			dot(0.72f, 0.5f, 0.05f);
			break;
		case Icon::Search:
			Ring({X(0.16f), Y(0.16f), s * 0.48f, s * 0.48f}, s, stroke, colour);
			line(0.58f, 0.58f, 0.84f, 0.84f);
			break;
		case Icon::Sort:
			line(0.2f, 0.3f, 0.8f, 0.3f, 0.075f);
			line(0.2f, 0.5f, 0.64f, 0.5f, 0.075f);
			line(0.2f, 0.7f, 0.46f, 0.7f, 0.075f);
			break;
		case Icon::Back:
			line(0.2f, 0.5f, 0.8f, 0.5f);
			line(0.2f, 0.5f, 0.42f, 0.28f);
			line(0.2f, 0.5f, 0.42f, 0.72f);
			break;
		case Icon::Monitor:
			Ring({X(0.12f), Y(0.18f), s * 0.76f, s * 0.5f}, s * 0.08f, stroke * 0.85f, colour);
			line(0.5f, 0.68f, 0.5f, 0.82f);
			line(0.36f, 0.84f, 0.64f, 0.84f);
			break;
		case Icon::Screens: // a handheld's two screens
			Ring({X(0.24f), Y(0.12f), s * 0.52f, s * 0.34f}, s * 0.06f, stroke * 0.85f, colour);
			Ring({X(0.3f), Y(0.56f), s * 0.4f, s * 0.3f}, s * 0.06f, stroke * 0.85f, colour);
			break;
		case Icon::Speaker:
			Triangle(X(0.14f), Y(0.5f), X(0.5f), Y(0.16f), X(0.5f), Y(0.84f), colour, 0, s * 0.02f);
			Rect({X(0.14f), Y(0.36f), s * 0.2f, s * 0.28f}, s * 0.03f, colour);
			// the sound: two arcs, the right halves of rings
			PushClip({X(0.56f), Y(0.0f), s * 0.44f, s});
			Ring({X(0.4f), Y(0.32f), s * 0.36f, s * 0.36f}, s, stroke * 0.75f, colour);
			Ring({X(0.26f), Y(0.16f), s * 0.64f, s * 0.68f}, s, stroke * 0.75f, colour);
			PopClip();
			break;
		case Icon::Pad:
			Ring({X(0.08f), Y(0.28f), s * 0.84f, s * 0.44f}, s * 0.2f, stroke * 0.85f, colour);
			line(0.3f, 0.4f, 0.3f, 0.6f, 0.07f);
			line(0.2f, 0.5f, 0.4f, 0.5f, 0.07f);
			dot(0.66f, 0.44f, 0.05f);
			dot(0.76f, 0.56f, 0.05f);
			break;
		case Icon::Usb:
			line(0.5f, 0.1f, 0.5f, 0.72f);
			line(0.5f, 0.44f, 0.28f, 0.32f);
			line(0.5f, 0.54f, 0.72f, 0.42f);
			dot(0.28f, 0.3f, 0.06f);
			Rect({X(0.66f), Y(0.32f), s * 0.12f, s * 0.12f}, 0, colour);
			dot(0.5f, 0.8f, 0.09f);
			break;
		case Icon::Globe:
			Ring({X(0.14f), Y(0.14f), s * 0.72f, s * 0.72f}, s, stroke * 0.85f, colour);
			Ring({X(0.34f), Y(0.14f), s * 0.32f, s * 0.72f}, s, stroke * 0.75f, colour);
			line(0.14f, 0.5f, 0.86f, 0.5f, 0.07f);
			break;
		case Icon::Sparkle:
			Triangle(X(0.5f), Y(0.08f), X(0.62f), Y(0.5f), X(0.38f), Y(0.5f), colour, 0, s * 0.01f);
			Triangle(X(0.5f), Y(0.92f), X(0.38f), Y(0.5f), X(0.62f), Y(0.5f), colour, 0, s * 0.01f);
			Triangle(X(0.08f), Y(0.5f), X(0.5f), Y(0.38f), X(0.5f), Y(0.62f), colour, 0, s * 0.01f);
			Triangle(X(0.92f), Y(0.5f), X(0.5f), Y(0.62f), X(0.5f), Y(0.38f), colour, 0, s * 0.01f);
			break;
		case Icon::Pulse:
			line(0.08f, 0.54f, 0.28f, 0.54f);
			line(0.28f, 0.54f, 0.38f, 0.26f);
			line(0.38f, 0.26f, 0.54f, 0.76f);
			line(0.54f, 0.76f, 0.64f, 0.44f);
			line(0.64f, 0.44f, 0.7f, 0.54f);
			line(0.7f, 0.54f, 0.92f, 0.54f);
			break;
		case Icon::Info:
			Ring({X(0.14f), Y(0.14f), s * 0.72f, s * 0.72f}, s, stroke * 0.85f, colour);
			line(0.5f, 0.46f, 0.5f, 0.68f, 0.085f);
			dot(0.5f, 0.32f, 0.055f);
			break;
		case Icon::Chip:
			Ring({X(0.26f), Y(0.26f), s * 0.48f, s * 0.48f}, s * 0.08f, stroke * 0.85f, colour);
			for (float f : {0.38f, 0.5f, 0.62f})
			{
				line(f, 0.1f, f, 0.24f, 0.06f);
				line(f, 0.76f, f, 0.9f, 0.06f);
				line(0.1f, f, 0.24f, f, 0.06f);
				line(0.76f, f, 0.9f, f, 0.06f);
			}
			break;
		case Icon::Artic: // a 3DS, open, its signal at both sides
			Ring({X(0.3f), Y(0.12f), s * 0.4f, s * 0.32f}, s * 0.05f, stroke * 0.8f, colour);
			Ring({X(0.3f), Y(0.52f), s * 0.4f, s * 0.36f}, s * 0.05f, stroke * 0.8f, colour);
			line(0.14f, 0.38f, 0.14f, 0.62f, 0.07f);
			line(0.86f, 0.38f, 0.86f, 0.62f, 0.07f);
			break;
		}
	}

	void Canvas::PushClip(const Box& box, float radius)
	{
		Box clip = box.Offset(m_dx, m_dy);
		if (!m_clips.empty())
		{
			const Box& outer = m_clips.back().box;
			const float left = std::max(clip.x, outer.x), top = std::max(clip.y, outer.y);
			const float right = std::min(clip.Right(), outer.Right()), bottom = std::min(clip.Bottom(), outer.Bottom());
			clip = {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
		}
		m_clips.push_back({clip, radius});
	}

	void Canvas::PopClip()
	{
		if (!m_clips.empty())
			m_clips.pop_back();
	}

	void Canvas::PushAlpha(float alpha)
	{
		m_alpha.push_back(Alpha() * std::clamp(alpha, 0.0f, 1.0f));
	}

	void Canvas::PopAlpha()
	{
		if (!m_alpha.empty())
			m_alpha.pop_back();
	}

	void Canvas::PushOffset(float dx, float dy)
	{
		m_offsets.push_back({m_dx, m_dy});
		m_dx += dx;
		m_dy += dy;
	}

	void Canvas::PopOffset()
	{
		if (m_offsets.empty())
			return;
		m_dx = m_offsets.back().first;
		m_dy = m_offsets.back().second;
		m_offsets.pop_back();
	}
}
