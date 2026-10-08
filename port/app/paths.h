// SPDX-License-Identifier: GPL-3.0-or-later
// PS5Cemu: where things live on the console.
//
//   /data/homebrew/PPSA99360/        the app (eboot.bin, sce_sys, assets) as ShadowMountPlus mounts it
//   /app0/                           the same, read-only, from inside the app's sandbox (AppDir)
//   /data/ps5cemu/                   everything PS5Cemu writes; survives app updates
//     settings.xml, ps5cemu.json     Cemu's settings and the launcher's
//     log.txt                        Cemu's log
//     controllerProfiles/            Cemu's controller profiles
//     mlc01/                         the Wii U's internal storage: installed games, updates, DLC, saves
//     games/                         the default game files folder (.wua, .wud/.wux, .rpx folders)
//     keys.txt                       disc keys for encrypted .wud/.wux dumps
//     graphicPacks/                  community packs (downloadedGraphicPacks/) and your own
//     cache/                         shader and pipeline caches (radv/: RADV's own)
//     covers/                        game icons converted for the launcher
//     logs/                          boot.log (the port), boot.prev.log and boot.2-4.log the sessions
//                                    before, cemu.prev.txt and cemu.2-4.txt their log.txt

#pragma once

#include <cstdio>
#include <initializer_list>
#include <string>
#include <sys/stat.h>

namespace ps5paths
{
	constexpr const char* kTitleId = "PPSA99360";
	constexpr const char* kInstallDir = "/data/homebrew/PPSA99360";
	constexpr const char* kMountedEboot = "/data/homebrew/PPSA99360/eboot.bin";
	// where the PS5 mounts the app it runs, whatever ShadowMountPlus mounted it from (a folder on
	// /data, an extended storage or USB drive, or an image)
	constexpr const char* kSystemMount = "/system_ex/app/PPSA99360";
	// ShadowMountPlus's record of that source: mount.lnk holds the folder's path, mount_img.lnk is
	// there for an image
	constexpr const char* kMountRecord = "/user/app/PPSA99360/mount.lnk";
	constexpr const char* kImageMountRecord = "/user/app/PPSA99360/mount_img.lnk";

// everything PS5CEMU-HAR writes: on the console /data/ps5cemu; the launcher's preview on a PC
// (tools/preview-shell.sh) gives a folder of its own
#ifndef PS5CEMU_DATA
#define PS5CEMU_DATA "/data/ps5cemu"
#endif
	constexpr const char* kRoot = PS5CEMU_DATA;
	constexpr const char* kMlc = PS5CEMU_DATA "/mlc01";
	constexpr const char* kGames = PS5CEMU_DATA "/games";
	constexpr const char* kCache = PS5CEMU_DATA "/cache";
	constexpr const char* kRadvCache = PS5CEMU_DATA "/cache/radv";
	constexpr const char* kLogs = PS5CEMU_DATA "/logs";
	constexpr const char* kLauncherSettings = PS5CEMU_DATA "/ps5cemu.json";
	constexpr const char* kCovers = PS5CEMU_DATA "/covers";

	// The app's own folder. The sandbox mounts it as /app0, but a process the HEN has jailbroken
	// sees the console's root, which has no /app0: there the app is read where the PS5 mounts it,
	// which follows ShadowMountPlus wherever it found the app (an install on a USB drive with an older
	// copy left in /data/homebrew included), else where it is usually installed, or where the sandbox
	// mounts it from (as ProsperoEden's storage_paths.h finds its own). Looked for afresh each call.
	inline std::string FindAppDir()
	{
		for (const char* candidate : {"/app0", kSystemMount, kInstallDir, "/mnt/sandbox/PPSA99360_000/app0"})
		{
			struct stat info{};
			if (stat((std::string(candidate) + "/eboot.bin").c_str(), &info) == 0 && S_ISREG(info.st_mode))
				return std::string(candidate);
		}
		return std::string(kInstallDir);
	}

	// FindAppDir, decided on first use, which comes after ps5privilege::ReachFolders: it and
	// ps5privilege::Acquire may have the elevation helper move the process to the console's root,
	// which takes /app0 away (#26: an /app0 decided before that left the launcher without its font)
	inline const std::string& AppDir()
	{
		static const std::string directory = FindAppDir();
		return directory;
	}

	// The folder ShadowMountPlus mounted the running app from, by its record (kMountRecord), when it
	// holds the app; empty without one. image: whether the app was mounted from an image instead,
	// whose files can't be replaced one by one.
	inline std::string MountSource(bool* image = nullptr)
	{
		struct stat info{};
		if (image)
			*image = stat(kImageMountRecord, &info) == 0;
		std::string path;
		if (FILE* file = std::fopen(kMountRecord, "rb"))
		{
			char text[512] = {};
			path.assign(text, std::fread(text, 1, sizeof(text) - 1, file));
			std::fclose(file);
		}
		while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' ' || path.back() == '\0' || path.back() == '/'))
			path.pop_back();
		if (path.size() < 2 || path[0] != '/' || stat((path + "/eboot.bin").c_str(), &info) != 0 || !S_ISREG(info.st_mode))
			return {};
		return path;
	}

	inline std::string Eboot() { return AppDir() + "/eboot.bin"; }
	inline std::string Assets() { return AppDir() + "/assets"; }
	// Cemu's read-only data (gameProfiles, resources)
	inline std::string CemuData() { return Assets() + "/cemu"; }
	inline std::string BundledGraphicPacks() { return Assets() + "/graphicPacks"; }
}
