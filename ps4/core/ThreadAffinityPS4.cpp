// SPDX-License-Identifier: GPL-3.0+

#include "ThreadAffinityPS4.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>

// OpenOrbis's public header declares these without parameters. Use the ABI
// verified by DolphinPS4 until the SDK declaration is corrected.
extern "C" void* scePthreadSelf();
extern "C" int scePthreadSetaffinity(void* thread, unsigned long long mask);
extern "C" void ps4_sampler_register_thread(const char* name); // ps4/runtime/ps4_sampler.cpp

namespace PS4ThreadAffinity
{
	static bool IsEnabled()
	{
		static std::atomic<int> enabled{-1};
		int value = enabled.load(std::memory_order_acquire);
		if (value >= 0)
			return value != 0;

		value = access("/data/PS4SX2/vm-no-thread-affinity", F_OK) == 0 ? 0 : 1;
		enabled.store(value, std::memory_order_release);
		return value != 0;
	}

	// The threads that called PinCurrentThread, for CurrentRole. A role that pins again (its thread
	// was recreated) takes over its slot.
	static constexpr int MAX_ROLES = 8;
	static std::atomic<std::uintptr_t> s_role_threads[MAX_ROLES];
	static std::atomic<const char*> s_role_names[MAX_ROLES];

	static void RememberRole(const char* role)
	{
		const std::uintptr_t self = (std::uintptr_t)pthread_self();
		for (int i = 0; i < MAX_ROLES; i++)
		{
			const char* name = s_role_names[i].load(std::memory_order_acquire);
			if (name && std::strcmp(name, role) == 0)
			{
				s_role_threads[i].store(self, std::memory_order_release);
				return;
			}
		}
		for (int i = 0; i < MAX_ROLES; i++)
		{
			const char* expected = nullptr;
			if (s_role_names[i].compare_exchange_strong(expected, role, std::memory_order_acq_rel))
			{
				s_role_threads[i].store(self, std::memory_order_release);
				return;
			}
		}
	}

	const char* CurrentRole()
	{
		const std::uintptr_t self = (std::uintptr_t)pthread_self();
		for (int i = 0; i < MAX_ROLES; i++)
		{
			if (s_role_threads[i].load(std::memory_order_acquire) == self)
				return s_role_names[i].load(std::memory_order_acquire);
		}
		return nullptr;
	}

	// The core vm-cores.txt gives this role, or -1.
	static int ConfiguredCore(const char* role)
	{
		FILE* file = std::fopen("/data/PS4SX2/vm-cores.txt", "r");
		if (!file)
			return -1;
		int core = -1;
		char line[128];
		while (std::fgets(line, sizeof(line), file))
		{
			char name[16];
			int value;
			if (std::sscanf(line, " %15[^= \t\r\n] = %d", name, &value) == 2 && strcasecmp(name, role) == 0 &&
				value >= 0 && value <= 7)
				core = value;
		}
		std::fclose(file);
		return core;
	}

	void PinCurrentThread(const char* role, int core)
	{
		RememberRole(role);
		// (AI-assisted) diag-time-sampler: with vm-time-profile, the four busy emulator threads are sampled
		// (ps4_sampler.cpp, logs/samples.log).
		static const bool s_time_profile = access("/data/PS4SX2/vm-time-profile", F_OK) == 0;
		if (s_time_profile && (std::strcmp(role, "EE") == 0 || std::strcmp(role, "VU1") == 0 ||
								  std::strcmp(role, "GS") == 0 || std::strcmp(role, "GSREPLAY") == 0))
			ps4_sampler_register_thread(role);
		if (!IsEnabled())
		{
			std::printf("PS4 thread affinity: %s left to scheduler (disabled by marker)\n", role);
			std::fflush(stdout);
			return;
		}

		const int configured = ConfiguredCore(role);
		const char* source = "default";
		if (configured >= 0)
		{
			core = configured;
			source = "vm-cores.txt";
		}

		const unsigned long long mask = 1ULL << core;
		const int result = scePthreadSetaffinity(scePthreadSelf(), mask);
		// (AI-assisted) Core placement needs proper testing across base/Pro consoles.
		std::printf("PS4 thread affinity: %s core=%d (%s) mask=0x%llx sce_result=0x%x\n", role, core, source, mask, result);
		std::fflush(stdout);
	}
} // namespace PS4ThreadAffinity
