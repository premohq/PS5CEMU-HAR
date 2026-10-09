// SPDX-License-Identifier: GPL-3.0-or-later
// PS5Cemu: getting out of the app sandbox (privilege.h).
//
// The HEN request follows PS5SX2's ProsperoHenJailbreak.cpp (GPL-3.0-or-later), itself ported from
// PS2-Library-Prototype: publish the request atomically (write a staged file, then rename), wait for
// the HEN to remove it, then give the HEN a moment to finish before trusting the credentials.

#include "privilege.h"
#include "kernel.h"
#include "log.h"
#include "../app/paths.h"

#include "elevation.hpp" // ps5-native-app-boilerplate examples/sandbox-elevation
#include "ps5platform/exec.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <csetjmp>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ps5privilege
{
	namespace
	{
		constexpr char kRequestPath[] = "/download0/etahen_jailbreak";
		constexpr char kStagedPath[] = "/download0/etahen_jailbreak.tmp";
		constexpr int kPollUs = 16667;
		constexpr int kMaxPolls = 300;				 // ~5 s for the HEN to take the request
		constexpr int kMaxPollsAfterConsume = 180;	 // ~3 s for it to finish the jailbreak

		Result s_result;

		bool PublishRequest(std::string& failure)
		{
			unlink(kRequestPath);
			unlink(kStagedPath);
			const int fd = open(kStagedPath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
			if (fd < 0)
			{
				failure = fmt::format("cannot create the request (errno {})", errno);
				return false;
			}
			fchmod(fd, 0666);
			const std::string request = fmt::format("{{\"PID\":{}}}\n", getpid());
			const bool written = write(fd, request.data(), request.size()) == (ssize_t)request.size() && fsync(fd) == 0;
			close(fd);
			if (!written || rename(kStagedPath, kRequestPath) != 0)
			{
				unlink(kStagedPath);
				failure = fmt::format("cannot publish the request (errno {})", errno);
				return false;
			}
			return true;
		}

		bool HenJailbreak(std::string& detail)
		{
			if (geteuid() == 0)
			{
				detail = "already root";
				return true;
			}
			std::string failure;
			if (!PublishRequest(failure))
			{
				detail = failure;
				return false;
			}
			int polls = 0;
			while (access(kRequestPath, F_OK) == 0)
			{
				if (++polls >= kMaxPolls)
				{
					unlink(kRequestPath);
					detail = "no HEN took the request (is a HEN loaded, with PPSA99360 in its app jailbreak list: etaHEN's list, or OnionHEN's exact_title_ids with no trailing comma?)";
					return false;
				}
				sceKernelUsleep(kPollUs);
			}
			// the HEN removes the request before it is done with the process
			for (int wait = 0; wait < kMaxPollsAfterConsume && geteuid() != 0; wait++)
				sceKernelUsleep(kPollUs);
			detail = fmt::format("HEN took the request after {} ms, euid {}", polls * kPollUs / 1000, geteuid());
			return true;
		}

		bool ProbeJit()
		{
			// one page, as the recompiler will ask for, given back after
			int handle = -1;
			if (sceKernelJitCreateSharedMemory(nullptr, ps5::kPageSize, ps5::kProtRead | ps5::kProtWrite | ps5::kProtExec, &handle) != 0)
				return false;
			void* address = nullptr;
			const bool mapped = sceKernelJitMapSharedMemory(handle, ps5::kProtRead | ps5::kProtWrite | ps5::kProtExec, &address) == 0 && address;
			if (mapped)
				sceKernelMunmap(address, ps5::kPageSize);
			close(handle);
			return mapped;
		}

		sigjmp_buf s_probeJump;

		void ProbeFault(int)
		{
			siglongjmp(s_probeJump, 1);
		}

		// Executable direct memory (ps5platform/exec.h), which needs no HEN: a few bytes of code
		// written and run (mov eax, 42; ret), as the recompiler will. Should the kernel take the
		// execute away on some firmware, the fault only means no: the app starts on the interpreter.
		bool ProbeExecutableDirect()
		{
			void* code = ps5_exec_allocate(ps5::kPageSize, 0);
			if (!code)
				return false;
			static const uint8_t kReturn42[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};
			std::memcpy(code, kReturn42, sizeof(kReturn42));
			struct sigaction fault{}, oldSegv{}, oldBus{};
			fault.sa_handler = ProbeFault;
			sigemptyset(&fault.sa_mask);
			sigaction(SIGSEGV, &fault, &oldSegv);
			sigaction(SIGBUS, &fault, &oldBus);
			volatile int result = 0;
			if (sigsetjmp(s_probeJump, 1) == 0)
				result = reinterpret_cast<int (*)()>(code)();
			sigaction(SIGSEGV, &oldSegv, nullptr);
			sigaction(SIGBUS, &oldBus, nullptr);
			ps5_exec_release(code);
			return result == 42;
		}

		// /data as the app needs it: a folder it can write and list. ShadowMountPlus 1.7 mounts /data
		// into a sandboxed app so that files open and write but a listing (and lstat) is refused with
		// EPERM: no game folder could be listed, so that is not access (#22)
		bool CanReachData()
		{
			struct stat st{};
			if (stat("/data", &st) != 0 || !S_ISDIR(st.st_mode) || access("/data", W_OK) != 0)
				return false;
			DIR* folder = opendir("/data");
			if (!folder)
				return false;
			closedir(folder);
			return true;
		}
	}

	Result Acquire()
	{
		Result r;
		std::string henDetail;
		// A test of the no-HEN start on a console that has one: with this file in the app's own
		// download0 (/mnt/sandbox/PPSA99360_000/download0 from outside), no HEN is asked
		if (access("/download0/ps5cemu_skip_hen", F_OK) == 0)
			henDetail = "not asked: download0/ps5cemu_skip_hen is there";
		else
			r.jailbroken = HenJailbreak(henDetail);
		// euid may stay 1 even with working credentials: the JIT probe is the ground truth. The
		// HEN's JIT memory first (what the recompiler has used since 1.0), else executable direct
		// memory, which any title gets: the recompiler runs either way
		const bool henJit = ProbeJit();
		// asked either way: Azahar's recompiler (dynarmic) takes this memory even where the HEN gives
		// JIT memory, and runs Azahar's interpreter without it
		const bool directExec = ProbeExecutableDirect();
		r.executable = directExec;
		r.jit = henJit || directExec;
		r.filesystem = CanReachData();
		std::string elevationDetail;
		if (!r.filesystem)
		{
			const auto status = elevation::request(elevation::Capability::filesystem);
			r.filesystem = status == elevation::Status::ok && CanReachData();
			elevationDetail = fmt::format(", elevation helper: {}", (int)status);
			ps5paths::RefindAppDir(); // as ReachFolders: nothing has asked for it yet, should anything have
		}
		r.summary = fmt::format("HEN: {} ({}); JIT {}{}; /data {}{}", r.jailbroken ? "ok" : "no", henDetail,
			henJit ? "available (the HEN's)" : directExec ? "available (executable direct memory, no HEN needed)" : "unavailable (interpreter only)",
			henJit && !directExec ? "; no executable direct memory: 3DS games run on Azahar's interpreter" : "",
			r.filesystem ? "reachable" : "unreachable", elevationDetail);
		s_result = r;
		return r;
	}

	const Result& Current()
	{
		return s_result;
	}

	namespace
	{
		// Seen but refused: what the sandbox does to a drive it keeps out (a missing folder is just
		// missing, an unplugged drive, and is no reason to ask)
		bool Refused(const std::string& path)
		{
			if (path.empty())
				return false;
			const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY);
			if (fd >= 0)
			{
				close(fd);
				return false;
			}
			return errno == EACCES || errno == EPERM;
		}
	}

	void ReachFolders(const std::vector<std::string>& folders)
	{
		std::vector<std::string> wanted = folders;
		for (int i = 0; i < 8; i++)
			wanted.push_back(fmt::format("/mnt/usb{}", i));
		wanted.push_back("/mnt/ext0");
		wanted.push_back("/mnt/ext1");
		std::string refused;
		for (const std::string& folder : wanted)
			if (Refused(folder))
			{
				refused = folder;
				break;
			}
		if (refused.empty())
			return;
		// a HEN that opened /data but not the drives (games on USB on 13.x, #16): the bundled helper
		// gives the whole filesystem, as when /data is out of reach. A jailbroken process has no
		// /app0, so the helper is asked for where the app is (installed on a USB drive, say).
		const std::string helper = ps5paths::AppDir() + "/sandbox-elevator.elf";
		const auto status = elevation::request(elevation::Capability::filesystem, helper.c_str());
		const bool reached = !Refused(refused);
		ps5log::Line("[privilege] {} could not be read; elevation helper ({}): {}, {}", refused, helper, (int)status,
			reached ? "it can now" : "it still cannot");
		// out of the sandbox, the /app0 the helper was found in is gone: the app's files are read
		// where the console's root has them
		if (ps5paths::RefindAppDir())
			ps5log::Line("[privilege] app folder now {}", ps5paths::AppDir());
		s_result.summary += reached ? fmt::format("; {} opened by the elevation helper", refused) :
									  fmt::format("; {} unreadable (elevation helper: {})", refused, (int)status);
	}
}

bool PS5_JitAvailable()
{
	return ps5privilege::Current().jit;
}
