// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.h"
#include "kernel.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace
{
	std::mutex s_mutex;
	FILE* s_file = nullptr; // flushed after every line, so a raw write(2) can follow its content
	std::string s_path;
	std::string s_pending; // lines from before /data was reachable

	// The last few sessions are kept, so a crash's log survives the restarts after it: newest is
	// <stem>.prev<ext>, then <stem>.2<ext> up to <stem>.<kKept - 1><ext>
	constexpr int kKept = 5;

	std::string Older(const std::string& folder, const char* stem, const char* ext, int age)
	{
		return age == 1 ? fmt::format("{}/{}.prev{}", folder, stem, ext) : fmt::format("{}/{}.{}{}", folder, stem, age, ext);
	}

	// stderr's descriptor, which keeps where the title's stderr went: lines that are not the driver's
	// still go there
	int s_stderr = -1;

	bool IsDriverMessage(std::string_view line)
	{
		return line.starts_with("radv") || line.starts_with("wsi/") || line.starts_with("MESA") || line.starts_with("ACO");
	}

	void ForwardStderr(int from)
	{
		std::string pending;
		char buffer[4096];
		for (;;)
		{
			const ssize_t got = read(from, buffer, sizeof(buffer));
			if (got < 0 && errno == EINTR)
				continue;
			if (got <= 0)
				return;
			pending.append(buffer, (size_t)got);
			size_t end;
			while ((end = pending.find('\n')) != std::string::npos)
			{
				const std::string_view line(pending.data(), end);
				// the boot log's line goes to stdout too, so the klog keeps it either way
				if (IsDriverMessage(line))
					ps5log::Write(fmt::format("[driver] {}", line));
				else
					(void)!write(s_stderr, pending.data(), end + 1);
				pending.erase(0, end + 1);
			}
			// a line with no end yet that will not fit: passed on as it is
			if (pending.size() > 16 * 1024)
			{
				(void)!write(s_stderr, pending.data(), pending.size());
				pending.clear();
			}
		}
	}

	void Rotate(const std::string& current, const std::string& folder, const char* stem, const char* ext)
	{
		struct stat st{};
		if (stat(current.c_str(), &st) != 0)
			return;
		for (int age = kKept - 1; age > 1; age--)
			rename(Older(folder, stem, ext, age - 1).c_str(), Older(folder, stem, ext, age).c_str());
		rename(current.c_str(), Older(folder, stem, ext, 1).c_str());
	}
}

namespace ps5log
{
	void Open(const char* folder)
	{
		std::lock_guard lock(s_mutex);
		if (s_file)
			return;
		std::string root = folder;
		root.erase(root.find_last_of('/'));
		// the folder above too, which a first start (#26: a fresh install) has not made yet
		mkdir(root.c_str(), 0777);
		mkdir(folder, 0777);
		s_path = std::string(folder) + "/boot.log";
		Rotate(s_path, folder, "boot", ".log");
		// Cemu's log.txt, one folder up, is rewritten each start: kept beside the boot logs it goes with
		Rotate(root + "/log.txt", folder, "cemu", ".txt");
		s_file = fopen(s_path.c_str(), "w");
		if (s_file)
		{
			std::fputs(s_pending.c_str(), s_file);
			std::fflush(s_file);
		}
		s_pending.clear();
		s_pending.shrink_to_fit();
	}

	const char* Path()
	{
		return s_path.c_str();
	}

	void ForwardDriverMessages()
	{
		static std::once_flag s_once;
		std::call_once(s_once, [] {
			// The stream stderr is moved to the pipe, not descriptor 2: a title may not dup2 (the
			// console ends the app at the call, 0xA002030A "illegal function call", seen on 13.60;
			// PS5_PayloadSDK's ps5platform/klog.h moves the stream for the same reason). Everything
			// written through stderr (the driver's fprintf) arrives; a raw write(2) to descriptor 2
			// goes where it did.
			int ends[2];
			if (pipe(ends) != 0)
				return;
			// a writer never waits on the forwarding thread: with the pipe full, its line is lost
			fcntl(ends[1], F_SETFL, fcntl(ends[1], F_GETFL) | O_NONBLOCK);
			FILE* stream = fdopen(ends[1], "w");
			if (!stream)
			{
				close(ends[0]);
				close(ends[1]);
				return;
			}
			setvbuf(stream, nullptr, _IOLBF, 0);
			std::fflush(stderr);
			s_stderr = STDERR_FILENO;
			stderr = stream;
			std::thread(ForwardStderr, ends[0]).detach();
		});
	}

	void Write(std::string_view line)
	{
		// milliseconds since the title started, which is what matters when reading a boot log
		const uint64_t ms = sceKernelGetProcessTime() / 1000;
		const std::string text = fmt::format("[{:6}.{:03}] {}\n", ms / 1000, ms % 1000, line);
		std::lock_guard lock(s_mutex);
		std::fputs(text.c_str(), stdout);
		std::fflush(stdout);
		if (s_file)
		{
			std::fputs(text.c_str(), s_file);
			// flushed to the kernel at once, so a crash does not take the last lines with it
			std::fflush(s_file);
		}
		else if (s_pending.size() < 256 * 1024)
			s_pending += text;
	}
}

// A line from Cemu's code into the boot log (patches/cemu), written at once like the port's own.
void PS5Cemu_LogLine(std::string_view line)
{
	ps5log::Write(line);
}

// Cemu's crash report (patches/cemu, ExceptionHandler), line by line from its signal handler: with
// write(2) on the file's descriptor, no lock taken (the crashed thread may hold the log's) and no
// buffer, so each line is on disk before the next. Every line is "[crash] ...".
void PS5Cemu_CrashLine(std::string_view text, bool newLine)
{
	static bool s_lineStarted = false;
	FILE* file = s_file;
	if (!file)
		return;
	const int fd = fileno(file);
	if (!s_lineStarted)
		(void)!write(fd, "[crash] ", 8);
	(void)!write(fd, text.data(), text.size());
	s_lineStarted = !newLine;
	if (newLine)
		(void)!write(fd, "\n", 1);
}
