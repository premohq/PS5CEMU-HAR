// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: each side's game list as last seen (catalog.h).

#include "catalog.h"
#include "paths.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace ps5catalog
{
	namespace
	{
		std::vector<Entry> s_games[3];
		bool s_loaded = false;

		std::string Path()
		{
			return std::string(ps5paths::kRoot) + "/library.json";
		}

		std::string Hex(uint64_t value)
		{
			char text[17];
			std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)value);
			return text;
		}

		std::string String(const rapidjson::Value& json, const char* key)
		{
			return json.HasMember(key) && json[key].IsString() ? json[key].GetString() : std::string();
		}

		int64_t Int(const rapidjson::Value& json, const char* key)
		{
			return json.HasMember(key) && json[key].IsInt64() ? json[key].GetInt64() : 0;
		}

		void Sort(std::vector<Entry>& games)
		{
			std::sort(games.begin(), games.end(), [](const Entry& a, const Entry& b) { return a.game.name < b.game.name; });
		}
	}

	void Load()
	{
		if (s_loaded)
			return;
		s_loaded = true;
		std::ifstream file(Path());
		if (!file)
			return;
		std::stringstream text;
		text << file.rdbuf();
		rapidjson::Document json;
		if (json.Parse(text.str().c_str()).HasParseError() || !json.IsObject())
			return;
		const char* sides[3] = {"wiiu", "3ds", "ds"};
		for (int side = 0; side < 3; side++)
		{
			if (!json.HasMember(sides[side]) || !json[sides[side]].IsArray())
				continue;
			for (const auto& item : json[sides[side]].GetArray())
			{
				if (!item.IsObject())
					continue;
				Entry entry;
				entry.game.titleId = std::strtoull(String(item, "titleId").c_str(), nullptr, 16);
				entry.game.name = String(item, "name");
				entry.game.path = String(item, "path");
				entry.game.version = (uint16_t)Int(item, "version");
				entry.game.hasUpdate = item.HasMember("hasUpdate") && item["hasUpdate"].IsBool() && item["hasUpdate"].GetBool();
				entry.game.dlcCount = (uint32_t)Int(item, "dlc");
				entry.game.format = String(item, "format");
				entry.game.publisher = String(item, "publisher");
				entry.game.gameId = String(item, "gameId");
				entry.game.nds = item.HasMember("nds") && item["nds"].IsBool() && item["nds"].GetBool();
				entry.ambient[0] = (uint32_t)std::strtoul(String(item, "ambient0").c_str(), nullptr, 16);
				entry.ambient[1] = (uint32_t)std::strtoul(String(item, "ambient1").c_str(), nullptr, 16);
				entry.added = Int(item, "added");
				entry.favourite = item.HasMember("favourite") && item["favourite"].IsBool() && item["favourite"].GetBool();
				entry.lastPlayed = Int(item, "lastPlayed");
				entry.minutesPlayed = (uint32_t)Int(item, "minutes");
				if (!entry.game.name.empty())
					s_games[side].push_back(std::move(entry));
			}
			Sort(s_games[side]);
		}
	}

	bool Save()
	{
		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		writer.StartObject();
		writer.Key("version");
		writer.Int(1);
		const char* sides[3] = {"wiiu", "3ds", "ds"};
		for (int side = 0; side < 3; side++)
		{
			writer.Key(sides[side]);
			writer.StartArray();
			for (const Entry& entry : s_games[side])
			{
				writer.StartObject();
				writer.Key("titleId");
				writer.String(Hex(entry.game.titleId).c_str());
				writer.Key("name");
				writer.String(entry.game.name.c_str());
				writer.Key("path");
				writer.String(entry.game.path.string().c_str());
				writer.Key("version");
				writer.Int(entry.game.version);
				writer.Key("hasUpdate");
				writer.Bool(entry.game.hasUpdate);
				writer.Key("dlc");
				writer.Int((int)entry.game.dlcCount);
				writer.Key("format");
				writer.String(entry.game.format.c_str());
				writer.Key("publisher");
				writer.String(entry.game.publisher.c_str());
				writer.Key("gameId");
				writer.String(entry.game.gameId.c_str());
				if (entry.game.nds)
				{
					writer.Key("nds");
					writer.Bool(true);
				}
				char colour[9];
				std::snprintf(colour, sizeof(colour), "%08x", entry.ambient[0]);
				writer.Key("ambient0");
				writer.String(colour);
				std::snprintf(colour, sizeof(colour), "%08x", entry.ambient[1]);
				writer.Key("ambient1");
				writer.String(colour);
				writer.Key("added");
				writer.Int64(entry.added);
				writer.Key("favourite");
				writer.Bool(entry.favourite);
				writer.Key("lastPlayed");
				writer.Int64(entry.lastPlayed);
				writer.Key("minutes");
				writer.Int64(entry.minutesPlayed);
				writer.EndObject();
			}
			writer.EndArray();
		}
		writer.EndObject();
		const std::string path = Path(), temporary = path + ".tmp";
		{
			std::ofstream file(temporary, std::ios::trunc);
			if (!file)
				return false;
			file << buffer.GetString() << '\n';
		}
		return std::rename(temporary.c_str(), path.c_str()) == 0;
	}

	const std::vector<Entry>& Games(System side)
	{
		Load();
		return s_games[(int)side];
	}

	Entry* Find(System side, uint64_t titleId)
	{
		Load();
		for (Entry& entry : s_games[(int)side])
			if (entry.game.titleId == titleId)
				return &entry;
		return nullptr;
	}

	void Replace(System side, const std::vector<ps5emu::Game>& scanned)
	{
		Load();
		std::vector<Entry>& games = s_games[(int)side];
		const bool first = games.empty();
		const int64_t now = std::time(nullptr);
		std::vector<Entry> fresh;
		fresh.reserve(scanned.size());
		for (const ps5emu::Game& game : scanned)
		{
			Entry entry;
			if (const Entry* known = Find(side, game.titleId))
				entry = *known;
			else
				entry.added = first ? 0 : now;
			entry.game = game;
			fresh.push_back(std::move(entry));
		}
		games = std::move(fresh);
		Sort(games);
	}

	void SetFavourite(System side, uint64_t titleId, bool favourite)
	{
		if (Entry* entry = Find(side, titleId))
			entry->favourite = favourite;
	}

	void SetAmbient(System side, uint64_t titleId, const uint32_t ambient[2])
	{
		if (Entry* entry = Find(side, titleId))
		{
			entry->ambient[0] = ambient[0];
			entry->ambient[1] = ambient[1];
		}
	}

	void Played(System side, uint64_t titleId, int64_t when)
	{
		if (Entry* entry = Find(side, titleId))
			entry->lastPlayed = when;
	}

	void AddMinutes(System side, uint64_t titleId, uint32_t minutes)
	{
		if (Entry* entry = Find(side, titleId))
			entry->minutesPlayed += minutes;
	}

	void Migrate(System side, const std::vector<uint64_t>& recent)
	{
		Load();
		for (Entry& entry : s_games[(int)side])
			if (entry.lastPlayed != 0)
				return; // the catalogue has its own times
		// the recent list newest first: a second apart, a day ago
		const int64_t base = std::time(nullptr) - 86400;
		for (size_t i = 0; i < recent.size(); i++)
			if (Entry* entry = Find(side, recent[i]))
				entry->lastPlayed = base - (int64_t)i;
	}
}
