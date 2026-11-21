// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) perf-test12: Vulkan call hooks for the GS render thread plan (GS-RENDER-THREAD-PLAN.md).
#pragma once

#include <cstddef>

namespace PS4VkReplay
{
	// Stage 1 (vm-gs-replay-count): count, per Vulkan function, the calls a replay thread could defer and the
	// calls that would make the GS thread wait for it. Pass-through: no behaviour change.
	extern bool g_count;

	// After Vulkan::LoadVulkanDeviceFunctions, before anything else reads the entry points (VMA included).
	void Install();

	// One heartbeat line (cumulative counts and the six most frequent causes of a wait).
	void FormatStats(char* out, size_t size);
} // namespace PS4VkReplay
