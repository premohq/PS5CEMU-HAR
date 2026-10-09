// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the new launcher on the console (shell.h's Host). It draws through the RADV the app
// links, on VideoOut's display surface (ps5vk, the one Cemu's renderer presents to) at 3840 x 2160,
// from the app's assets/ui, with the console's own fonts for the scripts Lexend lacks, and keeps its
// pipeline cache with the app's other caches. Gfx::Stop gives VideoOut back before a game: the
// driver keeps it open, and main_ps5.cpp asks it for the game's refresh rate again
// (ps5display::ConfigureOutput, PS5_Mesa patch 0006).

#include "../shell.h"
#include "../../app/paths.h"
#include "../../ps5/display.h"
#include "../../ps5/kernel.h"
#include "../../ps5/log.h"
#include "../../ps5/vulkan_display.h"

#include <cstdlib>
#include <filesystem>
#include <iterator>

namespace ps5shell
{
	Host DefaultHost()
	{
		Host host;
		host.getInstanceProcAddr = ps5vk::GetInstanceProcAddr();
		host.target = [] {
			ui::Target target;
			target.width = ps5display::kWidth;
			target.height = ps5display::kHeight;
			target.instanceExtensions.assign(std::begin(ps5vk::kSurfaceExtensions), std::end(ps5vk::kSurfaceExtensions));
			target.createSurface = [](VkInstance instance, std::string& error) { return ps5vk::CreateDisplaySurface(instance, error); };
			// RADV's shader cache where Cemu's renderer keeps it (app/emulator.cpp sets the same), there
			// before the launcher's device reads where it is; a jailbroken process has no /app0 for it
			std::error_code ec;
			std::filesystem::create_directories(ps5paths::kRadvCache, ec);
			setenv("MESA_SHADER_CACHE_DIR", ps5paths::kRadvCache, 0);
			target.pipelineCache = std::string(ps5paths::kCache) + "/ui-pipelines.bin";
			return target;
		};
		host.assets = ps5paths::Assets() + "/ui";
		// the font is the launcher's first file: not where the app's folder was found (the folder moved
		// under the process, as the sandbox helper moves it, #34), it is looked for where the app is now
		// (here only: other threads may be reading the folder found before)
		std::error_code missing;
		if (!std::filesystem::exists(host.assets + "/fonts/lexend.sdf", missing))
		{
			const std::string found = ps5paths::FindAppDir() + "/assets/ui";
			ps5log::Line("[ui] {} has no launcher font: {} instead", host.assets, found);
			host.assets = found;
		}
		// the console's system fonts (CJK, Thai, Arabic...), where its firmwares keep them; the ones
		// missing are skipped, and the boot log says how many were found on first use
		host.fontFolders = {"/system/common/font", "/system_ex/common/font", "/preinst/common/font", "/system/common/font2"};
		host.clock = [] { return sceKernelGetProcessTime() / 1e6; };
		return host;
	}
}
