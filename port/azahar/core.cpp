// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: Azahar's core on the PS5 (azahar.h), built in Azahar's tree against its libraries
// (tools/build-azahar.sh) and linked into the app with Cemu.
//
//  - its data under kRoot: the 3DS's storage (sdmc, nand), sysdata (where aes_keys.txt goes for
//    encrypted games), the shader cache, its log (log/azahar_log.txt);
//  - the window is VideoOut, a VK_KHR_display surface (ps5/vulkan_display.h) for Azahar's own
//    Vulkan renderer, whose instance takes RADV's vkGetInstanceProcAddr (the hooks below, which
//    patches/azahar adds to vk_platform.cpp);
//  - the emulation runs on a thread of its own; the game's loop on the main thread reads the
//    DualSense (input.h) and the port's shortcuts (ps5/pad.h), and applies what the in-game menu
//    changes (app/ingame3ds.h, which Azahar's renderer draws over its screens).

// first: Azahar's Vulkan headers, with its options, before the port's, which include vulkan.h plainly
#include "video_core/renderer_vulkan/vk_frontend_overlay.h"

#include "azahar.h"
#include "controls.h"
#include "input.h"
#include "library.h"
#include "../app/boxart.h"
#include "../app/gameinfo.h"
#include "../app/ingame3ds.h"
#include "../app/lang.h"
#include "../ps5/display.h"
#include "../ps5/kernel.h"
#include "../ps5/log.h"
#include "../ps5/notify.h"
#include "../ps5/pad.h"
#include "../ps5/privilege.h"
#include "../ps5/vulkan_display.h"

#include "audio_core/sink_details.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/applets/swkbd.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/image_interface.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/apt/applet_manager.h"
#include "core/hle/service/apt/apt.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/nfc/nfc.h"
#include "core/cheats/cheat_base.h"
#include "core/cheats/cheats.h"
#include "core/hle/service/hid/hid.h"
#include "core/hle/service/ir/ir_rst.h"
#include "core/hle/service/ir/ir_user.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"
#include "core/loader/loader.h"
#include "core/savestate.h"
#include "core/system_titles.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "video_core/shader/generator/glsl_shader_gen.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <thread>

extern "C" int sceKernelUsleep(unsigned int microseconds);

// What Azahar's vk_platform.cpp asks the PS5 frontend for (patches/azahar).
PFN_vkGetInstanceProcAddr AzaharPS5_GetInstanceProcAddr()
{
	return ps5vk::GetInstanceProcAddr();
}

VkSurfaceKHR AzaharPS5_CreateSurface(VkInstance instance)
{
	std::string error;
	const VkSurfaceKHR surface = ps5vk::CreateDisplaySurface(instance, error);
	if (surface == VK_NULL_HANDLE)
		throw std::runtime_error("PS5: " + error);
	return surface;
}

namespace ps5azahar
{
	namespace
	{
		// VideoOut, whole: the layout places the 3DS's screens on the 4K picture.
		class Window final : public Frontend::EmuWindow
		{
		public:
			Window()
			{
				window_info.type = Frontend::WindowSystemType::PS5;
				window_info.render_surface = this; // not headless: there is a surface
				window_info.render_surface_scale = 1.0f;
				UpdateLayout();
			}

			void UpdateLayout()
			{
				UpdateCurrentFramebufferLayout(ps5display::kWidth, ps5display::kHeight);
			}

			void PollEvents() override {}
			void MakeCurrent() override {}
			void DoneCurrent() override {}

			CursorInfo GetCursorInfo() const override
			{
				CursorInfo cursor;
				float x, y;
				const auto& layout = GetFramebufferLayout();
				// only over a bottom screen that is shown (not with the top screen alone)
				cursor.visible = input::Cursor(x, y) && !ps5ingame3ds::MenuOpen() && !ps5ingame3ds::KeyboardOpen() && layout.bottom_screen_enabled &&
					layout.bottom_screen.GetWidth() > 0;
				// in the bottom screen's pixels on the TV, from its top left corner, as Azahar draws
				// its crosshair: where the touch lands
				cursor.projected_x = x * layout.bottom_screen.GetWidth();
				cursor.projected_y = y * layout.bottom_screen.GetHeight();
				return cursor;
			}
		};

		// A game asking for text (a name, a password): the port's keyboard (app/ingame3ds.h), where
		// Azahar's default one answers at once with the console's user name
		class Keyboard final : public Frontend::SoftwareKeyboard
		{
		public:
			void Execute(const Frontend::KeyboardConfig& config) override
			{
				SoftwareKeyboard::Execute(config);
				ps5ingame3ds::KeyboardRequest request;
				request.hint = config.hint_text;
				request.maxLength = config.max_text_length;
				// the game's own labels where it gives them, as the 3DS's keyboard shows them
				std::vector<std::string> defaults;
				switch (config.button_config)
				{
				case Frontend::ButtonConfig::Dual: defaults = {Frontend::SWKBD_BUTTON_CANCEL, Frontend::SWKBD_BUTTON_OKAY}; break;
				case Frontend::ButtonConfig::Triple:
					defaults = {Frontend::SWKBD_BUTTON_CANCEL, Frontend::SWKBD_BUTTON_FORGOT, Frontend::SWKBD_BUTTON_OKAY};
					break;
				default: defaults = {Frontend::SWKBD_BUTTON_OKAY}; break;
				}
				for (size_t i = 0; i < defaults.size(); i++)
					request.buttons.push_back(i < config.button_text.size() && !config.button_text[i].empty() ? config.button_text[i] : defaults[i]);
				ps5log::Line("[azahar] the game asks for text ({} characters at most, {} buttons)", config.max_text_length, request.buttons.size());
				ps5ingame3ds::OpenKeyboard(request);
			}

			void ShowError(const std::string& error) override
			{
				ps5ingame3ds::KeyboardError(error);
			}
		};
		std::shared_ptr<Keyboard> s_keyboard;

		const char* KeyboardMessage(Frontend::ValidationError error)
		{
			using Error = Frontend::ValidationError;
			switch (error)
			{
			// tr: why a 3DS game did not take what was typed on its keyboard
			case Error::MaxDigitsExceeded: return ps5lang::Tr("Too many digits.");
			case Error::AtSignNotAllowed: return ps5lang::Tr("The @ sign is not allowed here.");
			case Error::PercentNotAllowed: return ps5lang::Tr("The % sign is not allowed here.");
			case Error::BackslashNotAllowed: return ps5lang::Tr("The \\ sign is not allowed here.");
			case Error::ProfanityNotAllowed: return ps5lang::Tr("That word is not allowed.");
			case Error::FixedLengthRequired: return ps5lang::Tr("The text must be exactly the length asked for.");
			case Error::MaxLengthExceeded: return ps5lang::Tr("The text is too long.");
			case Error::BlankInputNotAllowed: return ps5lang::Tr("The text cannot be only spaces.");
			case Error::EmptyInputNotAllowed: return ps5lang::Tr("The text cannot be empty.");
			default: return ps5lang::Tr("The game did not accept it.");
			}
		}

		std::once_flag s_setUp;
		std::unique_ptr<Window> s_window;
		std::thread s_emulation;
		std::atomic_bool s_stop = false;
		std::atomic_bool s_running = false;
		ps5settings::N3ds s_settings; // the game's loop's: as the menu leaves them
		std::string s_name;
		uint64_t s_titleId = 0;
		bool s_coreTouched = false; // LaunchGame got as far as starting Azahar's core
		// a save state the menu asked for, for the emulation thread: a save to slot n (n), a load (-n)
		std::atomic<int> s_stateRequest{0};
		std::atomic<uint64_t> s_stateAskedAt{0}; // sceKernelGetProcessTime

		// The menu's save state slots: when each was saved, as Azahar lists them
		void ShowStateSlots()
		{
			std::vector<std::string> times(ps5ingame3ds::kStateSlots);
			for (const Core::SaveStateInfo& info : Core::ListSaveStates(s_titleId, 0))
			{
				if (info.slot < 1 || info.slot > (u32)times.size())
					continue;
				const std::time_t time = (std::time_t)info.time;
				// tr: a save state's slot whose date is unknown
				std::string text = ps5lang::Tr("saved");
				if (const std::tm* local = std::localtime(&time))
					text = ps5lang::ShortDateTime(local->tm_mon + 1, local->tm_mday, local->tm_hour, local->tm_min);
				// an earlier build's state of the same format loads too (Azahar patch 0011)
				// tr: in place of a save state's date: it was made by another version of the app
				times[info.slot - 1] = info.status != Core::SaveStateInfo::ValidationStatus::BuildMismatch ? text : ps5lang::Tr("another version's");
			}
			ps5ingame3ds::SetStateSlots(times);
		}

		// Amiibo dumps for both emulators' menus (the Wii U side reads the same folder)
		constexpr const char* kAmiiboFolder = PS5CEMU_DATA "/amiibo";
		std::vector<std::string> s_amiiboFiles;

		// The menu's amiibo files and the game's cheats, as they are now
		void ShowExtras()
		{
			std::vector<std::string> amiibo;
			std::error_code error;
			for (const auto& entry : std::filesystem::directory_iterator(kAmiiboFolder, error))
			{
				std::string extension = entry.path().extension().string();
				std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
				if (entry.is_regular_file(error) && extension == ".bin")
					amiibo.push_back(entry.path().filename().string());
			}
			std::sort(amiibo.begin(), amiibo.end());
			s_amiiboFiles = amiibo;
			std::vector<std::pair<std::string, bool>> cheats;
			for (const auto& cheat : Core::System::GetInstance().CheatEngine().GetCheats())
				cheats.emplace_back(cheat->GetName(), cheat->IsEnabled());
			ps5ingame3ds::SetExtras(amiibo, cheats);
		}

		// An amiibo held to the 3DS's reader or taken off it, or a cheat turned on or off (kept in the
		// game's cheat file, as the desktop Azahar's cheat window does)
		void HandleExtras(const ps5ingame3ds::ExtrasRequest& request)
		{
			using Request = ps5ingame3ds::ExtrasRequest;
			Core::System& system = Core::System::GetInstance();
			std::string message;
			if (request.kind == Request::Amiibo || request.kind == Request::RemoveAmiibo)
			{
				auto nfc = system.ServiceManager().GetService<Service::NFC::Module::Interface>("nfc:u");
				if (!nfc)
					message = ps5lang::Tr("No amiibo reader");
				else if (request.kind == Request::RemoveAmiibo)
				{
					nfc->RemoveAmiibo();
					// tr: the amiibo was taken off the 3DS's reader
					message = ps5lang::Tr("Taken away");
				}
				else if (request.index >= 0 && request.index < (int)s_amiiboFiles.size())
				{
					const std::string& name = s_amiiboFiles[request.index];
					message = nfc->LoadAmiibo(std::string(kAmiiboFolder) + "/" + name) ? ps5lang::TrF("Scanned {0}", name) :
																						   std::string(ps5lang::Tr("Not an amiibo dump"));
				}
			}
			else if (request.kind == Request::Cheat)
			{
				const auto cheats = system.CheatEngine().GetCheats();
				if (request.index >= 0 && request.index < (int)cheats.size())
				{
					const auto& cheat = cheats[request.index];
					cheat->SetEnabled(!cheat->IsEnabled());
					system.CheatEngine().SaveCheatFile(s_titleId);
					// tr: a cheat ({0}) turned on or off
					message = cheat->IsEnabled() ? ps5lang::TrF("{0}: on", cheat->GetName()) : ps5lang::TrF("{0}: off", cheat->GetName());
				}
			}
			ps5log::Line("[azahar] {}", message);
			if (request.kind != Request::Cheat)
				ps5ingame3ds::SetExtrasMessage(message);
			ShowExtras();
		}

		void ReloadControls();

		// The border's picture, from the app's assets/borders (tools/render-borders.py: uncompressed
		// 24- or 32-bit TGA), for the menu's renderer to draw round the screens
		void LoadBorder(int theme)
		{
			theme = std::clamp(theme, 0, ps5ingame3ds::kBorderCount - 1);
			std::vector<uint8_t> rgba;
			int width = 0, height = 0;
			if (theme > 0)
			{
				std::string name = ps5ingame3ds::kBorderNames[theme];
				std::transform(name.begin(), name.end(), name.begin(), ::tolower);
				const std::string path = ps5paths::Assets() + "/borders/" + name + ".tga";
				std::ifstream file(path, std::ios::binary);
				std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
				const int bytes = data.size() >= 18 ? data[16] / 8 : 0;
				if (data.size() >= 18 && data[2] == 2 && (bytes == 3 || bytes == 4))
				{
					width = data[12] | data[13] << 8;
					height = data[14] | data[15] << 8;
					const bool topFirst = data[17] & 0x20;
					const size_t start = 18 + data[0];
					if (data.size() >= start + (size_t)width * height * bytes)
					{
						rgba.resize((size_t)width * height * 4);
						for (int y = 0; y < height; y++)
							for (int x = 0; x < width; x++)
							{
								const uint8_t* in = &data[start + ((size_t)(topFirst ? y : height - 1 - y) * width + x) * bytes];
								uint8_t* out = &rgba[((size_t)y * width + x) * 4];
								out[0] = in[2];
								out[1] = in[1];
								out[2] = in[0];
								out[3] = 255;
							}
					}
				}
				if (rgba.empty())
					ps5log::Line("[azahar] border {}: {} could not be read", ps5ingame3ds::kBorderNames[theme], path);
			}
			ps5ingame3ds::SetBorder(theme, std::move(rgba), width, height);
		}

		// Where the screens are, for the border
		void PublishScreens()
		{
			if (!s_window)
				return;
			const auto& layout = s_window->GetFramebufferLayout();
			std::vector<ps5ingame3ds::ScreenRect> screens;
			auto add = [&](const Common::Rectangle<u32>& r) {
				if (r.GetWidth() > 0 && r.GetHeight() > 0)
					screens.push_back({(float)r.left, (float)r.top, (float)r.right, (float)r.bottom});
			};
			if (layout.top_screen_enabled)
				add(layout.top_screen);
			if (layout.bottom_screen_enabled)
				add(layout.bottom_screen);
			ps5ingame3ds::SetScreens(screens, (float)layout.width, (float)layout.height);
		}

		// What came of a save or load, in the menu and the boot log
		void ReportState(int request, Core::System::ResultStatus result, Core::System& system)
		{
			using Status = Core::System::ResultStatus;
			const bool load = request < 0;
			const int slot = std::abs(request);
			// tr: how a save state's save or load went
			const std::string message = result == Status::Success ? (load ? ps5lang::TrF("Loaded slot {0}", slot) : ps5lang::TrF("Saved slot {0}", slot)) :
				result == Status::ErrorSavestateBuildMismatch ? std::string(ps5lang::Tr("Made by another version")) :
																std::string(ps5lang::Tr("It did not work"));
			ps5log::Line("[azahar] {} slot {}: {}{}", load ? "load from" : "save to", slot, message,
				result == Status::Success ? std::string() : fmt::format(" ({})", system.GetStatusDetails()));
			ps5ingame3ds::SetStateMessage(message);
			ShowStateSlots();
			// a load makes the 3DS's input services over: the DualSense goes back to them
			if (load && result == Status::Success)
				ReloadControls();
		}

		// Azahar's paths and log, once, before anything of Azahar's runs (a CIA install from the
		// launcher, or a game).
		void SetUp()
		{
			std::call_once(s_setUp, [] {
				FileUtil::SetUserPath(std::string(kRoot) + "/");
				Common::Log::Filter filter;
				filter.ParseFilterString("*:Info");
				Common::Log::Initialize();
				Common::Log::SetGlobalFilter(filter);
				Common::Log::Start();
				// every 3DS system service emulated at a high level, as Azahar's frontends set it up
				// (the services look themselves up in this table when the 3DS starts)
				for (const auto& module : Service::service_module_map)
					Settings::values.lle_modules.emplace(module.name, false);
				ps5log::Line("[azahar] data in {}", FileUtil::GetUserPath(FileUtil::UserPath::UserDir));
			});
		}

		Settings::LayoutOption LayoutOf(int layout)
		{
			switch ((Layout)layout)
			{
			case Layout::Stacked: return Settings::LayoutOption::Default;
			case Layout::TopOnly: return Settings::LayoutOption::SingleScreen;
			case Layout::LargeTop: return Settings::LayoutOption::LargeScreen;
			case Layout::SideBySide: return Settings::LayoutOption::SideScreen;
			}
			return Settings::LayoutOption::LargeScreen;
		}

		// The launcher's 3DS settings in Azahar's.
		void ApplySettings(const ps5settings::N3ds& settings)
		{
			auto& values = Settings::values;
			values.graphics_api = Settings::GraphicsAPI::Vulkan;
			values.physical_device = 0;
			values.use_hw_shader = true;
			values.use_shader_jit = true;
			values.use_disk_shader_cache = true;
			values.async_shader_compilation = true; // a draw waits for no shader: no stutter
			values.async_presentation = true;
			values.use_vsync = true;
			values.use_skip_duplicate_frames = false; // every frame drawn, so the menu is too
			values.frame_limit = 100;
			values.resolution_factor = (u32)std::clamp(settings.resolution, 1, 10);
			values.texture_filter = (Settings::TextureFilter)std::clamp(settings.textureFilter, 0, 5);
			values.layout_option = LayoutOf(settings.layout);
			values.swap_screen = false;
			// dynarmic's code goes in executable direct memory (ps5platform/exec.h): where the console
			// refuses it, Azahar's interpreter runs the game (slower) rather than nothing at all
			values.use_cpu_jit = ps5privilege::Current().executable;
			if (!values.use_cpu_jit.GetValue())
				ps5log::Line("[azahar] no executable direct memory: the 3DS CPU runs on Azahar's interpreter");
			values.cpu_clock_percentage = std::clamp(settings.cpuClock, 25, 400);
			// every vertex position computed invariantly, as Azahar does on Apple GPUs (patch 0010):
			// for Pokemon X's moving black stipples (#23)
			Pica::Shader::Generator::GLSL::g_ps5InvariantPosition = settings.invariantPosition;
			if (settings.invariantPosition)
				ps5log::Line("[azahar] vertex positions invariant (n3ds.invariantPosition)");
			values.is_new_3ds = true;
			// automatic (-1) takes the game's own region; a game made for another one may refuse to
			// start or show other languages (#17)
			values.region_value = std::clamp(settings.region, -1, 6);
			// texture packs from azahar/load/textures/<title ID>, as the desktop Azahar loads them
			values.custom_textures = settings.customTextures;
			values.preload_textures = false;
			values.async_custom_loading = true;
			values.output_type = AudioCore::SinkType::PS5;
			values.audio_emulation = Settings::AudioEmulation::HLE;
			values.enable_audio_stretching = true;
			values.volume = std::clamp(settings.volume, 0, 100) / 100.0f;
			values.input_type = AudioCore::InputType::Null;
			values.camera_name.fill("blank"); // the 3DS cameras see nothing: the PS5 has none to lend
			input::Configure(settings);
		}

		// The menu's view of the settings, and the settings it changed
		ps5ingame3ds::Settings MenuSettings(const ps5settings::N3ds& settings)
		{
			ps5ingame3ds::Settings menu;
			menu.layout = settings.layout;
			menu.swapScreens = Settings::values.swap_screen.GetValue();
			menu.resolution = settings.resolution;
			menu.textureFilter = settings.textureFilter;
			menu.volume = settings.volume;
			menu.performance = settings.performance;
			menu.cpuClock = settings.cpuClock;
			menu.speedLimit = (int)Settings::values.frame_limit.GetValue();
			menu.motion = settings.motion;
			menu.deadzone = settings.deadzone;
			menu.aOnCircle = MappedInput(settings, Button::A) == ps5emu::PadInput::Circle;
			menu.border = settings.border;
			return menu;
		}

		void ShowInMenu()
		{
			ps5ingame3ds::Start(s_name, s_titleId, MenuSettings(s_settings));
		}

		// The 3DS's controls again, after a change: as Azahar's ApplySettings reloads them, without
		// its sound output, which it would open again
		void ReloadControls()
		{
			input::Configure(s_settings);
			Core::System& system = Core::System::GetInstance();
			if (!system.IsPoweredOn())
				return;
			if (auto hid = Service::HID::GetModule(system))
				hid->ReloadInputDevices();
			if (auto apt = Service::APT::GetModule(system))
				apt->GetAppletManager()->ReloadInputDevices();
			auto& services = system.ServiceManager();
			if (auto irUser = services.GetService<Service::IR::IR_USER>("ir:USER"))
				irUser->ReloadInputDevices();
			if (auto irRst = services.GetService<Service::IR::IR_RST>("ir:rst"))
				irRst->ReloadInputDevices();
		}

		// What the menu changed, while the game runs; kept in the launcher's settings for the next
		// games
		void ApplyMenu(const ps5ingame3ds::Settings& menu)
		{
			auto& values = Settings::values;
			const bool controls = menu.motion != s_settings.motion || menu.deadzone != s_settings.deadzone ||
				menu.aOnCircle != (MappedInput(s_settings, Button::A) == ps5emu::PadInput::Circle);
			s_settings.layout = menu.layout;
			s_settings.resolution = menu.resolution;
			s_settings.textureFilter = menu.textureFilter;
			s_settings.volume = menu.volume;
			s_settings.performance = menu.performance;
			const bool clock = menu.cpuClock != s_settings.cpuClock;
			s_settings.cpuClock = menu.cpuClock;
			s_settings.motion = menu.motion;
			s_settings.deadzone = menu.deadzone;
			if (menu.border != s_settings.border)
			{
				s_settings.border = menu.border;
				LoadBorder(menu.border);
			}
			if (menu.aOnCircle != (MappedInput(s_settings, Button::A) == ps5emu::PadInput::Circle))
			{
				// A, B, X and Y: on Circle, Cross, Triangle and Square, or on Cross, Circle, Square and
				// Triangle
				using ps5emu::PadInput;
				SetMapping(s_settings, (size_t)Button::A, menu.aOnCircle ? PadInput::Circle : PadInput::Cross);
				SetMapping(s_settings, (size_t)Button::B, menu.aOnCircle ? PadInput::Cross : PadInput::Circle);
				SetMapping(s_settings, (size_t)Button::X, menu.aOnCircle ? PadInput::Triangle : PadInput::Square);
				SetMapping(s_settings, (size_t)Button::Y, menu.aOnCircle ? PadInput::Square : PadInput::Triangle);
			}
			// the renderer and the sound read these as they go
			values.layout_option = LayoutOf(menu.layout);
			values.swap_screen = menu.swapScreens;
			values.resolution_factor = (u32)std::clamp(menu.resolution, 1, 10);
			values.texture_filter = (Settings::TextureFilter)std::clamp(menu.textureFilter, 0, 5);
			values.volume = std::clamp(menu.volume, 0, 100) / 100.0f;
			s_window->UpdateLayout();
			if (controls)
				ReloadControls();
			if (clock)
			{
				// as Azahar's ApplySettings: the cores' timers take the new rate from their next slice
				values.cpu_clock_percentage = std::clamp(menu.cpuClock, 25, 400);
				Core::System& system = Core::System::GetInstance();
				if (system.IsPoweredOn())
					system.CoreTiming().UpdateClockSpeed(values.cpu_clock_percentage.GetValue());
				ps5log::Line("[azahar] CPU clock {}%", values.cpu_clock_percentage.GetValue());
			}
			if (menu.speedLimit != (int)values.frame_limit.GetValue())
			{
				// read at every frame (the frame limiter); for this game only, as ApplySettings starts
				// each one at 100%
				values.frame_limit = std::clamp(menu.speedLimit, 0, 1000);
				ps5log::Line("[azahar] speed limit {}", menu.speedLimit ? fmt::format("{}%", menu.speedLimit) : "none");
			}

			ps5settings::Launcher all = ps5settings::Load();
			all.n3ds = s_settings;
			all.side = "3ds";
			ps5settings::Save(all);
		}

		const char* LoadError(Core::System::ResultStatus status)
		{
			using Status = Core::System::ResultStatus;
			switch (status)
			{
			// tr: why a 3DS game did not start
			case Status::ErrorGetLoader: return ps5lang::Tr("This is not a 3DS game Azahar can start.");
			case Status::ErrorLoader: return ps5lang::Tr("The game could not be loaded.");
			case Status::ErrorLoader_ErrorEncrypted:
				return ps5lang::Tr("The game is encrypted: decrypt it, or put the 3DS's aes_keys.txt in /data/ps5cemu/azahar/sysdata.");
			case Status::ErrorLoader_ErrorInvalidFormat: return ps5lang::Tr("The game's format is not supported.");
			case Status::ErrorLoader_ErrorGbaTitle: return ps5lang::Tr("GBA Virtual Console games are not supported.");
			case Status::ErrorSystemMode: return ps5lang::Tr("The game's system mode could not be found.");
			case Status::ErrorSystemFiles: return ps5lang::Tr("The game needs 3DS system files Azahar does not have.");
			case Status::ErrorLoader_ErrorPatches:
			case Status::ErrorLoader_ErrorPatchesInvalidTitle: return ps5lang::Tr("The game's patches could not be applied.");
			case Status::ErrorNotInitialized: return ps5lang::Tr("Azahar's renderer or CPU did not start.");
			case Status::ErrorArticDisconnected:
				return ps5lang::Tr("The Artic Base server did not answer: check the 3DS's address, that Artic Base runs on it, and that the 3DS "
								   "and the PS5 are on the same network.");
			default: return ps5lang::Tr("The game could not be started.");
			}
		}

		// Azahar's renderer, twice a frame: the menu, in its frame
		void DrawMenu(const Vulkan::FrontendOverlayTarget& target)
		{
			ps5ingame3ds::Record({target.instance, target.physical_device, target.device, target.queue_family, target.queue,
				target.render_pass, target.image_count, target.command_buffer, target.width, target.height,
				target.inside_render_pass, target.generation});
		}

		void Emulate()
		{
			Common::SetCurrentThreadName("AzaharEmu");
			Core::System& system = Core::System::GetInstance();
			system.RegisterCoreLoopThreadId();
			while (!s_stop)
			{
				// a save state between two of the loop's steps, signalled only when no system call is
				// still working in the background: then the step below makes it, and what it returns
				// says how it went (Azahar would otherwise put it off, and say nothing of it)
				const int request = s_stateRequest.load();
				// the game held while its menu is open: the screens presented again, without the 3DS
				// running (the menu is drawn in Azahar's frames, so they must go on), unless a save state
				// is waiting to be made; the game's sound goes quiet meanwhile
				if (!request && ps5ingame3ds::MenuOpen())
				{
					system.GPU().Renderer().SwapBuffers();
					sceKernelUsleep(16000);
					continue;
				}
				bool stateNow = false;
				if (request && !system.Kernel().AreAsyncOperationsPending())
				{
					s_stateRequest = 0;
					system.SendSignal(request > 0 ? Core::System::Signal::Save : Core::System::Signal::Load, (u32)std::abs(request));
					stateNow = true;
				}
				else if (request && sceKernelGetProcessTime() - s_stateAskedAt > 5000000)
				{
					s_stateRequest = 0;
					ps5log::Line("[azahar] save state: the game stayed busy for 5 s");
					ps5ingame3ds::SetStateMessage(ps5lang::Tr("The game is busy: try again"));
				}
				const auto result = system.RunLoop();
				if (stateNow)
					ReportState(request, result, system);
				if (result == Core::System::ResultStatus::ErrorSavestate || result == Core::System::ResultStatus::ErrorSavestateBuildMismatch)
					continue; // the game carries on as it was
				if (result == Core::System::ResultStatus::ShutdownRequested)
				{
					ps5log::Line("[azahar] the game shut the 3DS down");
					break;
				}
				if (result != Core::System::ResultStatus::Success)
				{
					ps5log::Line("[azahar] the emulation stopped: {} ({})", (int)result, system.GetStatusDetails());
					ps5notify::Send(ps5lang::TrF("The 3DS game stopped: {0}", system.GetStatusDetails()));
					break;
				}
			}
			s_running = false;
		}
	}

	bool Available()
	{
		return true;
	}

	bool CoreTouched()
	{
		return s_coreTouched;
	}

	bool HomeMenu(int region, ps5emu::Game& game)
	{
		SetUp(); // Azahar's paths, for its NAND
		for (u32 candidate = 0; candidate < Core::NUM_SYSTEM_TITLE_REGIONS; candidate++)
		{
			if (region >= 0 && (int)candidate != region)
				continue;
			const std::string path = Core::GetHomeMenuNcchPath(candidate);
			if (path.empty() || !FileUtil::Exists(path))
				continue;
			game = {};
			game.titleId = Core::GetHomeMenuTitleId(candidate);
			game.name = "Home Menu";
			game.path = path;
			game.format = "HOME MENU";
			return true;
		}
		return false;
	}

	bool LaunchGame(const ps5emu::Game& game, const ps5settings::N3ds& settings, std::string& error)
	{
		SetUp();
		std::string path = game.path.string();
		ps5log::Line("[azahar] launching {} ({:016x}) from {}", game.name, game.titleId, path);
		if (game.path.extension() == ".cia" || game.path.extension() == ".CIA")
		{
			// a CIA is installed, not played: its game starts from the 3DS's SD card
			path = Service::AM::GetTitleContentPath(Service::FS::MediaType::SDMC, game.titleId);
			if (!FileUtil::Exists(path))
			{
				error = ps5lang::Tr("A CIA file is installed, not played: install it from Settings > Install CIA files, then start the game.");
				return false;
			}
			ps5log::Line("[azahar] the CIA's game is installed: {}", path);
		}
		s_settings = settings;
		ApplySettings(settings);

		Core::System& system = Core::System::GetInstance();
		Frontend::RegisterDefaultApplets(system);
		s_keyboard = std::make_shared<Keyboard>();
		system.RegisterSoftwareKeyboard(s_keyboard);
		system.RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());
		Vulkan::SetFrontendOverlay(&DrawMenu);

		s_window = std::make_unique<Window>();
		s_coreTouched = true;
		Core::System::ResultStatus status;
		try
		{
			status = system.Load(*s_window, path);
		}
		catch (const std::exception& ex)
		{
			error = ps5lang::TrF("Azahar did not start: {0}", ex.what());
			return false;
		}
		if (status != Core::System::ResultStatus::Success)
		{
			error = LoadError(status);
			const std::string details = system.GetStatusDetails();
			ps5log::Line("[azahar] load failed: {} ({})", error, details);
			return false;
		}

		u64 programId = 0;
		system.GetAppLoader().ReadProgramId(programId);
		// the 3DS's language, set before the game first runs and asks for it (Load may have set one
		// for the game's region; the launcher's choice comes after)
		if (settings.language >= 0)
			if (auto cfg = Service::CFG::GetModule(system))
			{
				const auto language = (Service::CFG::SystemLanguage)std::clamp(settings.language, 0, 11);
				if (cfg->GetSystemLanguage() != language)
				{
					cfg->SetSystemLanguage(language);
					cfg->UpdateConfigNANDSavegame();
				}
				ps5log::Line("[azahar] system language {}", (int)language);
			}
		system.GPU().ApplyPerProgramSettings(programId);
		std::atomic_bool stopLoading = false;
		system.GPU().Renderer().Rasterizer()->LoadDefaultDiskResources(stopLoading, nullptr);

		s_name = game.name;
		std::string title;
		if (IsArtic(path) && system.GetAppLoader().ReadTitle(title) == Loader::ResultStatus::Success && !title.empty())
			s_name = title; // the game on the 3DS, by its own name
		s_titleId = programId ? programId : game.titleId;
		{
			// the menu's top: GameTDB's publisher and year, and the box art (or the game's icon)
			ps5gameinfo::Info info;
			std::string details;
			if (ps5gameinfo::Find(ps5boxart::System::N3ds, game.gameId, info))
				for (const std::string& part : {info.publisher, ps5gameinfo::Year(info.released)})
					if (!part.empty())
						details += (details.empty() ? "" : "  /  ") + part;
			std::string cover = ps5boxart::Path(ps5boxart::System::N3ds, game.gameId);
			if (cover.empty())
				cover = CoverPath(game.titleId);
			ps5ingame3ds::SetGame(details, cover);
		}
		ShowInMenu();
		ShowStateSlots();
		ShowExtras();
		LoadBorder(settings.border);
		PublishScreens();
		s_stop = false;
		s_running = true;
		s_emulation = std::thread(Emulate);
		ps5log::Line("[azahar] {:016x} is running", programId);
		return true;
	}

	void RunGame()
	{
		// tr: the PS5's notification as a 3DS game starts
		ps5notify::Send(ps5lang::Tr("Touchpad click + Options: the in-game menu. A touchpad click alone: the bottom screen"));
		Core::System& system = Core::System::GetInstance();
		uint64_t polls = 0;
		ps5emu::LogMemory();
		while (s_running && !s_stop)
		{
			sceKernelUsleep(4000);
			input::Update(*s_window, ps5ingame3ds::MenuOpen() || ps5ingame3ds::KeyboardOpen()); // the game sees no buttons while either is up
			PublishScreens(); // the layout may have changed (the menu, a shortcut)
			if (++polls % 500 == 0)
				ps5pad::Rescan(); // controllers joining or leaving, about every two seconds
			if (polls % 15000 == 0)
				ps5emu::LogMemory(); // about once a minute: what keeps growing is a leak
			if (polls % 250 == 0)
			{
				// about once a second: the overlay's numbers, and where the frames' time went (Azahar's
				// own measures, per 3DS frame: the CPU's code and the rest, the system calls and
				// services, the 3DS GPU's commands, and the hand-over to the PS5's GPU with its waits)
				const auto stats = system.GetAndResetPerfStats();
				const double ms = 1000.0;
				const std::string breakdown = stats.time_vblank_interval > 0 ?
					fmt::format("frame {:.1f} ms: CPU {:.1f}  SVC {:.1f}  IPC {:.1f}  GPU {:.1f}  swap {:.1f}",
						stats.time_vblank_interval * ms, stats.time_remaining * ms, stats.time_hle_svc * ms,
						stats.time_hle_ipc * ms, stats.time_gpu * ms, stats.time_swap * ms) :
					std::string();
				ps5ingame3ds::SetPerformance(stats.game_fps, stats.emulation_speed * 100.0, breakdown);
				// every ten seconds in the boot log, so a slow game can be told from a slow PS5
				if (polls % 2500 == 0)
					ps5log::Line("[perf3ds] {:.0f} fps, speed {:.0f}%; {}; CPU clock {}%, {}x, filter {}", stats.game_fps,
						stats.emulation_speed * 100.0, breakdown.empty() ? "no frames" : breakdown, s_settings.cpuClock,
						s_settings.resolution, s_settings.textureFilter);
			}
			switch (ps5pad::TakeShortcut())
			{
			case ps5pad::Shortcut::Menu:
				ps5ingame3ds::ToggleMenu();
				break;
			case ps5pad::Shortcut::SwapScreens:
				Settings::values.swap_screen = !Settings::values.swap_screen.GetValue();
				s_window->UpdateLayout();
				ShowInMenu();
				break;
			case ps5pad::Shortcut::CornerScreen:
			{
				// the next layout, as the menu's Screens item goes
				ps5ingame3ds::Settings menu = MenuSettings(s_settings);
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
			std::string typed;
			int button = 0;
			if (s_keyboard && ps5ingame3ds::TakeKeyboardResult(typed, button))
			{
				// checked as the 3DS's keyboard does; refused, the keyboard opens again saying why
				const auto result = s_keyboard->Finalize(typed, (u8)button);
				if (result != Frontend::ValidationError::None)
					ps5ingame3ds::KeyboardError(KeyboardMessage(result));
				ps5log::Line("[azahar] keyboard: {} characters, button {}{}", typed.size(), button,
					result != Frontend::ValidationError::None ? fmt::format(", refused ({})", (int)result) : std::string());
			}
			ps5ingame3ds::ExtrasRequest extras;
			if (ps5ingame3ds::TakeExtrasRequest(extras))
				HandleExtras(extras);
			bool load = false;
			int slot = 0;
			if (ps5ingame3ds::TakeStateRequest(load, slot))
			{
				s_stateAskedAt = sceKernelGetProcessTime();
				s_stateRequest = load ? -slot : slot;
			}
			if (ps5ingame3ds::TakeLibraryRequest())
			{
				ps5log::Line("[azahar] back to the library");
				s_stop = true;
			}
		}
		s_stop = true;
		const auto started = std::chrono::steady_clock::now();
		auto elapsed = [&] {
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		};
		// The app starts over after this, and the game's saves are on the SD card already (written as
		// the game writes them): a shutdown that takes long (18 s was seen, the screen frozen) is cut
		// short by starting over from here.
		static std::atomic_bool s_shutDown = false;
		std::thread([] {
			for (int waited = 0; waited < 50 && !s_shutDown; waited++)
				sceKernelUsleep(100000);
			if (s_shutDown)
				return;
			ps5log::Line("[azahar] shutting down takes over 5 s: starting over without waiting");
			ps5settings::Launcher all = ps5settings::Load();
			all.side = "3ds";
			ps5settings::Save(all);
			ps5emu::RestartToLibrary();
		}).detach();
		if (s_emulation.joinable())
			s_emulation.join();
		ps5log::Line("[azahar] emulation stopped in {} ms", elapsed());
		Vulkan::SetFrontendOverlay(nullptr);
		// the 3DS's files closed (saves are written as the game writes them)
		system.Shutdown();
		ps5log::Line("[azahar] shut down in {} ms", elapsed());
		input::Shutdown();
		s_shutDown = true;
	}

	namespace
	{
		std::mutex s_installMutex;
		ps5emu::InstallStatus s_install;
		std::atomic_bool s_installing = false;
		std::atomic<bool> s_cancelInstall = false; // read by Azahar's install loop (patches/azahar)
	}

	bool StartInstall(const std::string& cia, std::string& error)
	{
		if (s_installing)
		{
			error = ps5lang::Tr("An install is already running.");
			return false;
		}
		SetUp();
		{
			std::lock_guard lock(s_installMutex);
			s_install = {};
			s_install.state = ps5emu::InstallStatus::State::Running;
		}
		s_cancelInstall = false;
		s_installing = true;
		std::thread([cia] {
			ps5log::Line("[azahar] installing {}", cia);
			using Result = Service::AM::InstallStatus;
			// Checked first, as Azahar's own frontends do: InstallCIA looks only at the CIA's own
			// encryption, so a CIA whose game is still encrypted inside starts, is refused part-way
			// and comes back as ErrorAborted
			bool compressed = false;
			auto result = Service::AM::CheckCIAToInstall(cia, compressed, true);
			if (result == Result::Success && s_cancelInstall)
				result = Result::Cancelled;
			else if (result == Result::Success)
				result = Service::AM::InstallCIA(
					cia,
					[](std::size_t written, std::size_t total) {
						std::lock_guard lock(s_installMutex);
						s_install.copied = written;
						s_install.total = total;
					},
					&s_cancelInstall);
			std::lock_guard lock(s_installMutex);
			switch (result)
			{
			case Result::Success: s_install.state = ps5emu::InstallStatus::State::Done; break;
			case Result::Cancelled: s_install.state = ps5emu::InstallStatus::State::Cancelled; break;
			case Result::ErrorEncrypted:
				// Azahar installs only CIAs decrypted all the way through, the CIA and the game in
				// it; the 3DS's keys do not change that
				// tr: why a CIA file could not be installed, after "It could not be installed:"
				s_install.message = ps5lang::Tr("it is encrypted, the CIA or the game inside it. Azahar installs only fully decrypted CIA files: "
												"decrypt it with GodMode9 on a 3DS, or play the game's decrypted .3ds or .cci file instead");
				break;
			case Result::ErrorInvalid: s_install.message = ps5lang::Tr("it is not a CIA file, or it is damaged (check the file's size)"); break;
			case Result::ErrorFileNotFound:
			case Result::ErrorFailedToOpenFile: s_install.message = ps5lang::Tr("the file could not be read"); break;
			case Result::ErrorAborted:
				s_install.message = ps5lang::Tr("Azahar stopped part-way through; /data/ps5cemu/azahar/log/azahar_log.txt says why");
				break;
			default: s_install.message = ps5lang::Tr("it is not a CIA Azahar can install"); break;
			}
			if (result != Result::Success && result != Result::Cancelled)
				s_install.state = ps5emu::InstallStatus::State::Failed;
			ps5log::Line("[azahar] install of {}: {}", cia,
				result == Result::Success ? "done" : result == Result::Cancelled ? "cancelled, what it wrote removed" : s_install.message);
			s_installing = false;
		}).detach();
		return true;
	}

	ps5emu::InstallStatus GetInstallStatus()
	{
		std::lock_guard lock(s_installMutex);
		return s_install;
	}

	void CancelInstall()
	{
		// Azahar writes a CIA's contents in place as it reads them: its install loop stops at its next
		// 64 KiB and removes the title's contents, as it does for an install that fails part-way
		s_cancelInstall = true;
	}
}
