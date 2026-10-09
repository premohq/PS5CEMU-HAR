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
			ReadInt(ui, "libraryFilterWiiU", out.libraryFilter[0], 0, 3);
			ReadInt(ui, "libraryFilter3ds", out.libraryFilter[1], 0, 3);
			ReadInt(ui, "librarySortWiiU", out.librarySort[0], 0, 3);
			ReadInt(ui, "librarySort3ds", out.librarySort[1], 0, 3);
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
		writer.Key("libraryFilterWiiU");
		writer.Int(settings.ui.libraryFilter[0]);
		writer.Key("libraryFilter3ds");
		writer.Int(settings.ui.libraryFilter[1]);
		writer.Key("librarySortWiiU");
		writer.Int(settings.ui.librarySort[0]);
		writer.Key("librarySort3ds");
		writer.Int(settings.ui.librarySort[1]);
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

	void AddRecent(uint64_t& lastGame, std::vector<uint64_t>& recent, uint64_t titleId)
	{
		lastGame = titleId;
		std::erase(recent, titleId);
		recent.insert(recent.begin(), titleId);
		if (recent.size() > 4)
			recent.resize(4);
	}
}
