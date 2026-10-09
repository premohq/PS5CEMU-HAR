// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's UI kit: drawing a frame (docs/UI-REDESIGN.md, 9.3). The canvas records shapes,
// pictures and text into the frame's draw list in the 1920 x 1080 layout space, inside the clips,
// opacities and offsets pushed; Gfx draws the list. Icons and the controller's glyphs are drawn as
// shapes, so they tint and scale with no atlas.

#pragma once

#include "gfx.h"
#include "text.h"

#include <string_view>
#include <vector>

namespace ui
{
	struct Box
	{
		float x = 0, y = 0, w = 0, h = 0;

		float Right() const { return x + w; }
		float Bottom() const { return y + h; }
		float CentreX() const { return x + w * 0.5f; }
		float CentreY() const { return y + h * 0.5f; }
		Box Inset(float by) const { return {x + by, y + by, w - 2 * by, h - 2 * by}; }
		Box Inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
		Box Offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
		// grown about its centre by a factor
		Box Scaled(float factor) const { return {x - w * (factor - 1) * 0.5f, y - h * (factor - 1) * 0.5f, w * factor, h * factor}; }
	};

	// Colours: RGBA, R in the lowest byte (tokens.h)
	uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
	uint32_t WithAlpha(uint32_t colour, float alpha);  // alpha times the colour's own
	uint32_t SetAlpha(uint32_t colour, float alpha);   // alpha in place of the colour's own
	uint32_t Mix(uint32_t a, uint32_t b, float t);
	float Luminance(uint32_t colour);

	enum class Align
	{
		Left,
		Centre,
		Right,
	};

	enum class Icon
	{
		Cross,
		Circle,
		Square,
		Triangle,
		Options,
		Touchpad,
		L1,
		R1,
		L2,
		R2,
		UpDown,
		LeftRight,
		Play,
		Check,
		Warning,
		Dash,
		ChevronLeft,
		ChevronRight,
		ChevronDown,
		More,
		Gear,
		Star,
		Folder,
		Drive,
		Search,
		Sort,
		Back,
		// Settings' pages
		Monitor,
		Screens,
		Speaker,
		Pad,
		Usb,
		Globe,
		Sparkle,
		Pulse,
		Info,
		Chip,
		Artic,
	};

	class Canvas
	{
	public:
		Canvas(DrawList& list, Fonts& fonts) : m_list(list), m_fonts(fonts) {}

		Fonts& GetFonts() { return m_fonts; }

		void Rect(const Box& box, float radius, uint32_t colour);
		void LinearGradient(const Box& box, float radius, uint32_t from, uint32_t to, float x0, float y0, float x1, float y1, float start = 0);
		// from at the centre to `to` at the ellipse's edge (radii rx, ry)
		void RadialGradient(const Box& box, float radius, uint32_t from, uint32_t to, float cx, float cy, float rx, float ry, float start = 0);
		void Ring(const Box& box, float radius, float width, uint32_t colour);
		void Shadow(const Box& box, float radius, float offsetY, float softness, uint32_t colour);
		// light around the box, none over it (a focused element's glow, drawn after it)
		void Glow(const Box& box, float radius, float softness, uint32_t colour);
		// a picture in the box, its rectangle (u0, v0)-(u1, v1)
		void Image(TextureId texture, const Box& box, float radius, uint32_t tint = 0xffffffff, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1);
		// a picture filling the box, cut to its shape (width x height: the picture's)
		void ImageCover(TextureId texture, int width, int height, const Box& box, float radius, uint32_t tint = 0xffffffff, float alignY = 0.5f);
		void Line(float x0, float y0, float x1, float y1, float width, uint32_t colour);
		void Triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t colour, float outline = 0, float round = 0);
		void Wave(const Box& box, float base, float amplitude, float wavelength, float phase, uint32_t colour);
		void Grain(const Box& box, uint32_t colour);
		void Draw(Icon icon, const Box& box, uint32_t colour);

		// text from its block's top left; Draw returns the block laid out
		void Text(const TextBlock& block, float x, float y, uint32_t colour, Align align = Align::Left, float alignWidth = 0);
		TextBlock Text(const TextStyle& style, float x, float y, std::string_view utf8, uint32_t colour, float maxWidth = 0, int maxLines = 0,
			Align align = Align::Left);

		void PushClip(const Box& box, float radius = 0);
		void PopClip();
		void PushAlpha(float alpha);
		void PopAlpha();
		void PushOffset(float dx, float dy);
		void PopOffset();
		float Alpha() const { return m_alpha.empty() ? 1.0f : m_alpha.back(); }

	private:
		void Add(Instance instance, TextureId texture = 0);
		Instance Make(const Box& quad, const Box& shape, float radius, Kind kind, uint32_t colour);

		struct Clip
		{
			Box box;
			float radius;
		};
		DrawList& m_list;
		Fonts& m_fonts;
		std::vector<Clip> m_clips;
		std::vector<float> m_alpha;
		std::vector<std::pair<float, float>> m_offsets;
		float m_dx = 0, m_dy = 0;
	};
}
