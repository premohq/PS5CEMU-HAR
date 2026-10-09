// SPDX-License-Identifier: MPL-2.0
// PS5Cemu: installing games, updates and DLC into the MLC for the launcher (emulator.h).
//
// As Cemu's "Install game title, update or DLC" (src/gui/wxgui/GameUpdateWindow.cpp, ParseUpdate
// and ThreadWork): the title in a folder with code, content and meta is copied to its place in the
// MLC (usr/title/0005000e/... for an update, 0005000c/... for DLC), and what was there is kept
// aside until the copy is complete, then dropped, or put back when it is cancelled or fails. This
// file keeps Cemu's MPL-2.0 licence.

#include "emulator.h"
#include "lang.h"
#include "../ps5/log.h"

#include "Cafe/TitleList/TitleId.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/TitleList/TitleList.h"
#include "config/ActiveSettings.h"

#include <cstdio>

namespace ps5emu
{
	namespace
	{
		constexpr const char* kParts[] = {"content", "code", "meta"};

		std::mutex s_mutex;
		InstallStatus s_status; // under s_mutex
		std::atomic<bool> s_cancel{false};

		void SetStatus(InstallStatus::State state, std::string message = {})
		{
			std::lock_guard lock(s_mutex);
			s_status.state = state;
			s_status.message = std::move(message);
		}

		void AddCopied(uint64_t bytes)
		{
			std::lock_guard lock(s_mutex);
			s_status.copied += bytes;
		}

		std::string Gigabytes(uint64_t bytes)
		{
			return fmt::format("{:.1f} GB", bytes / 1e9);
		}

		// A file in pieces, so the progress moves within the large .app files and a cancel is quick.
		bool CopyInPieces(const fs::path& from, const fs::path& to, std::vector<char>& buffer)
		{
			std::FILE* in = std::fopen(_pathToUtf8(from).c_str(), "rb");
			if (!in)
				return false;
			std::FILE* out = std::fopen(_pathToUtf8(to).c_str(), "wb");
			if (!out)
			{
				std::fclose(in);
				return false;
			}
			bool ok = true;
			for (;;)
			{
				if (s_cancel)
				{
					ok = false;
					break;
				}
				const size_t read = std::fread(buffer.data(), 1, buffer.size(), in);
				if (read == 0)
				{
					ok = !std::ferror(in);
					break;
				}
				if (std::fwrite(buffer.data(), 1, read, out) != read)
				{
					ok = false;
					break;
				}
				AddCopied(read);
			}
			std::fclose(in);
			ok = std::fclose(out) == 0 && ok;
			return ok;
		}

		void Work(fs::path source, fs::path target, std::string title)
		{
			std::error_code ec;
			uint64_t total = 0;
			for (const char* part : kParts)
				for (const auto& entry : fs::recursive_directory_iterator(source / part, ec))
					if (entry.is_regular_file(ec))
						total += entry.file_size(ec);
			{
				std::lock_guard lock(s_mutex);
				s_status.total = total;
			}
			const fs::space_info space = fs::space(ActiveSettings::GetMlcPath(), ec);
			if (!ec && space.available <= total)
			{
				// tr: {0} and {1} are sizes (2.4 GB)
				SetStatus(InstallStatus::State::Failed, ps5lang::TrF("There is not enough space: it needs {0}, and {1} is free.",
					Gigabytes(total), Gigabytes(space.available)));
				return;
			}

			// what is installed there now waits aside until the copy is complete
			fs::path backup;
			if (fs::exists(target, ec))
			{
				backup = target;
				backup.replace_extension(".backup");
				fs::remove_all(backup, ec);
				fs::rename(target, backup, ec);
				if (ec)
				{
					SetStatus(InstallStatus::State::Failed, ps5lang::TrF("What is installed there now could not be moved aside: {0}", ec.message()));
					return;
				}
			}

			std::vector<char> buffer(4 << 20);
			std::string failure;
			for (const char* part : kParts)
			{
				if (!fs::is_directory(source / part, ec))
					continue;
				fs::create_directories(target / part, ec);
				for (auto it = fs::recursive_directory_iterator(source / part, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
				{
					if (s_cancel)
						break;
					const fs::path relative = it->path().lexically_relative(source);
					if (it->is_directory(ec))
					{
						fs::create_directories(target / relative, ec);
						continue;
					}
					if (!CopyInPieces(it->path(), target / relative, buffer) && !s_cancel)
					{
						failure = "A file could not be copied: " + _pathToUtf8(relative);
						break;
					}
				}
				if (ec && failure.empty())
					failure = "The folder could not be read: " + ec.message();
				if (s_cancel || !failure.empty())
					break;
			}

			if (s_cancel || !failure.empty())
			{
				fs::remove_all(target, ec);
				if (!backup.empty())
					fs::rename(backup, target, ec);
				ps5log::Line("[install] {}: {}", title, s_cancel ? "cancelled" : failure);
				SetStatus(s_cancel ? InstallStatus::State::Cancelled : InstallStatus::State::Failed, failure);
				return;
			}
			if (!backup.empty())
				fs::remove_all(backup, ec);
			ps5log::Line("[install] {} installed in {}", title, _pathToUtf8(target));
			SetStatus(InstallStatus::State::Done);
		}
	}

	InstallCandidate InspectInstall(const std::string& folder)
	{
		InstallCandidate candidate;
		const fs::path path = _utf8ToPath(folder);
		std::error_code ec;
		if (!fs::is_directory(path / "meta", ec) || !fs::is_directory(path / "content", ec))
		{
			candidate.note = fs::exists(path / "title.tmd", ec) ?
				ps5lang::Tr("This one is not unpacked (title.tmd and .app files). Cemu reads it from the game files folder as it is: no install needed.") :
				ps5lang::Tr("There is no title here: a game, update or DLC to install is a folder with code, content and meta.");
			return candidate;
		}
		TitleInfo title(path);
		if (!title.IsValid())
		{
			candidate.note = ps5lang::Tr("Its meta folder does not describe a Wii U title (meta.xml).");
			return candidate;
		}
		candidate.name = title.GetMetaTitleName();
		candidate.titleId = title.GetAppTitleId();
		candidate.version = title.GetAppTitleVersion();
		const TitleIdParser parser(candidate.titleId);
		switch (parser.GetType())
		{
		case TitleIdParser::TITLE_TYPE::BASE_TITLE:
		case TitleIdParser::TITLE_TYPE::BASE_TITLE_DEMO: candidate.kind = InstallCandidate::Kind::Game; break;
		case TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE: candidate.kind = InstallCandidate::Kind::Update; break;
		case TitleIdParser::TITLE_TYPE::AOC: candidate.kind = InstallCandidate::Kind::Dlc; break;
		default: candidate.kind = InstallCandidate::Kind::System; break;
		}
		const fs::path target = ActiveSettings::GetMlcPath(title.GetInstallPath());
		if (fs::equivalent(target, path, ec))
		{
			candidate.kind = InstallCandidate::Kind::None;
			candidate.note = ps5lang::Tr("This is where it is installed already.");
			return candidate;
		}
		if (fs::exists(target, ec))
		{
			const TitleInfo installed(target);
			if (installed.IsValid())
				candidate.installedVersion = installed.GetAppTitleVersion();
		}
		return candidate;
	}

	bool StartInstall(const std::string& folder, std::string& error)
	{
		if (GetInstallStatus().state == InstallStatus::State::Running)
		{
			error = ps5lang::Tr("An install is already running.");
			return false;
		}
		const InstallCandidate candidate = InspectInstall(folder);
		if (candidate.kind == InstallCandidate::Kind::None)
		{
			error = candidate.note;
			return false;
		}
		const fs::path source = _utf8ToPath(folder);
		const fs::path target = ActiveSettings::GetMlcPath(TitleInfo(source).GetInstallPath());
		{
			std::lock_guard lock(s_mutex);
			s_status = {};
			s_status.state = InstallStatus::State::Running;
		}
		s_cancel = false;
		const std::string title = fmt::format("{} ({:016x} v{})", candidate.name, candidate.titleId, candidate.version);
		ps5log::Line("[install] {} from {}", title, folder);
		// on its own: what it does shows in GetInstallStatus, and only one runs at a time
		std::thread(Work, source, target, title).detach();
		return true;
	}

	InstallStatus GetInstallStatus()
	{
		std::lock_guard lock(s_mutex);
		return s_status;
	}

	void CancelInstall()
	{
		s_cancel = true;
	}

	void Rescan()
	{
		CafeTitleList::Refresh();
	}
}
