// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"
#include "../app/paths.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace ps5settings
{
	namespace
	{
		using Writer = rapidjson::PrettyWriter<rapidjson::StringBuffer>;

		std::string Hex(uint64_t value)
		{
			char text[17];
			std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)value);
			return text;
		}

		void ReadString(const rapidjson::Value& json, const char* key, std::string& out)
		{
			if (json.HasMember(key) && json[key].IsString())
				out = json[key].GetString();
		}

		void ReadInt(const rapidjson::Value& json, const char* key, int& out, int low, int high)
		{
			if (json.HasMember(key) && json[key].IsInt())
				out = std::clamp(json[key].GetInt(), low, high);
		}

		void ReadBool(const rapidjson::Value& json, const char* key, bool& out)
		{
			if (json.HasMember(key) && json[key].IsBool())
				out = json[key].GetBool();
		}

		void ReadGames(const rapidjson::Value& json, uint64_t& lastGame, std::vector<uint64_t>& recent)
		{
			if (json.HasMember("lastGame") && json["lastGame"].IsString())
				lastGame = std::strtoull(json["lastGame"].GetString(), nullptr, 16);
			if (json.HasMember("recent") && json["recent"].IsArray())
				for (const auto& entry : json["recent"].GetArray())
					if (entry.IsString() && recent.size() < 4)
						recent.push_back(std::strtoull(entry.GetString(), nullptr, 16));
		}

		void WriteGames(Writer& writer, uint64_t lastGame, const std::vector<uint64_t>& recent)
		{
			writer.Key("lastGame");
			writer.String(Hex(lastGame).c_str());
			writer.Key("recent");
			writer.StartArray();
			for (uint64_t titleId : recent)
				writer.String(Hex(titleId).c_str());
			writer.EndArray();
		}

		void ReadN3ds(const rapidjson::Value& json, N3ds& n3ds)
		{
			ReadString(json, "gamesFolder", n3ds.gamesFolder);
			ReadInt(json, "resolution", n3ds.resolution, 1, 10);
			ReadInt(json, "layout", n3ds.layout, 0, 3);
			ReadInt(json, "textureFilter", n3ds.textureFilter, 0, 5);
			ReadInt(json, "volume", n3ds.volume, 0, 100);
			ReadBool(json, "motion", n3ds.motion);
			ReadInt(json, "deadzone", n3ds.deadzone, 0, 50);
			ReadBool(json, "performance", n3ds.performance);
			ReadInt(json, "cpuClock", n3ds.cpuClock, 25, 400);
			ReadBool(json, "invariantPosition", n3ds.invariantPosition);
			ReadInt(json, "region", n3ds.region, -1, 6);
			ReadInt(json, "language", n3ds.language, -1, 11);
			ReadBool(json, "customTextures", n3ds.customTextures);
			ReadInt(json, "border", n3ds.border, 0, 5);
			ReadInt(json, "dsFilter", n3ds.dsFilter, 0, 2);
			ReadBool(json, "dsJit", n3ds.dsJit);
			ReadBool(json, "dsOwnBios", n3ds.dsOwnBios);
			ReadString(json, "articAddress", n3ds.articAddress);
			ReadInt(json, "gameCount", n3ds.gameCount, -1, 1000000);
			if (json.HasMember("buttons") && json["buttons"].IsObject())
				for (const auto& member : json["buttons"].GetObject())
					if (member.value.IsString())
						n3ds.buttons[member.name.GetString()] = member.value.GetString();
			ReadGames(json, n3ds.lastGame, n3ds.recent);
		}

		void WriteN3ds(Writer& writer, const N3ds& n3ds)
		{
			writer.StartObject();
			writer.Key("gamesFolder");
			writer.String(n3ds.gamesFolder.c_str());
			writer.Key("resolution");
			writer.Int(n3ds.resolution);
			writer.Key("layout");
			writer.Int(n3ds.layout);
			writer.Key("textureFilter");
			writer.Int(n3ds.textureFilter);
			writer.Key("volume");
			writer.Int(n3ds.volume);
			writer.Key("motion");
			writer.Bool(n3ds.motion);
			writer.Key("deadzone");
			writer.Int(n3ds.deadzone);
			writer.Key("performance");
			writer.Bool(n3ds.performance);
			writer.Key("cpuClock");
			writer.Int(n3ds.cpuClock);
			writer.Key("invariantPosition");
			writer.Bool(n3ds.invariantPosition);
			writer.Key("region");
			writer.Int(n3ds.region);
			writer.Key("language");
			writer.Int(n3ds.language);
			writer.Key("customTextures");
			writer.Bool(n3ds.customTextures);
			writer.Key("border");
			writer.Int(n3ds.border);
			writer.Key("dsFilter");
			writer.Int(n3ds.dsFilter);
			writer.Key("dsJit");
			writer.Bool(n3ds.dsJit);
			writer.Key("dsOwnBios");
			writer.Bool(n3ds.dsOwnBios);
			writer.Key("articAddress");
			writer.String(n3ds.articAddress.c_str());
			writer.Key("gameCount");
			writer.Int(n3ds.gameCount);
			writer.Key("buttons");
			writer.StartObject();
			for (const auto& [button, input] : n3ds.buttons)
			{
				writer.Key(button.c_str());
				writer.String(input.c_str());
			}
			writer.EndObject();
			WriteGames(writer, n3ds.lastGame, n3ds.recent);
			writer.EndObject();
		}
	}

	Launcher Load()
	{
		Launcher settings;
		std::ifstream file(ps5paths::kLauncherSettings);
		if (!file)
			return settings;
		std::stringstream text;
		text << file.rdbuf();
		rapidjson::Document json;
		if (json.Parse(text.str().c_str()).HasParseError() || !json.IsObject())
			return settings;
		ReadString(json, "gamesFolder", settings.gamesFolder);
		ReadInt(json, "upscaleFilter", settings.upscaleFilter, 0, 3);
		ReadBool(json, "highFrameRate", settings.highFrameRate);
		ReadInt(json, "framePacing", settings.framePacing, 1, 3);
		ReadBool(json, "overlay", settings.overlay);
		ReadBool(json, "rumble", settings.rumble);
		ReadBool(json, "pinCpuThreads", settings.pinCpuThreads);
		ReadString(json, "radvDebug", settings.radvDebug);
		if (json.HasMember("radvEnvironment") && json["radvEnvironment"].IsObject())
			for (const auto& member : json["radvEnvironment"].GetObject())
				if (member.value.IsString())
					settings.radvEnvironment[member.name.GetString()] = member.value.GetString();
		ReadInt(json, "cemuSubmitDraws", settings.cemuSubmitDraws, 0, 5000);
		ReadInt(json, "volume", settings.volume, 0, 100);
		ReadInt(json, "gameCount", settings.gameCount, -1, 1000000);
		ReadString(json, "launchError", settings.launchError);
		ReadString(json, "side", settings.side);
		ReadGames(json, settings.lastGame, settings.recent);
		if (json.HasMember("n3ds") && json["n3ds"].IsObject())
			ReadN3ds(json["n3ds"], settings.n3ds);
		if (json.HasMember("nds") && json["nds"].IsObject())
			ReadN3ds(json["nds"], settings.nds);
		if (json.HasMember("games") && json["games"].IsObject())
			for (const auto& game : json["games"].GetObject())
			{
				if (!game.value.IsObject())
					continue;
				const std::string key = game.name.GetString();
				const std::string side = key.substr(0, key.find(':'));
				GameValues values;
				for (const auto& member : game.value.GetObject())
				{
					const std::string name = member.name.GetString();
					const auto& names = GameSettingNames(side);
					if (std::find(names.begin(), names.end(), name) == names.end())
						continue;
					if (member.value.IsInt())
						values[name] = member.value.GetInt();
					else if (member.value.IsBool())
						values[name] = member.value.GetBool() ? 1 : 0;
				}
				if (!values.empty())
					settings.games[key] = std::move(values);
			}
		ReadString(json, "music", settings.music);
		// the setup theme is the launcher's only music now: the shop theme's players get it
		if (settings.music == "shop")
			settings.music = "setup";
		else if (settings.music != "setup")
			settings.music = "off";
		ReadInt(json, "musicVolume", settings.musicVolume, 0, 100);
		ReadBool(json, "boxArt", settings.boxArt);
		ReadBool(json, "asyncShaders", settings.asyncShaders);
		ReadBool(json, "gamePadSpeaker", settings.gamePadSpeaker);
		ReadBool(json, "menuSounds", settings.menuSounds);
		if (json.HasMember("ui") && json["ui"].IsObject())
		{
			const rapidjson::Value& ui = json["ui"];
			Ui& out = settings.ui;
			ReadString(ui, "startOn", out.startOn);
			if (out.startOn != "ask")
				out.startOn = "last";
			ReadString(ui, "lastSide", out.lastSide);
			ReadBool(ui, "largerText", out.largerText);
			ReadBool(ui, "highContrast", out.highContrast);
			ReadBool(ui, "reduceMotion", out.reduceMotion);
			ReadInt(ui, "holdMs", out.holdMs, 400, 1500);
			ReadBool(ui, "gamePictures", out.gamePictures);
			ReadBool(ui, "setupDone", out.setupDone);
			// each side's filter and sort (3.5.0 kept two of each under names of their own; its sort, A to
			// Z by default, is not taken: the Library opens on the games played last now that it is
			// where a side opens)
			ReadInt(ui, "libraryFilterWiiU", out.libraryFilter[0], 0, 3);
			ReadInt(ui, "libraryFilter3ds", out.libraryFilter[1], 0, 2);
			auto readSides = [&](const char* key, int (&out)[3], int most) {
				if (ui.HasMember(key) && ui[key].IsArray())
					for (rapidjson::SizeType i = 0; i < ui[key].Size() && i < 3; i++)
						if (ui[key][i].IsInt())
							out[i] = std::clamp(ui[key][i].GetInt(), 0, i == 0 ? most : std::min(most, 2));
			};
			readSides("libraryFilter", out.libraryFilter, 3);
			readSides("librarySort", out.librarySort, 3);
		}
		return settings;
	}

	bool Save(const Launcher& settings)
	{
		rapidjson::StringBuffer buffer;
		Writer writer(buffer);
		writer.StartObject();
		writer.Key("gamesFolder");
		writer.String(settings.gamesFolder.c_str());
		writer.Key("upscaleFilter");
		writer.Int(settings.upscaleFilter);
		writer.Key("highFrameRate");
		writer.Bool(settings.highFrameRate);
		writer.Key("framePacing");
		writer.Int(settings.framePacing);
		writer.Key("overlay");
		writer.Bool(settings.overlay);
		writer.Key("rumble");
		writer.Bool(settings.rumble);
		writer.Key("pinCpuThreads");
		writer.Bool(settings.pinCpuThreads);
		writer.Key("radvDebug");
		writer.String(settings.radvDebug.c_str());
		writer.Key("radvEnvironment");
		writer.StartObject();
		for (const auto& [name, value] : settings.radvEnvironment)
		{
			writer.Key(name.c_str());
			writer.String(value.c_str());
		}
		writer.EndObject();
		writer.Key("cemuSubmitDraws");
		writer.Int(settings.cemuSubmitDraws);
		writer.Key("volume");
		writer.Int(settings.volume);
		writer.Key("gameCount");
		writer.Int(settings.gameCount);
		WriteGames(writer, settings.lastGame, settings.recent);
		writer.Key("n3ds");
		WriteN3ds(writer, settings.n3ds);
		writer.Key("nds");
		WriteN3ds(writer, settings.nds);
		writer.Key("games");
		writer.StartObject();
		for (const auto& [key, values] : settings.games)
		{
			if (values.empty())
				continue;
			writer.Key(key.c_str());
			writer.StartObject();
			for (const auto& [name, value] : values)
			{
				writer.Key(name.c_str());
				writer.Int(value);
			}
			writer.EndObject();
		}
		writer.EndObject();
		writer.Key("music");
		writer.String(settings.music.c_str());
		writer.Key("musicVolume");
		writer.Int(settings.musicVolume);
		writer.Key("boxArt");
		writer.Bool(settings.boxArt);
		writer.Key("asyncShaders");
		writer.Bool(settings.asyncShaders);
		writer.Key("gamePadSpeaker");
		writer.Bool(settings.gamePadSpeaker);
		writer.Key("menuSounds");
		writer.Bool(settings.menuSounds);
		writer.Key("side");
		writer.String(settings.side.c_str());
		writer.Key("launchError");
		writer.String(settings.launchError.c_str());
		writer.Key("ui");
		writer.StartObject();
		writer.Key("startOn");
		writer.String(settings.ui.startOn.c_str());
		writer.Key("lastSide");
		writer.String(settings.ui.lastSide.c_str());
		writer.Key("largerText");
		writer.Bool(settings.ui.largerText);
		writer.Key("highContrast");
		writer.Bool(settings.ui.highContrast);
		writer.Key("reduceMotion");
		writer.Bool(settings.ui.reduceMotion);
		writer.Key("holdMs");
		writer.Int(settings.ui.holdMs);
		writer.Key("gamePictures");
		writer.Bool(settings.ui.gamePictures);
		writer.Key("setupDone");
		writer.Bool(settings.ui.setupDone);
		writer.Key("libraryFilter");
		writer.StartArray();
		for (int filter : settings.ui.libraryFilter)
			writer.Int(filter);
		writer.EndArray();
		writer.Key("librarySort");
		writer.StartArray();
		for (int sort : settings.ui.librarySort)
			writer.Int(sort);
		writer.EndArray();
		writer.EndObject();
		writer.EndObject();
		const std::string temporary = std::string(ps5paths::kLauncherSettings) + ".tmp";
		{
			std::ofstream file(temporary, std::ios::trunc);
			if (!file)
				return false;
			file << buffer.GetString() << '\n';
		}
		return std::rename(temporary.c_str(), ps5paths::kLauncherSettings) == 0;
	}

	// -- games' own settings ------------------------------------------------------------------------

	namespace
	{
		// A setting a number stands for, and its range: on a side's struct (Launcher for the Wii U,
		// N3ds for the 3DS and the DS)
		struct Field
		{
			const char* name;
			int Launcher::* number = nullptr;
			bool Launcher::* flag = nullptr;
			int N3ds::* handheldNumber = nullptr;
			bool N3ds::* handheldFlag = nullptr;
			int low = 0, high = 1;
		};

		const std::vector<Field>& WiiUFields()
		{
			static const std::vector<Field> fields = {
				{"upscaleFilter", &Launcher::upscaleFilter, nullptr, nullptr, nullptr, 0, 3},
				{"highFrameRate", nullptr, &Launcher::highFrameRate},
				{"framePacing", &Launcher::framePacing, nullptr, nullptr, nullptr, 1, 3},
				{"overlay", nullptr, &Launcher::overlay},
				{"asyncShaders", nullptr, &Launcher::asyncShaders},
				{"volume", &Launcher::volume, nullptr, nullptr, nullptr, 0, 100},
			};
			return fields;
		}

		const std::vector<Field>& HandheldFields()
		{
			static const std::vector<Field> fields = {
				{"resolution", nullptr, nullptr, &N3ds::resolution, nullptr, 1, 10},
				{"textureFilter", nullptr, nullptr, &N3ds::textureFilter, nullptr, 0, 5},
				{"customTextures", nullptr, nullptr, nullptr, &N3ds::customTextures},
				{"layout", nullptr, nullptr, &N3ds::layout, nullptr, 0, 3},
				{"border", nullptr, nullptr, &N3ds::border, nullptr, 0, 5},
				{"volume", nullptr, nullptr, &N3ds::volume, nullptr, 0, 100},
				{"region", nullptr, nullptr, &N3ds::region, nullptr, -1, 6},
				{"language", nullptr, nullptr, &N3ds::language, nullptr, -1, 11},
				{"cpuClock", nullptr, nullptr, &N3ds::cpuClock, nullptr, 25, 400},
				{"dsFilter", nullptr, nullptr, &N3ds::dsFilter, nullptr, 0, 2},
				{"dsJit", nullptr, nullptr, nullptr, &N3ds::dsJit},
				{"dsOwnBios", nullptr, nullptr, nullptr, &N3ds::dsOwnBios},
			};
			return fields;
		}

		const Field* FindField(const std::string& side, const std::string& name)
		{
			for (const Field& field : side == "wiiu" ? WiiUFields() : HandheldFields())
				if (name == field.name)
					return &field;
			return nullptr;
		}

		const N3ds& Handheld(const Launcher& settings, const std::string& side)
		{
			return side == "ds" ? settings.nds : settings.n3ds;
		}

		N3ds& Handheld(Launcher& settings, const std::string& side)
		{
			return side == "ds" ? settings.nds : settings.n3ds;
		}
	}

	std::string GameKey(const std::string& side, uint64_t titleId)
	{
		return side + ":" + Hex(titleId);
	}

	const std::vector<std::string>& GameSettingNames(const std::string& side)
	{
		static const std::vector<std::string> wiiu = {"upscaleFilter", "highFrameRate", "framePacing", "overlay", "asyncShaders", "volume"};
		static const std::vector<std::string> n3ds = {"resolution", "textureFilter", "customTextures", "layout", "border", "volume", "region",
			"language", "cpuClock"};
		static const std::vector<std::string> nds = {"dsFilter", "dsJit", "layout", "border", "volume", "language", "dsOwnBios"};
		static const std::vector<std::string> none;
		return side == "wiiu" ? wiiu : side == "3ds" ? n3ds : side == "ds" ? nds : none;
	}

	bool Value(const Launcher& settings, const std::string& side, const std::string& name, int& value)
	{
		const Field* field = FindField(side, name);
		if (!field)
			return false;
		if (field->number)
			value = settings.*(field->number);
		else if (field->flag)
			value = settings.*(field->flag) ? 1 : 0;
		else if (field->handheldNumber)
			value = Handheld(settings, side).*(field->handheldNumber);
		else
			value = Handheld(settings, side).*(field->handheldFlag) ? 1 : 0;
		return true;
	}

	void SetValue(Launcher& settings, const std::string& side, const std::string& name, int value)
	{
		const Field* field = FindField(side, name);
		if (!field)
			return;
		value = std::clamp(value, field->low, field->high);
		if (field->number)
			settings.*(field->number) = value;
		else if (field->flag)
			settings.*(field->flag) = value != 0;
		else if (field->handheldNumber)
			Handheld(settings, side).*(field->handheldNumber) = value;
		else
			Handheld(settings, side).*(field->handheldFlag) = value != 0;
	}

	Launcher ForGame(const Launcher& settings, const std::string& side, uint64_t titleId)
	{
		Launcher game = settings;
		const auto found = settings.games.find(GameKey(side, titleId));
		if (found != settings.games.end())
			for (const auto& [name, value] : found->second)
				SetValue(game, side, name, value);
		return game;
	}

	void SaveChanges(const std::string& side, uint64_t titleId, const Launcher& before, const Launcher& after)
	{
		Launcher all = Load();
		Launcher merged = all;
		// what the menu changes that no game has its own of: as after has it (the buttons, motion,
		// the deadzone, the performance overlay of a handheld), all else the launcher's as it is
		if (side == "wiiu")
			merged.gamePadSpeaker = after.gamePadSpeaker;
		else
		{
			N3ds& to = Handheld(merged, side);
			const N3ds& from = Handheld(after, side);
			to.motion = from.motion;
			to.deadzone = from.deadzone;
			to.performance = from.performance;
			to.buttons = from.buttons;
		}
		const std::string key = GameKey(side, titleId);
		const auto own = all.games.find(key);
		const bool hasOwn = own != all.games.end() && !own->second.empty();
		for (const std::string& name : GameSettingNames(side))
		{
			int was = 0, now = 0;
			if (!Value(before, side, name, was) || !Value(after, side, name, now) || was == now)
				continue;
			if (hasOwn)
				merged.games[key][name] = now;
			else
				SetValue(merged, side, name, now);
		}
		// should the app start over from here, it comes back to this side
		merged.side = side;
		Save(merged);
	}

	void AddRecent(uint64_t& lastGame, std::vector<uint64_t>& recent, uint64_t titleId)
	{
		lastGame = titleId;
		std::erase(recent, titleId);
		recent.insert(recent.begin(), titleId);
		if (recent.size() > 4)
			recent.resize(4);
	}
}
