// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's UI kit: text (text.h).

#include "text.h"
#include "gfx.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <sys/stat.h>

namespace ui
{
	namespace
	{
		constexpr uint32_t kFreeRows = 640;	   // for the glyphs rendered on demand, at first
		constexpr uint32_t kFreeRowsCjk = 1600; // a CJK language's menus need about a thousand of them
		constexpr uint32_t kGrowRows = 1024;	   // more, each time they fill
		constexpr uint32_t kMostRows = 8192;
		constexpr char32_t kEllipsis = 0x2026, kZeroWidthSpace = 0x200b;
		// how much bolder a fallback glyph is drawn for each weight (it is rendered once, regular)
		constexpr float kFallbackWeight[] = {0.0f, 0.03f, 0.06f, 0.09f};

		uint64_t Key(uint32_t codepoint, Weight weight)
		{
			return (uint64_t)codepoint << 8 | (uint8_t)weight;
		}

		template<typename T>
		bool Get(const std::vector<uint8_t>& data, size_t& at, T& value)
		{
			if (at + sizeof(T) > data.size())
				return false;
			std::memcpy(&value, data.data() + at, sizeof(T));
			at += sizeof(T);
			return true;
		}

		bool IsHangul(char32_t c)
		{
			return (c >= 0xac00 && c <= 0xd7af) || (c >= 0x1100 && c <= 0x11ff) || (c >= 0x3130 && c <= 0x318f);
		}

		bool IsCjk(char32_t c)
		{
			return (c >= 0x2e80 && c <= 0x9fff) || IsHangul(c) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xff00 && c <= 0xffef) || c >= 0x20000;
		}

		bool IsThai(char32_t c)
		{
			return c >= 0x0e00 && c <= 0x0e7f;
		}

		bool IsSpace(char32_t c)
		{
			return c == ' ' || c == '\t' || c == 0x3000;
		}

		// a mark drawn over or under the character before it: never the first of a line
		bool IsMark(char32_t c)
		{
			return (c >= 0x0300 && c <= 0x036f) || (c >= 0x064b && c <= 0x065f) || c == 0x0670 || c == 0x0e31 || (c >= 0x0e33 && c <= 0x0e3a) ||
				(c >= 0x0e47 && c <= 0x0e4e) || c == 0x0e30 || c == 0x0e32 || c == 0x0e45;
		}

		// CJK line breaking's rules (kinsoku): what never starts a line, and what never ends one
		bool NoBreakBefore(char32_t c)
		{
			static constexpr char32_t kMarks[] = {0x3001, 0x3002, 0xff0c, 0xff0e, 0x30fb, 0xff1a, 0xff1b, 0xff1f, 0xff01, 0xff09, 0x300d, 0x300f,
				0x3011, 0x3015, 0x3009, 0x300b, 0x30fc, 0x3005, 0x303b, 0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087,
				0x308e, 0x30a1, 0x30a3, 0x30a5, 0x30a7, 0x30a9, 0x30c3, 0x30e3, 0x30e5, 0x30e7, 0x30ee, 0x30f5, 0x30f6, ')', ']', '}', ',', '.',
				'!', '?', ':', ';', '%', 0x2026, 0x201d, 0x2019, 0xff05};
			return std::find(std::begin(kMarks), std::end(kMarks), c) != std::end(kMarks) || IsMark(c);
		}

		bool NoBreakAfter(char32_t c)
		{
			static constexpr char32_t kMarks[] = {0xff08, 0x300c, 0x300e, 0x3010, 0x3014, 0x3008, 0x300a, '(', '[', '{', 0x201c, 0x2018,
				// Thai's leading vowels go with the consonant after them
				0x0e40, 0x0e41, 0x0e42, 0x0e43, 0x0e44};
			return std::find(std::begin(kMarks), std::end(kMarks), c) != std::end(kMarks);
		}

		// Font files in a folder and the folders in it, two levels down
		void FindFonts(const std::string& folder, int depth, std::vector<std::string>& out)
		{
			DIR* dir = opendir(folder.c_str());
			if (!dir)
				return;
			std::vector<std::string> names;
			while (dirent* entry = readdir(dir))
				if (entry->d_name[0] != '.')
					names.push_back(entry->d_name);
			closedir(dir);
			std::sort(names.begin(), names.end());
			for (const std::string& name : names)
			{
				const std::string path = folder + "/" + name;
				struct stat info{};
				if (stat(path.c_str(), &info) != 0)
					continue;
				if (S_ISDIR(info.st_mode))
				{
					if (depth > 0)
						FindFonts(path, depth - 1, out);
					continue;
				}
				std::string lower = name;
				for (char& c : lower)
					c = (char)std::tolower((unsigned char)c);
				if (lower.ends_with(".ttf") || lower.ends_with(".otf") || lower.ends_with(".ttc"))
					out.push_back(path);
			}
		}

		std::string Lower(std::string text)
		{
			for (char& c : text)
				c = (char)std::tolower((unsigned char)c);
			return text;
		}

		// Which CJK language a font is for, from its file's and family's names (the console's fonts
		// and the PC's say it there: SSTJpPro, Noto Sans CJK SC): ja, ko, zh-Hans, zh-Hant, cjk (one
		// for all of them), or empty for a font that is not a CJK one
		std::string CjkOf(const std::string& names)
		{
			auto has = [&](std::initializer_list<const char*> words) {
				for (const char* word : words)
					if (names.find(word) != std::string::npos)
						return true;
				return false;
			};
			if (has({"jp", "japan", "jis"}))
				return "ja";
			if (has({"kr", "kor", "hangul"}))
				return "ko";
			if (has({" sc", "-sc", "_sc", "chs", "gb", "simplified", "hans", "prc", "schinese"}))
				return "zh-Hans";
			if (has({" tc", "-tc", "_tc", " hk", "cht", "big5", "traditional", "hant", "tchinese", "taiwan"}))
				return "zh-Hant";
			if (has({"cjk", "chinese", "han"}))
				return "cjk";
			return {};
		}

		// -- Arabic: its letters' forms, as Unicode's presentation forms B (isolated, final, initial,
		// medial; the right-joining letters have the first two)

		struct ArabicForms
		{
			char32_t letter, isolated, final, initial, medial;
		};
		constexpr ArabicForms kArabic[] = {
			{0x0621, 0xfe80, 0, 0, 0}, {0x0622, 0xfe81, 0xfe82, 0, 0}, {0x0623, 0xfe83, 0xfe84, 0, 0}, {0x0624, 0xfe85, 0xfe86, 0, 0},
			{0x0625, 0xfe87, 0xfe88, 0, 0}, {0x0626, 0xfe89, 0xfe8a, 0xfe8b, 0xfe8c}, {0x0627, 0xfe8d, 0xfe8e, 0, 0},
			{0x0628, 0xfe8f, 0xfe90, 0xfe91, 0xfe92}, {0x0629, 0xfe93, 0xfe94, 0, 0}, {0x062a, 0xfe95, 0xfe96, 0xfe97, 0xfe98},
			{0x062b, 0xfe99, 0xfe9a, 0xfe9b, 0xfe9c}, {0x062c, 0xfe9d, 0xfe9e, 0xfe9f, 0xfea0}, {0x062d, 0xfea1, 0xfea2, 0xfea3, 0xfea4},
			{0x062e, 0xfea5, 0xfea6, 0xfea7, 0xfea8}, {0x062f, 0xfea9, 0xfeaa, 0, 0}, {0x0630, 0xfeab, 0xfeac, 0, 0}, {0x0631, 0xfead, 0xfeae, 0, 0},
			{0x0632, 0xfeaf, 0xfeb0, 0, 0}, {0x0633, 0xfeb1, 0xfeb2, 0xfeb3, 0xfeb4}, {0x0634, 0xfeb5, 0xfeb6, 0xfeb7, 0xfeb8},
			{0x0635, 0xfeb9, 0xfeba, 0xfebb, 0xfebc}, {0x0636, 0xfebd, 0xfebe, 0xfebf, 0xfec0}, {0x0637, 0xfec1, 0xfec2, 0xfec3, 0xfec4},
			{0x0638, 0xfec5, 0xfec6, 0xfec7, 0xfec8}, {0x0639, 0xfec9, 0xfeca, 0xfecb, 0xfecc}, {0x063a, 0xfecd, 0xfece, 0xfecf, 0xfed0},
			{0x0641, 0xfed1, 0xfed2, 0xfed3, 0xfed4}, {0x0642, 0xfed5, 0xfed6, 0xfed7, 0xfed8}, {0x0643, 0xfed9, 0xfeda, 0xfedb, 0xfedc},
			{0x0644, 0xfedd, 0xfede, 0xfedf, 0xfee0}, {0x0645, 0xfee1, 0xfee2, 0xfee3, 0xfee4}, {0x0646, 0xfee5, 0xfee6, 0xfee7, 0xfee8},
			{0x0647, 0xfee9, 0xfeea, 0xfeeb, 0xfeec}, {0x0648, 0xfeed, 0xfeee, 0, 0}, {0x0649, 0xfeef, 0xfef0, 0, 0},
			{0x064a, 0xfef1, 0xfef2, 0xfef3, 0xfef4},
		};

		const ArabicForms* FormsOf(char32_t c)
		{
			if (c < 0x0621 || c > 0x064a)
				return nullptr;
			for (const ArabicForms& forms : kArabic)
				if (forms.letter == c)
					return &forms;
			return nullptr;
		}

		bool IsTransparent(char32_t c)
		{
			return (c >= 0x064b && c <= 0x065f) || c == 0x0670;
		}

		// the letter after it may join it: a dual-joining letter, or the tatweel
		bool JoinsForward(char32_t c)
		{
			if (c == 0x0640)
				return true;
			const ArabicForms* forms = FormsOf(c);
			return forms && forms->initial;
		}

		bool JoinsBackward(char32_t c)
		{
			if (c == 0x0640)
				return true;
			const ArabicForms* forms = FormsOf(c);
			return forms && forms->final;
		}

		// a presentation form's letter, for a font that has the letter and not its forms
		char32_t LetterOf(char32_t form)
		{
			if (form < 0xfe70 || form > 0xfeff)
				return 0;
			for (const ArabicForms& forms : kArabic)
				if (form == forms.isolated || form == forms.final || form == forms.initial || form == forms.medial)
					return forms.letter;
			switch (form)
			{
			case 0xfef5: case 0xfef6: return 0x0622; // lam with an alef: the alef stands for it
			case 0xfef7: case 0xfef8: return 0x0623;
			case 0xfef9: case 0xfefa: return 0x0625;
			case 0xfefb: case 0xfefc: return 0x0627;
			default: return 0;
			}
		}

		// -- the bidirectional algorithm's common cases

		enum class Direction : uint8_t
		{
			Left,	 // Latin, CJK and the rest
			Right,	 // Arabic and Hebrew
			Number,	 // European digits
			Neutral, // spaces, punctuation, symbols
		};

		Direction DirectionOf(char32_t c)
		{
			if (c >= '0' && c <= '9')
				return Direction::Number;
			if ((c >= 0x0590 && c <= 0x08ff && c != 0x060c && !(c >= 0x0660 && c <= 0x0669)) || (c >= 0xfb1d && c <= 0xfdff) ||
				(c >= 0xfe70 && c <= 0xfeff))
				return Direction::Right;
			if (c >= 0x0660 && c <= 0x0669)
				return Direction::Number;
			if (c < 0x80)
				return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ? Direction::Left : Direction::Neutral;
			if ((c >= 0x2000 && c <= 0x2bff) || (c >= 0x3000 && c <= 0x303f) || c == 0x00a0 || c == 0x00ab || c == 0x00bb || c == 0x00b7 ||
				c == 0x00d7 || c == 0x00b0 || c == 0x060c || c == 0x00a9 || c == 0x00ae)
				return Direction::Neutral;
			return Direction::Left;
		}

		char32_t Mirrored(char32_t c)
		{
			switch (c)
			{
			case '(': return ')';
			case ')': return '(';
			case '[': return ']';
			case ']': return '[';
			case '{': return '}';
			case '}': return '{';
			case '<': return '>';
			case '>': return '<';
			case 0x00ab: return 0x00bb;
			case 0x00bb: return 0x00ab;
			case 0x2039: return 0x203a;
			case 0x203a: return 0x2039;
			default: return c;
			}
		}
	}

	std::u32string ShapeArabic(std::u32string_view text)
	{
		std::u32string out;
		out.reserve(text.size());
		auto neighbour = [&](size_t at, int step) -> char32_t {
			for (long i = (long)at + step; i >= 0 && i < (long)text.size(); i += step)
				if (!IsTransparent(text[i]))
					return text[i];
			return 0;
		};
		for (size_t i = 0; i < text.size(); i++)
		{
			const char32_t c = text[i];
			const ArabicForms* forms = FormsOf(c);
			if (!forms)
			{
				out.push_back(c);
				continue;
			}
			const char32_t before = neighbour(i, -1), after = neighbour(i, 1);
			const bool joinedBefore = before && JoinsForward(before) && forms->final;
			// lam and alef: one ligature
			if (c == 0x0644 && (after == 0x0622 || after == 0x0623 || after == 0x0625 || after == 0x0627))
			{
				const char32_t isolated = after == 0x0622 ? 0xfef5 : after == 0x0623 ? 0xfef7 : after == 0x0625 ? 0xfef9 : 0xfefb;
				out.push_back(joinedBefore ? isolated + 1 : isolated);
				// the alef is in it: skip to it, keeping the marks between
				size_t j = i + 1;
				while (j < text.size() && IsTransparent(text[j]))
					out.push_back(text[j++]);
				i = j;
				continue;
			}
			const bool joinsAfter = forms->initial && after && JoinsBackward(after);
			const char32_t form = joinedBefore && joinsAfter ? forms->medial : joinedBefore ? forms->final : joinsAfter ? forms->initial : forms->isolated;
			out.push_back(form ? form : c);
		}
		return out;
	}

	bool HasRightToLeft(std::u32string_view text)
	{
		for (char32_t c : text)
			if (DirectionOf(c) == Direction::Right)
				return true;
		return false;
	}

	std::u32string VisualOrder(std::u32string_view line, bool rtl)
	{
		const size_t n = line.size();
		if (n == 0 || (!rtl && !HasRightToLeft(line)))
			return std::u32string(line);
		std::vector<Direction> types(n);
		for (size_t i = 0; i < n; i++)
			types[i] = DirectionOf(line[i]);
		// a number's separators (2,400 · 4.5 · 192.168.1.2 · 10:30) and its percent sign go with it
		for (size_t i = 1; i + 1 < n; i++)
			if (types[i] == Direction::Neutral && types[i - 1] == Direction::Number && types[i + 1] == Direction::Number &&
				(line[i] == '.' || line[i] == ',' || line[i] == ':' || line[i] == '/'))
				types[i] = Direction::Number;
		for (size_t i = 0; i < n; i++)
			if (line[i] == '%' || line[i] == 0x066a)
			{
				if ((i > 0 && types[i - 1] == Direction::Number) || (i + 1 < n && types[i + 1] == Direction::Number))
					types[i] = Direction::Number;
			}
		// levels: the paragraph's, Arabic's odd, Latin's and numbers' even
		const int base = rtl ? 1 : 0;
		std::vector<int> levels(n, base);
		Direction lastStrong = rtl ? Direction::Right : Direction::Left;
		for (size_t i = 0; i < n; i++)
		{
			switch (types[i])
			{
			case Direction::Right: levels[i] = 1, lastStrong = Direction::Right; break;
			case Direction::Left: levels[i] = rtl ? 2 : 0, lastStrong = Direction::Left; break;
			case Direction::Number: levels[i] = rtl || lastStrong == Direction::Right ? 2 : 0; break;
			case Direction::Neutral: break;
			}
		}
		// a run of neutrals takes the direction on both its sides when they agree (a number counts as
		// Arabic there), else the paragraph's
		for (size_t i = 0; i < n;)
		{
			if (types[i] != Direction::Neutral)
			{
				i++;
				continue;
			}
			size_t end = i;
			while (end < n && types[end] == Direction::Neutral)
				end++;
			auto side = [&](long at) -> int {
				if (at < 0 || at >= (long)n)
					return base == 1 ? 1 : 0; // the paragraph's ends
				return types[at] == Direction::Left ? 0 : 1;
			};
			const int before = side((long)i - 1), after = side((long)end);
			const int level = before == after ? (before == 1 ? 1 : (rtl ? 2 : 0)) : base;
			for (size_t k = i; k < end; k++)
				levels[k] = level;
			i = end;
		}
		// the paragraph's own spaces at the line's ends stay its
		std::u32string out(line);
		for (size_t i = 0; i < n; i++)
			if (levels[i] % 2 == 1)
				out[i] = Mirrored(out[i]);
		int highest = 0;
		for (int level : levels)
			highest = std::max(highest, level);
		for (int level = highest; level >= 1; level--)
			for (size_t i = 0; i < n;)
			{
				if (levels[i] < level)
				{
					i++;
					continue;
				}
				size_t end = i;
				while (end < n && levels[end] >= level)
					end++;
				std::reverse(out.begin() + i, out.begin() + end);
				std::reverse(levels.begin() + i, levels.begin() + end);
				i = end;
			}
		return out;
	}

	struct Fonts::Fallback
	{
		struct Face
		{
			FT_Face face = nullptr;
			std::string path;
			int index = 0;
			std::string cjk; // CjkOf
		};
		FT_Library library = nullptr;
		std::vector<Face> faces;
		std::vector<size_t> cjkOrder, otherOrder; // the faces to try, for a CJK character and for the rest
		bool opened = false;

		~Fallback()
		{
			for (Face& face : faces)
				FT_Done_Face(face.face);
			if (library)
				FT_Done_FreeType(library);
		}

		// the menus' language's fonts first for CJK characters (a Han character drawn as that language
		// writes it), the fonts that are not CJK ones first for the rest (a Russian menu in a Latin and
		// Cyrillic font, not in a Japanese one's Cyrillic)
		void Order(const std::string& language)
		{
			cjkOrder.clear();
			otherOrder.clear();
			auto rank = [&](const Face& face) {
				if (face.cjk.empty())
					return 3;
				if (face.cjk == language)
					return 0;
				if (face.cjk == "cjk")
					return 1;
				return 2;
			};
			for (int r = 0; r <= 3; r++)
				for (size_t i = 0; i < faces.size(); i++)
					if (rank(faces[i]) == r)
						cjkOrder.push_back(i);
			for (size_t i = 0; i < faces.size(); i++)
				if (faces[i].cjk.empty())
					otherOrder.push_back(i);
			for (size_t i : cjkOrder)
				if (!faces[i].cjk.empty())
					otherOrder.push_back(i);
		}

		// Japanese kana go to a Japanese font and Hangul to a Korean one, whatever the language
		const std::vector<size_t>& For(char32_t c, std::vector<size_t>& scratch, const std::string& language) const
		{
			if (!IsCjk(c))
				return otherOrder;
			const char* script = (c >= 0x3040 && c <= 0x30ff) ? "ja" : IsHangul(c) ? "ko" : nullptr;
			if (!script || language == script)
				return cjkOrder;
			scratch.clear();
			for (size_t i : cjkOrder)
				if (faces[i].cjk == script)
					scratch.push_back(i);
			for (size_t i : cjkOrder)
				if (faces[i].cjk != script)
					scratch.push_back(i);
			return scratch;
		}
	};

	Fonts::Fonts() = default;
	Fonts::~Fonts() = default;

	bool Fonts::Load(const std::string& path, std::string& error)
	{
		std::ifstream file(path, std::ios::binary);
		const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		size_t at = 8;
		if (data.size() < 64 || std::memcmp(data.data(), "UISDF01", 8) != 0)
		{
			error = path + " is missing or not the kit's font";
			return false;
		}
		int32_t ascender, descender, lineGap, unitsPerEm;
		uint32_t width, height, weights, glyphs;
		Get(data, at, m_size);
		Get(data, at, m_spread);
		Get(data, at, ascender);
		Get(data, at, descender);
		Get(data, at, lineGap);
		Get(data, at, unitsPerEm);
		Get(data, at, width);
		Get(data, at, height);
		Get(data, at, weights);
		if (!Get(data, at, glyphs) || unitsPerEm <= 0 || weights > 4 || width == 0 || width > 8192 || height > 8192)
		{
			error = path + " has a header the kit cannot read";
			return false;
		}
		m_ascender = (float)ascender / unitsPerEm;
		m_descender = (float)descender / unitsPerEm;
		at += weights * sizeof(uint16_t);
		m_glyphs.clear();
		for (uint32_t i = 0; i < glyphs; i++)
		{
			uint32_t codepoint;
			uint16_t weight;
			Glyph glyph;
			Get(data, at, codepoint);
			Get(data, at, weight);
			Get(data, at, glyph.x);
			Get(data, at, glyph.y);
			Get(data, at, glyph.width);
			Get(data, at, glyph.height);
			Get(data, at, glyph.left);
			Get(data, at, glyph.top);
			if (!Get(data, at, glyph.advance))
			{
				error = path + " is cut short";
				return false;
			}
			glyph.found = true;
			glyph.baked = true;
			m_glyphs[Key(codepoint, (Weight)weight)] = glyph;
		}
		uint32_t packed = 0;
		if (!Get(data, at, packed) || at + packed > data.size())
		{
			error = path + " is cut short";
			return false;
		}
		m_width = width;
		m_bakedRows = height;
		const bool cjk = m_language == "ja" || m_language == "ko" || m_language.rfind("zh", 0) == 0;
		m_height = std::min(height + (cjk ? kFreeRowsCjk : kFreeRows), kMostRows);
		m_atlas.assign((size_t)m_width * m_height, 0);
		uLongf unpacked = (uLongf)width * height;
		if (uncompress(m_atlas.data(), &unpacked, data.data() + at, packed) != Z_OK || unpacked != (uLongf)width * height)
		{
			error = path + "'s atlas cannot be unpacked";
			return false;
		}
		for (int weight = 0; weight < 4; weight++)
		{
			m_digitAdvance[weight] = 0;
			for (char32_t digit = '0'; digit <= '9'; digit++)
				m_digitAdvance[weight] = std::max(m_digitAdvance[weight], Find(digit, (Weight)weight).advance);
		}
		m_shelfX = 0;
		m_shelfY = m_bakedRows + 1;
		m_shelfHeight = 0;
		m_changedFirst = 0;
		m_changedEnd = m_height;
		return true;
	}

	void Fonts::SetFallbackFolders(std::vector<std::string> folders)
	{
		m_folders = std::move(folders);
	}

	void Fonts::SetLanguage(const std::string& code)
	{
		if (code == m_language)
			return;
		const bool cjkBefore = m_language == "ja" || m_language == "ko" || m_language.rfind("zh", 0) == 0;
		const bool cjkAfter = code == "ja" || code == "ko" || code.rfind("zh", 0) == 0;
		m_language = code;
		if (m_fallback && m_fallback->opened)
			m_fallback->Order(m_language);
		// the glyphs already rendered came from the old language's fonts: rendered again as they are met
		if (cjkBefore || cjkAfter)
			ForgetRendered();
	}

	void Fonts::ForgetRendered()
	{
		for (auto it = m_glyphs.begin(); it != m_glyphs.end();)
			it = it->second.baked ? std::next(it) : m_glyphs.erase(it);
		m_shelfX = 0;
		m_shelfY = m_bakedRows + 1;
		m_shelfHeight = 0;
	}

	bool Fonts::TakeChanged(uint32_t& firstRow, uint32_t& rows)
	{
		if (m_changedEnd <= m_changedFirst)
			return false;
		firstRow = m_changedFirst;
		rows = m_changedEnd - m_changedFirst;
		m_changedFirst = m_changedEnd = 0;
		return true;
	}

	// the free rows full: more of them, for as long as the atlas may grow (the canvas takes glyphs'
	// places in texels, so what was laid out before still finds its glyphs)
	bool Fonts::Grow()
	{
		if (m_height >= kMostRows)
			return false;
		const uint32_t height = std::min(m_height + kGrowRows, kMostRows);
		// the shell gives Gfx the taller atlas whole (SetAtlas), so no rows need marking
		m_atlas.resize((size_t)m_width * height, 0);
		Log("[ui] text: the atlas grows to " + std::to_string(height) + " rows");
		m_height = height;
		return true;
	}

	const Fonts::Glyph& Fonts::Find(uint32_t codepoint, Weight weight)
	{
		const auto it = m_glyphs.find(Key(codepoint, weight));
		if (it != m_glyphs.end())
			return it->second;
		// another weight baked (a fallback glyph is rendered once, for all of them)
		if (weight != Weight::Regular)
		{
			const auto regular = m_glyphs.find(Key(codepoint, Weight::Regular));
			if (regular != m_glyphs.end() && !regular->second.found)
				return regular->second;
		}
		return Render(codepoint, weight);
	}

	void Fonts::OpenFallback()
	{
		if (!m_fallback)
			m_fallback = std::make_unique<Fallback>();
		Fallback& fallback = *m_fallback;
		if (fallback.opened)
			return;
		fallback.opened = true;
		std::vector<std::string> files;
		for (const std::string& folder : m_folders)
			FindFonts(folder, 2, files);
		if (!files.empty() && FT_Init_FreeType(&fallback.library) == 0)
		{
			FT_Int spread = (FT_Int)m_spread;
			FT_Property_Set(fallback.library, "bsdf", "spread", &spread);
			std::string names;
			for (const std::string& file : files)
			{
				// every face of a collection (a .ttc holds the CJK languages' fonts together)
				for (int index = 0, count = 1; index < count && fallback.faces.size() < 64; index++)
				{
					FT_Face face = nullptr;
					if (FT_New_Face(fallback.library, file.c_str(), index, &face) != 0)
						break;
					count = (int)std::min<FT_Long>(face->num_faces, 16);
					FT_Set_Pixel_Sizes(face, 0, m_size);
					const std::string name = file.substr(file.find_last_of('/') + 1);
					const std::string family = face->family_name ? face->family_name : "";
					fallback.faces.push_back({face, file, index, CjkOf(Lower(name + " " + family))});
					names += (names.empty() ? "" : ", ") + name + (count > 1 ? "#" + std::to_string(index) : "") +
						(fallback.faces.back().cjk.empty() ? "" : " (" + fallback.faces.back().cjk + ")");
				}
			}
			if (!names.empty())
				Log("[ui] text: fallback fonts " + names);
		}
		fallback.Order(m_language);
		Log("[ui] text: " + std::to_string(fallback.faces.size()) + " fallback fonts of " + std::to_string(files.size()) + " files found");
	}

	// A glyph Lexend lacks, from the first fallback font that has it, as a signed distance from its
	// bitmap (as tools/render-sdf-font.cpp makes Lexend's, at the atlas's size)
	const Fonts::Glyph& Fonts::Render(uint32_t codepoint, Weight weight)
	{
		Glyph& regular = m_glyphs[Key(codepoint, Weight::Regular)];
		if (regular.found || codepoint < 0x20)
			return weight == Weight::Regular ? regular : (m_glyphs[Key(codepoint, weight)] = regular);
		// a zero-width space: a line break's chance, nothing drawn
		if (codepoint == kZeroWidthSpace || codepoint == 0x200c || codepoint == 0x200d || codepoint == 0xfeff)
		{
			regular.found = true;
			return regular;
		}
		OpenFallback();
		Fallback& fallback = *m_fallback;
		Glyph glyph;
		std::vector<size_t> scratch;
		for (size_t i : fallback.For(codepoint, scratch, m_language))
		{
			FT_Face face = fallback.faces[i].face;
			const FT_UInt index = FT_Get_Char_Index(face, codepoint);
			if (index == 0 || FT_Load_Glyph(face, index, FT_LOAD_NO_HINTING) != 0)
				continue;
			glyph.found = true;
			glyph.advance = face->glyph->advance.x / 64.0f;
			if (face->glyph->outline.n_points == 0 || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0 ||
				FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF) != 0)
				break;
			const FT_Bitmap& bitmap = face->glyph->bitmap;
			if (m_shelfX + bitmap.width + 1 > m_width)
			{
				m_shelfX = 0;
				m_shelfY += m_shelfHeight + 1;
				m_shelfHeight = 0;
			}
			if (m_shelfY + bitmap.rows > m_height && !Grow())
			{
				Log("[ui] text: the atlas is full");
				break;
			}
			for (unsigned row = 0; row < bitmap.rows; row++)
				std::memcpy(&m_atlas[(size_t)(m_shelfY + row) * m_width + m_shelfX], bitmap.buffer + (ptrdiff_t)row * bitmap.pitch, bitmap.width);
			glyph.x = (uint16_t)m_shelfX;
			glyph.y = (uint16_t)m_shelfY;
			glyph.width = (uint16_t)bitmap.width;
			glyph.height = (uint16_t)bitmap.rows;
			glyph.left = (int16_t)face->glyph->bitmap_left;
			glyph.top = (int16_t)face->glyph->bitmap_top;
			if (m_changedEnd <= m_changedFirst)
				m_changedFirst = m_shelfY, m_changedEnd = m_shelfY + bitmap.rows;
			else
				m_changedFirst = std::min(m_changedFirst, m_shelfY), m_changedEnd = std::max(m_changedEnd, m_shelfY + bitmap.rows);
			m_shelfX += bitmap.width + 1;
			m_shelfHeight = std::max(m_shelfHeight, (uint32_t)bitmap.rows);
			break;
		}
		// an Arabic form no font has: its letter, unjoined, which the fonts are likelier to have
		if (!glyph.found)
			if (const char32_t letter = LetterOf(codepoint))
			{
				const Glyph base = Find(letter, Weight::Regular);
				Glyph& slot = m_glyphs[Key(codepoint, Weight::Regular)];
				slot = base;
				for (int other = 1; other < 4; other++)
				{
					Glyph bolder = base;
					bolder.weight = kFallbackWeight[other];
					m_glyphs[Key(codepoint, (Weight)other)] = bolder;
				}
				return m_glyphs[Key(codepoint, weight)];
			}
		Glyph& slot = m_glyphs[Key(codepoint, Weight::Regular)];
		slot = glyph;
		for (int other = 1; other < 4; other++)
		{
			Glyph bolder = glyph;
			bolder.weight = kFallbackWeight[other];
			m_glyphs[Key(codepoint, (Weight)other)] = bolder;
		}
		return m_glyphs[Key(codepoint, weight)];
	}

	Fonts::FontFile Fonts::FileFor(std::u32string_view characters)
	{
		// what Lexend lacks: not baked (the in-game menus' Lexend has what the atlas has)
		std::vector<char32_t> missing;
		for (char32_t c : characters)
		{
			const auto it = m_glyphs.find(Key(c, Weight::Regular));
			if (c >= 0x20 && c != kZeroWidthSpace && !(it != m_glyphs.end() && it->second.baked))
				missing.push_back(c);
		}
		if (missing.empty())
			return {};
		OpenFallback();
		const Fallback& fallback = *m_fallback;
		// the most of them, the language's own font first among equals
		const bool cjk = std::any_of(missing.begin(), missing.end(), IsCjk);
		const std::vector<size_t>& order = cjk ? fallback.cjkOrder : fallback.otherOrder;
		size_t best = order.size();
		size_t bestCount = 0;
		for (size_t k = 0; k < order.size(); k++)
		{
			FT_Face face = fallback.faces[order[k]].face;
			size_t count = 0;
			for (char32_t c : missing)
				count += FT_Get_Char_Index(face, c) != 0;
			if (count > bestCount)
			{
				best = k;
				bestCount = count;
			}
		}
		if (best == order.size())
			return {};
		const auto& face = fallback.faces[order[best]];
		return {face.path, face.index};
	}

	float Fonts::Advance(uint32_t codepoint, const TextStyle& style)
	{
		const float scale = style.size / m_size;
		const bool digit = style.tabular && codepoint >= '0' && codepoint <= '9';
		const Glyph& glyph = Find(codepoint, style.weight);
		const bool spacing = (glyph.found || digit) && !IsMark(codepoint) && codepoint != kZeroWidthSpace;
		return (digit ? m_digitAdvance[(int)style.weight] : glyph.advance) * scale + (spacing ? style.tracking : 0.0f);
	}

	TextBlock Fonts::Layout(const TextStyle& style, std::string_view utf8, float maxWidth, int maxLines)
	{
		std::u32string text = Decode(utf8);
		if (style.upper)
			text = Upper(text, m_language == "tr");
		if (HasRightToLeft(text))
			text = ShapeArabic(text);
		TextBlock block = LayoutAt(style, text, maxWidth, maxLines);
		// a longer language's words: a little smaller before an ellipsis cuts them
		if (block.truncated && maxWidth > 0 && style.minScale < 1)
		{
			TextStyle smaller = style;
			// never under 16 for it (small text, a cover's name, is cut instead: 7.2's smallest is larger)
			const float smallest = std::max(style.size * style.minScale, std::min(style.size, 16.0f));
			for (smaller.size = style.size - 1; smaller.size >= smallest; smaller.size -= 1)
			{
				smaller.tracking = style.tracking * smaller.size / style.size;
				TextBlock fitted = LayoutAt(smaller, text, maxWidth, maxLines);
				if (!fitted.truncated || smaller.size - 1 < smallest)
					return fitted;
			}
		}
		return block;
	}

	TextBlock Fonts::LayoutAt(const TextStyle& style, const std::u32string& text, float maxWidth, int maxLines)
	{
		// the paragraph's direction: Arabic's when its first letter (or, in an Arabic menu, any) is Arabic
		bool rtl = false;
		for (char32_t c : text)
		{
			const Direction d = DirectionOf(c);
			if (d == Direction::Left || d == Direction::Right)
			{
				rtl = d == Direction::Right;
				break;
			}
		}
		if (!rtl && m_language == "ar")
			rtl = HasRightToLeft(text);
		// the lines, as runs of the text
		std::vector<std::u32string> lines;
		bool truncated = false;
		size_t start = 0;
		while (start <= text.size())
		{
			size_t end = text.find(U'\n', start);
			if (end == std::u32string::npos)
				end = text.size();
			const std::u32string_view paragraph(text.data() + start, end - start);
			// greedy: as many words as fit; a word longer than a line is broken where it must be
			size_t lineStart = 0;
			while (lineStart < paragraph.size() || (lineStart == 0 && paragraph.empty()))
			{
				float width = 0;
				size_t at = lineStart, lastBreak = std::u32string::npos;
				for (; at < paragraph.size(); at++)
				{
					const char32_t c = paragraph[at];
					if (IsSpace(c) || c == kZeroWidthSpace)
						lastBreak = at;
					else if (at > lineStart && (IsCjk(c) || IsCjk(paragraph[at - 1])) && !IsHangul(c) && !NoBreakBefore(c) &&
						!NoBreakAfter(paragraph[at - 1]) && !IsSpace(paragraph[at - 1]))
						lastBreak = at; // before it
					const float advance = Advance(c, style);
					if (maxWidth > 0 && !IsSpace(c) && width + advance > maxWidth + 0.01f && at > lineStart)
						break;
					width += advance;
				}
				size_t lineEnd = at, next = at;
				if (at < paragraph.size())
				{
					if (lastBreak != std::u32string::npos && lastBreak > lineStart)
						lineEnd = next = lastBreak;
					else
						// in the middle of a word: never between a letter and its mark, nor after a Thai
						// vowel that goes before its consonant
						while (lineEnd > lineStart + 1 && (IsMark(paragraph[lineEnd]) || NoBreakAfter(paragraph[lineEnd - 1])))
							lineEnd = --next;
				}
				std::u32string line(paragraph.substr(lineStart, lineEnd - lineStart));
				while (!line.empty() && (IsSpace(line.back()) || line.back() == kZeroWidthSpace))
					line.pop_back();
				lines.push_back(std::move(line));
				while (next < paragraph.size() && (IsSpace(paragraph[next]) || paragraph[next] == kZeroWidthSpace))
					next++;
				if (next == lineStart)
					next++;
				lineStart = next;
				if (paragraph.empty())
					break;
			}
			start = end + 1;
		}
		if (maxLines > 0 && (int)lines.size() > maxLines)
		{
			lines.resize(maxLines);
			truncated = true;
		}
		auto lineWidth = [&](const std::u32string& line) {
			float w = 0;
			for (char32_t c : line)
				w += Advance(c, style);
			return w;
		};
		// one line wider than its room (a word that fits no line, cut where it had to be, needs none)
		if (!truncated && maxWidth > 0 && maxLines > 0)
			for (const std::u32string& line : lines)
				truncated |= lineWidth(line) > maxWidth + 0.5f;
		if (truncated && !lines.empty())
		{
			// the last line's words go, last first, until it and the ellipsis fit
			std::u32string& last = lines.back();
			const float ellipsis = Advance(kEllipsis, style);
			while (maxWidth > 0 && !last.empty() && lineWidth(last) + ellipsis > maxWidth)
			{
				size_t cut = last.find_last_of(U" \t​");
				if (cut == std::u32string::npos || cut == 0 || IsCjk(last.back()) || IsThai(last.back()))
				{
					last.pop_back(); // one long word, or a language without spaces: a character at a time
					while (!last.empty() && IsMark(last.back()))
						last.pop_back();
					continue;
				}
				last.resize(cut);
			}
			while (!last.empty() && (IsSpace(last.back()) || last.back() == ',' || last.back() == ':' || last.back() == ';' || last.back() == '.' ||
										last.back() == 0x3001 || last.back() == 0x3002 || last.back() == 0x060c || last.back() == kZeroWidthSpace))
				last.pop_back();
			last.push_back(kEllipsis);
		}

		TextBlock block;
		block.truncated = truncated;
		block.rightToLeft = rtl;
		block.size = style.size;
		block.lineHeight = style.size * style.lineHeight;
		const float ascent = m_ascender * style.size, descent = -m_descender * style.size;
		block.baseline = (block.lineHeight - (ascent + descent)) * 0.5f + ascent;
		const float scale = style.size / m_size;
		for (size_t index = 0; index < lines.size(); index++)
		{
			const float baseline = block.baseline + block.lineHeight * index;
			TextBlock::Line line{(uint32_t)block.glyphs.size(), 0, 0};
			float pen = 0;
			// Arabic, and the Latin words and numbers in it, in the order they are drawn
			const std::u32string drawn = rtl || HasRightToLeft(lines[index]) ? VisualOrder(lines[index], rtl) : lines[index];
			for (char32_t c : drawn)
			{
				const Glyph& glyph = Find(c, style.weight);
				const bool digit = style.tabular && c >= '0' && c <= '9';
				const float advance = Advance(c, style);
				const float centre = digit ? (m_digitAdvance[(int)style.weight] - glyph.advance) * 0.5f * scale : 0.0f;
				if (glyph.width > 0)
				{
					PlacedGlyph placed;
					placed.x = pen + centre + glyph.left * scale;
					placed.y = baseline - glyph.top * scale;
					placed.width = glyph.width * scale;
					placed.height = glyph.height * scale;
					placed.u0 = glyph.x;
					placed.v0 = glyph.y;
					placed.u1 = glyph.x + glyph.width;
					placed.v1 = glyph.y + glyph.height;
					placed.weight = glyph.weight;
					block.glyphs.push_back(placed);
				}
				pen += advance;
			}
			line.count = (uint32_t)block.glyphs.size() - line.first;
			line.width = lines[index].empty() ? 0.0f : pen - style.tracking;
			block.width = std::max(block.width, line.width);
			block.lines.push_back(line);
		}
		// a right-to-left paragraph's lines end at the block's right edge, where it starts reading them
		if (rtl && block.lines.size() > 1)
			for (TextBlock::Line& line : block.lines)
			{
				const float dx = block.width - line.width;
				for (uint32_t i = line.first; i < line.first + line.count; i++)
					block.glyphs[i].x += dx;
				line.width = block.width;
			}
		block.height = block.lineHeight * (float)block.lines.size();
		return block;
	}

	float Fonts::Width(const TextStyle& style, std::string_view utf8)
	{
		std::u32string text = Decode(utf8);
		if (style.upper)
			text = Upper(text, m_language == "tr");
		if (HasRightToLeft(text))
			text = ShapeArabic(text);
		float width = 0;
		for (char32_t c : text)
			width += Advance(c, style);
		return std::max(0.0f, width - style.tracking);
	}

	float Fonts::Fit(TextStyle style, std::string_view utf8, float maxWidth, int maxLines, float smallest, float step)
	{
		style.minScale = 1; // the sizes are this loop's to try
		for (;; style.size -= step)
		{
			if (style.size <= smallest)
				return smallest;
			const TextBlock block = Layout(style, utf8, maxWidth, maxLines);
			if (!block.truncated)
				return style.size;
		}
	}

	std::u32string Decode(std::string_view utf8)
	{
		std::u32string out;
		out.reserve(utf8.size());
		for (size_t i = 0; i < utf8.size();)
		{
			const unsigned char lead = (unsigned char)utf8[i];
			int length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 0;
			if (length == 0 || i + length > utf8.size())
			{
				out.push_back(0xfffd);
				i++;
				continue;
			}
			char32_t c = length == 1 ? lead : lead & (0x7f >> length);
			bool ok = true;
			for (int k = 1; k < length; k++)
			{
				const unsigned char next = (unsigned char)utf8[i + k];
				ok = ok && (next & 0xc0) == 0x80;
				c = c << 6 | (next & 0x3f);
			}
			out.push_back(ok ? c : 0xfffd);
			i += ok ? length : 1;
		}
		return out;
	}

	std::string Encode(std::u32string_view text)
	{
		std::string out;
		for (char32_t c : text)
		{
			if (c < 0x80)
				out += (char)c;
			else if (c < 0x800)
				out += {(char)(0xc0 | c >> 6), (char)(0x80 | (c & 0x3f))};
			else if (c < 0x10000)
				out += {(char)(0xe0 | c >> 12), (char)(0x80 | (c >> 6 & 0x3f)), (char)(0x80 | (c & 0x3f))};
			else
				out += {(char)(0xf0 | c >> 18), (char)(0x80 | (c >> 12 & 0x3f)), (char)(0x80 | (c >> 6 & 0x3f)), (char)(0x80 | (c & 0x3f))};
		}
		return out;
	}

	char32_t Upper(char32_t c, bool turkish)
	{
		if (c >= 'a' && c <= 'z')
			return turkish && c == 'i' ? 0x130 : c - 32;
		if (c < 0x80)
			return c;
		// Latin-1, Latin Extended-A
		if (c >= 0xe0 && c <= 0xfe && c != 0xf7)
			return c - 32;
		if (c == 0xff)
			return 0x178;
		if (c == 0x131)
			return 'I'; // Turkish's dotless ı
		if (c == 0x17f)
			return 'S';
		if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177))
			return (c & 1) ? c - 1 : c;
		if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e))
			return (c & 1) ? c : c - 1;
		// Latin Extended-B: Vietnamese's horned letters, Romanian's comma below, the caron vowels
		if (c == 0x1a1 || c == 0x1b0)
			return c - 1;
		if (c >= 0x1cd && c <= 0x1dc)
			return (c & 1) ? c : c - 1;
		if ((c >= 0x1de && c <= 0x1ef) || (c >= 0x1f8 && c <= 0x21f) || (c >= 0x222 && c <= 0x233))
			return (c & 1) ? c - 1 : c;
		// Latin Extended Additional (Vietnamese)
		if (c >= 0x1e00 && c <= 0x1eff && !(c >= 0x1e96 && c <= 0x1e9f))
			return (c & 1) ? c - 1 : c;
		// Greek: capitals without their accents
		switch (c)
		{
		case 0x3ac: case 0x386: return 0x391;
		case 0x3ad: case 0x388: return 0x395;
		case 0x3ae: case 0x389: return 0x397;
		case 0x3af: case 0x38a: return 0x399;
		case 0x3cc: case 0x38c: return 0x39f;
		case 0x3cd: case 0x38e: return 0x3a5;
		case 0x3ce: case 0x38f: return 0x3a9;
		case 0x3ca: case 0x390: return 0x3aa;
		case 0x3cb: case 0x3b0: return 0x3ab;
		case 0x3c2: return 0x3a3;
		default: break;
		}
		if (c >= 0x3b1 && c <= 0x3c9)
			return c - 0x20;
		// Cyrillic
		if (c >= 0x430 && c <= 0x44f)
			return c - 0x20;
		if (c >= 0x450 && c <= 0x45f)
			return c - 0x50;
		if ((c >= 0x460 && c <= 0x481) || (c >= 0x48a && c <= 0x4bf) || (c >= 0x4d0 && c <= 0x52f))
			return (c & 1) ? c - 1 : c;
		if (c >= 0x4c1 && c <= 0x4ce)
			return (c & 1) ? c : c - 1;
		if (c == 0x4cf)
			return 0x4c0;
		return c;
	}

	std::u32string Upper(std::u32string_view text, bool turkish)
	{
		std::u32string out;
		out.reserve(text.size());
		for (char32_t c : text)
		{
			if (c == 0xdf)
				out += U"SS"; // German's ß, as its capitals write it
			else
				out.push_back(Upper(c, turkish));
		}
		return out;
	}

	std::string Upper(std::string_view utf8, bool turkish)
	{
		return Encode(Upper(Decode(utf8), turkish));
	}
}
