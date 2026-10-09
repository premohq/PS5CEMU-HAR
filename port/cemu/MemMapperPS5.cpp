// SPDX-License-Identifier: MPL-2.0
// PS5Cemu: Cemu's MemMapper on the PS5 kernel.
//
// Cemu reserves address space (the 4 GiB guest memory, the recompiler's jump table) and commits
// pages in it on demand, and allocates executable memory for the recompiler. On Linux that is
// mmap and mprotect. A PS5 title instead:
//  - reserves with sceKernelReserveVirtualRange, kept out of RADV's GPU windows (kernel.h);
//  - backs committed pages with direct memory, the title's large memory pool, mapped at fixed
//    addresses into the reservation (ProsperoEden backs Eden's guest memory the same way);
//  - gets executable memory as JIT shared memory, which the kernel grants only once the HEN has
//    jailbroken the process (port/ps5/privilege.h).
//
// Mapping over a committed page would replace it with fresh memory, where Linux's mprotect keeps
// its contents, so committed pages are tracked per reservation. As on Linux, freeing pages of a
// reservation only takes their access away; their memory stays committed until the reservation
// is released.

#include "util/MemMapper/MemMapper.h"
#include "../app/lang.h"
#include "../ps5/kernel.h"
#include "../ps5/log.h"
#include "../ps5/notify.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <unistd.h>
#include <vector>

// The platform layer's executable direct memory (ps5platform/exec.h, in libps5platform.a with RADV)
extern "C"
{
	void* ps5_exec_allocate(size_t bytes, uintptr_t anchor);
	int ps5_exec_release(void* base);
}

namespace
{
	using MemMapper::PAGE_PERMISSION;

	struct Reservation
	{
		uintptr_t base;
		size_t size;
		std::vector<uint8> pageAccess; // per kernel page: 0 reserved, else the committed protection | 0x80
		std::vector<std::pair<off_t, size_t>> backing; // direct memory committed into it
	};

	struct Allocation
	{
		size_t size;
		off_t physical;	 // direct memory, or -1 for JIT memory
		int handle = -1; // JIT memory's, closed with it: the memory lasts as long as the handle;
						 // kExecRegion: the platform layer's executable direct memory
	};
	constexpr int kExecRegion = -2;

	constexpr uint8 kCommitted = 0x80;
	constexpr size_t kCommitChunk = 256 * 1024 * 1024;

	std::mutex s_mutex;
	std::map<uintptr_t, Reservation> s_reservations; // by base
	std::map<uintptr_t, Allocation> s_allocations;	 // stand-alone allocations, by address
	uintptr_t s_nextHint = ps5::kCpuMappingHint;

	int ToProtection(PAGE_PERMISSION permission)
	{
		int protection = 0;
		if (HAS_FLAG(permission, PAGE_PERMISSION::P_READ))
			protection |= ps5::kProtRead;
		if (HAS_FLAG(permission, PAGE_PERMISSION::P_WRITE))
			protection |= ps5::kProtWrite;
		if (HAS_FLAG(permission, PAGE_PERMISSION::P_EXECUTE))
			protection |= ps5::kProtExec;
		return protection;
	}

	uintptr_t AlignDown(uintptr_t value, size_t alignment) { return value & ~(uintptr_t)(alignment - 1); }
	uintptr_t AlignUp(uintptr_t value, size_t alignment) { return (value + alignment - 1) & ~(uintptr_t)(alignment - 1); }

	Reservation* FindReservation(uintptr_t address)
	{
		auto it = s_reservations.upper_bound(address);
		if (it == s_reservations.begin())
			return nullptr;
		--it;
		Reservation& r = it->second;
		return address < r.base + r.size ? &r : nullptr;
	}

	// Reserves address space outside RADV's windows. A null base takes the next free range from
	// the hint on; a given base is a fixed reservation.
	void* Reserve(void* base, size_t size, size_t alignment)
	{
		if (base)
		{
			void* address = base;
			if (sceKernelReserveVirtualRange(&address, size, ps5::kMapFixed, alignment) != 0 || address != base)
				return nullptr;
			return address;
		}
		for (int attempt = 0; attempt < 8; attempt++)
		{
			void* address = reinterpret_cast<void*>(s_nextHint);
			if (sceKernelReserveVirtualRange(&address, size, 0, alignment) != 0)
				return nullptr;
			const uintptr_t start = reinterpret_cast<uintptr_t>(address);
			if (start >= ps5::kGpuWindowLowEnd && !ps5::OverlapsGpuWindows(start, size))
			{
				s_nextHint = AlignUp(start + size, alignment);
				return address;
			}
			// the kernel put it somewhere RADV needs: give it back and look past that window
			sceKernelMunmap(address, size);
			s_nextHint = start < ps5::kGpuWindowLowEnd ? ps5::kCpuMappingHint : ps5::kGpuWindowHighEnd;
		}
		return nullptr;
	}

	// Backs [address, address + size) with direct memory, mapped at that address.
	bool MapDirect(uintptr_t address, size_t size, int protection, off_t& physicalOut)
	{
		// 2 MiB-aligned blocks get 2 MiB pages: guest memory is walked on every access
		const size_t alignment = (address % ps5::kLargePageSize == 0 && size % ps5::kLargePageSize == 0) ? ps5::kLargePageSize : ps5::kPageSize;
		off_t physical = 0;
		if (sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), size, alignment, ps5::kDirectMemoryTypeCpu, &physical) != 0)
		{
			off_t start = 0;
			size_t largest = 0;
			sceKernelAvailableDirectMemorySize(0, (off_t)sceKernelGetDirectMemorySize(), 0, &start, &largest);
			ps5log::Line("[memmap] out of direct memory committing {:#x} bytes (largest free block {} MiB): the game ran out of memory",
				size, largest >> 20);
			// Cemu cannot go on without the memory, and the app ends soon after: the player is told
			// why, once, as the toast outlives the app
			static std::atomic<bool> s_told{false};
			if (!s_told.exchange(true))
				ps5notify::Send(ps5lang::Tr("The PS5 has no memory left for this Wii U game, so it has to stop. The boot log has the details."));
			return false;
		}
		void* at = reinterpret_cast<void*>(address);
		if (sceKernelMapDirectMemory(&at, size, protection | ps5::kProtRead | ps5::kProtWrite, ps5::kMapFixed, physical, alignment) != 0 ||
			at != reinterpret_cast<void*>(address))
		{
			sceKernelReleaseDirectMemory(physical, size);
			ps5log::Line("[memmap] could not map {:#x} bytes at {:#x}", size, address);
			return false;
		}
		// fresh memory reads as zero, as anonymous mmap does on Linux
		memset(at, 0, size);
		if ((protection & (ps5::kProtRead | ps5::kProtWrite)) != (ps5::kProtRead | ps5::kProtWrite))
			sceKernelMprotect(at, size, protection);
		physicalOut = physical;
		return true;
	}

	// Without the HEN's JIT memory: direct memory mapped read-write, then given execute, which the
	// kernel grants any title (the platform layer's exec.h; Azahar's recompiler and ProsperoEden's
	// run on it). Cemu's code calls out through 64-bit addresses, so it may lie anywhere.
	void* AllocateExecutableDirect(size_t size)
	{
		void* address = ps5_exec_allocate(size, 0);
		if (!address)
		{
			ps5log::Line("[memmap] executable direct memory refused too ({:#x} bytes): no recompiler", size);
			return nullptr;
		}
		static std::once_flag s_logged;
		std::call_once(s_logged, [] { ps5log::Line("[memmap] the recompiler's code is in executable direct memory (no HEN JIT grant needed)"); });
		s_allocations[reinterpret_cast<uintptr_t>(address)] = {size, -1, kExecRegion};
		return address;
	}

	void* AllocateJit(size_t size)
	{
		int handle = -1;
		if (sceKernelJitCreateSharedMemory(nullptr, size, ps5::kProtRead | ps5::kProtWrite | ps5::kProtExec, &handle) != 0)
			return AllocateExecutableDirect(size); // the HEN has not jailbroken the app, or this one gives no JIT
		void* address = nullptr;
		if (sceKernelJitMapSharedMemory(handle, ps5::kProtRead | ps5::kProtWrite | ps5::kProtExec, &address) != 0 || !address)
		{
			ps5log::Line("[memmap] JIT memory could not be mapped ({:#x} bytes)", size);
			close(handle);
			return nullptr;
		}
		s_allocations[reinterpret_cast<uintptr_t>(address)] = {size, -1, handle};
		return address;
	}
}

// The boot log's memory line (app/emulator.cpp): the direct memory committed to Cemu's
// reservations and stand-alone allocations, and its JIT memory.
void PS5Cemu_MemMapperUsage(size_t& committed, size_t& jit)
{
	std::lock_guard lock(s_mutex);
	committed = jit = 0;
	for (const auto& [base, reservation] : s_reservations)
		for (const auto& [physical, length] : reservation.backing)
			committed += length;
	for (const auto& [address, allocation] : s_allocations)
		(allocation.physical >= 0 ? committed : jit) += allocation.size;
}

namespace MemMapper
{
	size_t GetPageSize()
	{
		return ps5::kPageSize;
	}

	void* ReserveMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags)
	{
		std::lock_guard lock(s_mutex);
		size = AlignUp(size, ps5::kPageSize);
		void* address = Reserve(baseAddr, size, size >= ps5::kLargePageSize ? ps5::kLargePageSize : ps5::kPageSize);
		if (!address)
		{
			ps5log::Line("[memmap] could not reserve {:#x} bytes", size);
			return nullptr;
		}
		Reservation& r = s_reservations[reinterpret_cast<uintptr_t>(address)];
		r.base = reinterpret_cast<uintptr_t>(address);
		r.size = size;
		r.pageAccess.assign(size / ps5::kPageSize, 0);
		return address;
	}

	void FreeReservation(void* baseAddr, size_t size)
	{
		std::lock_guard lock(s_mutex);
		auto it = s_reservations.find(reinterpret_cast<uintptr_t>(baseAddr));
		if (it == s_reservations.end())
			return;
		sceKernelMunmap(baseAddr, it->second.size);
		for (auto& [physical, length] : it->second.backing)
			sceKernelReleaseDirectMemory(physical, length);
		s_reservations.erase(it);
	}

	void* AllocateMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags, bool fromReservation)
	{
		std::lock_guard lock(s_mutex);
		const int protection = ToProtection(permissionFlags);
		if (!fromReservation)
		{
			if (HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_EXECUTE))
				return AllocateJit(AlignUp(size, ps5::kPageSize));
			// a stand-alone allocation: its own reservation, backed at once
			size = AlignUp(size, ps5::kPageSize);
			void* address = Reserve(baseAddr, size, size >= ps5::kLargePageSize ? ps5::kLargePageSize : ps5::kPageSize);
			if (!address)
				return nullptr;
			off_t physical;
			if (!MapDirect(reinterpret_cast<uintptr_t>(address), size, protection, physical))
			{
				sceKernelMunmap(address, size);
				return nullptr;
			}
			s_allocations[reinterpret_cast<uintptr_t>(address)] = {size, physical};
			return address;
		}

		const uintptr_t requested = reinterpret_cast<uintptr_t>(baseAddr);
		Reservation* r = FindReservation(requested);
		if (!r)
		{
			ps5log::Line("[memmap] commit at {:#x} is outside every reservation", requested);
			return nullptr;
		}
		const uintptr_t start = AlignDown(requested, ps5::kPageSize);
		const uintptr_t end = std::min<uintptr_t>(AlignUp(requested + size, ps5::kPageSize), r->base + r->size);
		const uint8 wanted = kCommitted | protection;
		size_t page = (start - r->base) / ps5::kPageSize;
		const size_t lastPage = (end - r->base) / ps5::kPageSize;
		while (page < lastPage)
		{
			// the run of pages in the same state from here
			const uint8 state = r->pageAccess[page];
			size_t runEnd = page + 1;
			while (runEnd < lastPage && r->pageAccess[runEnd] == state)
				runEnd++;
			const uintptr_t runAddress = r->base + page * ps5::kPageSize;
			const size_t runSize = (runEnd - page) * ps5::kPageSize;
			if (state == 0)
			{
				// in chunks, so a large commit (Cemu commits all of MEM2 at once) does not need
				// one contiguous gigabyte of direct memory
				for (size_t offset = 0; offset < runSize; offset += kCommitChunk)
				{
					const size_t chunk = std::min(kCommitChunk, runSize - offset);
					off_t physical;
					if (!MapDirect(runAddress + offset, chunk, protection, physical))
						return nullptr;
					r->backing.emplace_back(physical, chunk);
				}
			}
			else if (state != wanted && sceKernelMprotect(reinterpret_cast<void*>(runAddress), runSize, protection) != 0)
			{
				ps5log::Line("[memmap] could not change the access of {:#x} bytes at {:#x}", runSize, runAddress);
				return nullptr;
			}
			std::fill(r->pageAccess.begin() + page, r->pageAccess.begin() + runEnd, wanted);
			page = runEnd;
		}
		return baseAddr;
	}

	void FreeMemory(void* baseAddr, size_t size, bool fromReservation)
	{
		std::lock_guard lock(s_mutex);
		if (!fromReservation)
		{
			auto it = s_allocations.find(reinterpret_cast<uintptr_t>(baseAddr));
			if (it == s_allocations.end())
				return;
			if (it->second.handle == kExecRegion)
			{
				ps5_exec_release(baseAddr);
				s_allocations.erase(it);
				return;
			}
			sceKernelMunmap(baseAddr, it->second.size);
			if (it->second.physical >= 0)
				sceKernelReleaseDirectMemory(it->second.physical, it->second.size);
			if (it->second.handle >= 0)
				close(it->second.handle);
			s_allocations.erase(it);
			return;
		}
		// as on Linux: the pages stay committed, without access
		const uintptr_t start = AlignDown(reinterpret_cast<uintptr_t>(baseAddr), ps5::kPageSize);
		Reservation* r = FindReservation(start);
		if (!r)
			return;
		const uintptr_t end = std::min<uintptr_t>(AlignUp(reinterpret_cast<uintptr_t>(baseAddr) + size, ps5::kPageSize), r->base + r->size);
		size_t page = (start - r->base) / ps5::kPageSize;
		const size_t lastPage = (end - r->base) / ps5::kPageSize;
		while (page < lastPage)
		{
			if (!(r->pageAccess[page] & kCommitted) || r->pageAccess[page] == kCommitted)
			{
				page++;
				continue;
			}
			size_t runEnd = page + 1;
			while (runEnd < lastPage && (r->pageAccess[runEnd] & kCommitted) && r->pageAccess[runEnd] != kCommitted)
				runEnd++;
			if (sceKernelMprotect(reinterpret_cast<void*>(r->base + page * ps5::kPageSize), (runEnd - page) * ps5::kPageSize, 0) == 0)
				std::fill(r->pageAccess.begin() + page, r->pageAccess.begin() + runEnd, kCommitted);
			page = runEnd;
		}
	}
}
