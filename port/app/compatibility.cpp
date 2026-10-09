// SPDX-License-Identifier: GPL-3.0-or-later
#include "compatibility.h"
#include "lang.h"
#include "paths.h"

#include <cctype>
#include <fstream>
#include <mutex>
#include <vector>

namespace ps5compat
{
	namespace
	{
		std::mutex s_mutex;
		std::string s_path;
		bool s_loaded = false;
		std::vector<Report> s_reports[2]; // the Wii U's, the 3DS's

		// lower case letters and digits only
		std::string Key(const std::string& name)
		{
			std::string key;
			for (const char c : name)
				if (std::isalnum((unsigned char)c))
					key += (char)std::tolower((unsigned char)c);
			return key;
		}

		// A cell as text: no bold, links as their words
		std::string Plain(std::string cell)
		{
			std::string out;
			for (size_t i = 0; i < cell.size(); i++)
			{
				if (cell.compare(i, 2, "**") == 0)
				{
					i++;
					continue;
				}
				if (cell[i] == '[')
				{
					const size_t close = cell.find("](", i);
					const size_t end = close == std::string::npos ? std::string::npos : cell.find(')', close);
					if (end != std::string::npos)
					{
						out += cell.substr(i + 1, close - i - 1);
						i = end;
						continue;
					}
				}
				out += cell[i];
			}
			const size_t first = out.find_first_not_of(' '), last = out.find_last_not_of(' ');
			return first == std::string::npos ? std::string() : out.substr(first, last - first + 1);
		}

		void Load()
		{
			s_loaded = true;
			std::ifstream file(s_path.empty() ? ps5paths::Assets() + "/compatibility.md" : s_path);
			int table = -1; // which side's section
			for (std::string line; std::getline(file, line);)
			{
				if (line.rfind("## ", 0) == 0)
					table = line.find("Wii U") != std::string::npos ? 0 : line.find("3DS") != std::string::npos ? 1 : -1;
				if (table < 0 || line.empty() || line[0] != '|' || line.rfind("|---", 0) == 0 || line.rfind("| Game", 0) == 0)
					continue;
				std::vector<std::string> cells;
				size_t start = 1;
				for (size_t bar; (bar = line.find('|', start)) != std::string::npos; start = bar + 1)
					cells.push_back(Plain(line.substr(start, bar - start)));
				if (cells.size() >= 4 && !cells[0].empty())
					s_reports[table].push_back({cells[0], cells[1], cells[2], cells[3]});
			}
		}
	}

	const Report* Find(bool n3ds, const std::string& name)
	{
		std::lock_guard lock(s_mutex);
		if (!s_loaded)
			Load();
		const std::string key = Key(name);
		if (key.empty())
			return nullptr;
		for (const Report& report : s_reports[n3ds ? 1 : 0])
			if (Key(report.name) == key)
				return &report;
		return nullptr;
	}

	const char* Kind(const std::string& status)
	{
		// tr: the compatibility list's statuses, as the launcher shows them
		static constexpr const char* kStatuses[] = {ps5lang::TrMarkC("status", "Playable"), ps5lang::TrMarkC("status", "Issues"),
			ps5lang::TrMarkC("status", "Crashes"), ps5lang::TrMarkC("status", "Won't start")};
		(void)kStatuses;
		if (status == "Playable")
			return "good";
		if (status == "Issues")
			return "warn";
		if (status == "Crashes" || status == "Won't start")
			return "bad";
		return "";
	}

	void SetPath(const std::string& path)
	{
		std::lock_guard lock(s_mutex);
		s_path = path;
		s_loaded = false;
		s_reports[0].clear();
		s_reports[1].clear();
	}
}
