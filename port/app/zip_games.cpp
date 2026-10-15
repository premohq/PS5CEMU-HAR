// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: ZIP discovery and bounded, validated extraction to a persistent game cache.
#include "zip_games.h"
#include "paths.h"

#include <zip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace ps5zip
{
namespace
{
	constexpr int kScanDepth = 4;
	constexpr zip_int64_t kMaxEntries = 200000;
	constexpr uint64_t kMaxSingleFile = 64ull << 30; // ZIP64; a Wii U WUD can be very large
	constexpr uint64_t kMaxTotalBytes = 80ull << 30;
	constexpr uint64_t kReserve = 128ull << 20;

	std::string Lower(std::string s)
	{
		for (char& c : s)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	bool ZipExtension(const fs::path& path)
	{
		return Lower(path.extension().string()) == ".zip";
	}

	// Never permit names chosen by an archive to escape the cache's freshly made directory.
	// Reject Windows paths too, even though the PS5 itself uses forward slashes.
	bool SafeName(const std::string& name)
	{
		if (name.empty() || name.size() > 4096 || name.front() == '/' || name.find('\\') != std::string::npos ||
			name.find(':') != std::string::npos)
			return false;
		size_t at = 0;
		while (at < name.size())
		{
			const size_t end = name.find('/', at);
			const std::string part = name.substr(at, end == std::string::npos ? end : end - at);
			if (part.empty() || part == "." || part == "..")
				return false;
			if (end == std::string::npos)
				return true;
			at = end + 1;
		}
		return name.back() == '/'; // a single trailing slash is a directory entry
	}

	bool ValidExtension(const std::string& name, System system)
	{
		const std::string ext = Lower(fs::path(name).extension().string());
		switch (system)
		{
		case System::WiiU:
			return ext == ".wua" || ext == ".wud" || ext == ".wux" || ext == ".wuhb" ||
				ext == ".rpx" || ext == ".elf";
		case System::N3ds:
			return ext == ".3ds" || ext == ".cci" || ext == ".cxi" || ext == ".3dsx" ||
				ext == ".app" || ext == ".elf" || ext == ".axf" || ext == ".cia" ||
				ext == ".z3ds" || ext == ".zcci" || ext == ".zcxi" || ext == ".z3dsx";
		case System::Nds:
			return ext == ".nds" || ext == ".srl" || ext == ".dsi";
		}
		return false;
	}

	uint64_t Hash(const std::string& text)
	{
		uint64_t h = 14695981039346656037ull;
		for (unsigned char c : text)
			h = (h ^ c) * 1099511628211ull;
		return h;
	}

	std::string Hex(uint64_t n)
	{
		char out[17];
		std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(n));
		return out;
	}

	struct Candidate
	{
		std::string member;
		std::string display;
		std::string format;
		bool folder = false;
	};

	struct Archive
	{
		zip_t* zip = nullptr;
		explicit Archive(const fs::path& file)
		{
			int error = 0;
			zip = zip_open(file.string().c_str(), ZIP_RDONLY, &error);
		}
		~Archive() { if (zip) zip_close(zip); }
		Archive(const Archive&) = delete;
		Archive& operator=(const Archive&) = delete;
	};

	// ZIPs of an unpacked Wii U title often have code/*.rpx, content/*, meta/meta.xml.
	// Return their common directory as a single playable title, rather than just an RPX.
	std::string TitleRoot(const std::string& name)
	{
		const std::string low = Lower(name);
		const size_t pos = low.find("code/");
		if (pos == std::string::npos || (pos != 0 && low[pos - 1] != '/'))
			return {};
		if (Lower(fs::path(name).extension().string()) != ".rpx")
			return {};
		return name.substr(0, pos);
	}

	std::vector<Candidate> Candidates(zip_t* archive, System system)
	{
		std::vector<Candidate> result;
		const zip_int64_t n = zip_get_num_entries(archive, 0);
		if (n < 0 || n > kMaxEntries)
			return result;
		std::vector<Candidate> files;
		std::set<std::string> metaRoots;
		std::set<std::string> codeRoots;
		for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(n); ++i)
		{
			zip_stat_t st;
			zip_stat_init(&st);
			if (zip_stat_index(archive, i, 0, &st) != 0 || !st.name)
				continue;
			const std::string name(st.name);
			if (!SafeName(name))
				continue;
			// Skip encrypted and obviously oversized files; size is taken from the central directory.
			if ((st.valid & ZIP_STAT_ENCRYPTION_METHOD) && st.encryption_method != ZIP_EM_NONE)
				continue;
			if ((st.valid & ZIP_STAT_SIZE) && st.size > kMaxSingleFile)
				continue;
			if (system == System::WiiU)
			{
				const std::string low = Lower(name);
				const std::string suffix = "meta/meta.xml";
				if (low.size() >= suffix.size() && low.ends_with(suffix))
				{
					const size_t prefixSize = low.size() - suffix.size();
					if (prefixSize == 0 || low[prefixSize - 1] == '/')
						metaRoots.insert(name.substr(0, prefixSize));
				}
				if (const std::string root = TitleRoot(name); !root.empty() || low.starts_with("code/"))
					codeRoots.insert(root);
			}
			if (name.back() == '/' || !ValidExtension(name, system))
				continue;
			Candidate c;
			c.member = name;
			c.display = fs::path(name).stem().string();
			c.format = Lower(fs::path(name).extension().string()).substr(1);
			files.push_back(std::move(c));
		}
		std::set<std::string> found;
		if (system == System::WiiU)
		{
			for (const std::string& root : codeRoots)
			{
				if (!metaRoots.contains(root))
					continue;
				Candidate c;
				c.member = root;
				c.folder = true;
				std::string label = root;
				if (!label.empty() && label.back() == '/')
					label.pop_back();
				c.display = fs::path(label).filename().string();
				c.format = "folder";
				result.push_back(c);
				found.insert(root);
			}
		}
		for (Candidate& c : files)
		{
			if (system == System::WiiU && Lower(fs::path(c.member).extension().string()) == ".rpx" &&
				found.contains(TitleRoot(c.member)))
				continue;
			result.push_back(std::move(c));
		}
		return result;
	}

	void Scan(const fs::path& folder, int depth, System system, std::vector<ps5emu::Game>& games)
	{
		std::error_code ec;
		for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
			 it != end && !ec; it.increment(ec))
		{
			const fs::directory_entry& e = *it;
			if (e.is_directory(ec))
			{
				if (depth < kScanDepth)
					Scan(e.path(), depth + 1, system, games);
				continue;
			}
			if (!e.is_regular_file(ec) || !ZipExtension(e.path()))
				continue;
			Archive zip(e.path());
			if (!zip.zip)
				continue;
			for (Candidate& candidate : Candidates(zip.zip, system))
			{
				ps5emu::Game game;
				game.path = e.path();
				game.archiveMember = candidate.member;
				game.archiveFolder = candidate.folder;
				game.titleId = Hash(e.path().string() + "\n" + candidate.member + (candidate.folder ? "/" : "")) |
					0xF000000000000000ull; // a stable library ID outside the consoles' real title IDs
				game.name = candidate.display.empty() ? e.path().stem().string() : candidate.display;
				game.format = "ZIP " + candidate.format;
				game.nds = system == System::Nds;
				games.push_back(std::move(game));
			}
		}
	}

	bool Selected(const std::string& name, const ps5emu::Game& game)
	{
		return game.archiveFolder ? name.starts_with(game.archiveMember) : name == game.archiveMember;
	}
}

void AddArchives(const fs::path& folder, System system, std::vector<ps5emu::Game>& games)
{
	Scan(folder, 0, system, games);
}

bool IsArchiveGame(const ps5emu::Game& game)
{
	return ZipExtension(game.path) && (game.archiveFolder || !game.archiveMember.empty());
}

std::string TileKey(const ps5emu::Game& game)
{
	if (!IsArchiveGame(game))
		return game.path.string();
	// A single ZIP can contain several games: the archive path alone is ambiguous.
	return game.path.string() + "#ps5zip-" + Hex(Hash(game.archiveMember + (game.archiveFolder ? "/" : "")));
}

bool Prepare(const ps5emu::Game& game, ps5emu::Game& ready, std::string& error)
{
	ready = game;
	if (!IsArchiveGame(game))
		return true;
	Archive archive(game.path);
	if (!archive.zip)
	{
		error = "The ZIP archive cannot be opened: " + game.path.string();
		return false;
	}
	const zip_int64_t count = zip_get_num_entries(archive.zip, 0);
	if (count < 0 || count > kMaxEntries)
	{
		error = "The ZIP has an invalid or excessive number of entries.";
		return false;
	}
	// Validation is performed again at launch, not only when the library was scanned.
	struct Entry { zip_uint64_t index; std::string name; uint64_t size; };
	std::vector<Entry> selected;
	uint64_t total = 0;
	for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(count); i++)
	{
		zip_stat_t st;
		zip_stat_init(&st);
		if (zip_stat_index(archive.zip, i, 0, &st) != 0 || !st.name)
			continue;
		const std::string name(st.name);
		if (!Selected(name, game))
			continue;
		if (!SafeName(name) || (st.valid & ZIP_STAT_ENCRYPTION_METHOD && st.encryption_method != ZIP_EM_NONE) ||
			!(st.valid & ZIP_STAT_SIZE) || st.size > kMaxSingleFile || st.size > kMaxTotalBytes - total)
		{
			error = "The ZIP contains an unsafe, encrypted, or oversized game file.";
			return false;
		}
		total += st.size;
		if (name.back() != '/')
			selected.push_back({i, name, st.size});
	}
	if (selected.empty() || (!game.archiveFolder && selected.size() != 1))
	{
		error = "The selected game is no longer in this ZIP archive.";
		return false;
	}
	std::error_code ec;
	const uint64_t zipSize = fs::file_size(game.path, ec);
	if (ec)
	{
		error = "The ZIP file is not accessible.";
		return false;
	}
	const auto modified = fs::last_write_time(game.path, ec);
	if (ec)
	{
		error = "The ZIP modification time could not be read.";
		return false;
	}
	const std::string key = game.path.string() + "\n" + std::to_string(zipSize) + "\n" +
		std::to_string(static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(modified.time_since_epoch()).count())) + "\n" + game.archiveMember + (game.archiveFolder ? "D" : "F");
	const fs::path cacheRoot = fs::path(ps5paths::kCache) / "zip-games";
	fs::create_directories(cacheRoot, ec);
	if (ec)
	{
		error = "The ZIP game cache cannot be created: " + ec.message();
		return false;
	}
	const fs::path final = cacheRoot / Hex(Hash(key));
	const fs::path rel = fs::path(game.archiveMember);
	const fs::path playable = game.archiveFolder ? (final / rel) : (final / rel);
	if (fs::exists(final / ".complete", ec) && fs::exists(playable, ec))
	{
		ready.path = playable;
		return true;
	}
	// Do not start extracting when the PS5's /data lacks space; a WUD may need ~25 GiB.
	const fs::space_info available = fs::space(cacheRoot, ec);
	if (!ec && (total > available.available || available.available - total < kReserve))
	{
		error = "Not enough free storage to unpack the ZIP game (" + std::to_string(total / (1 << 20)) + " MiB needed).";
		return false;
	}
	const fs::path temporary = cacheRoot / (Hex(Hash(key)) + ".partial");
	fs::remove_all(temporary, ec); // leftover from a previous interrupted extraction
	ec.clear();
	fs::create_directories(temporary, ec);
	if (ec)
	{
		error = "Cannot create temporary ZIP game folder: " + ec.message();
		return false;
	}
	const auto fail = [&](const std::string& reason) {
		std::error_code ignored;
		fs::remove_all(temporary, ignored);
		error = reason;
		return false;
	};
	std::array<char, 64 * 1024> buffer;
	for (const auto& [index, member, expected] : selected)
	{
		const fs::path target = temporary / fs::path(member);
		fs::create_directories(target.parent_path(), ec);
		if (ec)
			return fail("Cannot make a ZIP game's folder: " + ec.message());
		zip_file_t* input = zip_fopen_index(archive.zip, index, 0);
		if (!input)
			return fail("A ZIP game entry cannot be decompressed.");
		std::ofstream output(target, std::ios::binary | std::ios::trunc);
		if (!output)
		{
			zip_fclose(input);
			return fail("Cannot write the ZIP game to the game cache.");
		}
		zip_int64_t n = 0;
		uint64_t written = 0;
		while ((n = zip_fread(input, buffer.data(), buffer.size())) > 0)
		{
			if (static_cast<uint64_t>(n) > expected - written)
			{
				n = -1; // archive metadata lied: stop instead of expanding without bound
				break;
			}
			output.write(buffer.data(), static_cast<std::streamsize>(n));
			written += static_cast<uint64_t>(n);
			if (!output)
				break;
		}
		output.close();
		const bool wrote = static_cast<bool>(output);
		const int closed = zip_fclose(input); // verifies the entry's CRC on supported libzip versions
		if (!wrote || n < 0 || written != expected || closed != 0)
			return fail("The ZIP game is damaged or could not be written (CRC/decompression error).");
	}
	// A marker is only written after every entry was fully extracted and checked.
	{
		std::ofstream marker(temporary / ".complete", std::ios::trunc);
		marker << key;
		if (!marker)
			return fail("The ZIP game cache could not be finalised.");
	}
	fs::remove_all(final, ec);
	ec.clear();
	fs::rename(temporary, final, ec);
	if (ec)
		return fail("The ZIP game cache could not be finalised: " + ec.message());
	ready.path = playable;
	return true;
}
}
