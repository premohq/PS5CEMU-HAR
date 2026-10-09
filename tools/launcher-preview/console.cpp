// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the launchers' previews on a PC (console.h): the script, the PNGs, and the console and
// the emulators in brief.

#include "console.h"

#include "app/boxart.h"
#include "app/compatibility.h"
#include "app/emulator.h"
#include "app/gameinfo.h"
#include "app/pack_updates.h"
#include "app/updates.h"
#include "app/usb_devices.h"
#include "azahar/library.h"
#include "frontend/settings.h"
#include "frontend/sound.h"
#include "ps5/kernel.h"
#include "ps5/log.h"
#include "ps5/notify.h"
#include "ps5/pad.h"
#include "ps5/privilege.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/syscall.h>
#include <unistd.h>

namespace preview
{
	std::string output;
	uint64_t timeUs = 1000000;
	uint32_t buttons = 0;
	std::vector<uint8_t> frame((size_t)kWidth * kHeight * 4);

	void WritePng(const std::string& path, int level)
	{
		std::vector<uint8_t> raw;
		raw.reserve((size_t)kHeight * (kWidth * 3 + 1));
		for (int y = 0; y < kHeight; y++)
		{
			raw.push_back(0);
			for (int x = 0; x < kWidth; x++)
			{
				const uint8_t* p = &frame[((size_t)y * kWidth + x) * 4];
				raw.insert(raw.end(), {p[2], p[1], p[0]});
			}
		}
		uLongf size = compressBound(raw.size());
		std::vector<uint8_t> packed(size);
		compress2(packed.data(), &size, raw.data(), raw.size(), level);
		packed.resize(size);
		std::ofstream out(path, std::ios::binary);
		auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
			const uint8_t length[4] = {(uint8_t)(data.size() >> 24), (uint8_t)(data.size() >> 16), (uint8_t)(data.size() >> 8), (uint8_t)data.size()};
			out.write((const char*)length, 4);
			std::vector<uint8_t> body(type, type + 4);
			body.insert(body.end(), data.begin(), data.end());
			out.write((const char*)body.data(), body.size());
			const uLong crc = crc32(0, body.data(), body.size());
			const uint8_t crcBytes[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
			out.write((const char*)crcBytes, 4);
		};
		out.write("\x89PNG\r\n\x1a\n", 8);
		chunk("IHDR", {0, 0, kWidth >> 8, kWidth & 255, 0, 0, kHeight >> 8, kHeight & 255, 8, 2, 0, 0, 0});
		chunk("IDAT", packed);
		chunk("IEND", {});
		if (level == 6)
			std::printf("%s\n", path.c_str());
	}

	namespace
	{
		// -- the script ------------------------------------------------------------------------------

		struct Step
		{
			enum Kind
			{
				Press,
				Hold,
				Wait,
				Shot,
				Record,
				Stop,
			} kind;
			uint32_t buttons = 0;
			int frames = 0;
			std::string name;
		};
		std::vector<Step> s_script;
		size_t s_step = 0;
		int s_stepFrame = 0;
		std::string s_recording; // the folder frames go to while recording
		int s_recorded = 0, s_recordTick = 0;

		uint32_t ButtonMask(const std::string& name)
		{
			static const std::map<std::string, uint32_t> kNames = {
				{"up", ps5pad::kUp}, {"down", ps5pad::kDown}, {"left", ps5pad::kLeft}, {"right", ps5pad::kRight},
				{"cross", ps5pad::kCross}, {"circle", ps5pad::kCircle}, {"square", ps5pad::kSquare}, {"triangle", ps5pad::kTriangle},
				{"l1", ps5pad::kL1}, {"r1", ps5pad::kR1}, {"l2", ps5pad::kL2}, {"r2", ps5pad::kR2}, {"l3", ps5pad::kL3},
				{"r3", ps5pad::kR3}, {"options", ps5pad::kOptions}, {"create", ps5pad::kCreate}, {"touchpad", ps5pad::kTouchPad},
				{"ls-up", kLeftUp}, {"ls-down", kLeftDown}, {"ls-left", kLeftLeft}, {"ls-right", kLeftRight},
				{"rs-up", kRightUp}, {"rs-down", kRightDown}, {"rs-left", kRightLeft}, {"rs-right", (uint32_t)kRightRight},
			};
			const auto it = kNames.find(name);
			if (it == kNames.end())
			{
				std::fprintf(stderr, "unknown button %s\n", name.c_str());
				std::exit(2);
			}
			return it->second;
		}

		void LoadScriptFile(const std::string& path)
		{
			std::ifstream file(path);
			if (!file)
			{
				std::fprintf(stderr, "cannot read %s\n", path.c_str());
				std::exit(2);
			}
			std::string line;
			while (std::getline(file, line))
			{
				line = line.substr(0, line.find('#'));
				std::istringstream words(line);
				std::string command;
				if (!(words >> command))
					continue;
				Step step{};
				if (command == "press")
				{
					// one press after another; buttons joined by + together
					for (std::string buttons; words >> buttons;)
					{
						Step press{Step::Press};
						for (size_t start = 0; start <= buttons.size();)
						{
							const size_t plus = std::min(buttons.find('+', start), buttons.size());
							press.buttons |= ButtonMask(buttons.substr(start, plus - start));
							start = plus + 1;
						}
						s_script.push_back(press);
					}
					continue;
				}
				else if (command == "hold")
				{
					std::string button;
					words >> button >> step.frames;
					step.kind = Step::Hold;
					step.buttons = ButtonMask(button);
				}
				else if (command == "wait")
				{
					step.kind = Step::Wait;
					words >> step.frames;
				}
				else if (command == "shot" || command == "record")
				{
					step.kind = command == "shot" ? Step::Shot : Step::Record;
					words >> step.name;
				}
				else if (command == "stop")
					step.kind = Step::Stop;
				else
				{
					std::fprintf(stderr, "unknown command %s\n", command.c_str());
					std::exit(2);
				}
				s_script.push_back(step);
			}
		}

		// After each frame: what the buttons are for the next, and the shots.
		void AdvanceScriptStep()
		{
			if (!s_recording.empty() && s_recordTick++ % 2 == 0)
				WritePng(fmt::format("{}/{:05d}.png", s_recording, s_recorded++), 1);
			for (;;)
			{
				if (s_step >= s_script.size())
					std::exit(0);
				const Step& step = s_script[s_step];
				if (step.kind == Step::Shot)
				{
					WritePng(output + "/" + step.name + ".png");
					s_step++;
					continue;
				}
				if (step.kind == Step::Record || step.kind == Step::Stop)
				{
					if (step.kind == Step::Record)
					{
						s_recording = output + "/" + step.name;
						std::filesystem::create_directories(s_recording);
						s_recorded = s_recordTick = 0;
					}
					else if (!s_recording.empty())
					{
						std::printf("%s: %d frames\n", s_recording.c_str(), s_recorded);
						s_recording.clear();
					}
					s_step++;
					continue;
				}
				const int down = step.kind == Step::Press ? 3 : step.kind == Step::Hold ? step.frames : 0;
				const int total = step.kind == Step::Wait ? step.frames : down + 3;
				if (s_stepFrame >= total)
				{
					s_step++;
					s_stepFrame = 0;
					continue;
				}
				buttons = s_stepFrame < down ? step.buttons : 0;
				s_stepFrame++;
				return;
			}
		}
	}

	void LoadScript(const std::string& path)
	{
		LoadScriptFile(path);
	}

	void AdvanceScript()
	{
		AdvanceScriptStep();
	}
}

using namespace preview;

// -- the console, in brief -------------------------------------------------------------------------

extern "C"
{
	int32_t sceKernelGetdents(int fd, char* buffer, int length)
	{
		// Linux's getdents64 records are laid out as its struct dirent, which the launcher reads them as
		return (int32_t)syscall(SYS_getdents64, fd, buffer, length);
	}
	uint64_t sceKernelGetProcessTime() { return timeUs; }
	int32_t sceKernelUsleep(uint32_t) { return 0; }
	int32_t sceSystemServiceHideSplashScreen() { return 0; }
	// the PS5's language (parameter 1): PREVIEW_SYSTEM_LANGUAGE's number, else English (United Kingdom)
	int32_t sceSystemServiceParamGetInt(int32_t paramId, int32_t* value)
	{
		const char* language = std::getenv("PREVIEW_SYSTEM_LANGUAGE");
		*value = paramId == 1 && language ? std::atoi(language) : 18;
		return 0;
	}
}

bool PS5_JitAvailable() { return true; }

// what the HEN granted: everything but JIT memory (the recompilers make their own), or with
// PREVIEW_NO_DATA no /data, for the Setup check
namespace ps5privilege
{
	const Result& Current()
	{
		static const Result result = [] {
			Result r;
			r.filesystem = !std::getenv("PREVIEW_NO_DATA");
			r.jailbroken = r.filesystem;
			r.executable = true;
			r.summary = r.filesystem ? "Jailbroken by the HEN: /data reachable, executable memory" : "Sandboxed: /data unreachable";
			return r;
		}();
		return result;
	}
}

namespace ps5log
{
	void Open(const char*) {}
	void Write(std::string_view line) { std::fprintf(stderr, "%.*s\n", (int)line.size(), line.data()); }
	const char* Path() { return ""; }
}

namespace ps5notify
{
	void Send(const std::string& message) { std::fprintf(stderr, "[notify] %s\n", message.c_str()); }
}

// the launcher's sound: what would play, in the log
namespace ps5sound
{
	bool s_menuSounds = true;
	void Start(const std::string& music, int volume, bool menuSounds)
	{
		s_menuSounds = menuSounds;
		std::fprintf(stderr, "[sound] music %s at %d%%, menu sounds %s\n", music.c_str(), volume, menuSounds ? "on" : "off");
	}
	void SetMusic(const std::string& music, int volume) { std::fprintf(stderr, "[sound] music %s at %d%%\n", music.c_str(), volume); }
	void SetMenuSounds(bool on) { s_menuSounds = on; }
	void Play(Effect effect)
	{
		static constexpr const char* kNames[] = {"move", "select", "back", "denied", "launch"};
		if (s_menuSounds)
			std::fprintf(stderr, "[sound] %s\n", kNames[(int)effect]);
	}
	void Stop() { std::fprintf(stderr, "[sound] stop\n"); }
}

namespace ps5pad
{
	bool Init() { return true; }
	void Shutdown() {}
	void Rescan() {}
	bool Read(int player, Data& out)
	{
		if (player != 0)
			return false;
		out = {};
		out.buttons = buttons & 0x00ffffffu;
		out.leftX = buttons & kLeftLeft ? 0 : buttons & kLeftRight ? 255 : 128;
		out.leftY = buttons & kLeftUp ? 0 : buttons & kLeftDown ? 255 : 128;
		out.rightX = buttons & kRightLeft ? 0 : buttons & kRightRight ? 255 : 128;
		out.rightY = buttons & kRightUp ? 0 : buttons & kRightDown ? 255 : 128;
		out.l2 = buttons & kL2 ? 255 : 0;
		out.r2 = buttons & kR2 ? 255 : 0;
		out.connected = 1;
		return true;
	}
	bool IsConnected(int player) { return player == 0; }
	void TouchResolution(int, float& width, float& height) { width = 1920, height = 1070; }
	void SetVibration(int, uint8_t, uint8_t) {}
	void SetVibrationEnabled(bool) {}
	void SetLightBar(int, uint8_t, uint8_t, uint8_t) {}
	Filtered FilterShortcuts(int, uint32_t buttons) { return {buttons, false}; }
	Shortcut TakeShortcut() { return Shortcut::None; }
}

// -- GameTDB's box art, in brief: what the preview's own folder has ---------------------------------
// (build/preview/boxart/<wiiu|3ds>/<ID>.tga, put there by hand: GameTDB's covers are not ours to
// keep in the repository)

namespace ps5boxart
{
	std::string Path(System system, const std::string& id)
	{
		const std::string path = output + "/boxart/" + (system == System::WiiU ? "wiiu/" : "3ds/") + id + ".tga";
		return !id.empty() && std::ifstream(path).good() ? path : std::string();
	}
	void Fetch(System, const std::vector<std::string>&) {}
	uint32_t Arrivals() { return 0; }
	void SetEnabled(bool) {}
	void Stop() {}
	int Answered() { return -1; } // nothing asked of GameTDB
	bool ImageSize(const std::string& path, int& width, int& height)
	{
		unsigned char header[18];
		std::ifstream in(path, std::ios::binary);
		if (!in.read((char*)header, sizeof(header)))
			return false;
		width = header[12] | header[13] << 8;
		height = header[14] | header[15] << 8;
		return width > 0 && height > 0;
	}
}

// -- the community graphic packs' update, in brief: what is installed, and a check that finds
// nothing newer

namespace ps5packs
{
	std::string InstalledVersion() { return "Github987"; }
	bool Newer(const std::string&, const std::string&) { return false; }
	void Start() {}
	Status GetStatus() { return {}; }
	void Stop() {}
}

// -- Cemu's emulated USB devices, in brief: switches only

namespace ps5usb
{
	namespace
	{
		bool s_usbOn[3] = {true, false, false};
	}
	const char* Name(Device device)
	{
		return device == Device::Skylanders ? "Skylanders Portal of Power" : device == Device::Infinity ? "Disney Infinity Base" : "LEGO Dimensions Toypad";
	}
	std::string Folder(Device device)
	{
		return std::string("/data/ps5cemu/figures/") + (device == Device::Skylanders ? "skylanders" : device == Device::Infinity ? "infinity" : "dimensions");
	}
	bool Enabled(Device device) { return s_usbOn[(int)device]; }
	void SetEnabled(Device device, bool on) { s_usbOn[(int)device] = on; }
	bool Plugged(Device device) { return s_usbOn[(int)device]; }
	std::vector<std::string> Figures(Device) { return {}; }
	std::vector<Slot> Slots(Device) { return {}; }
	void RequestFigure(Device, size_t, const std::string&) {}
	void Refresh() {}
	void GameStarted() {}
	void ServiceRequests() {}
	std::string LastError() { return {}; }
}

// -- the app's own update, in brief: with PREVIEW_UPDATE set, a newer release is found as the app
// starts, and installing it takes a few seconds

namespace ps5update
{
	namespace
	{
		Status s_update;
		bool s_updateDismissed = false;
		uint64_t s_installAt = 0;
	}

	void Start()
	{
		s_update.state = Status::State::Available;
		s_update.latest = "v3.0.1";
	}
	void Check()
	{
		Start();
		s_updateDismissed = false;
	}
	void Install()
	{
		if (s_update.state != Status::State::Available)
			return;
		s_update.installing = true;
		s_update.state = Status::State::Downloading;
		s_installAt = timeUs;
	}
	Status GetStatus()
	{
		if (s_update.installing && s_update.state != Status::State::Ready)
		{
			const double t = (timeUs - s_installAt) / 1e6;
			s_update.total = 49ull << 20;
			s_update.received = (uint64_t)(std::min(1.0, t / 4.0) * (double)s_update.total);
			s_update.state = t < 4 ? Status::State::Downloading : t < 5 ? Status::State::Verifying : t < 6 ? Status::State::Installing : Status::State::Ready;
		}
		return s_update;
	}
	bool Prompting()
	{
		GetStatus();
		return s_update.installing || (s_update.state == Status::State::Available && !s_updateDismissed);
	}
	void Dismiss()
	{
		if (s_update.state == Status::State::Ready || (s_update.installing && s_update.state != Status::State::Failed))
			return;
		s_updateDismissed = true;
		s_update.installing = false;
	}
	void Restart() {}
	void Stop() {}
}

// -- Cemu, in brief: sample games, packs and controllers ------------------------------------------

namespace ps5emu
{
	namespace
	{
		constexpr uint64_t kBreathOfTheWild = 0x00050000101C9400;

		struct SamplePreset
		{
			std::string category, name, needs; // needs: the aspect ratio it is listed for
		};
		struct SamplePack
		{
			GraphicPackInfo info;
			std::vector<SamplePreset> presets;
			std::map<std::string, std::string> active;
		};

		std::vector<SamplePack> MakePacks()
		{
			std::vector<SamplePack> packs;
			auto add = [&](std::string folder, std::string name, std::string description, bool enabled, std::vector<SamplePreset> presets = {}) {
				SamplePack pack;
				pack.info = {name, folder, description, enabled, {}};
				pack.presets = std::move(presets);
				for (const auto& preset : pack.presets)
					if (!pack.active.count(preset.category) && (preset.needs.empty() || preset.needs == "16:9 (Default)"))
						pack.active[preset.category] = preset.name;
				packs.push_back(std::move(pack));
			};
			std::vector<SamplePreset> graphics;
			for (const char* ratio : {"16:9 (Default)", "16:10", "21:9", "32:9", "4:3"})
				graphics.push_back({"Aspect Ratio", ratio, ""});
			for (const char* r : {"320x180", "640x360", "960x540", "1280x720 (HD, Default)", "1600x900 (HD+)", "1920x1080 (Full HD)",
					 "2560x1440 (2K)", "3200x1800", "3840x2160 (4K)", "5120x2880 (5K)", "7680x4320 (8K)"})
				graphics.push_back({"Resolution", r, "16:9 (Default)"});
			for (const char* r : {"1280x800", "1440x900", "1680x1050", "1920x1200", "2560x1600"})
				graphics.push_back({"Resolution", r, "16:10"});
			for (const char* r : {"1720x720", "2560x1080", "3440x1440", "5120x2160"})
				graphics.push_back({"Resolution", r, "21:9"});
			for (const char* r : {"3840x1080", "5120x1440"})
				graphics.push_back({"Resolution", r, "32:9"});
			for (const char* r : {"800x600", "1024x768", "1600x1200"})
				graphics.push_back({"Resolution", r, "4:3"});
			for (const char* s : {"Low (0.5x)", "Medium (100%, Default)", "High (200%)", "Very High (300%)", "Ultra (400%)"})
				graphics.push_back({"Shadows", s, ""});
			for (const char* a : {"Disabled", "Normal FXAA (Default)", "Enhanced FXAA", "Ultra FXAA"})
				graphics.push_back({"Anti-Aliasing", a, ""});
			add("", "Graphics", "Allows you to change the game resolution, shadow resolution and anti-aliasing.\nMade by Kiri, Skalfate, rajkosto and NAVras.", false, graphics);
			add("", "Enhancements", "Adjusts the lighting, the colours and the sharpness of the picture.\n", false,
				{{"Lighting", "Original (Default)", ""}, {"Lighting", "High", ""}, {"Contrast", "Original (Default)", ""}, {"Contrast", "Slightly higher", ""}, {"Saturation", "Original (Default)", ""}, {"Saturation", "Vibrant", ""}});
			add("Mods", "FPS++", "Unlocks the frame rate. Needs a powerful machine.\nMade by Epigramx, Xalphenos, rajkosto, Crementif and Exzap.", true,
				{{"Mode", "Advanced Settings", ""}, {"Mode", "Simple", ""}, {"Framerate Limit", "30FPS (ideal for 240/120/60Hz displays)", ""}, {"Framerate Limit", "60FPS (ideal for 240/120/60Hz displays)", ""}});
			add("Mods", "Extended Memory", "More memory for the game's heap, for mods that need it.\n", false);
			add("Mods", "Remove Fog", "Removes the fog in the distance.\n", false);
			add("Mods", "Draw Distance", "Draws more of the world in the distance.\n", false, {{"Distance", "Normal (Default)", ""}, {"Distance", "High", ""}, {"Distance", "Ultra", ""}});
			add("Mods", "Day Length", "Makes days in Hyrule longer.\n", false, {{"", "x2", ""}, {"", "x4", ""}});
			add("Workarounds", "AMD Shader Crash", "Fixes a crash on AMD graphics.\n", true);
			add("Workarounds", "Grass Workaround", "Fixes flickering grass.\n", false);
			add("Cheats", "Infinite Stamina", "Your stamina never runs out.\n", false);
			add("Cheats", "Infinite Hearts", "Your hearts never run out.\n", false);
			return packs;
		}

		std::vector<SamplePack>& Packs(uint64_t titleId)
		{
			static std::map<uint64_t, std::vector<SamplePack>> packs;
			auto& list = packs[titleId];
			if (list.empty() && titleId == kBreathOfTheWild)
				list = MakePacks();
			return list;
		}

		void FillChoices(SamplePack& pack)
		{
			pack.info.choices.clear();
			const std::string ratio = pack.active.count("Aspect Ratio") ? pack.active["Aspect Ratio"] : "";
			for (const auto& preset : pack.presets)
			{
				if (!preset.needs.empty() && preset.needs != ratio)
					continue;
				auto it = std::find_if(pack.info.choices.begin(), pack.info.choices.end(), [&](auto& c) { return c.category == preset.category; });
				if (it == pack.info.choices.end())
				{
					pack.info.choices.push_back({preset.category, {}, 0});
					it = pack.info.choices.end() - 1;
				}
				if (pack.active[preset.category] == preset.name)
					it->active = (int)it->presets.size();
				it->presets.push_back(preset.name);
			}
		}

		std::array<PlayerControls, 4> s_players = [] {
			std::array<PlayerControls, 4> players{};
			players[0] = {EmulatedType::GamePad, true, true, true, 50, 25, 25};
			for (int i = 1; i < 4; i++)
				players[i] = {EmulatedType::Pro, false, false, false, 0, 25, 25};
			return players;
		}();
		std::array<std::vector<ButtonMapping>, 4> s_mappings;

		std::vector<ButtonMapping>& Mappings(int player)
		{
			auto& list = s_mappings[player];
			if (list.empty())
				list = {{"A", "Circle"}, {"B", "Cross"}, {"X", "Triangle"}, {"Y", "Square"}, {"L", "L1"}, {"R", "R1"}, {"ZL", "L2"},
					{"ZR", "R2"}, {"+ (Start)", "Options"}, {"- (Select)", "Create"}, {"D-pad up", "D-pad up"}, {"D-pad down", "D-pad down"},
					{"D-pad left", "D-pad left"}, {"D-pad right", "D-pad right"}, {"Left stick up", "Left stick up"},
					{"Left stick down", "Left stick down"}, {"Left stick left", "Left stick left"}, {"Left stick right", "Left stick right"},
					{"Left stick click", "L3"}, {"Right stick up", "Right stick up"}, {"Right stick down", "Right stick down"},
					{"Right stick left", "Right stick left"}, {"Right stick right", "Right stick right"}, {"Right stick click", "R3"},
					{"Home", ""}, {"Blow into the mic", ""}, {"Show the GamePad's screen", ""}};
			return list;
		}

		InstallStatus s_install;
	}

	bool Scanning() { return false; }
	void ApplyOptions(const Options&) {}
	void Rescan() {}
	// the games' icons, when build/preview/covers has them (copied there by hand, as the box art)
	std::string CoverPath(uint64_t titleId)
	{
		const std::string path = fmt::format("{}/covers/{:016x}.tga", output, titleId);
		return std::filesystem::exists(path) ? path : std::string();
	}
	// and their boot screens, the same way in build/preview/covers/boot
	std::string BootScreenPath(uint64_t titleId)
	{
		const std::string path = fmt::format("{}/covers/boot/{:016x}.tga", output, titleId);
		return std::filesystem::exists(path) ? path : std::string();
	}

	std::vector<Game> ListGames()
	{
		std::vector<Game> games;
		auto add = [&](uint64_t id, const char* name, uint16_t version, bool update, uint32_t dlc, const char* format, const char* box = "") {
			games.push_back({id, name, std::string("/data/ps5cemu/games/") + name + ".wua", version, update, dlc, format});
			games.back().gameId = box;
		};
		add(0x0005000010110E00, "Bayonetta 2", 0, false, 0, "WUA", "AQUE01");
		add(0x0005000010138300, "Donkey Kong Country: Tropical Freeze", 17, true, 0, "WUX", "ARKE01");
		add(0x0005000010180700, "Captain Toad: Treasure Tracker", 0, false, 1, "FOLDER", "AKBE01");
		add(0x0005000010145D00, "Super Mario 3D World", 0, false, 0, "WUA", "ARDE01");
		add(0x000500001010EC00, "Mario Kart 8", 64, true, 2, "WUA", "AMKE01");
		add(0x0005000010101D00, "New Super Mario Bros. U + New Super Luigi U", 0, false, 1, "WUD", "ATWE01");
		add(0x0005000010176900, "Splatoon", 288, true, 0, "WUA", "AGME01");
		add(kBreathOfTheWild, "The Legend of Zelda: Breath of the Wild", 208, true, 1, "WUA", "ALZE01");
		add(0x0005000010143500, "The Legend of Zelda: The Wind Waker HD", 0, false, 0, "FOLDER", "BCZE01");
		add(0x000500001014B800, "Xenoblade Chronicles X", 33, true, 1, "WUX", "AX5E01");
		add(0x0005000010172600, "Pikmin 3", 0, false, 0, "NUS", "AC3E01");
		std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) { return a.name < b.name; });
		return games;
	}

	std::vector<GraphicPackInfo> ListGraphicPacks(uint64_t titleId)
	{
		std::vector<GraphicPackInfo> result;
		for (auto& pack : Packs(titleId))
		{
			FillChoices(pack);
			result.push_back(pack.info);
		}
		return result;
	}

	int EnabledGraphicPackCount(uint64_t titleId)
	{
		int count = 0;
		for (auto& pack : Packs(titleId))
			count += pack.info.enabled;
		return count;
	}

	bool ToggleGraphicPack(uint64_t titleId, size_t index)
	{
		auto& packs = Packs(titleId);
		return index < packs.size() && (packs[index].info.enabled = !packs[index].info.enabled);
	}

	void SetGraphicPackPreset(uint64_t titleId, size_t index, const std::string& category, const std::string& preset)
	{
		auto& packs = Packs(titleId);
		if (index >= packs.size())
			return;
		auto& pack = packs[index];
		pack.active[category] = preset;
		pack.info.enabled = true;
		if (category == "Aspect Ratio")
		{
			// as Cemu's ValidatePresetSelections: the first resolution the new ratio has
			pack.active.erase("Resolution");
			for (const auto& p : pack.presets)
				if (p.category == "Resolution" && p.needs == preset)
				{
					pack.active["Resolution"] = p.name;
					break;
				}
		}
	}

	PlayerControls GetPlayerControls(int player) { return s_players[player]; }
	void SetEmulatedType(int player, EmulatedType type)
	{
		s_players[player].type = type;
		s_players[player].hasMotion = type == EmulatedType::GamePad || type == EmulatedType::Wiimote || type == EmulatedType::Nunchuk;
		s_mappings[player].clear();
	}
	void SetMotion(int player, bool enabled) { s_players[player].motion = enabled; }
	void SetRumble(int player, int percent) { s_players[player].rumble = percent; }
	void SetDeadzones(int player, int left, int right) { s_players[player].leftDeadzone = left, s_players[player].rightDeadzone = right; }
	void ResetControls(int player) { s_mappings[player].clear(); }
	std::vector<ButtonMapping> ListMappings(int player) { return Mappings(player); }
	void SetMapping(int player, size_t index, PadInput input)
	{
		static const char* kNames[] = {"", "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3", "Create", "Options",
			"D-pad up", "D-pad down", "D-pad left", "D-pad right", "Left stick up", "Left stick down", "Left stick left", "Left stick right",
			"Right stick up", "Right stick down", "Right stick left", "Right stick right"};
		if (index < Mappings(player).size())
			Mappings(player)[index].input = kNames[(int)input];
	}
	void ClearMapping(int player, size_t index)
	{
		if (index < Mappings(player).size())
			Mappings(player)[index].input.clear();
	}

	InstallCandidate InspectInstall(const std::string& folder)
	{
		InstallCandidate candidate;
		std::ifstream meta(folder + "/meta/meta.xml");
		if (!meta)
		{
			candidate.note = "There is no title here: a game, update or DLC to install is a folder with code, content and meta.";
			return candidate;
		}
		const std::string name = folder.substr(folder.find_last_of('/') + 1);
		candidate.name = "The Legend of Zelda: Breath of the Wild";
		candidate.titleId = name.find("DLC") != std::string::npos ? 0x0005000C101C9400 : 0x0005000E101C9400;
		candidate.kind = name.find("DLC") != std::string::npos ? InstallCandidate::Kind::Dlc : InstallCandidate::Kind::Update;
		candidate.version = candidate.kind == InstallCandidate::Kind::Dlc ? 80 : 208;
		candidate.installedVersion = candidate.kind == InstallCandidate::Kind::Update ? 192 : -1;
		return candidate;
	}

	bool StartInstall(const std::string&, std::string&)
	{
		s_install = {InstallStatus::State::Running, 0, 3200000000ull, ""};
		return true;
	}

	InstallStatus GetInstallStatus()
	{
		if (s_install.state == InstallStatus::State::Running)
		{
			s_install.copied += 40000000ull;
			if (s_install.copied >= s_install.total)
				s_install.state = InstallStatus::State::Done;
		}
		return s_install;
	}

	void CancelInstall() { s_install.state = InstallStatus::State::Cancelled; }
	void ReloadGraphicPacks() {}
}
