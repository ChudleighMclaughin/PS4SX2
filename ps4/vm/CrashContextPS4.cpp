// SPDX-License-Identifier: GPL-3.0+

// (AI-assisted) Extra lines for crash.log: which thread crashed, which recompiler block the
// faulting instruction lives in, which emulated memory region was touched, the guest program
// counters, and whatever ProbeLog lines were still queued. Called from ps4_crashlog.cpp's signal
// and terminate handlers, so everything here only reads plain memory and uses write().

#include "Memory.h"
#include "R3000A.h"
#include "R5900.h"
#include "VU.h"

#include "ps4/core/ProbeLoggerPS4.h"
#include "ps4/core/ThreadAffinityPS4.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace
{
	void WriteLine(int fd, const char* format, ...)
	{
		char line[256];
		va_list args;
		va_start(args, format);
		const int length = std::vsnprintf(line, sizeof(line), format, args);
		va_end(args);
		if (length <= 0)
			return;
		size_t remaining = (size_t)length < sizeof(line) ? (size_t)length : sizeof(line) - 1;
		const char* cursor = line;
		while (remaining > 0)
		{
			const ssize_t written = write(fd, cursor, remaining);
			if (written <= 0)
				return;
			cursor += written;
			remaining -= (size_t)written;
		}
	}

	struct Region
	{
		const char* name;
		uint32_t offset;
		uint32_t size;
	};

	constexpr Region CODE_REGIONS[] = {
		{"EErec", HostMemoryMap::EErecOffset, HostMemoryMap::EErecSize},
		{"IOPrec", HostMemoryMap::IOPrecOffset, HostMemoryMap::IOPrecSize},
		{"VIF0rec", HostMemoryMap::VIF0recOffset, HostMemoryMap::VIF0recSize},
		{"VIF1rec", HostMemoryMap::VIF1recOffset, HostMemoryMap::VIF1recSize},
		{"mVU0rec", HostMemoryMap::mVU0recOffset, HostMemoryMap::mVU0recSize},
		{"mVU1rec", HostMemoryMap::mVU1recOffset, HostMemoryMap::mVU1recSize},
		{"VIFUnpackRec", HostMemoryMap::VIFUnpackRecOffset, HostMemoryMap::VIFUnpackRecSize},
		{"SWrec", HostMemoryMap::SWrecOffset, HostMemoryMap::SWrecSize},
	};

	constexpr Region DATA_REGIONS[] = {
		{"EEmem", HostMemoryMap::EEmemOffset, HostMemoryMap::EEmemSize},
		{"IOPmem", HostMemoryMap::IOPmemOffset, HostMemoryMap::IOPmemSize},
		{"VUmem", HostMemoryMap::VUmemOffset, HostMemoryMap::VUmemSize},
		{"VTLBVirtualMap", HostMemoryMap::VTLBVirtualMapOffset, HostMemoryMap::VTLBVirtualMapSize},
		{"VTLBAddressMap", HostMemoryMap::VTLBAddressMapOffset, HostMemoryMap::VTLBAddressMapSize},
	};

	template <size_t N>
	const Region* FindRegion(const Region (&regions)[N], uintptr_t base, uintptr_t address, uint64_t* offset_out)
	{
		if (base == 0 || address < base)
			return nullptr;
		const uint64_t offset = address - base;
		for (const Region& region : regions)
		{
			if (offset >= region.offset && offset < (uint64_t)region.offset + region.size)
			{
				*offset_out = offset - region.offset;
				return &region;
			}
		}
		return nullptr;
	}
} // namespace

extern "C" void ps4_crash_report_hook(int fd, uint64_t rip, uint64_t fault_address)
{
	WriteLine(fd, "\n--- PS4SX2 crash context ---\n");

	const char* role = PS4ThreadAffinity::CurrentRole();
	WriteLine(fd, "thread: %s\n", role ? role : "main/other");

	const uintptr_t data = (uintptr_t)SysMemory::GetDataPtr(0);
	const uintptr_t code = (uintptr_t)SysMemory::GetCodePtr(0);
	WriteLine(fd, "data map: 0x%016llx size=0x%x\n", (unsigned long long)data, HostMemoryMap::MainSize);
	WriteLine(fd, "code map: 0x%016llx size=0x%x\n", (unsigned long long)code, HostMemoryMap::CodeSize);
	if (data && code)
	{
		const uintptr_t low = data < code ? data : code;
		const uintptr_t high_end = data < code ? code + HostMemoryMap::CodeSize : data + HostMemoryMap::MainSize;
		WriteLine(fd, "data<->code span: 0x%llx bytes (over 2 GiB means rel32 jumps between them can't reach)\n",
			(unsigned long long)(high_end - low));
	}

	if (rip)
	{
		uint64_t offset = 0;
		if (const Region* region = FindRegion(CODE_REGIONS, code, rip, &offset))
			WriteLine(fd, "rip is in JIT block cache %s +0x%llx\n", region->name, (unsigned long long)offset);
		else
			WriteLine(fd, "rip is not in the JIT code map (emulator code or a library)\n");
	}

	if (rip || fault_address)
	{
		uint64_t offset = 0;
		if (const Region* region = FindRegion(DATA_REGIONS, data, fault_address, &offset))
			WriteLine(fd, "fault address is in %s +0x%llx\n", region->name, (unsigned long long)offset);
		else if (const Region* code_region = FindRegion(CODE_REGIONS, code, fault_address, &offset))
			WriteLine(fd, "fault address is in JIT block cache %s +0x%llx\n", code_region->name, (unsigned long long)offset);
		else if (fault_address < 0x100000000ULL)
			WriteLine(fd, "fault address is below 4 GiB: looks like a host pointer cut to 32 bits ([sibwarn] TRUNCATED)\n");
		else
			WriteLine(fd, "fault address is outside the emulator maps\n");
	}

	WriteLine(fd, "EE  pc=0x%08x cycle=%llu\n", cpuRegs.pc, (unsigned long long)cpuRegs.cycle);
	WriteLine(fd, "IOP pc=0x%08x\n", psxRegs.pc);
	WriteLine(fd, "VU0 tpc=0x%04x  VU1 tpc=0x%04x\n", vuRegs[0].VI[REG_TPC].UL, vuRegs[1].VI[REG_TPC].UL);

	WriteLine(fd, "queued ProbeLog lines are flushed to the end of PCSX2-vm-boot.log\n");
	PS4ProbeLogger::DrainForCrash();
}
