// SPDX-License-Identifier: GPL-3.0-or-later
#include "library.h"
#include "azahar.h"
#include "../app/paths.h"
#include "../ps5/log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace ps5azahar
{
	namespace
	{
		constexpr uint64_t kMediaUnit = 0x200;
		constexpr size_t kSmdhSize = 0x36C0;
		constexpr int kIconSize = 48;
		constexpr int kScanDepth = 4;

		std::mutex s_mutex;
		std::vector<ps5emu::Game> s_games; // under s_mutex
		std::atomic<bool> s_scanning{false};

		struct File
		{
			std::ifstream stream;
			uint64_t size = 0;

			explicit File(const std::string& path) : stream(path, std::ios::binary)
			{
				std::error_code ec;
				size = fs::file_size(path, ec);
			}

			bool Read(uint64_t offset, void* out, size_t bytes)
			{
				if (!stream || offset + bytes > size)
					return false;
				stream.seekg((std::streamoff)offset);
				stream.read(static_cast<char*>(out), (std::streamsize)bytes);
				return (size_t)stream.gcount() == bytes;
			}

			std::vector<uint8_t> Read(uint64_t offset, size_t bytes)
			{
				std::vector<uint8_t> data(bytes);
				if (!Read(offset, data.data(), bytes))
					data.clear();
				return data;
			}
		};

		uint32_t U32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
		uint64_t U64(const uint8_t* p) { return U32(p) | (uint64_t)U32(p + 4) << 32; }
		uint64_t U64BE(const uint8_t* p)
		{
			uint64_t value = 0;
			for (int i = 0; i < 8; i++)
				value = value << 8 | p[i];
			return value;
		}

		std::string Lower(std::string text)
		{
			for (char& c : text)
				c = (char)std::tolower((unsigned char)c);
			return text;
		}

		// UTF-16LE (up to a NUL) as UTF-8, its line breaks as spaces
		std::string Utf8(const uint8_t* data, size_t bytes)
		{
			std::string out;
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
				if (c == '\n' || c == '\r')
					c = ' ';
				if (c < 0x80)
					out += (char)c;
				else if (c < 0x800)
					out += {(char)(0xC0 | c >> 6), (char)(0x80 | (c & 0x3F))};
				else if (c < 0x10000)
					out += {(char)(0xE0 | c >> 12), (char)(0x80 | (c >> 6 & 0x3F)), (char)(0x80 | (c & 0x3F))};
				else
					out += {(char)(0xF0 | c >> 18), (char)(0x80 | (c >> 12 & 0x3F)), (char)(0x80 | (c >> 6 & 0x3F)), (char)(0x80 | (c & 0x3F))};
			}
			// a short title is padded with spaces sometimes
			while (!out.empty() && out.back() == ' ')
				out.pop_back();
			return out;
		}

		// The SMDH's English name and publisher (or the first language that has them), and its large icon.
		bool ReadSmdh(const std::vector<uint8_t>& smdh, Title& title)
		{
			if (smdh.size() < kSmdhSize || std::memcmp(smdh.data(), "SMDH", 4) != 0)
				return false;
			for (const int language : {1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11})
			{
				const uint8_t* entry = smdh.data() + 0x8 + language * 0x200;
				const std::string name = Utf8(entry, 0x80);
				if (name.empty())
					continue;
				title.name = name;
				title.publisher = Utf8(entry + 0x180, 0x80);
				break;
			}
			// 48x48 RGB565, in 8x8 tiles, each tile's pixels in Morton order
			const uint8_t* icon = smdh.data() + 0x24C0;
			title.icon.resize(kIconSize * kIconSize * 4);
			for (int y = 0; y < kIconSize; y++)
				for (int x = 0; x < kIconSize; x++)
				{
					const int tile = (y / 8) * (kIconSize / 8) + x / 8;
					const int tx = x % 8, ty = y % 8;
					const int morton = (tx & 1) | (ty & 1) << 1 | (tx & 2) << 1 | (ty & 2) << 2 | (tx & 4) << 2 | (ty & 4) << 3;
					const uint8_t* p = icon + (tile * 64 + morton) * 2;
					const uint16_t c = p[0] | p[1] << 8;
					uint8_t* out = &title.icon[(y * kIconSize + x) * 4];
					out[0] = (uint8_t)(((c & 0x1F) * 255 + 15) / 31);
					out[1] = (uint8_t)((((c >> 5) & 0x3F) * 255 + 31) / 63);
					out[2] = (uint8_t)(((c >> 11) * 255 + 15) / 31);
					out[3] = 255;
				}
			return true;
		}

		// The product code's last part (CTR-P-AREE, KTR-N-AREE: AREE), the ID on the game's box
		std::string BoxId(const uint8_t* productCode)
		{
			std::string code((const char*)productCode, strnlen((const char*)productCode, 16));
			const size_t dash = code.rfind('-');
			if (dash == std::string::npos || code.size() - dash - 1 != 4)
				return {};
			std::string id = code.substr(dash + 1);
			for (const char c : id)
				if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
					return {};
			return id;
		}

		// An NCCH at offset: its title ID, its product code, and its SMDH from its ExeFS when it is
		// not encrypted (the header never is).
		bool ReadNcch(File& file, uint64_t offset, Title& title)
		{
			uint8_t header[0x200];
			if (!file.Read(offset, header, sizeof(header)) || std::memcmp(header + 0x100, "NCCH", 4) != 0)
				return false;
			title.titleId = U64(header + 0x118); // the program ID
			title.boxId = BoxId(header + 0x150);
			title.encrypted = (header[0x18F] & 0x4) == 0; // NoCrypto
			if (title.encrypted)
				return true;
			const uint64_t exefs = offset + U32(header + 0x1A0) * kMediaUnit;
			uint8_t entries[0xA0];
			if (U32(header + 0x1A0) == 0 || !file.Read(exefs, entries, sizeof(entries)))
				return true;
			for (int i = 0; i < 10; i++)
			{
				const uint8_t* entry = entries + i * 16;
				if (std::strncmp((const char*)entry, "icon", 8) == 0)
				{
					ReadSmdh(file.Read(exefs + 0x200 + U32(entry + 8), std::min<size_t>(U32(entry + 12), kSmdhSize)), title);
					break;
				}
			}
			return true;
		}

		bool ReadCia(File& file, Title& title)
		{
			uint8_t header[0x20];
			if (!file.Read(0, header, sizeof(header)) || U32(header) != 0x2020)
				return false;
			auto align = [](uint64_t value) { return (value + 63) & ~uint64_t(63); };
			const uint64_t certs = align(U32(header));
			const uint64_t ticket = certs + align(U32(header + 0x8));
			const uint64_t tmd = ticket + align(U32(header + 0xC));
			const uint64_t content = tmd + align(U32(header + 0x10));
			const uint64_t meta = content + align(U64(header + 0x18));
			// the TMD after its RSA-2048 signature: the title ID and version, big-endian
			const std::vector<uint8_t> tmdHeader = file.Read(tmd, 0x140 + 0xA0);
			if (tmdHeader.size() == 0x140 + 0xA0 && U32(tmdHeader.data()) == 0x04000100)
			{
				title.titleId = U64BE(tmdHeader.data() + 0x140 + 0x4C);
				title.version = (uint16_t)(tmdHeader[0x140 + 0x9C] << 8 | tmdHeader[0x140 + 0x9D]);
			}
			if (U32(header + 0x14) >= 0x400 + kSmdhSize)
				ReadSmdh(file.Read(meta + 0x400, kSmdhSize), title);
			// the first content's NCCH header, for the product code, when the CIA's contents are not
			// encrypted with its title key
			uint8_t ncch[0x200];
			if (file.Read(content, ncch, sizeof(ncch)) && std::memcmp(ncch + 0x100, "NCCH", 4) == 0)
				title.boxId = BoxId(ncch + 0x150);
			return true;
		}

		bool Read3dsx(File& file, Title& title)
		{
			uint8_t header[0x2C];
			if (!file.Read(0, header, sizeof(header)) || std::memcmp(header, "3DSX", 4) != 0)
				return false;
			const uint16_t size = header[4] | header[5] << 8;
			if (size >= 0x2C)
				ReadSmdh(file.Read(U32(header + 0x20), std::min<size_t>(U32(header + 0x24), kSmdhSize)), title);
			return true;
		}

		uint64_t PathId(const std::string& path)
		{
			// FNV-1a, in a range no title ID uses
			uint64_t hash = 0xcbf29ce484222325ull;
			for (const char c : path)
				hash = (hash ^ (uint8_t)c) * 0x100000001b3ull;
			return hash | 0xF000000000000000ull;
		}

		bool Known(const std::string& extension)
		{
			for (const char* known : {".3ds", ".cci", ".cxi", ".cia", ".3dsx", ".app", ".elf", ".axf", ".z3ds", ".zcci", ".zcxi", ".z3dsx"})
				if (extension == known)
					return true;
			return false;
		}

		std::string CoverFile(uint64_t id)
		{
			return fmt::format("{}/3ds/{:016x}.tga", ps5paths::kCovers, id);
		}

		void WriteCover(uint64_t id, const std::vector<uint8_t>& icon)
		{
			// four times the size, each pixel a 4x4 square: the launcher scales it smoothly, and the
			// icon stays sharp where a 48-pixel one would blur
			constexpr int kScale = 4, kSize = kIconSize * kScale;
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

		void Scan(const fs::path& folder, int depth, std::vector<ps5emu::Game>& games)
		{
			std::error_code ec;
			for (const auto& entry : fs::directory_iterator(folder, ec))
			{
				if (entry.is_directory(ec))
				{
					if (depth < kScanDepth)
						Scan(entry.path(), depth + 1, games);
					continue;
				}
				const std::string path = entry.path().string();
				if (!entry.is_regular_file(ec))
					continue;
				if (!Known(Lower(entry.path().extension().string())))
					continue;
				Title title = Inspect(path);
				if (!title.readable && title.format.empty())
					continue;
				// an update's or DLC's CIA is installed with its game (Settings > Game files > Install a CIA file), not played
				const uint32_t kind = (uint32_t)(title.titleId >> 32);
				if (title.format == "CIA" && (kind == 0x0004000E || kind == 0x0004008C))
					continue;
				ps5emu::Game game;
				game.titleId = title.titleId ? title.titleId : PathId(path);
				game.name = !title.name.empty() ? title.name : entry.path().stem().string();
				game.path = entry.path();
				game.version = title.version;
				game.format = title.encrypted ? "ENCRYPTED" : title.format;
				game.publisher = title.publisher;
				game.gameId = title.boxId;
				if (!title.icon.empty())
					WriteCover(game.titleId, title.icon);
				games.push_back(std::move(game));
			}
		}

		// The games installed from CIA files into the 3DS's SD card (Azahar's sdmc): title/00040000/
		// <id>/content holds the game's contents (00040002/<id> a demo's), the one with an ExeFS
		// (and its SMDH) the program Azahar starts.
		void ScanInstalled(std::vector<ps5emu::Game>& games)
		{
			std::error_code ec;
			const fs::path root = fs::path(kRoot) / "sdmc" / "Nintendo 3DS";
			for (const auto& id0 : fs::directory_iterator(root, ec))
				for (const auto& id1 : fs::directory_iterator(id0.path(), ec))
					for (const char* kind : {"00040000", "00040002"})
					for (const auto& title : fs::directory_iterator(id1.path() / "title" / kind, ec))
					{
						std::vector<fs::path> contents;
						for (const auto& entry : fs::directory_iterator(title.path() / "content", ec))
							if (Lower(entry.path().extension().string()) == ".app")
								contents.push_back(entry.path());
						std::sort(contents.begin(), contents.end());
						for (const fs::path& content : contents)
						{
							Title inspected = Inspect(content.string());
							if (!inspected.readable || (inspected.name.empty() && !inspected.encrypted))
								continue; // a manual or other data, no program
							ps5emu::Game game;
							game.titleId = inspected.titleId;
							game.name = !inspected.name.empty() ? inspected.name : fmt::format("{:016X}", inspected.titleId);
							game.path = content;
							game.format = inspected.encrypted ? "ENCRYPTED" : "INSTALLED";
							game.publisher = inspected.publisher;
							game.gameId = inspected.boxId;
							if (!inspected.icon.empty())
								WriteCover(game.titleId, inspected.icon);
							// the installed copy stands for a file of the same game in the folder
							std::erase_if(games, [&](const ps5emu::Game& other) { return other.titleId == game.titleId; });
							games.push_back(std::move(game));
							break;
						}
					}
		}
	}

	Title Inspect(const std::string& path)
	{
		Title title;
		std::string extension = Lower(fs::path(path).extension().string());
		File file(path);
		if (!file.stream)
			return title;
		if (extension == ".3ds" || extension == ".cci")
		{
			uint8_t header[0x200];
			title.format = "3DS";
			if (file.Read(0, header, sizeof(header)) && std::memcmp(header + 0x100, "NCSD", 4) == 0)
				title.readable = ReadNcch(file, U32(header + 0x120) * kMediaUnit, title);
		}
		else if (extension == ".cxi" || extension == ".app")
		{
			title.format = extension == ".cxi" ? "CXI" : "APP";
			title.readable = ReadNcch(file, 0, title);
		}
		else if (extension == ".cia")
		{
			title.format = "CIA";
			title.readable = ReadCia(file, title);
		}
		else if (extension == ".3dsx")
		{
			title.format = "3DSX";
			title.readable = Read3dsx(file, title);
		}
		else if (extension == ".elf" || extension == ".axf")
		{
			title.format = "ELF";
			title.readable = true;
		}
		else
		{
			// Azahar's compressed dumps: listed, their contents read by Azahar when they start
			title.format = Lower(extension) == ".z3dsx" ? "Z3DSX" : "Z3DS";
			title.readable = true;
		}
		return title;
	}

	void StartScan(const std::string& folder)
	{
		if (s_scanning.exchange(true))
			return;
		// Azahar's folders from the first start, so there is somewhere to put games and keys before
		// a game has run: the default games folder, and sysdata for aes_keys.txt
		std::error_code ec;
		for (const char* sub : {"games", "sysdata", "sdmc"})
			fs::create_directories(fs::path(kRoot) / sub, ec);
		std::thread([folder] {
			std::vector<ps5emu::Game> games;
			Scan(folder, 0, games);
			ScanInstalled(games);
			std::sort(games.begin(), games.end(), [](const ps5emu::Game& a, const ps5emu::Game& b) { return Lower(a.name) < Lower(b.name); });
			ps5log::Line("[azahar] {} 3DS games in {}", games.size(), folder);
			{
				std::lock_guard lock(s_mutex);
				s_games = std::move(games);
			}
			s_scanning = false;
		}).detach();
	}

	bool Scanning()
	{
		return s_scanning;
	}

	std::vector<ps5emu::Game> ListGames()
	{
		std::lock_guard lock(s_mutex);
		return s_games;
	}

	std::string CoverPath(uint64_t titleId)
	{
		const std::string path = CoverFile(titleId);
		std::error_code ec;
		return fs::exists(path, ec) ? path : std::string();
	}
}
