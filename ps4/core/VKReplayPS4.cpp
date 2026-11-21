// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) perf-test12: stage 1 of GS-RENDER-THREAD-PLAN.md. With /data/PS4SX2/vm-gs-replay-count the Vulkan
// entry points the GS uses are wrapped in pass-through counters. "Deferred" calls are the ones stage 2 would hand
// to a replay thread; any other hooked call would first wait for that thread, and when deferred calls are pending
// that wait is a "break". Few breaks per second means long runs of draws the replay thread could take.

#include "ps4/core/VKReplayPS4.h"
#include "GS/Renderers/Vulkan/VKLoader.h"
#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <unistd.h>

namespace PS4VkReplay
{
	bool g_count = false;

	namespace
	{
		constexpr int MAX_IDS = 64;
		const char* s_names[MAX_IDS] = {};
		bool s_deferred[MAX_IDS] = {};
		std::atomic<unsigned long long> s_calls[MAX_IDS];
		std::atomic<unsigned long long> s_breaks[MAX_IDS];
		std::atomic<bool> s_pending{false};

		void Note(int id)
		{
			s_calls[id].fetch_add(1, std::memory_order_relaxed);
			if (s_deferred[id])
				s_pending.store(true, std::memory_order_relaxed);
			else if (s_pending.exchange(false, std::memory_order_relaxed))
				s_breaks[id].fetch_add(1, std::memory_order_relaxed);
		}

		template <int Id, typename Fn>
		struct Hook;
		template <int Id, typename R, typename... A>
		struct Hook<Id, R(VKAPI_PTR*)(A...)>
		{
			static inline R(VKAPI_PTR* real)(A...) = nullptr;
			static R VKAPI_PTR Call(A... args)
			{
				Note(Id);
				return real(args...);
			}
		};
	} // namespace

	void Install()
	{
		g_count = access("/data/PS4SX2/vm-gs-replay-count", F_OK) == 0;
		if (!g_count)
			return;

		int hooked = 0;
#define PS4_VK_HOOK(name, id, deferred) \
	do \
	{ \
		static_assert(id < MAX_IDS); \
		using H = Hook<id, PFN_##name>; \
		if (name) \
		{ \
			H::real = name; \
			name = &H::Call; \
			s_names[id] = #name; \
			s_deferred[id] = deferred; \
			hooked++; \
		} \
	} while (0)

		// Stage 2's deferred set: the draw path.
		PS4_VK_HOOK(vkCmdDrawIndexed, 0, true);
		PS4_VK_HOOK(vkCmdDraw, 1, true);
		PS4_VK_HOOK(vkCmdBindPipeline, 2, true);
		PS4_VK_HOOK(vkCmdBindDescriptorSets, 3, true);
		PS4_VK_HOOK(vkCmdPushDescriptorSetKHR, 4, true);
		PS4_VK_HOOK(vkCmdPushConstants, 5, true);
		PS4_VK_HOOK(vkCmdSetViewport, 6, true);
		PS4_VK_HOOK(vkCmdSetScissor, 7, true);
		PS4_VK_HOOK(vkCmdSetLineWidth, 8, true);
		PS4_VK_HOOK(vkCmdSetBlendConstants, 9, true);
		PS4_VK_HOOK(vkCmdBindVertexBuffers, 10, true);
		PS4_VK_HOOK(vkCmdBindIndexBuffer, 11, true);
		PS4_VK_HOOK(vkCmdPipelineBarrier, 12, true);
		PS4_VK_HOOK(vkCmdBeginRenderPass, 13, true);
		PS4_VK_HOOK(vkCmdEndRenderPass, 14, true);
		PS4_VK_HOOK(vkCmdClearAttachments, 15, true);

		// Other command-buffer calls: direct (after a wait) in stage 2, candidates for stage 3.
		PS4_VK_HOOK(vkCmdWriteTimestamp, 16, false);
		PS4_VK_HOOK(vkCmdResetQueryPool, 17, false);
		PS4_VK_HOOK(vkCmdBeginQuery, 18, false);
		PS4_VK_HOOK(vkCmdEndQuery, 19, false);
		PS4_VK_HOOK(vkCmdClearColorImage, 20, false);
		PS4_VK_HOOK(vkCmdClearDepthStencilImage, 21, false);
		PS4_VK_HOOK(vkCmdCopyImage, 22, false);
		PS4_VK_HOOK(vkCmdBlitImage, 23, false);
		PS4_VK_HOOK(vkCmdCopyBufferToImage, 24, false);
		PS4_VK_HOOK(vkCmdCopyImageToBuffer, 25, false);
		PS4_VK_HOOK(vkCmdCopyBuffer, 26, false);
		PS4_VK_HOOK(vkCmdFillBuffer, 27, false);
		PS4_VK_HOOK(vkCmdDispatch, 28, false);

		// Calls that need the command buffer complete, or objects queued calls may still use: always a wait.
		PS4_VK_HOOK(vkBeginCommandBuffer, 32, false);
		PS4_VK_HOOK(vkEndCommandBuffer, 33, false);
		PS4_VK_HOOK(vkQueueSubmit, 34, false);
		PS4_VK_HOOK(vkResetCommandPool, 35, false);
		PS4_VK_HOOK(vkResetCommandBuffer, 36, false);
		PS4_VK_HOOK(vkUpdateDescriptorSets, 37, false);
		PS4_VK_HOOK(vkAllocateMemory, 38, false);
		PS4_VK_HOOK(vkFreeMemory, 39, false);
		PS4_VK_HOOK(vkCreateImage, 40, false);
		PS4_VK_HOOK(vkCreateImageView, 41, false);
		PS4_VK_HOOK(vkCreateBuffer, 42, false);
		PS4_VK_HOOK(vkCreateFramebuffer, 43, false);
		PS4_VK_HOOK(vkDestroyImage, 44, false);
		PS4_VK_HOOK(vkDestroyImageView, 45, false);
		PS4_VK_HOOK(vkDestroyBuffer, 46, false);
		PS4_VK_HOOK(vkDestroyFramebuffer, 47, false);
		PS4_VK_HOOK(vkWaitForFences, 48, false);
		PS4_VK_HOOK(vkDeviceWaitIdle, 49, false);
		PS4_VK_HOOK(vkQueueWaitIdle, 50, false);
		PS4_VK_HOOK(vkBindImageMemory, 51, false);
		PS4_VK_HOOK(vkBindBufferMemory, 52, false);
		PS4_VK_HOOK(vkCreateGraphicsPipelines, 53, false);
		PS4_VK_HOOK(vkCreateComputePipelines, 54, false);
#undef PS4_VK_HOOK

		Console.WriteLn("GS replay counting: enabled by /data/PS4SX2/vm-gs-replay-count (%d Vulkan functions hooked, pass-through)", hooked);
	}

	void FormatStats(char* out, size_t size)
	{
		unsigned long long deferred = 0, waits = 0, breaks = 0;
		int order[MAX_IDS];
		int n = 0;
		for (int i = 0; i < MAX_IDS; i++)
		{
			if (!s_names[i])
				continue;
			const unsigned long long calls = s_calls[i].load(std::memory_order_relaxed);
			(s_deferred[i] ? deferred : waits) += calls;
			breaks += s_breaks[i].load(std::memory_order_relaxed);
			if (!s_deferred[i])
				order[n++] = i;
		}
		std::sort(order, order + n, [](int a, int b) {
			return s_breaks[a].load(std::memory_order_relaxed) > s_breaks[b].load(std::memory_order_relaxed);
		});

		int len = std::snprintf(out, size, "GS replay count cumulative: deferred=%llu wait_calls=%llu breaks=%llu top_breaks:",
			deferred, waits, breaks);
		for (int k = 0; k < n && k < 6 && len > 0 && static_cast<size_t>(len) < size; k++)
			len += std::snprintf(out + len, size - len, " %s=%llu", s_names[order[k]] + 2,
				s_breaks[order[k]].load(std::memory_order_relaxed));
	}
} // namespace PS4VkReplay
