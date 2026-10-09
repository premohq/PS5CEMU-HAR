// SPDX-License-Identifier: GPL-3.0-or-later
#include "library.h"
#include "melonds.h"
#include "../app/paths.h"
#include "../ps5/log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace ps5melonds
{
	namespace
	{
		constexpr int kIconSize = 32;
		constexpr size_t kHeaderSize = 0x200;
		// the banner as far as the Korean title: version 1 has the six languages, 2 adds Chinese, 3 Korean
		constexpr size_t kBannerSize = 0xA40;

		uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
		uint32_t U32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

		std::string Lower(std::string text)
		{
			for (char& c : text)
				c = (char)std::tolower((unsigned char)c);
			return text;
		}

		// A banner title (UTF-16LE, up to a NUL) as UTF-8 lines
		std::vector<std::string> Lines(const uint8_t* data, size_t bytes)
		{
			std::vector<std::string> lines(1);
			for (size_t i = 0; i + 1 < bytes; i += 2)
			{
				uint32_t c = data[i] | data[i + 1] << 8;
				if (c == 0)
					break;
				if (c >= 0xD800 && c < 0xDC00 && i + 3 < bytes)
				{
					const uint32_t low = data[i + 2] | data[i + 3] << 8;
					if (low >= 0xDC00 && low < 0xE000)
					{
						c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
						i += 2;
					}
				}
				if (c == '\n')
				{
					lines.emplace_back();
					continue;
				}
				if (c == '\r')
					continue;
				std::string& out = lines.back();
				if (c < 0x80)
					out += (char)c;
				else if (c < 0x800)
					out += {(char)(0xC0 | c >> 6), (char)(0x80 | (c & 0x3F))};
				else if (c < 0x10000)
					out += {(char)(0xE0 | c >> 12), (char)(0x80 | (c >> 6 & 0x3F)), (char)(0x80 | (c & 0x3F))};
				else
					out += {(char)(0xF0 | c >> 18), (char)(0x80 | (c >> 12 & 0x3F)), (char)(0x80 | (c >> 6 & 0x3F)), (char)(0x80 | (c & 0x3F))};
			}
			for (std::string& line : lines)
			{
				while (!line.empty() && line.back() == ' ')
					line.pop_back();
				while (!line.empty() && line.front() == ' ')
					line.erase(line.begin());
			}
			std::erase_if(lines, [](const std::string& line) { return line.empty(); });
			return lines;
		}

		// The banner's title in English (or the first language that has one): its last line the
		// publisher, the lines before it the name (a subtitle after a colon)
		void ReadTitle(const std::vector<uint8_t>& banner, Title& title)
		{
			for (const int language : {1, 0, 2, 3, 4, 5})
			{
				const std::vector<std::string> lines = Lines(banner.data() + 0x240 + language * 0x100, 0x100);
				if (lines.empty())
					continue;
				size_t nameLines = lines.size() > 1 ? lines.size() - 1 : 1;
				if (lines.size() > 1)
					title.publisher = lines.back();
				std::string name = lines[0];
				for (size_t i = 1; i < nameLines; i++)
				{
					const char last = name.empty() ? ' ' : name.back();
					name += std::isalnum((unsigned char)last) ? ": " + lines[i] : " " + lines[i];
				}
				title.name = name;
				return;
			}
		}

		// 32x32, 4 bits a pixel in 8x8 tiles (the left pixel in the low nibble), with 16 BGR555
		// colours, the first one transparent
		void ReadIcon(const std::vector<uint8_t>& banner, Title& title)
		{
			const uint8_t* pixels = banner.data() + 0x20;
			const uint8_t* palette = banner.data() + 0x220;
			title.icon.resize(kIconSize * kIconSize * 4);
			for (int y = 0; y < kIconSize; y++)
				for (int x = 0; x < kIconSize; x++)
				{
					const int tile = (y / 8) * (kIconSize / 8) + x / 8;
					const uint8_t pair = pixels[tile * 32 + (y % 8) * 4 + (x % 8) / 2];
					const int index = (x & 1) ? pair >> 4 : pair & 0xF;
					const uint16_t colour = U16(palette + index * 2);
					uint8_t* out = &title.icon[(y * kIconSize + x) * 4];
					out[0] = (uint8_t)(((colour >> 10) & 0x1F) * 255 / 31); // blue
					out[1] = (uint8_t)(((colour >> 5) & 0x1F) * 255 / 31);
					out[2] = (uint8_t)((colour & 0x1F) * 255 / 31);
					out[3] = index == 0 ? 0 : 255;
				}
		}

		bool ValidCode(const std::string& code)
		{
			return code.size() == 4 &&
				std::all_of(code.begin(), code.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
		}

		std::string CoverFile(uint64_t id)
		{
			return fmt::format("{}/ds/{:016x}.tga", ps5paths::kCovers, id);
		}

		void WriteCover(uint64_t id, const std::vector<uint8_t>& icon)
		{
			// six times the size, each pixel a 6x6 square: the size of a 3DS game's (48 at four times),
			// sharp where the launcher scales it
			constexpr int kScale = 6, kSize = kIconSize * kScale;
			const std::string path = CoverFile(id);
			std::error_code ec;
			if (fs::exists(path, ec))
				return;
			fs::create_directories(fs::path(path).parent_path(), ec);
			std::vector<uint8_t> tga(18 + kSize * kSize * 4);
			tga[2] = 2;
			tga[12] = kSize & 255, tga[13] = kSize >> 8, tga[14] = kSize & 255, tga[15] = kSize >> 8;
			tga[16] = 32, tga[17] = 0x28;
			for (int y = 0; y < kSize; y++)
				for (int x = 0; x < kSize; x++)
					std::memcpy(&tga[18 + (y * kSize + x) * 4], &icon[((y / kScale) * kIconSize + x / kScale) * 4], 4);
			const std::string temporary = path + ".tmp";
			{
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				out.write((const char*)tga.data(), (std::streamsize)tga.size());
				if (!out)
					return;
			}
			fs::rename(temporary, path, ec);
		}
	}

	bool IsDsFile(const std::string& path)
	{
		const std::string extension = Lower(fs::path(path).extension().string());
		return extension == ".nds" || extension == ".srl" || extension == ".dsi";
	}

	Title Inspect(const std::string& path)
	{
		Title title;
		std::ifstream file(path, std::ios::binary);
		uint8_t header[kHeaderSize];
		if (!file.read((char*)header, sizeof(header)))
			return title;
		std::error_code ec;
		const uint64_t size = fs::file_size(path, ec);
		// the header's own sanity: the ARM9's code in the file, a unit code the DS or DSi knows
		const uint32_t arm9Offset = U32(header + 0x20), arm9Size = U32(header + 0x2C);
		const uint8_t unit = header[0x12];
		if (ec || unit > 3 || arm9Offset < 0x200 || arm9Offset >= size || arm9Size == 0)
			return title;
		title.readable = true;
		title.gameCode.assign((const char*)header + 0x0C, 4);
		if (!ValidCode(title.gameCode))
			title.gameCode.clear();
		title.version = header[0x1E];
		title.dsiOnly = unit == 3;
		title.dsiEnhanced = unit == 2;
		// the header's checksum (of its first 0x15E bytes) and the game code, as they are in the file
		title.titleId = kIdTag | (uint64_t)U16(header + 0x15E) << 32 | U32(header + 0x0C);
		const uint32_t bannerOffset = U32(header + 0x68);
		if (bannerOffset >= 0x200 && bannerOffset + kBannerSize <= size)
		{
			std::vector<uint8_t> banner(kBannerSize);
			file.seekg(bannerOffset);
			if (file.read((char*)banner.data(), (std::streamsize)banner.size()))
			{
				ReadTitle(banner, title);
				ReadIcon(banner, title);
			}
		}
		if (title.name.empty())
		{
			// no banner (homebrew): the header's title, in capitals
			std::string internal((const char*)header, strnlen((const char*)header, 12));
			while (!internal.empty() && internal.back() == ' ')
				internal.pop_back();
			title.name = internal;
		}
		return title;
	}

	bool ReadGame(const std::string& path, ps5emu::Game& game)
	{
		const Title title = Inspect(path);
		if (!title.readable)
			return false;
		game = {};
		game.titleId = title.titleId;
		game.name = !title.name.empty() ? title.name : fs::path(path).stem().string();
		game.path = path;
		game.version = title.version;
		game.format = title.dsiOnly ? "DSI" : "NDS";
		game.publisher = title.publisher;
		game.gameId = title.gameCode;
		game.nds = true;
		if (!title.icon.empty())
			WriteCover(game.titleId, title.icon);
		return true;
	}

	std::string CoverPath(uint64_t titleId)
	{
		const std::string path = CoverFile(titleId);
		std::error_code ec;
		return fs::exists(path, ec) ? path : std::string();
	}

	bool OwnBiosFound()
	{
		// the sizes melonDS takes: the ARM9's 4 KiB, the ARM7's 16 KiB
		std::error_code ec;
		const std::string folder = std::string(kRoot) + "/bios/";
		return fs::file_size(folder + "bios9.bin", ec) == 0x1000 && !ec && fs::file_size(folder + "bios7.bin", ec) == 0x4000 && !ec;
	}
}
