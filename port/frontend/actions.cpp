// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: what the launchers do besides drawing (actions.h), moved unchanged from launcher.cpp.

#include "actions.h"
#include "../app/paths.h"
#include "../ps5/kernel.h"
#include "../ps5/log.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ps5actions
{
	std::string Hex(uint64_t value)
	{
		return fmt::format("{:016X}", value);
	}

	// A path that fits a label: its end is what tells folders apart.
	std::string ShortPath(const std::string& path, size_t limit)
	{
		return path.size() <= limit ? path : "..." + path.substr(path.size() - (limit - 3));
	}

	std::string Plural(int count, const char* one, const char* many)
	{
		return fmt::format("{} {}", count, count == 1 ? one : many);
	}

	std::string Lower(std::string text)
	{
		for (char& c : text)
			c = (char)std::tolower((unsigned char)c);
		return text;
	}

	std::string Upper(std::string text)
	{
		for (char& c : text)
			c = (char)std::toupper((unsigned char)c);
		return text;
	}

	std::string Gigabytes(uint64_t bytes)
	{
		return fmt::format("{:.1f} GB", bytes / 1e9);
	}

	// "a  /  b  /  c" from the parts that are not empty
	std::string Join(std::initializer_list<std::string> parts, const char* between)
	{
		std::string out;
		for (const std::string& part : parts)
			if (!part.empty())
				out += (out.empty() ? "" : between) + part;
		return out;
	}

	// A folder's entry as a path; an entry that is a path already (a drive's, below) stays one
	std::string JoinPath(const std::string& folder, const std::string& name)
	{
		if (name.starts_with('/'))
			return name;
		return folder == "/" ? "/" + name : folder + "/" + name;
	}

	// The drives plugged in (USB, and the PS5's extended storage), for the folder browser to list
	// first: otherwise reached only by climbing to / and into /mnt
	std::vector<std::string> ConnectedDrives()
	{
		std::vector<std::string> drives;
		for (int i = 0; i < 8; i++)
			drives.push_back(fmt::format("/mnt/usb{}", i));
		drives.push_back("/mnt/ext0");
		drives.push_back("/mnt/ext1");
		std::erase_if(drives, [](const std::string& drive) {
			struct stat info{};
			return stat(drive.c_str(), &info) != 0 || !S_ISDIR(info.st_mode);
		});
		return drives;
	}

	static bool CopyFile(const std::string& from, const std::string& to)
	{
		std::ifstream in(from, std::ios::binary);
		if (!in)
			return false;
		std::ofstream out(to, std::ios::binary | std::ios::trunc);
		out << in.rdbuf();
		return (bool)out;
	}

	// Diagnostics > Copy logs to USB: what a report needs, in a folder of its own on the first
	// USB drive that takes one. What it did, as the row shows it.
	std::string CopyLogsToUsb()
	{
		char stamp[32] = "logs";
		const std::time_t now = std::time(nullptr);
		if (const std::tm* local = std::localtime(&now))
			std::strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H%M%S", local);
		const std::string root = ps5paths::kRoot;
		const std::string logs = ps5paths::kLogs;
		std::vector<std::string> files = {logs + "/boot.log", root + "/log.txt", root + "/azahar/log/azahar_log.txt",
			ps5paths::kLauncherSettings, root + "/settings.xml"};
		// the sessions before, each with Cemu's log of it (port/ps5/log.cpp)
		for (const char* age : {"prev", "2", "3", "4"})
		{
			files.push_back(fmt::format("{}/boot.{}.log", logs, age));
			files.push_back(fmt::format("{}/cemu.{}.txt", logs, age));
		}
		for (int drive = 0; drive < 8; drive++)
		{
			const std::string folder = fmt::format("/mnt/usb{}/PS5CEMU-HAR-logs-{}", drive, stamp);
			if (!IsFolder(fmt::format("/mnt/usb{}", drive)) || mkdir(folder.c_str(), 0777) != 0)
				continue;
			int copied = 0;
			for (const std::string& file : files)
				if (IsFile(file) && CopyFile(file, folder + file.substr(file.find_last_of('/'))))
					copied++;
			ps5log::Line("[launcher] {} log files copied to {}", copied, folder);
			return fmt::format("{} files to USB drive {}", copied, drive + 1);
		}
		ps5log::Line("[launcher] no USB drive took the logs");
		return "No USB drive found";
	}

	// Diagnostics > Clear shader caches, for the side it is on: Cemu's (its transferable,
	// precompiled and driver caches: the .bin files, so a copy set aside by hand stays) or
	// Azahar's. Every game builds its shaders again as it meets them.
	std::string ClearShaderCaches(bool n3ds)
	{
		namespace fs = std::filesystem;
		const std::string folder = n3ds ? std::string(ps5paths::kRoot) + "/azahar/shaders" : std::string(ps5paths::kCache) + "/shaderCache";
		std::error_code ec;
		int files = 0;
		uintmax_t bytes = 0;
		std::vector<fs::path> doomed;
		for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
			if (it->is_regular_file(ec) && (n3ds || it->path().extension() == ".bin"))
				doomed.push_back(it->path());
		for (const fs::path& file : doomed)
		{
			const uintmax_t size = fs::file_size(file, ec);
			if (fs::remove(file, ec))
			{
				files++;
				bytes += ec ? 0 : size;
			}
		}
		ps5log::Line("[launcher] shader caches cleared in {}: {} files, {} MiB", folder, files, bytes >> 20);
		return files ? fmt::format("{} files deleted ({} MB)", files, bytes / 1000000) : "None to delete";
	}

	// How the browser names a drive: "USB drive 1", "Extended storage 1"
	std::string DriveName(const std::string& path)
	{
		if (path.starts_with("/mnt/usb"))
			return fmt::format("USB drive {}", std::atoi(path.c_str() + 8) + 1);
		if (path.starts_with("/mnt/ext"))
			return fmt::format("Extended storage {}", std::atoi(path.c_str() + 8) + 1);
		return path;
	}

	std::string ParentPath(const std::string& folder)
	{
		const size_t slash = folder.find_last_of('/');
		return slash == 0 || slash == std::string::npos ? "/" : folder.substr(0, slash);
	}

	bool IsFolder(const std::string& path)
	{
		struct stat info{};
		return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
	}

	bool IsFile(const std::string& path)
	{
		struct stat info{};
		return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
	}

	// The names of a folder's subfolders (folders) or files, sorted without regard to case. An
	// entry that cannot be read is skipped: the browser walks the whole console filesystem.
	std::vector<std::string> ListEntries(const std::string& path, bool folders, bool& ok)
	{
		ok = false;
		std::vector<std::string> names;
		const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY);
		if (fd < 0)
			return names;
		std::vector<char> buffer(65536);
		for (;;)
		{
			const int count = sceKernelGetdents(fd, buffer.data(), (int)buffer.size());
			if (count == 0)
			{
				ok = true;
				break;
			}
			if (count < 0 || count > (int)buffer.size())
				break;
			for (size_t offset = 0; offset + offsetof(dirent, d_name) < (size_t)count;)
			{
				uint16_t length;
				uint8_t type;
				std::memcpy(&length, buffer.data() + offset + offsetof(dirent, d_reclen), sizeof(length));
				std::memcpy(&type, buffer.data() + offset + offsetof(dirent, d_type), sizeof(type));
				if (length <= offsetof(dirent, d_name) || offset + length > (size_t)count)
					break;
				const char* name = buffer.data() + offset + offsetof(dirent, d_name);
				const std::string entry(name, strnlen(name, length - offsetof(dirent, d_name)));
				offset += length;
				if (entry.empty() || entry == "." || entry == "..")
					continue;
				bool isFolder = type == DT_DIR, isFile = type == DT_REG;
				if (type == DT_UNKNOWN || type == DT_LNK)
				{
					const std::string full = JoinPath(path, entry);
					isFolder = IsFolder(full);
					isFile = IsFile(full);
				}
				if (folders ? isFolder : isFile)
					names.push_back(entry);
			}
		}
		close(fd);
		std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
		return names;
	}

	bool Has3dsExtension(const std::string& name)
	{
		const std::string lower = Lower(name);
		// the 3DS's, and the DS's the 3DS side lists too (port/melonds)
		for (const char* extension : {".3ds", ".cci", ".cxi", ".cia", ".3dsx", ".app", ".elf", ".axf", ".z3ds", ".zcci", ".zcxi", ".z3dsx",
				 ".nds", ".srl", ".dsi"})
			if (lower.size() > std::strlen(extension) && lower.ends_with(extension))
				return true;
		return false;
	}

	// The 3DS games right in a folder (Azahar's library looks in the folders in it too). -1: unreadable.
	int Count3dsGames(const std::string& folder)
	{
		bool ok = false;
		const auto files = ListEntries(folder, false, ok);
		return ok ? (int)std::count_if(files.begin(), files.end(), Has3dsExtension) : -1;
	}

	// What Cemu would find in a folder: Wii U images and executables, and folders that hold an
	// unpacked game (code, content, meta) or an installable one (title.tmd). -1: unreadable.
	int CountGames(const std::string& folder)
	{
		bool ok = false;
		int count = 0;
		for (const auto& name : ListEntries(folder, false, ok))
		{
			const std::string lower = Lower(name);
			for (const char* extension : {".wua", ".wud", ".wux", ".rpx", ".elf", ".wuhb"})
				if (lower.size() > std::strlen(extension) && lower.ends_with(extension))
				{
					count++;
					break;
				}
		}
		if (!ok)
			return -1;
		for (const auto& name : ListEntries(folder, true, ok))
		{
			const std::string path = JoinPath(folder, name);
			if (IsFolder(path + "/code") || IsFile(path + "/title.tmd"))
				count++;
		}
		return count;
	}

	const char* TypeName(ps5emu::EmulatedType type)
	{
		switch (type)
		{
		case ps5emu::EmulatedType::GamePad: return "Wii U GamePad";
		case ps5emu::EmulatedType::Pro: return "Wii U Pro Controller";
		case ps5emu::EmulatedType::Classic: return "Classic Controller";
		case ps5emu::EmulatedType::Wiimote: return "Wii Remote";
		case ps5emu::EmulatedType::Nunchuk: return "Wii Remote + Nunchuk";
		case ps5emu::EmulatedType::None: break;
		}
		return "No controller";
	}

	const char* KindName(ps5emu::InstallCandidate::Kind kind)
	{
		switch (kind)
		{
		case ps5emu::InstallCandidate::Kind::Game: return "Game";
		case ps5emu::InstallCandidate::Kind::Update: return "Update";
		case ps5emu::InstallCandidate::Kind::Dlc: return "DLC";
		case ps5emu::InstallCandidate::Kind::System: return "System title";
		case ps5emu::InstallCandidate::Kind::None: break;
		}
		return "";
	}

	// An IPv4 address's four numbers, from "a.b.c.d". False when it is not one.
	bool ParseAddress(const std::string& text, std::array<int, 4>& octets)
	{
		unsigned a, b, c, d;
		char extra;
		if (std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
			return false;
		octets = {(int)a, (int)b, (int)c, (int)d};
		return true;
	}

	// The PS5's own address, a first guess at the 3DS's on the same network: the one a UDP socket
	// would send from (connecting a UDP socket sends nothing).
	std::array<int, 4> OwnAddress()
	{
		std::array<int, 4> octets{192, 168, 1, 2};
		const int fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0)
			return octets;
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_port = htons(53);
		to.sin_addr.s_addr = htonl(0x08080808);
		sockaddr_in from{};
		socklen_t length = sizeof(from);
		if (connect(fd, (const sockaddr*)&to, sizeof(to)) == 0 && getsockname(fd, (sockaddr*)&from, &length) == 0 &&
			from.sin_addr.s_addr != 0)
		{
			const uint32_t address = ntohl(from.sin_addr.s_addr);
			for (int i = 0; i < 4; i++)
				octets[i] = (int)((address >> (24 - 8 * i)) & 255);
		}
		close(fd);
		return octets;
	}

	// The DualSense input pressed now, or None. Sticks and triggers count past halfway.
	ps5emu::PadInput Pressed(const ps5pad::Data& data)
	{
		using ps5emu::PadInput;
		static constexpr std::pair<uint32_t, PadInput> kButtons[] = {
			{ps5pad::kCross, PadInput::Cross}, {ps5pad::kCircle, PadInput::Circle}, {ps5pad::kSquare, PadInput::Square},
			{ps5pad::kTriangle, PadInput::Triangle}, {ps5pad::kL1, PadInput::L1}, {ps5pad::kR1, PadInput::R1},
			{ps5pad::kL3, PadInput::L3}, {ps5pad::kR3, PadInput::R3}, {ps5pad::kCreate, PadInput::Create},
			{ps5pad::kOptions, PadInput::Options}, {ps5pad::kUp, PadInput::Up}, {ps5pad::kDown, PadInput::Down},
			{ps5pad::kLeft, PadInput::Left}, {ps5pad::kRight, PadInput::Right},
		};
		for (const auto& [mask, input] : kButtons)
			if (data.buttons & mask)
				return input;
		if (data.l2 > 160 || (data.buttons & ps5pad::kL2))
			return PadInput::L2;
		if (data.r2 > 160 || (data.buttons & ps5pad::kR2))
			return PadInput::R2;
		if (data.leftY < 40) return PadInput::LeftStickUp;
		if (data.leftY > 215) return PadInput::LeftStickDown;
		if (data.leftX < 40) return PadInput::LeftStickLeft;
		if (data.leftX > 215) return PadInput::LeftStickRight;
		if (data.rightY < 40) return PadInput::RightStickUp;
		if (data.rightY > 215) return PadInput::RightStickDown;
		if (data.rightX < 40) return PadInput::RightStickLeft;
		if (data.rightX > 215) return PadInput::RightStickRight;
		return PadInput::None;
	}

	// A CIA's kind, from its title ID's high half
	const char* CiaKind(uint64_t titleId)
	{
		switch (titleId >> 32)
		{
		case 0x00040000: return "Game";
		case 0x0004000E: return "Update";
		case 0x0004008C: return "DLC";
		case 0x00040002: return "Demo";
		default: return (titleId >> 32 & 0x10) ? "System title" : "Title";
		}
	}
}
