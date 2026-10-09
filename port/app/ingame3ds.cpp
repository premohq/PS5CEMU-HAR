// SPDX-License-Identifier: GPL-3.0-or-later
#include "ingame3ds.h"
#include "lang.h"
#include "menu_canvas.h"
#include "side_menu.h"
#include "tga.h"
#include "../ps5/kernel.h"
#include "../ui/text.h"
#include "../ps5/log.h"
#include "../ps5/pad.h"

#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "imgui/imgui_impl_vulkan.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>

// Cemu's overlay font, compressed into Cemu (resource/CafeDefaultFont.cpp)
uint8* extractCafeDefaultFont(sint32* size);

namespace ps5ingame3ds
{
	namespace
	{
		using ps5menu::Canvas;
		using ps5lang::Percent;
		using ps5lang::Tr;
		using ps5lang::TrC;
		using ps5lang::TrF;
		using ps5lang::TrMark;
		using ps5lang::TrMarkC;
		const ps5menu::Palette& kColours = ps5menu::kGold;

		// What the game's loop and the menu share
		std::mutex s_mutex;
		Settings s_settings;  // under s_mutex
		bool s_changed = false;
		std::string s_name;
		std::string s_details;	 // GameTDB's publisher and year (SetGame)
		std::string s_coverPath; // the game's box art, or its icon
		uint64_t s_titleId = 0;
		double s_fps = 0, s_speed = 0;
		std::string s_breakdown;
		std::atomic<bool> s_open{false};
		std::atomic<uint64_t> s_openedAt{0}; // sceKernelGetProcessTime
		std::atomic<bool> s_libraryRequested{false};
		std::vector<std::string> s_stateTimes; // under s_mutex, with the two below
		std::string s_stateMessage;
		int s_stateRequest = 0; // a save to slot n (n), a load from it (-n)
		std::vector<std::string> s_amiibo; // under s_mutex, with the four below
		std::vector<std::pair<std::string, bool>> s_cheats;
		std::string s_extrasMessage;
		ExtrasRequest s_extrasRequest;
		int s_amiiboIndex = 0;
		// the border, under s_mutex: the theme, its picture until the renderer takes it, the screens
		int s_borderTheme = 0;
		std::vector<uint8_t> s_borderPixels;
		int s_borderWidth = 0, s_borderHeight = 0;
		bool s_borderFresh = false;
		std::vector<ScreenRect> s_screens;
		float s_screensWidth = 0, s_screensHeight = 0;

		// The keyboard: asked for on Azahar's thread, typed on its renderer's, its result taken by
		// the game's loop
		std::atomic<bool> s_keyboardOpen{false};
		KeyboardRequest s_keyboard; // under s_mutex, with the four below
		std::string s_keyboardError;
		bool s_keyboardFresh = false; // a new request: the renderer starts its text over
		bool s_keyboardDone = false;
		std::string s_keyboardResult;
		int s_keyboardButton = 0;

		// The rest belongs to Azahar's renderer thread, which draws the menu.
		struct Gpu
		{
			bool ready = false, failed = false;
			VkDevice device = VK_NULL_HANDLE;
			uint64_t generation = 0; // Azahar's renderer's (Target::generation)
			VkRenderPass renderPass = VK_NULL_HANDLE;
			uint32_t width = 0, height = 0; // the screen's pass's picture
			ImGui_ImplVulkan_InitInfo info{};
			VkDescriptorPool pool = VK_NULL_HANDLE;
			ImGuiContext* context = nullptr;
			ImFontAtlas* atlas = nullptr;
			ImFont* head = nullptr;	 // the menu's styles (menu_canvas.h): heading
			ImFont* body = nullptr;	 // body
			ImFont* row = nullptr;	 // label
			ImFont* small = nullptr; // caption
			ImFont* chip = nullptr;
			bool fontsUploaded = false;
			int uploadAge = -1;	   // frames since the font upload, until its buffer goes
			bool pending = false;  // a frame's draw data waits for the render pass
			ImTextureID border = nullptr;
			ImTextureID cover = nullptr; // the game's box art, made the first time the menu opens
			bool coverTried = false;
			int coverWidth = 0, coverHeight = 0;
			std::vector<std::pair<ImTextureID, int>> retired; // textures and their age in frames, until the GPU is done with them
			uint64_t lastFrame = 0;
		};
		Gpu g;
		uint32_t s_buttons = 0, s_pressed = 0;
		uint32_t s_menuButtons = 0; // the same, with the left stick's directions as the D-pad's
		// the menu's panel: its place in the list and the open category (the renderer's), and a new
		// opening, asked for by ToggleMenu on the game's loop
		ps5menu::SideMenu s_side;
		std::atomic<bool> s_menuFresh{false};
		int s_stateSlot = 1;
		std::string s_stateAction; // the last save or load asked for in this opening: its tile shows how it went

		// tr: the 3DS's screen layouts
		constexpr const char* kLayouts[] = {TrMark("Top above bottom"), TrMark("Top screen only"), TrMark("Large top screen"), TrMark("Side by side")};
		// tr: texture filters: None, then the filters' own names
		constexpr const char* kFilters[] = {TrMarkC("texture filter", "None"), "Anime4K", TrMark("Bicubic"), "ScaleForce", "xBRZ", "MMPX"};

		std::string LayoutName(int layout)
		{
			return Tr(kLayouts[std::clamp(layout, 0, 3)]);
		}

		std::string FilterName(int filter)
		{
			filter = std::clamp(filter, 0, 5);
			return filter == 0 ? TrC("texture filter", kFilters[0]) : Tr(kFilters[filter]);
		}

		std::string BorderName(int border)
		{
			border = std::clamp(border, 0, kBorderCount - 1);
			return border + 1 < kBorderCount ? TrC("border", kBorderNames[border]) : kBorderNames[border];
		}
		// the CPU clock's steps, percent of the 3DS's
		constexpr int kClocks[] = {25, 50, 75, 100, 125, 150, 200, 300, 400};

		int NextClock(int clock, int change)
		{
			constexpr int count = (int)std::size(kClocks);
			int at = 3; // 100%
			for (int i = 0; i < count; i++)
				if (kClocks[i] == clock)
					at = i;
			return kClocks[(at + change + count) % count];
		}

		// the speed limit's steps, percent of the 3DS's; 0 is none
		constexpr int kSpeedLimits[] = {100, 150, 200, 300, 0};

		int NextSpeedLimit(int limit, int change)
		{
			constexpr int count = (int)std::size(kSpeedLimits);
			int at = 0; // 100%
			for (int i = 0; i < count; i++)
				if (kSpeedLimits[i] == limit)
					at = i;
			return kSpeedLimits[(at + change + count) % count];
		}

		void Change(const Settings& settings)
		{
			std::lock_guard lock(s_mutex);
			s_settings = settings;
			s_changed = true;
		}

		void CloseMenu()
		{
			s_open = false;
		}

		// The theme's frame round each screen, at the design's 4K sizes (tools/render-borders.py)
		struct BorderStyle
		{
			float shadow;
			ImU32 line;
			bool ring;
		};
		constexpr BorderStyle kBorderStyles[] = {
			{0, 0, false},
			{0.55f, IM_COL32(255, 255, 255, 34), false}, // Midnight
			{0.50f, IM_COL32(255, 210, 90, 60), false},	 // Waves
			{0.55f, IM_COL32(255, 255, 255, 30), false}, // Aurora
			{0.35f, IM_COL32(255, 255, 255, 22), true},	 // Shell
			{0.55f, IM_COL32(255, 255, 255, 30), false}, // PS5CEMU-HAR
		};

		// The border's picture where no screen is (the space round them cut into the cells of a grid
		// on the screens' edges), then each screen's frame, all outside the screens
		void DrawBorder(int theme)
		{
			std::vector<ScreenRect> screens;
			float width, height;
			{
				std::lock_guard lock(s_mutex);
				screens = s_screens;
				width = s_screensWidth;
				height = s_screensHeight;
			}
			const ImVec2 size = ImGui::GetIO().DisplaySize;
			if (width <= 0 || height <= 0)
				return;
			const float sx = size.x / width, sy = size.y / height;
			for (ScreenRect& r : screens)
				r = {r.left * sx, r.top * sy, r.right * sx, r.bottom * sy};
			ImDrawList* draw = ImGui::GetBackgroundDrawList();
			std::vector<float> xs{0, size.x}, ys{0, size.y};
			for (const ScreenRect& r : screens)
			{
				xs.insert(xs.end(), {r.left, r.right});
				ys.insert(ys.end(), {r.top, r.bottom});
			}
			std::sort(xs.begin(), xs.end());
			std::sort(ys.begin(), ys.end());
			for (size_t i = 0; i + 1 < xs.size(); i++)
				for (size_t j = 0; j + 1 < ys.size(); j++)
				{
					const float x0 = xs[i], x1 = xs[i + 1], y0 = ys[j], y1 = ys[j + 1];
					if (x1 - x0 < 0.5f || y1 - y0 < 0.5f)
						continue;
					const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
					bool inside = false;
					for (const ScreenRect& r : screens)
						inside |= cx > r.left && cx < r.right && cy > r.top && cy < r.bottom;
					if (!inside)
						draw->AddImage(g.border, {x0, y0}, {x1, y1}, {x0 / size.x, y0 / size.y}, {x1 / size.x, y1 / size.y});
				}
			const BorderStyle& style = kBorderStyles[std::clamp(theme, 0, kBorderCount - 1)];
			const float k = size.y / 2160.0f;
			for (const ScreenRect& r : screens)
			{
				// a soft shadow, in rings that fade outwards, deeper below
				constexpr int kRings = 12;
				for (int i = 0; i < kRings; i++)
				{
					const float d = (3 + i * 6) * k;
					const int alpha = (int)(255 * style.shadow * 0.22f * (1.0f - (float)i / kRings));
					draw->AddRect({r.left - d, r.top - d * 0.6f}, {r.right + d, r.bottom + d * 1.6f}, IM_COL32(0, 0, 0, alpha), 0, 0, 6 * k);
				}
				if (style.ring)
				{
					// Shell's recessed ring, a bezel round the screen
					draw->AddRect({r.left - 17 * k, r.top - 17 * k}, {r.right + 17 * k, r.bottom + 17 * k}, IM_COL32(20, 24, 30, 255), 30 * k, 0, 34 * k);
					draw->AddRect({r.left - 34 * k, r.top - 34 * k}, {r.right + 34 * k, r.bottom + 34 * k}, IM_COL32(255, 255, 255, 18), 30 * k, 0, 3 * k);
				}
				draw->AddRect({r.left - 1.5f * k, r.top - 1.5f * k}, {r.right + 1.5f * k, r.bottom + 1.5f * k}, style.line, 0, 0, 3 * k);
			}
		}

		// Cemu's Vulkan entry points from Azahar's instance and device (Cemu's renderer never ran),
		// a descriptor pool for the font, an ImGui context of its own with Cemu's font at the
		// menu's four sizes, and ImGui's Vulkan backend on Azahar's render pass.
		bool CreatePool(VkDevice device)
		{
			const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
			VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
			poolInfo.maxSets = 16; // the font, the border, borders on their way out
			poolInfo.poolSizeCount = 1;
			poolInfo.pPoolSizes = &size;
			return vkCreateDescriptorPool(device, &poolInfo, nullptr, &g.pool) == VK_SUCCESS;
		}

		void InitializeBackend(const Target& target)
		{
			ImGui_ImplVulkan_InitInfo info{};
			info.Instance = target.instance;
			info.PhysicalDevice = target.physicalDevice;
			info.Device = target.device;
			info.QueueFamily = target.queueFamily;
			info.Queue = target.queue;
			info.DescriptorPool = g.pool;
			info.MinImageCount = std::max(2u, target.imageCount);
			info.ImageCount = info.MinImageCount;
			ImGui_ImplVulkan_Init(&info, target.renderPass);
			g.info = info;
			g.device = target.device;
			g.generation = target.generation;
			g.renderPass = target.renderPass;
			g.width = target.width;
			g.height = target.height;
		}

		// Azahar made its renderer again, device and all (it does when a save state is loaded): what
		// the menu made on the old device went with it, so it is forgotten, not destroyed, and made
		// again on the new one. The ImGui context and the fonts' atlas stay.
		bool Reattach(const Target& target)
		{
			ps5log::Line("[ingame3ds] Azahar made its renderer again: the menu starts over on the new device");
			ImGui::SetCurrentContext(g.context);
			ImGui_ImplVulkan_ForgetDeviceObjects();
			g.border = nullptr;
			g.cover = nullptr;
			g.coverTried = false;
			g.retired.clear();
			g.fontsUploaded = false;
			g.uploadAge = -1;
			g.pending = false;
			g.pool = VK_NULL_HANDLE;
			if (!InitializeInstanceVulkan(target.instance) || !InitializeDeviceVulkan(target.device) || !CreatePool(target.device))
			{
				ps5log::Line("[ingame3ds] the menu could not start on the new device: no menu");
				return false;
			}
			InitializeBackend(target);
			std::lock_guard lock(s_mutex);
			s_borderFresh = s_borderTheme > 0 && !s_borderPixels.empty(); // the border's picture again
			return true;
		}

		bool Initialize(const Target& target, float scale)
		{
			if (!InitializeGlobalVulkan() || !InitializeInstanceVulkan(target.instance) || !InitializeDeviceVulkan(target.device))
			{
				ps5log::Line("[ingame3ds] Cemu's Vulkan commands did not load from Azahar's device: no menu");
				return false;
			}
			if (!CreatePool(target.device))
			{
				ps5log::Line("[ingame3ds] no descriptor pool: no menu");
				return false;
			}

			// Lexend's three weights at the menu's sizes, or the Wii U's system font if they are missing, with
			// the console's font for what Lexend lacks (a Russian menu, a Japanese title), cut to the
			// characters the language's words and the game's name have (docs/UI-REDESIGN.md, 7.7)
			std::string title;
			{
				std::lock_guard lock(s_mutex);
				title = s_name;
			}
			const ImWchar* ranges = ps5menu::GlyphRanges(title);
			int mergeFace = 0;
			const auto& merge = ps5menu::MergeFont(mergeFace);
			g.atlas = new ImFontAtlas();
			auto add = [&](ps5menu::Weight weight, float size) {
				ImFontConfig config{};
				config.FontDataOwnedByAtlas = false; // kept: the atlas reads them
				const auto& file = ps5menu::FontFile(weight);
				if (!file.empty())
				{
					config.OversampleH = 2;
					ImFont* font = g.atlas->AddFontFromMemoryTTF((void*)file.data(), (int)file.size(), size * scale, &config, ranges);
					if (!merge.empty())
					{
						ImFontConfig second{};
						second.FontDataOwnedByAtlas = false;
						second.MergeMode = true;
						second.FontNo = mergeFace;
						second.OversampleH = 2;
						g.atlas->AddFontFromMemoryTTF((void*)merge.data(), (int)merge.size(), size * scale, &second, ranges);
					}
					return font;
				}
				static sint32 fallbackSize = 0;
				static uint8* fallback = extractCafeDefaultFont(&fallbackSize);
				return g.atlas->AddFontFromMemoryTTF(fallback, fallbackSize, size * scale, &config);
			};
			using ps5menu::Weight;
			g.head = add(Weight::SemiBold, ps5menu::kHeading);
			g.body = add(Weight::Medium, ps5menu::kBody);
			g.row = add(Weight::Regular, ps5menu::kLabel);
			g.small = add(Weight::Regular, ps5menu::kCaption);
			g.chip = add(Weight::SemiBold, ps5menu::kChip);
			g.context = ImGui::CreateContext(g.atlas);
			ImGui::SetCurrentContext(g.context);
			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
			io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

			InitializeBackend(target);
			ps5log::Line("[ingame3ds] the menu is ready ({} images in flight{})", g.info.ImageCount,
				merge.empty() ? std::string() : fmt::format("; its second font {}, {} KiB", ps5lang::GetMenuFont().path, merge.size() >> 10));
			return true;
		}

		// ImGui's input from player 1's DualSense: the D-pad and the left stick move between items,
		// Cross chooses; Circle and Options are read here (ImGui gives Circle another use).
		void Input()
		{
			ImGuiIO& io = ImGui::GetIO();
			ps5pad::Data data{};
			const bool connected = ps5pad::Read(0, data) && !(data.buttons & ps5pad::kIntercepted);
			const uint32_t buttons = connected ? data.buttons : 0;
			s_pressed = buttons & ~s_buttons;
			s_buttons = buttons;
			const bool touchpadHeld = buttons & ps5pad::kTouchPad; // a shortcut's
			auto key = [&](ImGuiKey imguiKey, uint32_t mask) { io.AddKeyEvent(imguiKey, !touchpadHeld && (buttons & mask)); };
			key(ImGuiKey_GamepadDpadUp, ps5pad::kUp);
			key(ImGuiKey_GamepadDpadDown, ps5pad::kDown);
			key(ImGuiKey_GamepadDpadLeft, ps5pad::kLeft);
			key(ImGuiKey_GamepadDpadRight, ps5pad::kRight);
			key(ImGuiKey_GamepadFaceDown, ps5pad::kCross);
			auto stick = [&](ImGuiKey negative, ImGuiKey positive, uint8_t raw) {
				const float value = connected ? std::clamp((raw - 128) / 127.0f, -1.0f, 1.0f) : 0.0f;
				io.AddKeyAnalogEvent(negative, value < -0.5f, std::max(-value, 0.0f));
				io.AddKeyAnalogEvent(positive, value > 0.5f, std::max(value, 0.0f));
			};
			stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, data.leftX);
			stick(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, data.leftY);
			s_menuButtons = buttons;
			if (connected)
			{
				if (data.leftY < 64)
					s_menuButtons |= ps5pad::kUp;
				else if (data.leftY > 192)
					s_menuButtons |= ps5pad::kDown;
				if (data.leftX < 64)
					s_menuButtons |= ps5pad::kLeft;
				else if (data.leftX > 192)
					s_menuButtons |= ps5pad::kRight;
			}
			io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
			io.MouseDown[0] = false;
		}

		// The menu's rows as things are now: a few settings, and categories that open in place
		std::vector<ps5menu::Row> MenuRows(const Settings& settings)
		{
			using Row = ps5menu::Row;
			std::string time, stateMessage, amiibo, amiiboMessage;
			std::vector<std::pair<std::string, bool>> cheats;
			bool noAmiibo;
			{
				std::lock_guard lock(s_mutex);
				if (s_stateSlot <= (int)s_stateTimes.size())
					time = s_stateTimes[s_stateSlot - 1];
				stateMessage = s_stateMessage;
				s_amiiboIndex = s_amiibo.empty() ? 0 : std::clamp(s_amiiboIndex, 0, (int)s_amiibo.size() - 1);
				noAmiibo = s_amiibo.empty();
				amiibo = noAmiibo ? Tr("None") : s_amiibo[s_amiiboIndex];
				amiiboMessage = s_extrasMessage;
				cheats = s_cheats;
			}
			const int resolution = std::clamp(settings.resolution, 1, 10);
			std::vector<Row> rows;
			const std::string on = Tr("On"), off = Tr("Off");
			rows.push_back({"screens", Tr("Screens and border"), fmt::format("{} \u00b7 {}", LayoutName(settings.layout), BorderName(settings.border)), false,
				Tr("The screens' layout, which one is the main one, and the border."), {
				{"layout", Tr("Layout"), LayoutName(settings.layout), true,
					Tr("How the two screens share the TV. In the game, touchpad click + R1 goes to the next.")},
				// tr: the 3DS's top and bottom screens
				{"swap", Tr("Main screen"), settings.swapScreens ? Tr("Bottom") : Tr("Top"), true,
					Tr("Which screen takes the top screen's place. In the game, touchpad click + L1 swaps them.")},
				{"border", Tr("Border"), BorderName(settings.border), true,
					Tr("Artwork around the screens, never over them. Also in the launcher's Settings > Screens and borders.")},
			}});
			rows.push_back({"graphics", Tr("Graphics"), fmt::format("{}x \u00b7 {}", resolution, FilterName(settings.textureFilter)), false,
				Tr("Internal resolution, texture filter and the performance overlay."), {
				{"resolution", Tr("Internal resolution"), fmt::format("{}x  ({}x{})", resolution, 400 * resolution, 240 * resolution), true,
					Tr("How large the 3D scenes are drawn before they are scaled to the TV. Higher is sharper and asks more of the GPU.")},
				{"filter", Tr("Texture filter"), FilterName(settings.textureFilter), true,
					Tr("Smooths textures as they are scaled up. The costliest setting here: if a game stutters, try None first.")},
				{"performance", Tr("Performance overlay"), settings.performance ? on : off, true,
					Tr("The frame rate and the emulation's speed, in the top left corner.")},
			}});
			rows.push_back({"pace", Tr("Speed"), settings.speedLimit ? Percent(settings.speedLimit) : std::string(Tr("Unlimited")), false,
				Tr("The emulated CPU's clock, and how fast the game may run."), {
				{"cpu", Tr("CPU clock"), Percent(settings.cpuClock), true,
					Tr("Below 100% can bring a slow game to full speed; above it smooths games that dropped frames on the 3DS.")},
				// tr: no speed limit
				{"speed", Tr("Speed limit"), settings.speedLimit ? Percent(settings.speedLimit) : std::string(TrC("speed limit", "None")), true,
					Tr("Above 100% hurries through slow scenes; None runs as fast as the PS5 can. For this game only.")},
			}});
			Row volume{"volume", Tr("Volume"), Percent(settings.volume), true, Tr("The game's sound. Left and Right change it by 10%.")};
			volume.slider = settings.volume / 100.0f;
			rows.push_back({"states", Tr("Save states"), TrF("Slot {0}", s_stateSlot), false,
				Tr("Five slots for this game: save exactly where you are, and come back to it."), {
				// tr: a save state slot's number, then when it was saved (or empty)
				{"slot", Tr("Slot"), fmt::format("{}  {}", s_stateSlot, time.empty() ? Tr("(empty)") : time), true,
					Tr("Which of this game's five slots to save to or load from.")},
				{"save", Tr("Save to this slot"), stateMessage, false,
					Tr("Saves the game as it is now, replacing what the slot had. Newer app versions may not load it: keep saving in the game too.")},
			}});
			Row cheatRows{"cheats", Tr("Cheats"), "", false, Tr("The game's cheats, from azahar/cheats/<title ID>.txt (Gateway format).")};
			for (size_t i = 0; i < cheats.size(); i++)
				cheatRows.rows.push_back({fmt::format("cheat{}", i), cheats[i].first, cheats[i].second ? on : off, true,
					Tr("Cross, Left or Right turns it on or off, at once; it is kept for next time.")});
			if (cheats.empty())
				cheatRows.rows.push_back({"nocheats", Tr("No cheats for this game"), "", false,
					Tr("Put them in /data/ps5cemu/azahar/cheats/<title ID>.txt, then start the game again.")});
			else
				// tr: how many of the game's cheats are on: {0} of {1}
				cheatRows.value = TrF("{0} of {1} on", std::count_if(cheats.begin(), cheats.end(), [](const auto& c) { return c.second; }), cheats.size());
			rows.push_back(cheatRows);
			rows.push_back({"amiibos", Tr("Amiibo"), amiibo, false, Tr("Hold an amiibo dump to the 3DS's reader when the game asks for one."), {
				{"amiibo", Tr("Amiibo"), amiibo, true,
					noAmiibo ? Tr("Put amiibo dumps (.bin) in /data/ps5cemu/amiibo to scan them here.") :
							   Tr("Left and Right choose an amiibo dump; Cross holds it to the reader.")},
				{"noamiibo", Tr("Take the amiibo away"), amiiboMessage, false, Tr("Takes the amiibo off the reader, as lifting it off would.")},
			}});
			rows.push_back(volume);
			rows.push_back({"controls", Tr("Controls"), settings.aOnCircle ? Tr("A on Circle") : Tr("A on Cross"), false,
				Tr("Motion controls, the sticks' deadzone and where A and B are."), {
				{"motion", Tr("Motion controls"), settings.motion ? on : off, true,
					Tr("The DualSense's gyroscope and accelerometer as the 3DS's, for the games that aim or steer by tilting.")},
				{"deadzone", Tr("Stick deadzone"), Percent(settings.deadzone), true,
					Tr("How far a stick moves before the game sees it. Raise it if something drifts when you let go.")},
				{"ab", Tr("A and B"), settings.aOnCircle ? Tr("A on Circle") : Tr("A on Cross"), true,
					Tr("A on Circle and B on Cross, where the 3DS has them, or the other way round, with X and Y swapped to match.")},
			}});
			Row library{"library", Tr("Quit to the library"), "", false,
				Tr("Leaves the game for the library. What you have not saved in the game is lost, so Cross is held.")};
			library.apart = true;
			library.hold = true;
			// loading a state, as a row too: held, as the quick action is
			Row load{"load", Tr("Load this slot"), time.empty() ? Tr("Empty") : "", false,
				Tr("Goes back to the moment the slot was saved. What you did since is lost, so Cross is held.")};
			load.hold = !time.empty();
			for (Row& row : rows)
				if (row.id == "states")
					row.rows.push_back(load);
			rows.push_back(library);
			return rows;
		}

		// The quick actions above the list: back to the game, a state saved to or loaded (held) from
		// the slot the list chooses, and the next layout of the screens
		std::vector<ps5menu::Tile> Tiles(const Settings& settings)
		{
			bool empty;
			std::string message;
			{
				std::lock_guard lock(s_mutex);
				empty = s_stateSlot > (int)s_stateTimes.size() || s_stateTimes[s_stateSlot - 1].empty();
				message = s_stateMessage;
			}
			// tr: the quick actions: big tiles at the top of the in-game menu, a word or two each
			std::vector<ps5menu::Tile> tiles{{"resume", Tr("Resume"), "resume"},
				{"save", Tr("Save state"), "save", TrF("Slot {0}", s_stateSlot)},
				{"load", Tr("Load state"), "load", empty ? TrF("Slot {0} empty", s_stateSlot) : TrF("Slot {0}", s_stateSlot), !empty},
				{"layout", Tr("Screens"), "screens", LayoutName(settings.layout)}};
			// how the last save or load went, on its tile (Made by another version, Saved slot 1...)
			for (ps5menu::Tile& tile : tiles)
				if (!message.empty() && tile.id == s_stateAction)
				{
					tile.caption = message;
					tile.help = TrF("Slot {0}: {1}", s_stateSlot, message);
				}
			return tiles;
		}

		// What a row chosen or changed does
		void Act(const ps5menu::Action& action, const Settings& settings)
		{
			const std::string& id = action.id;
			const int change = action.change;
			const bool chosen = action.chosen;
			const int resolution = std::clamp(settings.resolution, 1, 10);
			Settings next = settings;
			bool changed = true; // a setting's row; the others set it false
			if (id == "resume")
			{
				CloseMenu();
				changed = false;
			}
			else if (id == "layout")
				next.layout = (settings.layout + change + 4) % 4;
			else if (id == "swap")
				next.swapScreens = !settings.swapScreens;
			else if (id == "border")
				next.border = (std::clamp(settings.border, 0, kBorderCount - 1) + change + kBorderCount) % kBorderCount;
			else if (id == "resolution")
				next.resolution = chosen && resolution >= 10 ? 1 : std::clamp(resolution + change, 1, 10);
			else if (id == "filter")
				next.textureFilter = (settings.textureFilter + change + 6) % 6;
			else if (id == "cpu")
				next.cpuClock = NextClock(settings.cpuClock, change);
			else if (id == "speed")
				next.speedLimit = NextSpeedLimit(settings.speedLimit, change);
			else if (id == "performance")
				next.performance = !settings.performance;
			else if (id == "volume")
				next.volume = chosen && settings.volume >= 100 ? 0 : std::clamp(settings.volume + change * 10, 0, 100);
			else if (id == "motion")
				next.motion = !settings.motion;
			else if (id == "deadzone")
				next.deadzone = chosen && settings.deadzone >= 50 ? 0 : std::clamp(settings.deadzone + change * 5, 0, 50);
			else if (id == "ab")
				next.aOnCircle = !settings.aOnCircle;
			else
			{
				changed = false;
				std::lock_guard lock(s_mutex);
				if (id == "slot")
				{
					s_stateSlot = (s_stateSlot - 1 + change + kStateSlots) % kStateSlots + 1;
					s_stateAction.clear();
				}
				else if (id == "load" && (s_stateSlot > (int)s_stateTimes.size() || s_stateTimes[s_stateSlot - 1].empty()))
				{
					s_stateMessage = TrF("Slot {0} is empty", s_stateSlot);
					s_stateAction = id;
				}
				else if (id == "save" || id == "load")
				{
					// loading is held to here (side_menu.h)
					s_stateRequest = id == "save" ? s_stateSlot : -s_stateSlot;
					s_stateAction = id;
					s_stateMessage = id == "save" ? Tr("Saving…") : Tr("Loading…");
				}
				else if (id == "amiibo" && !chosen && !s_amiibo.empty())
					s_amiiboIndex = (s_amiiboIndex + change + (int)s_amiibo.size()) % (int)s_amiibo.size();
				else if (id == "amiibo" && !s_amiibo.empty())
					s_extrasRequest = {ExtrasRequest::Amiibo, s_amiiboIndex};
				else if (id == "noamiibo")
					s_extrasRequest = {ExtrasRequest::RemoveAmiibo, 0};
				else if (id.rfind("cheat", 0) == 0 && id.size() > 5 && std::isdigit((unsigned char)id[5]))
					s_extrasRequest = {ExtrasRequest::Cheat, std::atoi(id.c_str() + 5)};
				else if (id == "library")
					s_libraryRequested = true; // held to here
			}
			if (changed)
				Change(next);
		}

		void DrawMenu(float scale)
		{
			Settings settings;
			std::string name, details;
			{
				std::lock_guard lock(s_mutex);
				settings = s_settings;
				name = s_name;
				details = s_details;
			}
			if (s_menuFresh.exchange(false))
			{
				// opened: on its first row, every category closed, the shortcut's buttons not counted
				s_side.Reset();
				s_stateAction.clear();
			}
			bool close = false;
			const ps5menu::Action action = s_side.Update(MenuRows(settings), Tiles(settings), s_menuButtons, sceKernelGetProcessTime(), close);
			if (close)
			{
				CloseMenu();
				return;
			}
			if (!action.id.empty())
			{
				Act(action, settings);
				std::lock_guard lock(s_mutex);
				settings = s_settings; // as the change left them, for the rows drawn below
			}

			ImGuiIO& io = ImGui::GetIO();
			const ImVec2 origin{(io.DisplaySize.x - 1920.0f * scale) * 0.5f, (io.DisplaySize.y - 1080.0f * scale) * 0.5f};
			const Canvas canvas{ImGui::GetForegroundDrawList(), scale, origin, kColours};
			ps5menu::Header header;
			header.system = "3DS";
			// tr: the in-game menu's badge: the game waits behind the menu
			header.status = Tr("Paused");
			header.title = name;
			header.details = details;
			header.cover = g.cover;
			header.coverWidth = (float)g.coverWidth;
			header.coverHeight = (float)g.coverHeight;
			s_side.Draw(canvas, {g.head, g.body, g.row, g.small, g.chip}, header, MenuRows(settings), Tiles(settings),
				{{"cross", Tr("Choose")}, {"leftright", Tr("Change")}, {"circle", s_side.Open().empty() ? Tr("Back to the game") : Tr("Back")}});
		}

		// -- the keyboard ----------------------------------------------------------------------------

		std::string s_typed;		  // the renderer's
		bool s_shift = false;		  // capitals and the second symbols
		constexpr const char* kKeys[2][4] = {
			{"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-"},
			{"!?#$%&*()+", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM;:_"},
		};

		void FinishKeyboard(int button)
		{
			std::lock_guard lock(s_mutex);
			s_keyboardResult = s_typed;
			s_keyboardButton = button;
			s_keyboardDone = true;
			s_keyboardOpen = false;
		}

		void Type(const std::string& characters, int maxLength)
		{
			if (maxLength <= 0 || (int)(s_typed.size() + characters.size()) <= maxLength)
				s_typed += characters;
		}

		// The keyboard, laid out on the launcher's 1920x1080 as the menu is: what the game asks for,
		// the text, the keys, then the game's own buttons (the last confirms, as Options does)
		void DrawKeyboard(float scale)
		{
			ImGuiIO& io = ImGui::GetIO();
			KeyboardRequest request;
			std::string error;
			{
				std::lock_guard lock(s_mutex);
				request = s_keyboard;
				error = s_keyboardError;
				if (s_keyboardFresh)
				{
					s_typed.clear();
					s_shift = false;
					s_keyboardFresh = false;
				}
			}
			if (request.buttons.empty())
				request.buttons = {Tr("OK")};
			const ImVec2 origin{(io.DisplaySize.x - 1920.0f * scale) * 0.5f, (io.DisplaySize.y - 1080.0f * scale) * 0.5f};
			ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
			ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
			ImGui::SetNextWindowFocus();
			ImGui::PushStyleColor(ImGuiCol_NavHighlight, IM_COL32(0, 0, 0, 0));
			constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse;
			if (ImGui::Begin("PS5CEMU-HAR##Keyboard3ds", nullptr, kFlags))
			{
				const bool appearing = ImGui::IsWindowAppearing();
				if (appearing)
					ImGui::GetCurrentContext()->NavDisableHighlight = false;
				const Canvas canvas{ImGui::GetWindowDrawList(), scale, origin, kColours};
				canvas.draw->AddRectFilled({0, 0}, io.DisplaySize, kColours.dim);
				canvas.Panel(260, 110, 1400, 860);
				// tr: over the 3DS's keyboard: what the game asks to have typed
				canvas.Text(g.small, 20, 300, 136, kColours.kicker, ui::Upper(Tr("The game asks for"), ps5lang::Current() == "tr"));
				canvas.Text(g.head, 32, 300, 170, kColours.title, request.hint.empty() ? Tr("Some text") : request.hint, 1320);
				canvas.Row(300, 236, 1320, 72, false);
				canvas.Text(g.row, 24, 330, 258, kColours.text, s_typed + "_");
				if (request.maxLength > 0)
					canvas.TextRight(g.small, 20, 1590, 262, kColours.accent, fmt::format("{} / {}", s_typed.size(), request.maxLength));
				if (!error.empty())
					canvas.Text(g.small, 20, 300, 320, kColours.accent, error, 1320);

				// a key: true when chosen (Cross, or a touch of the touchpad's click)
				int keyIndex = 0;
				auto key = [&](float x, float y, float width, const std::string& label) {
					ImGui::SetCursorScreenPos(canvas.At(x, y));
					const bool chosen = ImGui::InvisibleButton(fmt::format("##key{}", keyIndex).c_str(), {width * scale, 72 * scale});
					if (keyIndex++ == 0 && appearing)
						ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
					canvas.Row(x, y, width, 72, ImGui::IsItemFocused());
					const ImVec2 size = g.row->CalcTextSizeA(24 * scale, FLT_MAX, 0.0f, label.c_str());
					canvas.draw->AddText(g.row, 24 * scale, canvas.At(x + width / 2 - size.x / scale / 2, y + 22), kColours.text, label.c_str());
					return chosen;
				};
				const auto& rows = kKeys[s_shift ? 1 : 0];
				for (int row = 0; row < 4; row++)
					for (int column = 0; column < 10; column++)
					{
						const std::string character(1, rows[row][column]);
						if (key(325 + column * 128, 360 + row * 84, 118, character))
							Type(character, request.maxLength);
					}
				// tr: the 3DS keyboard's keys
				if (key(325, 696, 246, s_shift ? Tr("Shift: on") : Tr("Shift")))
					s_shift = !s_shift;
				if (key(581, 696, 502, Tr("Space")))
					Type(" ", request.maxLength);
				if (key(1093, 696, 502, Tr("Delete")) && !s_typed.empty())
					s_typed.pop_back();
				// the game's buttons, right-aligned on the last row
				const float buttonWidth = 300;
				const int count = (int)request.buttons.size();
				for (int button = 0; button < count; button++)
				{
					const float x = 1595 - (count - button) * (buttonWidth + 10) + 10;
					if (key(x, 800, buttonWidth, request.buttons[button]))
						FinishKeyboard(button);
				}

				canvas.draw->AddLine(canvas.At(300, 900), canvas.At(1620, 900), kColours.line, scale);
				float x = 300;
				x = canvas.Hint(g.small, x, 915, "cross", Tr("Type"));
				x = canvas.Hint(g.small, x, 915, "circle", Tr("Delete"));
				x = canvas.Hint(g.small, x, 915, "triangle", Tr("Shift"));
				canvas.Hint(g.small, x, 915, "options", request.buttons.back());
			}
			ImGui::End();
			ImGui::PopStyleColor();

			// a USB keyboard types as the keys do; Enter confirms with the last button
			// Circle deletes, Triangle shifts, Options confirms with the last button (as on the Wii U's
			// keyboard); not while the touchpad is held for a shortcut
			if (!(s_buttons & ps5pad::kTouchPad))
			{
				if ((s_pressed & ps5pad::kCircle) && !s_typed.empty())
					s_typed.pop_back();
				if (s_pressed & ps5pad::kTriangle)
					s_shift = !s_shift;
				if (s_pressed & ps5pad::kOptions)
					FinishKeyboard((int)request.buttons.size() - 1);
			}
		}

		void DrawPerformance(float scale)
		{
			double fps, speed;
			std::string breakdown;
			{
				std::lock_guard lock(s_mutex);
				fps = s_fps;
				speed = s_speed;
				breakdown = s_breakdown;
			}
			std::string text = fmt::format("{:.0f} FPS   {:.0f}%", fps, speed);
			if (!breakdown.empty())
				text += "\n" + breakdown;
			ImDrawList* draw = ImGui::GetForegroundDrawList();
			const ImVec2 at{24 * scale, 20 * scale};
			const ImVec2 size = g.small->CalcTextSizeA(20 * scale, FLT_MAX, 0.0f, text.c_str());
			draw->AddRectFilled({at.x - 12 * scale, at.y - 8 * scale}, {at.x + size.x + 12 * scale, at.y + size.y + 8 * scale},
				kColours.panel, 10 * scale);
			draw->AddText(g.small, 20 * scale, at, kColours.title, text.c_str());
		}
	}

	void Start(const std::string& name, uint64_t titleId, const Settings& settings)
	{
		std::lock_guard lock(s_mutex);
		s_name = name;
		s_titleId = titleId;
		s_settings = settings;
		s_changed = false;
	}

	void SetGame(const std::string& details, const std::string& coverPath)
	{
		std::lock_guard lock(s_mutex);
		s_details = details;
		s_coverPath = coverPath;
	}

	void ToggleMenu()
	{
		if (s_open)
		{
			CloseMenu();
			return;
		}
		s_openedAt = sceKernelGetProcessTime();
		s_menuFresh = true;
		s_open = true;
	}

	bool MenuOpen()
	{
		return s_open;
	}

	bool TakeChanges(Settings& settings)
	{
		std::lock_guard lock(s_mutex);
		if (!s_changed)
			return false;
		s_changed = false;
		settings = s_settings;
		return true;
	}

	bool TakeLibraryRequest()
	{
		return s_libraryRequested.exchange(false);
	}

	void SetBorder(int theme, std::vector<uint8_t> rgba, int width, int height)
	{
		std::lock_guard lock(s_mutex);
		s_borderTheme = rgba.empty() ? 0 : theme;
		s_borderPixels = std::move(rgba);
		s_borderWidth = width;
		s_borderHeight = height;
		s_borderFresh = true;
	}

	void SetScreens(const std::vector<ScreenRect>& screens, float width, float height)
	{
		std::lock_guard lock(s_mutex);
		s_screens = screens;
		s_screensWidth = width;
		s_screensHeight = height;
	}

	void SetStateSlots(const std::vector<std::string>& times)
	{
		std::lock_guard lock(s_mutex);
		s_stateTimes = times;
	}

	void SetStateMessage(const std::string& message)
	{
		std::lock_guard lock(s_mutex);
		s_stateMessage = message;
	}

	void SetExtras(const std::vector<std::string>& amiibo, const std::vector<std::pair<std::string, bool>>& cheats)
	{
		std::lock_guard lock(s_mutex);
		s_amiibo = amiibo;
		s_cheats = cheats;
	}

	void SetExtrasMessage(const std::string& message)
	{
		std::lock_guard lock(s_mutex);
		s_extrasMessage = message;
	}

	bool TakeExtrasRequest(ExtrasRequest& request)
	{
		std::lock_guard lock(s_mutex);
		if (s_extrasRequest.kind == ExtrasRequest::None)
			return false;
		request = s_extrasRequest;
		s_extrasRequest = {};
		return true;
	}

	bool TakeStateRequest(bool& load, int& slot)
	{
		std::lock_guard lock(s_mutex);
		if (!s_stateRequest)
			return false;
		load = s_stateRequest < 0;
		slot = std::abs(s_stateRequest);
		s_stateRequest = 0;
		return true;
	}

	void OpenKeyboard(const KeyboardRequest& request)
	{
		std::lock_guard lock(s_mutex);
		s_keyboard = request;
		s_keyboardError.clear();
		s_keyboardFresh = true;
		s_keyboardDone = false;
		s_keyboardOpen = true;
	}

	bool KeyboardOpen()
	{
		return s_keyboardOpen;
	}

	bool TakeKeyboardResult(std::string& text, int& button)
	{
		std::lock_guard lock(s_mutex);
		if (!s_keyboardDone)
			return false;
		s_keyboardDone = false;
		text = s_keyboardResult;
		button = s_keyboardButton;
		return true;
	}

	void KeyboardError(const std::string& message)
	{
		std::lock_guard lock(s_mutex);
		s_keyboardError = message;
		s_keyboardOpen = true; // the text typed stays, to be corrected
	}

	void SetPerformance(double fps, double speed, const std::string& breakdown)
	{
		std::lock_guard lock(s_mutex);
		s_fps = fps;
		s_speed = speed;
		s_breakdown = breakdown;
	}

	void Record(const Target& target)
	{
		if (g.failed)
			return;
		const float scale = std::max(1.0f, target.height / 1080.0f);
		// Azahar's renderer made again (a loaded save state): its device is new even when it has the
		// old one's handle, as it often does (the menu then drew with the old device's objects, and
		// the driver crashed)
		if (g.ready && (target.generation != g.generation || target.device != g.device))
		{
			if (target.insideRenderPass)
				return; // the pass before it starts over
			if (!Reattach(target))
			{
				g.failed = true;
				return;
			}
		}
		if (!target.insideRenderPass)
		{
			g.pending = false;
			bool performance;
			{
				std::lock_guard lock(s_mutex);
				performance = s_settings.performance;
			}
			const bool open = s_open;
			const bool keyboard = s_keyboardOpen;
			int borderTheme;
			bool borderFresh;
			{
				std::lock_guard lock(s_mutex);
				borderTheme = s_borderTheme;
				borderFresh = s_borderFresh;
			}
			// textures given up, once the frames that drew them are long done
			for (size_t i = 0; i < g.retired.size();)
				if (++g.retired[i].second > 16)
				{
					ImGui_ImplVulkan_DeleteTexture(g.retired[i].first);
					g.retired.erase(g.retired.begin() + i);
				}
				else
					i++;
			const bool border = borderTheme > 0 && (g.border || borderFresh);
			if (!open && !performance && !keyboard && !border)
			{
				s_buttons = 0; // the menu sees a fresh controller when it next opens
				return;
			}
			if (!g.ready)
			{
				if (!Initialize(target, scale))
				{
					g.failed = true;
					return;
				}
				g.ready = true;
			}
			if (target.renderPass != g.renderPass && target.width == g.width && target.height == g.height)
			{
				// the screen's pass made again: Azahar starts its presentation over when a save state is
				// loaded. The new pass has the old one's formats, so ImGui's pipeline is compatible with
				// it and only the handle changes (tearing ImGui down and up again crashed in the driver).
				g.renderPass = target.renderPass;
				ps5log::Line("[ingame3ds] the screen's render pass was made again: the menu follows it");
			}
			if (target.renderPass != g.renderPass)
				return; // another pass than the screen's (a screenshot's)
			ImGui::SetCurrentContext(g.context);
			// the font, once; its staging buffer goes once the copy has long run
			if (!g.fontsUploaded)
			{
				ImGui_ImplVulkan_CreateFontsTexture(target.commandBuffer);
				g.fontsUploaded = true;
				g.uploadAge = 0;
			}
			else if (g.uploadAge >= 0 && ++g.uploadAge > 16)
			{
				ImGui_ImplVulkan_DestroyFontUploadObjects();
				g.uploadAge = -1;
			}
			// a new border picture: uploaded here, outside the render pass, as the font is (the
			// picture is kept, for a new device)
			if (borderFresh)
			{
				std::vector<uint8_t> pixels;
				int width, height;
				{
					std::lock_guard lock(s_mutex);
					pixels = s_borderPixels;
					width = s_borderWidth;
					height = s_borderHeight;
					s_borderFresh = false;
				}
				if (g.border)
					g.retired.emplace_back(g.border, 0);
				g.border = nullptr;
				if (!pixels.empty())
					g.border = ImGui_ImplVulkan_GenerateTexture(target.commandBuffer, pixels, {width, height});
			}
			// the game's box art, the first time the menu opens
			if (open && !g.coverTried)
			{
				g.coverTried = true;
				std::string path;
				{
					std::lock_guard lock(s_mutex);
					path = s_coverPath;
				}
				std::vector<uint8_t> rgba;
				int width = 0, height = 0;
				if (!path.empty() && ps5tga::Load(path, 264, 352, rgba, width, height))
				{
					g.cover = ImGui_ImplVulkan_GenerateTexture(target.commandBuffer, rgba, {width, height});
					g.coverWidth = width;
					g.coverHeight = height;
				}
				else
					ps5log::Line("[ingame3ds] no box art for the menu ({})", path.empty() ? "none fetched" : path);
			}
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = {(float)target.width, (float)target.height};
			const uint64_t now = sceKernelGetProcessTime();
			io.DeltaTime = g.lastFrame && now > g.lastFrame ? std::min((now - g.lastFrame) / 1e6f, 0.25f) : 1.0f / 60.0f;
			g.lastFrame = now;
			Input();
			ImGui::NewFrame();
			if (borderTheme > 0 && g.border)
				DrawBorder(borderTheme);
			if (open)
				DrawMenu(scale);
			else if (keyboard)
				DrawKeyboard(scale);
			if (performance)
				DrawPerformance(scale);
			ImGui::Render();
			g.pending = true;
			return;
		}
		if (!g.pending || target.renderPass != g.renderPass)
			return;
		g.pending = false;
		ImGui::SetCurrentContext(g.context);
		ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), target.commandBuffer);
	}
}
