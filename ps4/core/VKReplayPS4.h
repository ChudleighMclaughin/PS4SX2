// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) perf-test12/13: Vulkan call hooks for the GS render thread plan (GS-RENDER-THREAD-PLAN.md).
#pragma once

#include <cstddef>

namespace PS4VkReplay
{
	// Stage 1 (vm-gs-replay-count): count, per Vulkan function, the calls a replay thread could defer and the
	// calls that would make the GS thread wait for it. Pass-through: no behaviour change.
	extern bool g_count;

	// Stage 2 (vm-gs-replay): the draw-path vkCmd* calls are copied into a ring and recorded by a replay thread
	// (core 0, vm-cores.txt "GSREPLAY="); every other hooked call first waits until the ring is empty.
	extern bool g_replay;

	// After Vulkan::LoadVulkanDeviceFunctions, before anything else reads the entry points (VMA included).
	void Install();

	// One heartbeat line (cumulative counts).
	void FormatStats(char* out, size_t size);
} // namespace PS4VkReplay
