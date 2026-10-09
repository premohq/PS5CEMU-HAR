// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: melonDS's core on the PS5 (melonds.h), built in melonDS's tree against its core
// (tools/build-melonds.sh) and linked into the app with Cemu and Azahar.
//
//  - a DS in DS mode, started straight into the game (direct boot) with melonDS's own BIOS and
//    firmware (FreeBIOS and one it makes, with the PS5's user name and the launcher's language), or
//    a DS's own dumps from kRoot/bios;
//  - the CPUs on melonDS's JIT, its code in the console's executable direct memory (patches/melonds),
//    or on its interpreter where there is none; the 3D on its software renderer, on a thread of its own;
//  - the emulation, and the screens on VideoOut (screens.h), on a thread of their own: a DS frame for
//    each frame shown, VideoOut's flips pacing it; the game's loop on the main thread reads the
//    DualSense (input.cpp) and the port's shortcuts (ps5/pad.h), and applies what the in-game menu
//    (app/ingame3ds.h, drawn in the screens' frames) changes, as Azahar's does (port/azahar/core.cpp).

#include "melonds.h"
#include "frontend.h"
#include "library.h"
#include "screens.h"
#include "../app/boxart.h"
#include "../app/gameinfo.h"
#include "../app/ingame3ds.h"
#include "../azahar/controls.h"
#include "../ps5/display.h"
#include "../ps5/kernel.h"
#include "../ps5/log.h"
#include "../ps5/notify.h"
#include "../ps5/pad.h"
#include "../ps5/privilege.h"

#include "ARCodeFile.h"
#include "Args.h"
#include "GPU3D_Soft.h"
#include "NDS.h"
#include "NDSCart.h"
#include "Platform.h"
#include "SPI.h"
#include "SPI_Firmware.h"
#include "Savestate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <variant>
#include <vector>

extern "C" int32_t sceUserServiceGetUserName(int32_t userId, char* name, size_t size);

namespace fs = std::filesystem;

namespace ps5melonds
{
	namespace
	{
		using melonDS::u8;
		using melonDS::u32;

		// a DS's frame rate: its clock over the frame's cycles
		constexpr double kDsFrameRate = 33513982.0 / (2 * 6 * 263 * 355);
		constexpr int kStateSlots = ps5ingame3ds::kStateSlots;

		melonDS::NDS* s_nds = nullptr;
		std::thread s_emulation;
		std::atomic_bool s_stop = false;
		std::atomic_bool s_running = false;
		std::atomic<int> s_stopReason{-1}; // melonDS stopped the console itself (Stopped)
		bool s_coreTouched = false;			// LaunchGame took VideoOut
		ps5settings::N3ds s_settings;		// the game's loop's: as the menu leaves them
		ps5settings::N3ds s_launched;	// what the game started with, or what the menu last saved
		std::string s_name;
		std::string s_stem;		  // the game's file's name without its extension: its save's, states' and cheats'
		std::string s_folder;	  // the folder the game's file is in
		uint64_t s_titleId = 0;
		bool s_jit = false;

		// What the emulation reads before each frame; the game's loop changes it
		struct View
		{
			int layout = 2;
			bool swapped = false;
			int filter = 0;
			int speedLimit = 100; // percent of the DS's; 0: none
			bool lidClosed = false;
		};
		std::mutex s_viewMutex;
		View s_view;
		std::atomic<bool> s_bottomShown{true};

		// the game's loop's requests, for the emulation to make between two frames: a save state saved
		// to slot n (n) or loaded from it (-n), cheats turned on or off (by their index)
		std::atomic<int> s_stateRequest{0};
		std::mutex s_cheatMutex;
		std::vector<int> s_cheatToggles;

		// the emulation's: the game's cheats
		std::unique_ptr<melonDS::ARCodeFile> s_cheats;

		// the game's save, as it last changed, for the game's loop to write
		std::mutex s_saveMutex;
		std::vector<u8> s_save;
		bool s_saveDirty = false;
		uint64_t s_saveChangedAt = 0; // sceKernelGetProcessTime

		// the emulation's frames, for the overlay's numbers
		std::atomic<uint32_t> s_framesEmulated{0}, s_framesShown{0};

		std::string Folder(const char* name)
		{
			return std::string(kRoot) + "/" + name;
		}

		std::string SavePath()
		{
			return Folder("saves") + "/" + s_stem + ".sav";
		}

		std::string StatePath(int slot)
		{
			return fmt::format("{}/{}.ml{}", Folder("states"), s_stem, slot);
		}

		// melonDS's own place for the game's cheats, unless a .mch is beside the game
		std::string CheatPath()
		{
			const std::string own = Folder("cheats") + "/" + s_stem + ".mch";
			std::error_code ec;
			const std::string beside = s_folder + "/" + s_stem + ".mch";
			if (!fs::exists(own, ec) && fs::exists(beside, ec))
				return beside;
			return own;
		}

		std::vector<u8> ReadFile(const std::string& path)
		{
			std::ifstream file(path, std::ios::binary);
			return std::vector<u8>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		}

		bool WriteFile(const std::string& path, const u8* data, size_t length)
		{
			const std::string temporary = path + ".tmp";
			{
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				out.write((const char*)data, (std::streamsize)length);
				if (!out)
					return false;
			}
			std::error_code ec;
			fs::rename(temporary, path, ec);
			return !ec;
		}

		// -- the console's parts -----------------------------------------------------------------

		// A DS's own BIOS from kRoot/bios, when both are there at their sizes
		template<size_t N>
		std::unique_ptr<std::array<u8, N>> ReadBios(const char* name)
		{
			const std::vector<u8> data = ReadFile(Folder("bios") + "/" + name);
			if (data.size() != N)
				return nullptr;
			auto bios = std::make_unique<std::array<u8, N>>();
			std::copy(data.begin(), data.end(), bios->begin());
			return bios;
		}

		// UTF-8 into the firmware's UTF-16 name, 10 characters at most
		size_t Utf16(const std::string& text, char16_t* out, size_t most)
		{
			size_t count = 0;
			for (size_t i = 0; i < text.size() && count < most;)
			{
				const unsigned char c = text[i];
				uint32_t code = c;
				int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
				if (extra)
					code = c & (0x3F >> extra);
				i++;
				for (; extra > 0 && i < text.size(); extra--, i++)
					code = code << 6 | (text[i] & 0x3F);
				out[count++] = code < 0x10000 ? (char16_t)code : u'?';
			}
			return count;
		}

		// The firmware a DS game sees: a DS's own (firmware.bin) as it is, with the launcher's language;
		// else one melonDS makes, with the PS5 user's name as the DS's
		melonDS::Firmware MakeFirmware(const ps5settings::N3ds& settings, bool own, std::string& about)
		{
			melonDS::Firmware firmware(0);
			bool loaded = false;
			if (own)
				if (melonDS::Platform::FileHandle* file = melonDS::Platform::OpenFile(Folder("bios") + "/firmware.bin", melonDS::Platform::Read))
				{
					melonDS::Firmware dumped(file);
					melonDS::Platform::CloseFile(file);
					if (dumped.Buffer())
					{
						firmware = std::move(dumped);
						loaded = true;
					}
				}
			auto& user = firmware.GetEffectiveUserData();
			if (!loaded)
			{
				char name[32] = {};
				if (sceUserServiceGetUserName(ps5pad::UserId(0), name, sizeof(name) - 1) != 0 || !name[0])
					std::snprintf(name, sizeof(name), "PS5");
				std::fill(std::begin(user.Nickname), std::end(user.Nickname), u'\0');
				user.NameLength = (melonDS::u16)Utf16(name, user.Nickname, 10);
			}
			// the 3DS's languages are the DS's up to Chinese (6); Korean and the rest have none on a DS
			if (settings.language >= 0)
			{
				const int language = settings.language <= 6 ? settings.language : 1;
				user.Settings &= ~melonDS::Firmware::Language::Reserved;
				user.Settings |= (melonDS::u16)language;
			}
			firmware.UpdateChecksums();
			about = loaded ? "the DS's own firmware" : "melonDS's firmware";
			return firmware;
		}

		// The game's save: kRoot/saves, or the first time, a .sav beside the game (taken, not moved)
		std::vector<u8> ReadSave()
		{
			std::error_code ec;
			if (fs::exists(SavePath(), ec))
				return ReadFile(SavePath());
			const std::string beside = s_folder + "/" + s_stem + ".sav";
			if (fs::exists(beside, ec))
			{
				ps5log::Line("[ds] the save beside the game is taken: {}", beside);
				return ReadFile(beside);
			}
			return {};
		}

		void FlushSave(bool now)
		{
			std::vector<u8> data;
			{
				std::lock_guard lock(s_saveMutex);
				// once the game has stopped writing for a second (a save is written in many pieces)
				if (!s_saveDirty || (!now && sceKernelGetProcessTime() - s_saveChangedAt < 1000000))
					return;
				data = s_save;
				s_saveDirty = false;
			}
			if (!WriteFile(SavePath(), data.data(), data.size()))
				ps5log::Line("[ds] the save could not be written: {}", SavePath());
		}

		// -- the menu ------------------------------------------------------------------------------

		// The menu's save state slots: when each was saved
		void ShowStateSlots()
		{
			std::vector<std::string> times(kStateSlots);
			for (int slot = 1; slot <= kStateSlots; slot++)
			{
				struct stat info{};
				if (stat(StatePath(slot).c_str(), &info) != 0)
					continue;
				const std::time_t time = info.st_mtime;
				char text[32] = "saved";
				if (const std::tm* local = std::localtime(&time))
					std::strftime(text, sizeof(text), "%b %d, %H:%M", local);
				times[slot - 1] = text;
			}
			ps5ingame3ds::SetStateSlots(times);
		}

		// The game's cheats as the menu lists them, in the order melonDS keeps them
		std::vector<melonDS::ARCode*> CheatList()
		{
			std::vector<melonDS::ARCode*> codes;
			if (!s_cheats || s_cheats->Error)
				return codes;
			for (auto& item : s_cheats->RootCat.Children)
			{
				if (auto* category = std::get_if<melonDS::ARCodeCat>(&item))
				{
					for (auto& child : category->Children)
						if (auto* code = std::get_if<melonDS::ARCode>(&child))
							codes.push_back(code);
				}
				else if (auto* code = std::get_if<melonDS::ARCode>(&item))
					codes.push_back(code);
			}
			return codes;
		}

		void ShowCheats()
		{
			std::vector<std::pair<std::string, bool>> cheats;
			for (const melonDS::ARCode* code : CheatList())
				cheats.emplace_back(code->Name, code->Enabled);
			ps5ingame3ds::SetExtras({}, cheats);
		}

		ps5ingame3ds::Settings MenuSettings()
		{
			View view;
			{
				std::lock_guard lock(s_viewMutex);
				view = s_view;
			}
			ps5ingame3ds::Settings menu;
			menu.layout = s_settings.layout;
			menu.swapScreens = view.swapped;
			menu.resolution = s_settings.resolution;
			menu.textureFilter = s_settings.textureFilter;
			menu.volume = s_settings.volume;
			menu.performance = s_settings.performance;
			menu.cpuClock = s_settings.cpuClock;
			menu.speedLimit = view.speedLimit;
			menu.motion = s_settings.motion;
			menu.deadzone = s_settings.deadzone;
			menu.aOnCircle = ps5azahar::MappedInput(s_settings, ps5azahar::Button::A) == ps5emu::PadInput::Circle;
			menu.border = s_settings.border;
			menu.screenFilter = s_settings.dsFilter;
			menu.lidClosed = view.lidClosed;
			return menu;
		}

		void ShowInMenu()
		{
			ps5ingame3ds::Start(s_name, s_titleId, MenuSettings(), ps5ingame3ds::Console::Nds);
		}

		// Where the screens are, for the border, the touch screen and the emulation
		void PublishScreens()
		{
			View view;
			{
				std::lock_guard lock(s_viewMutex);
				view = s_view;
			}
			const float width = (float)screens::Width(), height = (float)screens::Height();
			screens::Rect top, bottom;
			screens::Place(view.layout, view.swapped, width, height, top, bottom);
			std::vector<ps5ingame3ds::ScreenRect> shown;
			for (const screens::Rect& r : {top, bottom})
				if (r.Shown())
					shown.push_back({r.left, r.top, r.right, r.bottom});
			ps5ingame3ds::SetScreens(shown, width, height);
			s_bottomShown = bottom.Shown();
		}

		// What the menu changed, while the game runs; kept for the next games in the DS side's settings,
		// or the game's own when it has some
		void ApplyMenu(const ps5ingame3ds::Settings& menu)
		{
			using ps5azahar::Button;
			using ps5emu::PadInput;
			const bool aOnCircle = ps5azahar::MappedInput(s_settings, Button::A) == PadInput::Circle;
			const bool controls = menu.deadzone != s_settings.deadzone || menu.aOnCircle != aOnCircle;
			s_settings.layout = menu.layout;
			s_settings.volume = menu.volume;
			s_settings.performance = menu.performance;
			s_settings.deadzone = menu.deadzone;
			s_settings.dsFilter = menu.screenFilter;
			if (menu.border != s_settings.border)
			{
				s_settings.border = menu.border;
				ps5ingame3ds::LoadBorder(menu.border);
			}
			if (menu.aOnCircle != aOnCircle)
			{
				// A, B, X and Y: on Circle, Cross, Triangle and Square, or on Cross, Circle, Square and Triangle
				ps5azahar::SetMapping(s_settings, (size_t)Button::A, menu.aOnCircle ? PadInput::Circle : PadInput::Cross);
				ps5azahar::SetMapping(s_settings, (size_t)Button::B, menu.aOnCircle ? PadInput::Cross : PadInput::Circle);
				ps5azahar::SetMapping(s_settings, (size_t)Button::X, menu.aOnCircle ? PadInput::Triangle : PadInput::Square);
				ps5azahar::SetMapping(s_settings, (size_t)Button::Y, menu.aOnCircle ? PadInput::Square : PadInput::Triangle);
			}
			{
				std::lock_guard lock(s_viewMutex);
				s_view.layout = menu.layout;
				s_view.swapped = menu.swapScreens;
				s_view.filter = menu.screenFilter;
				s_view.lidClosed = menu.lidClosed;
				if (menu.speedLimit != s_view.speedLimit)
					ps5log::Line("[ds] speed limit {}", menu.speedLimit ? fmt::format("{}%", menu.speedLimit) : "none");
				s_view.speedLimit = menu.speedLimit;
			}
			PublishScreens();
			sound::SetVolume(menu.volume);
			if (controls)
				input::Configure(s_settings);

			// into the game's own settings when it has some, else the side's (docs/UI-REDESIGN.md, 6.5)
			ps5settings::Launcher before, after;
			before.nds = s_launched;
			after.nds = s_settings;
			ps5settings::SaveChanges("ds", s_titleId, before, after);
			s_launched = s_settings;
		}

		// -- the emulation ---------------------------------------------------------------------------

		// A save state saved or loaded, between two frames, and what came of it in the menu
		void MakeState(melonDS::NDS& nds, int request)
		{
			const bool load = request < 0;
			const int slot = std::abs(request);
			const std::string path = StatePath(slot);
			std::string message;
			if (!load)
			{
				melonDS::Savestate state;
				if (!state.Error)
					nds.DoSavestate(&state);
				message = !state.Error && WriteFile(path, (const u8*)state.Buffer(), state.Length()) ? fmt::format("Saved slot {}", slot) :
																										   "It did not work";
			}
			else
			{
				std::vector<u8> data = ReadFile(path);
				// what the game was, to go back to when the state cannot be read
				auto backup = std::make_unique<melonDS::Savestate>(melonDS::Savestate::DEFAULT_SIZE);
				if (data.empty())
					message = "It did not work";
				else if (backup->Error || !nds.DoSavestate(backup.get()) || backup->Error)
					message = "It did not work";
				else
				{
					melonDS::Savestate state(data.data(), (u32)data.size(), false);
					if (!state.Error && nds.DoSavestate(&state) && !state.Error)
						message = fmt::format("Loaded slot {}", slot);
					else
					{
						backup->Rewind(false);
						nds.DoSavestate(backup.get());
						message = "Made by another version";
					}
				}
			}
			ps5log::Line("[ds] {} slot {}: {}", load ? "load from" : "save to", slot, message);
			ps5ingame3ds::SetStateMessage(message);
			ShowStateSlots();
		}

		// Cheats turned on or off, kept in the game's cheat file as melonDS's own window keeps them
		void ToggleCheats(melonDS::NDS& nds)
		{
			std::vector<int> toggles;
			{
				std::lock_guard lock(s_cheatMutex);
				toggles.swap(s_cheatToggles);
			}
			if (toggles.empty())
				return;
			const std::vector<melonDS::ARCode*> codes = CheatList();
			for (const int index : toggles)
				if (index >= 0 && index < (int)codes.size())
				{
					codes[index]->Enabled = !codes[index]->Enabled;
					ps5log::Line("[ds] cheat {}: {}", codes[index]->Name, codes[index]->Enabled ? "on" : "off");
				}
			if (s_cheats)
			{
				s_cheats->Save();
				nds.AREngine.Cheats = s_cheats->GetCodes();
			}
			ShowCheats();
		}

		void Emulate()
		{
			melonDS::NDS& nds = *s_nds;
			const double refresh = ps5display::OutputRefresh() ? ps5display::OutputRefresh() / 1000.0 : 59.94;
			const uint64_t period = (uint64_t)(1e6 / refresh);
			double owed = 0;		   // DS frames to run at a speed above 100%
			uint64_t nextFrame = 0;	   // when the next frame is due (sceKernelGetProcessTime), should VideoOut not pace
			while (!s_stop)
			{
				if (const int request = s_stateRequest.exchange(0))
					MakeState(nds, request);
				ToggleCheats(nds);
				View view;
				{
					std::lock_guard lock(s_viewMutex);
					view = s_view;
				}
				if (nds.IsLidClosed() != view.lidClosed)
					nds.SetLidClosed(view.lidClosed);
				screens::Frame frame;
				screens::Place(view.layout, view.swapped, (float)screens::Width(), (float)screens::Height(), frame.topRect, frame.bottomRect);
				frame.filter = view.filter;
				const bool held = ps5ingame3ds::MenuOpen() || !nds.IsRunning();
				frame.cursor = input::Cursor(frame.cursorX, frame.cursorY) && !held;
				if (!held)
				{
					nds.SetKeyMask(input::KeyMask());
					int x, y;
					if (input::Touch(x, y))
						nds.TouchScreen((melonDS::u16)x, (melonDS::u16)y);
					else
						nds.ReleaseScreen();
					// the DS frames this frame shows: one, more at a higher speed limit, as many as fit
					// in 12 ms with none
					int frames = 1;
					if (view.speedLimit > 100)
					{
						owed += view.speedLimit / 100.0;
						frames = std::max(1, (int)owed);
						owed -= frames;
					}
					const uint64_t start = sceKernelGetProcessTime();
					for (int i = 0; !s_stop && (i < frames || (view.speedLimit == 0 && sceKernelGetProcessTime() - start < 12000)); i++)
					{
						nds.RunFrame();
						s_framesEmulated++;
					}
					sound::Pace(nds, refresh, view.speedLimit);
					frame.top = nds.GPU.Framebuffer[nds.GPU.FrontBuffer][0].get();
					frame.bottom = nds.GPU.Framebuffer[nds.GPU.FrontBuffer][1].get();
					// VideoOut's flips pace the frames; should they not, a frame is not shown more than
					// 2 ms before it is due
					const uint64_t now = sceKernelGetProcessTime();
					if (view.speedLimit == 100 && nextFrame > now + 2000)
						sceKernelUsleep((uint32_t)(nextFrame - now - 1000));
					nextFrame = std::max(nextFrame + period, sceKernelGetProcessTime());
				}
				if (!screens::Present(frame))
				{
					ps5log::Line("[ds] the screens stopped: the GPU's device was lost");
					ps5notify::Send("The DS game's screens stopped (the GPU's device was lost).");
					break;
				}
				s_framesShown++;
			}
			s_running = false;
		}
	}

	void Stopped(int reason)
	{
		s_stopReason = reason;
	}

	void SaveChanged(const u8* data, u32 length)
	{
		std::lock_guard lock(s_saveMutex);
		s_save.assign(data, data + length);
		s_saveDirty = true;
		s_saveChangedAt = sceKernelGetProcessTime();
	}

	bool Available()
	{
		return true;
	}

	bool CoreTouched()
	{
		return s_coreTouched;
	}

	bool LaunchGame(const ps5emu::Game& game, const ps5settings::N3ds& settings, std::string& error)
	{
		std::error_code ec;
		for (const char* name : {"saves", "states", "cheats", "bios"})
			fs::create_directories(Folder(name), ec);
		const std::string path = game.path.string();
		s_stem = game.path.stem().string();
		s_folder = game.path.parent_path().string();
		ps5log::Line("[ds] launching {} ({}) from {}", game.name, game.gameId.empty() ? "no game code" : game.gameId, path);

		const Title title = Inspect(path);
		if (!title.readable)
		{
			error = "This is not a DS game melonDS can read.";
			return false;
		}
		if (title.dsiOnly)
		{
			error = "This is a DSi game. melonDS plays DSi games only as a DSi, with a DSi's own BIOS and system memory, which "
					"this app does not set up yet.";
			return false;
		}
		std::vector<u8> file = ReadFile(path);
		if (file.empty())
		{
			error = "The game's file could not be read.";
			return false;
		}
		auto rom = std::make_unique<u8[]>(file.size());
		std::copy(file.begin(), file.end(), rom.get());
		const u32 romLength = (u32)file.size();
		file = {};
		std::vector<u8> save = ReadSave();
		melonDS::NDSCart::NDSCartArgs cartArgs;
		if (!save.empty())
		{
			cartArgs.SRAM = std::make_unique<u8[]>(save.size());
			std::copy(save.begin(), save.end(), cartArgs.SRAM.get());
			cartArgs.SRAMLength = (u32)save.size();
		}
		{
			std::lock_guard lock(s_saveMutex);
			s_save = save;
			s_saveDirty = false;
		}
		auto cart = melonDS::NDSCart::ParseROM(std::move(rom), romLength, nullptr, std::move(cartArgs));
		if (!cart)
		{
			error = "melonDS could not read the game's file: is it a whole DS game (.nds)?";
			return false;
		}

		// the console: a DS's own BIOS and firmware, or melonDS's
		melonDS::NDSArgs args;
		auto bios9 = settings.dsOwnBios ? ReadBios<melonDS::ARM9BIOSSize>("bios9.bin") : nullptr;
		auto bios7 = settings.dsOwnBios ? ReadBios<melonDS::ARM7BIOSSize>("bios7.bin") : nullptr;
		const bool ownBios = bios9 && bios7;
		if (ownBios)
		{
			args.ARM9BIOS = std::move(bios9);
			args.ARM7BIOS = std::move(bios7);
		}
		std::string firmwareAbout;
		args.Firmware = MakeFirmware(settings, ownBios, firmwareAbout);
		// the JIT where the console gives executable memory, its code reached by the slow path (no fast memory)
		s_jit = settings.dsJit && ps5privilege::Current().executable;
		if (s_jit)
			args.JIT = melonDS::JITArgs{32, true, true, false};
		else
			args.JIT = std::nullopt;
		args.OutputSampleRate = 48000.0;
		args.Renderer3D = std::make_unique<melonDS::SoftRenderer>();
		ps5log::Line("[ds] {} and {}; the CPUs on {}", ownBios ? "the DS's own BIOS" : "melonDS's BIOS (FreeBIOS)", firmwareAbout,
			s_jit ? "the JIT" : settings.dsJit ? "the interpreter (no executable memory)" : "the interpreter (dsJit off)");

		s_nds = new melonDS::NDS(std::move(args), nullptr);
		s_nds->Reset();
		s_nds->SPI.GetPowerMan()->SetBatteryLevelOkay(true);
		{
			// the DS's clock is the PS5's
			const std::time_t now = std::time(nullptr);
			if (const std::tm* local = std::localtime(&now))
				s_nds->RTC.SetDateTime(local->tm_year + 1900, local->tm_mon + 1, local->tm_mday, local->tm_hour, local->tm_min, local->tm_sec);
		}
		s_nds->SetNDSCart(std::move(cart));
		// straight into the game, past the DS's menu (melonDS's BIOS needs it)
		s_nds->SetupDirectBoot(game.path.filename().string());
		static_cast<melonDS::SoftRenderer&>(s_nds->GetRenderer3D()).SetThreaded(true, s_nds->GPU);
		s_cheats = std::make_unique<melonDS::ARCodeFile>(CheatPath());
		s_nds->AREngine.Cheats = s_cheats->GetCodes();
		s_nds->Start();

		// the screens: VideoOut is the game's from here
		s_coreTouched = true;
		if (!screens::Start(error))
		{
			error = "The DS's screens did not start: " + error;
			delete s_nds;
			s_nds = nullptr;
			return false;
		}

		s_settings = s_launched = settings;
		s_name = game.name;
		s_titleId = game.titleId;
		{
			std::lock_guard lock(s_viewMutex);
			s_view = {};
			s_view.layout = settings.layout;
			s_view.filter = settings.dsFilter;
		}
		s_stopReason = -1;
		s_stateRequest = 0;
		{
			// the menu's top: GameTDB's publisher and year, and the box art (or the game's icon)
			ps5gameinfo::Info info;
			std::string details;
			if (ps5gameinfo::Find(ps5boxart::System::Nds, game.gameId, info))
				for (const std::string& part : {info.publisher, ps5gameinfo::Year(info.released)})
					if (!part.empty())
						details += (details.empty() ? "" : "  /  ") + part;
			if (details.empty())
				details = game.publisher;
			std::string cover = ps5boxart::Path(ps5boxart::System::Nds, game.gameId);
			if (cover.empty())
				cover = CoverPath(game.titleId);
			ps5ingame3ds::SetGame(details, cover);
		}
		ShowInMenu();
		ShowStateSlots();
		ShowCheats();
		ps5ingame3ds::LoadBorder(settings.border);
		PublishScreens();
		input::Configure(settings);
		sound::Start(*s_nds);
		sound::SetVolume(settings.volume);
		s_stop = false;
		s_running = true;
		s_framesEmulated = 0;
		s_framesShown = 0;
		s_emulation = std::thread(Emulate);
		ps5log::Line("[ds] {} is running", game.name);
		return true;
	}

	void RunGame()
	{
		ps5notify::Send("Touchpad + Options: the in-game menu. Touchpad click: the touch screen. R3: the microphone");
		uint64_t polls = 0;
		uint32_t lastEmulated = 0, lastShown = 0;
		uint64_t lastSecond = sceKernelGetProcessTime();
		ps5emu::LogMemory();
		while (s_running && !s_stop)
		{
			sceKernelUsleep(4000);
			input::Update(ps5ingame3ds::MenuOpen(), s_bottomShown); // the game sees no buttons while the menu is up
			if (++polls % 500 == 0)
				ps5pad::Rescan(); // controllers joining or leaving, about every two seconds
			if (polls % 15000 == 0)
				ps5emu::LogMemory(); // about once a minute: what keeps growing is a leak
			if (polls % 250 == 0)
			{
				// about once a second: the overlay's numbers, from the frames shown and emulated
				const uint64_t now = sceKernelGetProcessTime();
				const double seconds = std::max(1e-3, (now - lastSecond) / 1e6);
				const uint32_t emulated = s_framesEmulated, shown = s_framesShown;
				const double fps = (shown - lastShown) / seconds;
				const double speed = (emulated - lastEmulated) / seconds / kDsFrameRate * 100.0;
				lastSecond = now;
				lastEmulated = emulated;
				lastShown = shown;
				ps5ingame3ds::SetPerformance(fps, speed, s_jit ? "DS: melonDS, JIT" : "DS: melonDS, interpreter");
				// every ten seconds in the boot log, so a slow game can be told from a slow PS5
				if (polls % 2500 == 0)
					ps5log::Line("[perfds] {:.0f} fps, speed {:.0f}%; {}; filter {}", fps, speed, s_jit ? "JIT" : "interpreter", s_settings.dsFilter);
			}
			FlushSave(false);
			switch (ps5pad::TakeShortcut())
			{
			case ps5pad::Shortcut::Menu:
				ps5ingame3ds::ToggleMenu();
				break;
			case ps5pad::Shortcut::SwapScreens:
			{
				ps5ingame3ds::Settings menu = MenuSettings();
				menu.swapScreens = !menu.swapScreens;
				ApplyMenu(menu);
				ShowInMenu();
				break;
			}
			case ps5pad::Shortcut::CornerScreen:
			{
				// the next layout, as the menu's Screens item goes
				ps5ingame3ds::Settings menu = MenuSettings();
				menu.layout = (menu.layout + 1) % 4;
				ApplyMenu(menu);
				ShowInMenu();
				break;
			}
			case ps5pad::Shortcut::None:
				break;
			}
			ps5ingame3ds::Settings menu;
			if (ps5ingame3ds::TakeChanges(menu))
				ApplyMenu(menu);
			ps5ingame3ds::ExtrasRequest extras;
			if (ps5ingame3ds::TakeExtrasRequest(extras) && extras.kind == ps5ingame3ds::ExtrasRequest::Cheat)
			{
				std::lock_guard lock(s_cheatMutex);
				s_cheatToggles.push_back(extras.index);
			}
			bool load = false;
			int slot = 0;
			if (ps5ingame3ds::TakeStateRequest(load, slot))
				s_stateRequest = load ? -slot : slot;
			if (ps5ingame3ds::TakeLibraryRequest())
			{
				ps5log::Line("[ds] back to the library");
				s_stop = true;
			}
			if (const int reason = s_stopReason.exchange(-1); reason >= 0)
			{
				// the game shut the DS down (or asked for the GBA's mode): back to the library
				using Reason = melonDS::Platform::StopReason;
				ps5log::Line("[ds] melonDS stopped the DS ({})", reason);
				if (reason == (int)Reason::GBAModeNotSupported)
					ps5notify::Send("The game asked for the DS's GBA mode, which melonDS has not.");
				else if (reason == (int)Reason::BadExceptionRegion)
					ps5notify::Send("The DS game crashed.");
				s_stop = true;
			}
		}
		s_stop = true;
		const auto started = std::chrono::steady_clock::now();
		auto elapsed = [&] {
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		};
		// The app starts over after this, and the save is written below: a shutdown that hangs is cut
		// short by starting over from here, as the 3DS's is
		static std::atomic_bool s_shutDown = false;
		std::thread([] {
			for (int waited = 0; waited < 50 && !s_shutDown; waited++)
				sceKernelUsleep(100000);
			if (s_shutDown)
				return;
			ps5log::Line("[ds] shutting down takes over 5 s: starting over without waiting");
			FlushSave(true);
			ps5settings::Launcher all = ps5settings::Load();
			all.side = "ds";
			ps5settings::Save(all);
			ps5emu::RestartToLibrary();
		}).detach();
		if (s_emulation.joinable())
			s_emulation.join();
		ps5log::Line("[ds] emulation stopped in {} ms", elapsed());
		FlushSave(true);
		sound::Stop();
		screens::Stop();
		delete s_nds; // its 3D renderer's thread with it
		s_nds = nullptr;
		s_cheats.reset();
		ps5log::Line("[ds] shut down in {} ms", elapsed());
		s_shutDown = true;
	}
}
