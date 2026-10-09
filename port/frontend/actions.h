// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: what the launcher (shell/) does besides drawing (docs/UI-REDESIGN.md, 9.7): the folder
// browser's listing (with sceKernelGetdents, as ProsperoEden's does) and the drives, what a folder
// holds for each side, copying the logs to USB, clearing the shader caches, the Artic Base address, a
// mapping's DualSense input, and the words it shows.

#pragma once

#include "../app/emulator.h"
#include "../ps5/pad.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace ps5actions
{
	// words
	std::string Hex(uint64_t value);						// 16 hex digits
	std::string ShortPath(const std::string& path, size_t limit); // its end, when it is too long
	std::string Plural(int count, const char* one, const char* many);
	std::string Lower(std::string text);
	std::string Upper(std::string text);
	std::string Gigabytes(uint64_t bytes);
	// "a  /  b  /  c" from the parts that are not empty
	std::string Join(std::initializer_list<std::string> parts, const char* between = "  /  ");
	const char* TypeName(ps5emu::EmulatedType type);		 // "Wii U GamePad"
	const char* KindName(ps5emu::InstallCandidate::Kind kind); // "Update"
	const char* CiaKind(uint64_t titleId);					 // "Game", "DLC"...

	// files and folders
	std::string JoinPath(const std::string& folder, const std::string& name);
	std::string ParentPath(const std::string& folder);
	bool IsFolder(const std::string& path);
	bool IsFile(const std::string& path);
	// the drives plugged in (USB, the PS5's extended storage)
	std::vector<std::string> ConnectedDrives();
	std::string DriveName(const std::string& path); // "USB drive 1"
	// a folder's subfolders (folders) or files, sorted without regard to case; ok false when it
	// cannot be read
	std::vector<std::string> ListEntries(const std::string& path, bool folders, bool& ok);
	bool Has3dsExtension(const std::string& name); // a 3DS game's
	bool HasDsExtension(const std::string& name);  // a DS game's
	// the games right in a folder, as each side would find them; -1 when it cannot be read
	int Count3dsGames(const std::string& folder);
	int CountDsGames(const std::string& folder);
	int CountGames(const std::string& folder);

	// Diagnostics: what each did, as its row shows it
	std::string CopyLogsToUsb();
	std::string ClearShaderCaches(bool n3ds);

	// Artic Base: an IPv4 address's four numbers; the PS5's own, as a first guess
	bool ParseAddress(const std::string& text, std::array<int, 4>& octets);
	std::array<int, 4> OwnAddress();

	// The DualSense input pressed now, or None (a mapping's capture). Sticks and triggers count past
	// halfway.
	ps5emu::PadInput Pressed(const ps5pad::Data& data);
}
