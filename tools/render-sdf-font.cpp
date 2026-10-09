// SPDX-License-Identifier: GPL-3.0-or-later
// The UI kit's font (port/ui/text.h): Lexend's regular, medium, semibold and bold, baked into one
// signed-distance atlas, so the app draws sharp text at any size without rendering glyphs as it
// starts. Built and run by tools/render-sdf-font.sh, which writes port/ui/fonts/lexend.sdf.
//
// Each glyph is drawn at four times the atlas's size with FreeType's rasteriser, which fills the
// variable font's overlapping contours as one shape, and turned into distances there (FreeType's
// bitmap SDF: its outline SDF measures to the overlapping contours too, leaving holes where they
// cross); four by four of those are averaged into one atlas texel.
//
// The file, little-endian:
//   char magic[8] "UISDF01\0"
//   u32 size, spread         the glyphs' size in pixels a em, the distance 0 and 255 stand for
//   i32 ascender, descender, lineGap, unitsPerEm   (the font's units)
//   u32 width, height        the atlas's
//   u32 weights, glyphs      how many of each
//   u16 weight[weights]      400, 500, 600, 700
//   glyph[glyphs]: u32 codepoint; u16 weight index, x, y, w, h; i16 left, top; f32 advance
//                            (left and top: the bitmap's corner from the pen, y up, in pixels;
//                            advance in pixels at size)
//   u32 compressed; then the atlas's rows, zlib-compressed: one byte a texel, 128 at the edge,
//   more inside

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_MULTIPLE_MASTERS_H

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	constexpr int kSize = 48, kSpread = 6, kOversample = 4, kAtlasWidth = 2048, kPadding = 1;
	constexpr uint16_t kWeights[] = {400, 500, 600, 700};

	struct Glyph
	{
		uint32_t codepoint;
		uint16_t weight;
		int width = 0, height = 0, left = 0, top = 0;
		float advance = 0;
		std::vector<uint8_t> pixels;
		int x = 0, y = 0;
	};

	// The code points baked: printable ASCII, Latin-1, Latin Extended-A, what Lexend has of Latin
	// Extended-B and Latin Extended Additional (Romanian's ș and ț, Vietnamese's letters: the languages
	// the menus speak in Latin script, docs/UI-REDESIGN.md 7.7), the spaces and the typographic marks
	// the app's text uses. A code point the font lacks is left out.
	std::vector<uint32_t> CodePoints()
	{
		std::vector<uint32_t> points;
		for (uint32_t c = 0x20; c <= 0x7e; c++)
			points.push_back(c);
		for (uint32_t c = 0xa0; c <= 0x24f; c++)
			points.push_back(c);
		for (uint32_t c = 0x1e00; c <= 0x1eff; c++)
			points.push_back(c);
		for (uint32_t c = 0x2000; c <= 0x200a; c++)
			points.push_back(c);
		points.push_back(0x202f);
		for (uint32_t c = 0x2010; c <= 0x2027; c++)
			points.push_back(c);
		for (uint32_t c : {0x2030u, 0x2039u, 0x203au, 0x20acu, 0x2122u, 0x2190u, 0x2191u, 0x2192u, 0x2193u, 0x2212u, 0x2215u, 0x2248u,
				 0x2260u, 0x2264u, 0x2265u, 0x25cfu, 0x2713u, 0x2715u})
			points.push_back(c);
		return points;
	}

	template<typename T>
	void Put(std::vector<uint8_t>& out, T value)
	{
		const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
		out.insert(out.end(), bytes, bytes + sizeof(T));
	}
}

int main(int argc, char* argv[])
{
	if (argc != 3)
	{
		std::fprintf(stderr, "render-sdf-font FONT.ttf OUT.sdf\n");
		return 2;
	}
	FT_Library library;
	FT_Face face;
	if (FT_Init_FreeType(&library) || FT_New_Face(library, argv[1], 0, &face))
	{
		std::fprintf(stderr, "cannot read %s\n", argv[1]);
		return 1;
	}
	FT_Int spread = kSpread * kOversample;
	FT_Property_Set(library, "bsdf", "spread", &spread);
	FT_Set_Pixel_Sizes(face, 0, kSize * kOversample);

	std::vector<Glyph> glyphs;
	for (uint16_t weight = 0; weight < std::size(kWeights); weight++)
	{
		FT_Fixed coordinate = (FT_Fixed)kWeights[weight] << 16;
		if (FT_Set_Var_Design_Coordinates(face, 1, &coordinate))
		{
			std::fprintf(stderr, "%s has no weight axis\n", argv[1]);
			return 1;
		}
		for (uint32_t codepoint : CodePoints())
		{
			const FT_UInt index = FT_Get_Char_Index(face, codepoint);
			if (index == 0 || FT_Load_Glyph(face, index, FT_LOAD_NO_HINTING))
				continue;
			Glyph glyph{codepoint, weight};
			glyph.advance = face->glyph->advance.x / 64.0f / kOversample;
			if (face->glyph->outline.n_points == 0 || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) ||
				FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF))
			{
				glyphs.push_back(std::move(glyph)); // a space: an advance only
				continue;
			}
			const FT_Bitmap& bitmap = face->glyph->bitmap;
			// the big bitmap on the atlas's grid: its corner moved to a multiple of the oversampling
			const int bigLeft = face->glyph->bitmap_left, bigTop = face->glyph->bitmap_top;
			const int shiftX = ((bigLeft % kOversample) + kOversample) % kOversample;
			const int shiftY = ((kOversample - bigTop % kOversample) % kOversample + kOversample) % kOversample;
			const int left = (bigLeft - shiftX) / kOversample;
			const int top = (bigTop + shiftY) / kOversample;
			const int width = ((int)bitmap.width + shiftX + kOversample - 1) / kOversample;
			const int height = ((int)bitmap.rows + shiftY + kOversample - 1) / kOversample;
			glyph.left = left;
			glyph.top = top;
			glyph.width = width;
			glyph.height = height;
			glyph.pixels.resize((size_t)width * height);
			for (int y = 0; y < height; y++)
				for (int x = 0; x < width; x++)
				{
					int sum = 0;
					for (int sy = 0; sy < kOversample; sy++)
						for (int sx = 0; sx < kOversample; sx++)
						{
							const int bx = x * kOversample + sx - shiftX, by = y * kOversample + sy - shiftY;
							sum += bx >= 0 && by >= 0 && bx < (int)bitmap.width && by < (int)bitmap.rows ? bitmap.buffer[by * bitmap.pitch + bx] : 0;
						}
					glyph.pixels[(size_t)y * width + x] = (uint8_t)((sum + kOversample * kOversample / 2) / (kOversample * kOversample));
				}
			glyphs.push_back(std::move(glyph));
		}
	}

	// shelves, tallest first
	std::vector<Glyph*> order;
	for (auto& glyph : glyphs)
		if (glyph.width > 0)
			order.push_back(&glyph);
	std::stable_sort(order.begin(), order.end(), [](const Glyph* a, const Glyph* b) { return a->height > b->height; });
	int x = 0, y = 0, shelf = 0;
	for (Glyph* glyph : order)
	{
		if (x + glyph->width + kPadding > kAtlasWidth)
		{
			x = 0;
			y += shelf + kPadding;
			shelf = 0;
		}
		glyph->x = x;
		glyph->y = y;
		x += glyph->width + kPadding;
		shelf = std::max(shelf, glyph->height);
	}
	const int height = (y + shelf + 3) & ~3;
	std::vector<uint8_t> atlas((size_t)kAtlasWidth * height, 0);
	for (const Glyph* glyph : order)
		for (int row = 0; row < glyph->height; row++)
			std::memcpy(&atlas[(size_t)(glyph->y + row) * kAtlasWidth + glyph->x], &glyph->pixels[(size_t)row * glyph->width], glyph->width);

	std::vector<uint8_t> out;
	out.insert(out.end(), "UISDF01", "UISDF01" + 8);
	Put<uint32_t>(out, kSize);
	Put<uint32_t>(out, kSpread);
	Put<int32_t>(out, face->ascender);
	Put<int32_t>(out, face->descender);
	Put<int32_t>(out, face->height - (face->ascender - face->descender));
	Put<int32_t>(out, face->units_per_EM);
	Put<uint32_t>(out, kAtlasWidth);
	Put<uint32_t>(out, (uint32_t)height);
	Put<uint32_t>(out, (uint32_t)std::size(kWeights));
	Put<uint32_t>(out, (uint32_t)glyphs.size());
	for (uint16_t weight : kWeights)
		Put<uint16_t>(out, weight);
	for (const Glyph& glyph : glyphs)
	{
		Put<uint32_t>(out, glyph.codepoint);
		Put<uint16_t>(out, glyph.weight);
		Put<uint16_t>(out, (uint16_t)glyph.x);
		Put<uint16_t>(out, (uint16_t)glyph.y);
		Put<uint16_t>(out, (uint16_t)glyph.width);
		Put<uint16_t>(out, (uint16_t)glyph.height);
		Put<int16_t>(out, (int16_t)glyph.left);
		Put<int16_t>(out, (int16_t)glyph.top);
		Put<float>(out, glyph.advance);
	}
	uLongf packedSize = compressBound(atlas.size());
	std::vector<uint8_t> packed(packedSize);
	compress2(packed.data(), &packedSize, atlas.data(), atlas.size(), 9);
	Put<uint32_t>(out, (uint32_t)packedSize);
	out.insert(out.end(), packed.begin(), packed.begin() + packedSize);

	FILE* file = std::fopen(argv[2], "wb");
	if (!file || std::fwrite(out.data(), 1, out.size(), file) != out.size())
	{
		std::fprintf(stderr, "cannot write %s\n", argv[2]);
		return 1;
	}
	std::fclose(file);
	std::printf("%s: %zu glyphs in %d weights, atlas %dx%d, %zu KB\n", argv[2], glyphs.size(), (int)std::size(kWeights), kAtlasWidth, height,
		out.size() / 1024);
	FT_Done_Face(face);
	FT_Done_FreeType(library);
	return 0;
}
