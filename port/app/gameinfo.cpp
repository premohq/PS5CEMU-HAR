// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameinfo.h"
#include "paths.h"
#include "../ps5/log.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ps5gameinfo
{
	namespace
	{
		// One system's database: its text, and where each game's line starts
		struct Database
		{
			bool loaded = false;
			std::string text;
			std::unordered_map<std::string, size_t> lines;
		};

		std::mutex s_mutex;
		std::string s_folder;
		std::map<ps5boxart::System, Database> s_databases;

		std::string Folder()
		{
			return s_folder.empty() ? ps5paths::Assets() + "/gametdb" : s_folder;
		}

		// The file whole (gzip, as render-gametdb.py writes it), and each line's ID
		void Load(ps5boxart::System system, Database& database)
		{
			database.loaded = true;
			const std::string path = Folder() + (system == ps5boxart::System::WiiU ? "/wiiu.tsv.gz" :
				system == ps5boxart::System::Nds ? "/ds.tsv.gz" : "/3ds.tsv.gz");
			gzFile file = gzopen(path.c_str(), "rb");
			if (!file)
			{
				ps5log::Line("[gameinfo] {} cannot be read: no game information", path);
				return;
			}
			std::vector<char> buffer(1 << 16);
			for (int read; (read = gzread(file, buffer.data(), (unsigned)buffer.size())) > 0;)
				database.text.append(buffer.data(), (size_t)read);
			gzclose(file);
			for (size_t start = 0; start < database.text.size();)
			{
				size_t end = database.text.find('\n', start);
				if (end == std::string::npos)
					end = database.text.size();
				const size_t tab = database.text.find('\t', start);
				if (tab != std::string::npos && tab < end)
					database.lines.emplace(database.text.substr(start, tab - start), start);
				start = end + 1;
			}
			ps5log::Line("[gameinfo] {}: {} games", path, database.lines.size());
		}

		// A field as written: \t, \n and \\ back to what they stand for
		std::string Unescape(std::string_view field)
		{
			std::string out;
			out.reserve(field.size());
			for (size_t i = 0; i < field.size(); i++)
			{
				if (field[i] != '\\' || i + 1 == field.size())
				{
					out += field[i];
					continue;
				}
				const char next = field[++i];
				out += next == 't' ? '\t' : next == 'n' ? '\n' : next;
			}
			return out;
		}
	}

	bool Find(ps5boxart::System system, const std::string& id, Info& info)
	{
		if (id.empty())
			return false;
		std::lock_guard lock(s_mutex);
		Database& database = s_databases[system];
		if (!database.loaded)
			Load(system, database);
		const auto it = database.lines.find(id);
		if (it == database.lines.end())
			return false;
		size_t end = database.text.find('\n', it->second);
		if (end == std::string::npos)
			end = database.text.size();
		std::vector<std::string> fields;
		for (size_t start = it->second; start <= end;)
		{
			size_t tab = database.text.find('\t', start);
			if (tab == std::string::npos || tab > end)
				tab = end;
			fields.push_back(Unescape(std::string_view(database.text).substr(start, tab - start)));
			start = tab + 1;
		}
		fields.resize(10);
		info = {};
		info.id = fields[0];
		info.title = fields[1];
		info.region = fields[2];
		info.synopsis = fields[3];
		info.developer = fields[4];
		info.publisher = fields[5];
		info.released = fields[6];
		info.genre = fields[7];
		info.players = std::atoi(fields[8].c_str());
		info.rating = fields[9];
		return true;
	}

	std::string ReleaseDate(const std::string& released)
	{
		static constexpr const char* kMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August",
			"September", "October", "November", "December"};
		int year = 0, month = 0, day = 0;
		const int parts = std::sscanf(released.c_str(), "%d-%d-%d", &year, &month, &day);
		if (parts < 1 || year <= 0)
			return {};
		if (parts < 2 || month < 1 || month > 12)
			return std::to_string(year);
		if (parts < 3 || day < 1)
			return fmt::format("{} {}", kMonths[month - 1], year);
		return fmt::format("{} {}, {}", kMonths[month - 1], day, year);
	}

	std::string Year(const std::string& released)
	{
		return released.size() >= 4 ? released.substr(0, 4) : std::string();
	}

	std::string Genres(const std::string& genre, int most)
	{
		std::string out;
		bool start = true;
		int count = 1;
		for (const char c : genre)
		{
			if (c == ',')
			{
				if (most > 0 && ++count > most)
					break;
				out += ", ";
				start = true;
				continue;
			}
			out += start ? (char)std::toupper((unsigned char)c) : c;
			start = false;
		}
		return out;
	}

	void SetFolder(const std::string& folder)
	{
		std::lock_guard lock(s_mutex);
		s_folder = folder;
		s_databases.clear();
	}
}
