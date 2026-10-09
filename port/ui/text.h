// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's UI kit: text (docs/UI-REDESIGN.md, 7.2, 7.7 and 9.6). Lexend's four weights come baked
// into a signed-distance atlas (fonts/lexend.sdf, tools/render-sdf-font.sh: Latin, with what Romanian
// and Vietnamese need), so nothing is rendered as the launcher starts; a character Lexend lacks (a
// Japanese title's, a Russian menu's) is rendered on first use with FreeType from the fonts in the
// fallback folders (the console's own), into the same atlas's free rows, which grow when they fill.
// Of the console's fonts, the menus' language's come first: a Japanese menu's kanji from its Japanese
// font, a Chinese menu's from its Chinese one.
//
// Layout is UTF-8 throughout: lines break at spaces, at zero-width spaces (Thai's word breaks),
// between CJK characters (never before a closing mark or after an opening one) and at a newline;
// text past its last line is first drawn a little smaller (TextStyle::minScale), then ends in an
// ellipsis that replaces whole words; changing numbers can take tabular figures. Arabic is joined
// (its letters' contextual forms) and laid out right to left, its lines' words and numbers in the
// order it reads them, the lines of a paragraph right-aligned in their block. Lines are placed as CSS
// places them: the line box's height is the size times the line height, the font's ascent and
// descent centred in it.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ui
{
	enum class Weight : uint8_t
	{
		Regular,
		Medium,
		SemiBold,
		Bold,
	};

	struct TextStyle
	{
		float size = 26;		// pixels a em, in the layout
		Weight weight = Weight::Regular;
		float lineHeight = 1.45f; // the line box, times the size
		float tracking = 0;		// added after each character, in pixels
		bool upper = false;		// in capitals
		bool tabular = false;	// figures all as wide
		// text that does not fit its lines is drawn this much smaller at the most before an ellipsis
		// cuts it (a longer language's words: 7.2)
		float minScale = 0.8f;
	};

	// A glyph as laid out: its quad (relative to the text's top left) and its rectangle in the atlas,
	// in texels (the canvas divides by the atlas's size as it draws, which may have grown since)
	struct PlacedGlyph
	{
		float x, y, width, height;
		float u0, v0, u1, v1;
		float weight; // how much the shader thickens it (a fallback glyph drawn bolder)
	};

	struct TextBlock
	{
		std::vector<PlacedGlyph> glyphs;
		struct Line
		{
			uint32_t first, count; // in glyphs
			float width;
		};
		std::vector<Line> lines;
		float width = 0, height = 0; // the widest line, and the lines' boxes
		float lineHeight = 0;
		float baseline = 0;			 // the first line's, from the top
		float size = 0;				 // the size it was laid out at (smaller than asked, to fit)
		bool truncated = false;		 // an ellipsis took the rest
		bool rightToLeft = false;	 // Arabic: its lines right-aligned in the block
	};

	class Fonts
	{
	public:
		Fonts();
		~Fonts();
		// The baked atlas. False, with the reason, when it cannot be read.
		bool Load(const std::string& path, std::string& error);
		// Where fonts for the characters Lexend lacks are looked for (.ttf, .otf, .ttc), in order;
		// read the first time one is needed.
		void SetFallbackFolders(std::vector<std::string> folders);
		// The menus' language (lang.h's code): which fallback fonts come first, and capitals (Turkish's
		// dotted İ). A change renders the fallback glyphs again, in the new language's fonts.
		void SetLanguage(const std::string& code);

		// text laid out in lines no wider than maxWidth (0: one line per paragraph, as long as it is),
		// at most maxLines of them (0: all), smaller when it does not fit (style.minScale)
		TextBlock Layout(const TextStyle& style, std::string_view utf8, float maxWidth = 0, int maxLines = 0);
		// one line's width
		float Width(const TextStyle& style, std::string_view utf8);
		// the size that fits text on its lines, from style's down to smallest in steps of step
		float Fit(TextStyle style, std::string_view utf8, float maxWidth, int maxLines, float smallest, float step = 4);

		// The fallback font with the most of these characters that Lexend lacks (the in-game menus'
		// second font, lang.h's MenuFont); an empty path when Lexend has them all
		struct FontFile
		{
			std::string path;
			int face = 0;
		};
		FontFile FileFor(std::u32string_view characters);

		// The atlas, one byte a texel (for Gfx::SetAtlas, again whenever its height changes), and the
		// rows changed since the last call
		uint32_t AtlasWidth() const { return m_width; }
		uint32_t AtlasHeight() const { return m_height; }
		const uint8_t* Atlas() const { return m_atlas.data(); }
		bool TakeChanged(uint32_t& firstRow, uint32_t& rows);

	private:
		struct Glyph
		{
			uint16_t x = 0, y = 0, width = 0, height = 0;
			int16_t left = 0, top = 0;
			float advance = 0;
			float weight = 0;
			bool found = false;
			bool baked = false; // Lexend's, from the atlas file
		};
		struct Fallback;

		const Glyph& Find(uint32_t codepoint, Weight weight);
		const Glyph& Render(uint32_t codepoint, Weight weight);
		float Advance(uint32_t codepoint, const TextStyle& style);
		TextBlock LayoutAt(const TextStyle& style, const std::u32string& text, float maxWidth, int maxLines);
		void OpenFallback();
		bool Grow();
		void ForgetRendered();

		uint32_t m_size = 48, m_spread = 6;
		float m_ascender = 1, m_descender = -0.25f; // a em
		uint32_t m_width = 0, m_height = 0;
		uint32_t m_bakedRows = 0;
		std::vector<uint8_t> m_atlas;
		std::unordered_map<uint64_t, Glyph> m_glyphs; // codepoint << 8 | weight
		float m_digitAdvance[4] = {};
		// the free rows' shelf for the glyphs rendered on demand
		uint32_t m_shelfX = 0, m_shelfY = 0, m_shelfHeight = 0;
		uint32_t m_changedFirst = 0, m_changedEnd = 0;
		std::vector<std::string> m_folders;
		std::unique_ptr<Fallback> m_fallback;
		std::string m_language = "en-GB";
	};

	// UTF-8 to code points (a malformed byte is U+FFFD)
	std::u32string Decode(std::string_view utf8);
	std::string Encode(std::u32string_view text);
	// Capitals (the overline's and the badges'): Latin, Greek (without its accents, as Greek capitals
	// are written), Cyrillic; ß as SS, and Turkish's i as İ when turkish
	char32_t Upper(char32_t c, bool turkish = false);
	std::u32string Upper(std::u32string_view text, bool turkish = false);
	std::string Upper(std::string_view utf8, bool turkish = false);
	// Arabic's letters in their joined forms (Unicode's presentation forms), and a line of text in the
	// order it is drawn left to right (Unicode's bidirectional algorithm, its common cases: runs of
	// Arabic reversed, numbers and Latin words kept in their order). rtl: the paragraph is Arabic's.
	std::u32string ShapeArabic(std::u32string_view text);
	std::u32string VisualOrder(std::u32string_view line, bool rtl);
	bool HasRightToLeft(std::u32string_view text);
}
