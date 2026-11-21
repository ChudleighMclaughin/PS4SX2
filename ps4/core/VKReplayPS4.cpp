// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) perf-test12: stage 1 of GS-RENDER-THREAD-PLAN.md. With /data/PS4SX2/vm-gs-replay-count the Vulkan
// entry points the GS uses are wrapped in pass-through counters. "Deferred" calls are the ones stage 2 would hand
// to a replay thread; any other hooked call would first wait for that thread, and when deferred calls are pending
// that wait is a "break". Few breaks per second means long runs of draws the replay thread could take.

#include "ps4/core/VKReplayPS4.h"
#include "ps4/core/ThreadAffinityPS4.h"
#include "GS/Renderers/Vulkan/VKLoader.h"
#include "common/Console.h"
#include "common/Threading.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <type_traits>
#include <unistd.h>
#include <x86intrin.h>

namespace PS4VkReplay
{
	bool g_count = false;
	bool g_replay = false;

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

	static void InstallCount()
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

	// ---------------------------------------------------------------------------------------------------------
	// (AI-assisted) perf-test13: stage 2, the replay thread.
	// One producer (the thread that last began a command buffer: the GS thread) copies deferred vkCmd* calls
	// into a byte ring; the replay thread on its own core calls the real functions in the same order. Every
	// other hooked call (submit, end/begin, create/destroy, fence waits, copies we don't defer...) first waits
	// until the ring is empty, so the command buffers and the objects they name are never used by both threads
	// at once. Calls from any other thread wait and go direct.
	// ---------------------------------------------------------------------------------------------------------
	namespace
	{
		constexpr size_t RING_SIZE = 8u << 20;
		constexpr size_t MAX_RECORD = RING_SIZE / 8;

		struct RecHeader
		{
			uint32_t size; // whole record, header included, multiple of 16
			uint32_t pad;
			void (*exec)(uint8_t* body); // nullptr: skip to the end of the ring
		};
		static_assert(sizeof(RecHeader) == 16);

		uint8_t* s_ring = nullptr;
		uint64_t s_wpos = 0; // producer only
		alignas(64) std::atomic<uint64_t> s_write{0};
		alignas(64) std::atomic<uint64_t> s_read{0};
		std::atomic<bool> s_sleeping{false};
		std::mutex s_mutex;
		std::condition_variable s_cv;
		bool s_started = false;
		bool s_ready = false; // guarded by s_mutex
		pthread_t s_owner{};
		uint8_t* s_body = nullptr; // record being written
		uint32_t s_total = 0;

		std::atomic<unsigned long long> s_deferred{0}, s_drains{0}, s_drain_tsc{0}, s_ring_waits{0}, s_fallbacks{0},
			s_sleeps{0};

		constexpr size_t R8(size_t n) { return (n + 7) & ~size_t(7); }
		constexpr size_t R16(size_t n) { return (n + 15) & ~size_t(15); }

		bool Defer() { return s_started && pthread_equal(pthread_self(), s_owner); }

		void Drain()
		{
			if (!s_started)
				return;
			// >=: on another thread the producer may keep posting; we only need what was queued before this call.
			const uint64_t target = s_write.load(std::memory_order_acquire);
			if (s_read.load(std::memory_order_acquire) >= target)
				return;
			const uint64_t t0 = __rdtsc();
			for (uint32_t spins = 0; s_read.load(std::memory_order_acquire) < target; spins++)
			{
				_mm_pause();
				if ((spins & 0xffff) == 0xffff)
					sched_yield();
			}
			s_drains.fetch_add(1, std::memory_order_relaxed);
			s_drain_tsc.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
		}

		// Space for a record whose body is `bytes` long, or nullptr (too big: the caller drains and goes direct).
		uint8_t* Begin(size_t bytes)
		{
			const size_t total = R16(sizeof(RecHeader) + bytes);
			if (total > MAX_RECORD)
				return nullptr;
			const size_t off = s_wpos % RING_SIZE;
			const size_t contig = RING_SIZE - off;
			const size_t need = total <= contig ? total : contig + total;
			if (s_wpos + need - s_read.load(std::memory_order_acquire) > RING_SIZE)
			{
				s_ring_waits.fetch_add(1, std::memory_order_relaxed);
				while (s_wpos + need - s_read.load(std::memory_order_acquire) > RING_SIZE)
					_mm_pause();
			}
			if (total > contig)
			{
				RecHeader* skip = reinterpret_cast<RecHeader*>(s_ring + off);
				skip->size = static_cast<uint32_t>(contig);
				skip->exec = nullptr;
				s_wpos += contig;
			}
			s_total = static_cast<uint32_t>(total);
			s_body = s_ring + (s_wpos % RING_SIZE) + sizeof(RecHeader);
			return s_body;
		}

		void End(void (*exec)(uint8_t*))
		{
			RecHeader* h = reinterpret_cast<RecHeader*>(s_body - sizeof(RecHeader));
			h->size = s_total;
			h->exec = exec;
			s_wpos += s_total;
			s_write.store(s_wpos, std::memory_order_seq_cst);
			s_deferred.fetch_add(1, std::memory_order_relaxed);
			if (s_sleeping.load(std::memory_order_seq_cst))
			{
				std::lock_guard lock(s_mutex);
				s_cv.notify_one();
			}
		}

		// Copies n elements after the record's fixed part; returns the copy (or nullptr for none).
		template <typename T>
		T* CopyArray(uint8_t*& cursor, const T* src, uint32_t n)
		{
			if (!src || n == 0)
				return nullptr;
			T* dst = reinterpret_cast<T*>(cursor);
			std::memcpy(dst, src, n * sizeof(T));
			cursor += R8(n * sizeof(T));
			return dst;
		}

		template <typename T>
		size_t ArrayBytes(const T* src, uint32_t n) { return (src && n) ? R8(n * sizeof(T)) : 0; }

		void WorkerMain()
		{
			PS4ThreadAffinity::PinCurrentThread("GSREPLAY", 0);
			{
				std::lock_guard lock(s_mutex);
				s_ready = true;
			}
			s_cv.notify_all();

			uint64_t r = s_read.load(std::memory_order_relaxed);
			for (;;)
			{
				uint64_t w = s_write.load(std::memory_order_acquire);
				for (uint32_t spins = 0; w == r; spins++)
				{
					if (spins < 20000)
					{
						_mm_pause();
						w = s_write.load(std::memory_order_acquire);
						continue;
					}
					std::unique_lock lock(s_mutex);
					s_sleeping.store(true, std::memory_order_seq_cst);
					s_cv.wait(lock, [&] { return (w = s_write.load(std::memory_order_seq_cst)) != r; });
					s_sleeping.store(false, std::memory_order_relaxed);
					s_sleeps.fetch_add(1, std::memory_order_relaxed);
					break;
				}
				while (r != w)
				{
					RecHeader* h = reinterpret_cast<RecHeader*>(s_ring + (r % RING_SIZE));
					if (h->exec)
						h->exec(reinterpret_cast<uint8_t*>(h + 1));
					r += h->size;
				}
				s_read.store(r, std::memory_order_release);
			}
		}

		// Called on the thread that begins a command buffer; it becomes the producer. Started once, with the
		// creator sleeping until the worker has left the creator's core (a new thread inherits it: perf-test8).
		void StartWorker()
		{
			void* raw = std::malloc(RING_SIZE + 64); // kept for the process, like the thread
			s_ring = raw ? reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(raw) + 63) & ~uintptr_t(63)) : nullptr;
			if (!s_ring)
			{
				Console.Error("GS replay: ring allocation failed; recording stays on the GS thread");
				g_replay = false;
				return;
			}
			Threading::Thread worker; // detached when it goes out of scope
			worker.SetStackSize(256 * 1024);
			if (!worker.Start(WorkerMain))
			{
				Console.Error("GS replay: thread creation failed; recording stays on the GS thread");
				g_replay = false;
				return;
			}
			std::unique_lock lock(s_mutex);
			s_cv.wait(lock, [] { return s_ready; });
			s_started = true;
		}

		// Calls that must not overlap queued work: wait for the ring to empty, then call directly.
		template <auto* Var, typename Fn>
		struct DrainHookImpl;
		template <auto* Var, typename R, typename... A>
		struct DrainHookImpl<Var, R(VKAPI_PTR*)(A...)>
		{
			static inline R(VKAPI_PTR* real)(A...) = nullptr;
			static R VKAPI_PTR Call(A... args)
			{
				Drain();
				return real(args...);
			}
		};
		template <auto* Var>
		using DrainHook = DrainHookImpl<Var, std::remove_pointer_t<decltype(Var)>>;

		PFN_vkBeginCommandBuffer r_BeginCommandBuffer;
		VkResult VKAPI_PTR H_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info)
		{
			Drain();
			if (g_replay && !s_started)
				StartWorker();
			s_owner = pthread_self();
			return r_BeginCommandBuffer(cb, info);
		}

		// Deferred calls. Each copies its arguments (and every array they point to) into the ring.
#define PS4_DEFER_OR_DIRECT(bytes, direct) \
	uint8_t* body = Defer() ? Begin(bytes) : nullptr; \
	if (!body) \
	{ \
		if (s_started) \
			s_fallbacks.fetch_add(1, std::memory_order_relaxed); \
		Drain(); \
		direct; \
		return; \
	}

		PFN_vkCmdDrawIndexed r_DrawIndexed;
		void VKAPI_PTR H_DrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t inst, uint32_t fi, int32_t vo, uint32_t finst)
		{
			struct A { VkCommandBuffer cb; uint32_t ic, inst, fi; int32_t vo; uint32_t finst; };
			PS4_DEFER_OR_DIRECT(sizeof(A), r_DrawIndexed(cb, ic, inst, fi, vo, finst));
			*reinterpret_cast<A*>(body) = {cb, ic, inst, fi, vo, finst};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_DrawIndexed(a->cb, a->ic, a->inst, a->fi, a->vo, a->finst); });
		}

		PFN_vkCmdDraw r_Draw;
		void VKAPI_PTR H_Draw(VkCommandBuffer cb, uint32_t vc, uint32_t inst, uint32_t fv, uint32_t finst)
		{
			struct A { VkCommandBuffer cb; uint32_t vc, inst, fv, finst; };
			PS4_DEFER_OR_DIRECT(sizeof(A), r_Draw(cb, vc, inst, fv, finst));
			*reinterpret_cast<A*>(body) = {cb, vc, inst, fv, finst};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_Draw(a->cb, a->vc, a->inst, a->fv, a->finst); });
		}

		PFN_vkCmdBindPipeline r_BindPipeline;
		void VKAPI_PTR H_BindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pipe)
		{
			struct A { VkCommandBuffer cb; VkPipelineBindPoint bp; VkPipeline pipe; };
			PS4_DEFER_OR_DIRECT(sizeof(A), r_BindPipeline(cb, bp, pipe));
			*reinterpret_cast<A*>(body) = {cb, bp, pipe};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_BindPipeline(a->cb, a->bp, a->pipe); });
		}

		PFN_vkCmdBindDescriptorSets r_BindDescriptorSets;
		void VKAPI_PTR H_BindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first,
			uint32_t n, const VkDescriptorSet* sets, uint32_t dn, const uint32_t* dyn)
		{
			struct A { VkCommandBuffer cb; VkPipelineBindPoint bp; VkPipelineLayout layout; uint32_t first, n, dn; VkDescriptorSet* sets; uint32_t* dyn; };
			PS4_DEFER_OR_DIRECT(R8(sizeof(A)) + ArrayBytes(sets, n) + ArrayBytes(dyn, dn),
				r_BindDescriptorSets(cb, bp, layout, first, n, sets, dn, dyn));
			A* a = reinterpret_cast<A*>(body);
			uint8_t* cur = body + R8(sizeof(A));
			*a = {cb, bp, layout, first, n, dn, CopyArray(cur, sets, n), CopyArray(cur, dyn, dn)};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_BindDescriptorSets(a->cb, a->bp, a->layout, a->first, a->n, a->sets, a->dn, a->dyn); });
		}

		PFN_vkCmdPushDescriptorSetKHR r_PushDescriptorSet;
		void VKAPI_PTR H_PushDescriptorSet(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set,
			uint32_t n, const VkWriteDescriptorSet* writes)
		{
			struct A { VkCommandBuffer cb; VkPipelineBindPoint bp; VkPipelineLayout layout; uint32_t set, n; VkWriteDescriptorSet* writes; };
			size_t bytes = R8(sizeof(A)) + ArrayBytes(writes, n);
			bool simple = true;
			for (uint32_t i = 0; i < n; i++)
			{
				simple &= writes[i].pNext == nullptr;
				bytes += ArrayBytes(writes[i].pImageInfo, writes[i].descriptorCount) +
				         ArrayBytes(writes[i].pBufferInfo, writes[i].descriptorCount) +
				         ArrayBytes(writes[i].pTexelBufferView, writes[i].descriptorCount);
			}
			PS4_DEFER_OR_DIRECT(simple ? bytes : MAX_RECORD, r_PushDescriptorSet(cb, bp, layout, set, n, writes));
			A* a = reinterpret_cast<A*>(body);
			uint8_t* cur = body + R8(sizeof(A));
			*a = {cb, bp, layout, set, n, CopyArray(cur, writes, n)};
			for (uint32_t i = 0; a->writes && i < n; i++)
			{
				VkWriteDescriptorSet& w = a->writes[i];
				w.pImageInfo = CopyArray(cur, w.pImageInfo, w.descriptorCount);
				w.pBufferInfo = CopyArray(cur, w.pBufferInfo, w.descriptorCount);
				w.pTexelBufferView = CopyArray(cur, w.pTexelBufferView, w.descriptorCount);
			}
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_PushDescriptorSet(a->cb, a->bp, a->layout, a->set, a->n, a->writes); });
		}

		PFN_vkCmdPushConstants r_PushConstants;
		void VKAPI_PTR H_PushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t off,
			uint32_t size, const void* data)
		{
			struct A { VkCommandBuffer cb; VkPipelineLayout layout; VkShaderStageFlags stages; uint32_t off, size; uint8_t* data; };
			const uint8_t* bytes = static_cast<const uint8_t*>(data);
			PS4_DEFER_OR_DIRECT(R8(sizeof(A)) + ArrayBytes(bytes, size), r_PushConstants(cb, layout, stages, off, size, data));
			A* a = reinterpret_cast<A*>(body);
			uint8_t* cur = body + R8(sizeof(A));
			*a = {cb, layout, stages, off, size, CopyArray(cur, bytes, size)};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_PushConstants(a->cb, a->layout, a->stages, a->off, a->size, a->data); });
		}

		PFN_vkCmdSetViewport r_SetViewport;
		void VKAPI_PTR H_SetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* vp)
		{
			struct A { VkCommandBuffer cb; uint32_t first, n; VkViewport* vp; };
			PS4_DEFER_OR_DIRECT(R8(sizeof(A)) + ArrayBytes(vp, n), r_SetViewport(cb, first, n, vp));
			A* a = reinterpret_cast<A*>(body);
			uint8_t* cur = body + R8(sizeof(A));
			*a = {cb, first, n, CopyArray(cur, vp, n)};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_SetViewport(a->cb, a->first, a->n, a->vp); });
		}

		PFN_vkCmdSetScissor r_SetScissor;
		void VKAPI_PTR H_SetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* rects)
		{
			struct A { VkCommandBuffer cb; uint32_t first, n; VkRect2D* rects; };
			PS4_DEFER_OR_DIRECT(R8(sizeof(A)) + ArrayBytes(rects, n), r_SetScissor(cb, first, n, rects));
			A* a = reinterpret_cast<A*>(body);
			uint8_t* cur = body + R8(sizeof(A));
			*a = {cb, first, n, CopyArray(cur, rects, n)};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_SetScissor(a->cb, a->first, a->n, a->rects); });
		}

		PFN_vkCmdSetLineWidth r_SetLineWidth;
		void VKAPI_PTR H_SetLineWidth(VkCommandBuffer cb, float width)
		{
			struct A { VkCommandBuffer cb; float width; };
			PS4_DEFER_OR_DIRECT(sizeof(A), r_SetLineWidth(cb, width));
			*reinterpret_cast<A*>(body) = {cb, width};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_SetLineWidth(a->cb, a->width); });
		}

		PFN_vkCmdSetBlendConstants r_SetBlendConstants;
		void VKAPI_PTR H_SetBlendConstants(VkCommandBuffer cb, const float c[4])
		{
			struct A { VkCommandBuffer cb; float c[4]; };
			PS4_DEFER_OR_DIRECT(sizeof(A), r_SetBlendConstants(cb, c));
			A* a = reinterpret_cast<A*>(body);
			a->cb = cb;
			std::memcpy(a->c, c, sizeof(a->c));
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_SetBlendConstants(a->cb, a->c); });
		}

		PFN_vkCmdBindVertexBuffers r_BindVertexBuffers;
		void VKAPI_PTR H_BindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* bufs, const VkDeviceSize* offs)
		{
			struct A { VkCommandBuffer cb; uint32_t first, n; VkBuffer* bufs; VkDeviceSize* offs; };
			PS4_DEFER_OR_DIRECT(R8(sizeof(A)) + ArrayBytes(bufs, n) + ArrayBytes(offs, n), r_BindVertexBuffers(cb, first, n, bufs, offs));
			A* a = reinterpret_cast<A*>(body);
			uint8_t* cur = body + R8(sizeof(A));
			*a = {cb, first, n, CopyArray(cur, bufs, n), CopyArray(cur, offs, n)};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_BindVertexBuffers(a->cb, a->first, a->n, a->bufs, a->offs); });
		}

		PFN_vkCmdBindIndexBuffer r_BindIndexBuffer;
		void VKAPI_PTR H_BindIndexBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType type)
		{
			struct A { VkCommandBuffer cb; VkBuffer buf; VkDeviceSize off; VkIndexType type; };
			PS4_DEFER_OR_DIRECT(sizeof(A), r_BindIndexBuffer(cb, buf, off, type));
			*reinterpret_cast<A*>(body) = {cb, buf, off, type};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_BindIndexBuffer(a->cb, a->buf, a->off, a->type); });
		}

		PFN_vkCmdPipelineBarrier r_PipelineBarrier;
		void VKAPI_PTR H_PipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
			uint32_t nm, const VkMemoryBarrier* mb, uint32_t nb, const VkBufferMemoryBarrier* bb, uint32_t ni, const VkImageMemoryBarrier* ib)
		{
			struct A { VkCommandBuffer cb; VkPipelineStageFlags src, dst; VkDependencyFlags dep; uint32_t nm, nb, ni;
				VkMemoryBarrier* mb; VkBufferMemoryBarrier* bb; VkImageMemoryBarrier* ib; };
			bool simple = true;
			for (uint32_t i = 0; mb && i < nm; i++) simple &= mb[i].pNext == nullptr;
			for (uint32_t i = 0; bb && i < nb; i++) simple &= bb[i].pNext == nullptr;
			for (uint32_t i = 0; ib && i < ni; i++) simple &= ib[i].pNext == nullptr;
			PS4_DEFER_OR_DIRECT(simple ? R8(sizeof(A)) + ArrayBytes(mb, nm) + ArrayBytes(bb, nb) + ArrayBytes(ib, ni) : MAX_RECORD,
				r_PipelineBarrier(cb, src, dst, dep, nm, mb, nb, bb, ni, ib));
			A* a = reinterpret_cast<A*>(body);
			uint8_t* cur = body + R8(sizeof(A));
			*a = {cb, src, dst, dep, nm, nb, ni, CopyArray(cur, mb, nm), CopyArray(cur, bb, nb), CopyArray(cur, ib, ni)};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p);
				r_PipelineBarrier(a->cb, a->src, a->dst, a->dep, a->nm, a->mb, a->nb, a->bb, a->ni, a->ib); });
		}

		PFN_vkCmdBeginRenderPass r_BeginRenderPass;
		void VKAPI_PTR H_BeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, VkSubpassContents contents)
		{
			struct A { VkCommandBuffer cb; VkRenderPassBeginInfo info; VkSubpassContents contents; };
			const bool simple = info->pNext == nullptr;
			PS4_DEFER_OR_DIRECT(simple ? R8(sizeof(A)) + ArrayBytes(info->pClearValues, info->clearValueCount) : MAX_RECORD,
				r_BeginRenderPass(cb, info, contents));
			A* a = reinterpret_cast<A*>(body);
			uint8_t* cur = body + R8(sizeof(A));
			a->cb = cb;
			a->info = *info;
			a->info.pClearValues = CopyArray(cur, info->pClearValues, info->clearValueCount);
			a->contents = contents;
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_BeginRenderPass(a->cb, &a->info, a->contents); });
		}

		PFN_vkCmdEndRenderPass r_EndRenderPass;
		void VKAPI_PTR H_EndRenderPass(VkCommandBuffer cb)
		{
			struct A { VkCommandBuffer cb; };
			PS4_DEFER_OR_DIRECT(sizeof(A), r_EndRenderPass(cb));
			*reinterpret_cast<A*>(body) = {cb};
			End([](uint8_t* p) { r_EndRenderPass(reinterpret_cast<A*>(p)->cb); });
		}

		PFN_vkCmdClearAttachments r_ClearAttachments;
		void VKAPI_PTR H_ClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment* att, uint32_t nr, const VkClearRect* rects)
		{
			struct A { VkCommandBuffer cb; uint32_t na, nr; VkClearAttachment* att; VkClearRect* rects; };
			PS4_DEFER_OR_DIRECT(R8(sizeof(A)) + ArrayBytes(att, na) + ArrayBytes(rects, nr), r_ClearAttachments(cb, na, att, nr, rects));
			A* a = reinterpret_cast<A*>(body);
			uint8_t* cur = body + R8(sizeof(A));
			*a = {cb, na, nr, CopyArray(cur, att, na), CopyArray(cur, rects, nr)};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_ClearAttachments(a->cb, a->na, a->att, a->nr, a->rects); });
		}

		PFN_vkCmdWriteTimestamp r_WriteTimestamp;
		void VKAPI_PTR H_WriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query)
		{
			struct A { VkCommandBuffer cb; VkPipelineStageFlagBits stage; VkQueryPool pool; uint32_t query; };
			PS4_DEFER_OR_DIRECT(sizeof(A), r_WriteTimestamp(cb, stage, pool, query));
			*reinterpret_cast<A*>(body) = {cb, stage, pool, query};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_WriteTimestamp(a->cb, a->stage, a->pool, a->query); });
		}

		PFN_vkCmdClearColorImage r_ClearColorImage;
		void VKAPI_PTR H_ClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout layout, const VkClearColorValue* color,
			uint32_t n, const VkImageSubresourceRange* ranges)
		{
			struct A { VkCommandBuffer cb; VkImage img; VkImageLayout layout; VkClearColorValue color; uint32_t n; VkImageSubresourceRange* ranges; };
			PS4_DEFER_OR_DIRECT(R8(sizeof(A)) + ArrayBytes(ranges, n), r_ClearColorImage(cb, img, layout, color, n, ranges));
			A* a = reinterpret_cast<A*>(body);
			uint8_t* cur = body + R8(sizeof(A));
			*a = {cb, img, layout, *color, n, CopyArray(cur, ranges, n)};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_ClearColorImage(a->cb, a->img, a->layout, &a->color, a->n, a->ranges); });
		}

		PFN_vkCmdClearDepthStencilImage r_ClearDepthStencilImage;
		void VKAPI_PTR H_ClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout layout, const VkClearDepthStencilValue* value,
			uint32_t n, const VkImageSubresourceRange* ranges)
		{
			struct A { VkCommandBuffer cb; VkImage img; VkImageLayout layout; VkClearDepthStencilValue value; uint32_t n; VkImageSubresourceRange* ranges; };
			PS4_DEFER_OR_DIRECT(R8(sizeof(A)) + ArrayBytes(ranges, n), r_ClearDepthStencilImage(cb, img, layout, value, n, ranges));
			A* a = reinterpret_cast<A*>(body);
			uint8_t* cur = body + R8(sizeof(A));
			*a = {cb, img, layout, *value, n, CopyArray(cur, ranges, n)};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_ClearDepthStencilImage(a->cb, a->img, a->layout, &a->value, a->n, a->ranges); });
		}

		PFN_vkCmdCopyImage r_CopyImage;
		void VKAPI_PTR H_CopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy* regions)
		{
			struct A { VkCommandBuffer cb; VkImage src; VkImageLayout sl; VkImage dst; VkImageLayout dl; uint32_t n; VkImageCopy* regions; };
			PS4_DEFER_OR_DIRECT(R8(sizeof(A)) + ArrayBytes(regions, n), r_CopyImage(cb, src, sl, dst, dl, n, regions));
			A* a = reinterpret_cast<A*>(body);
			uint8_t* cur = body + R8(sizeof(A));
			*a = {cb, src, sl, dst, dl, n, CopyArray(cur, regions, n)};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_CopyImage(a->cb, a->src, a->sl, a->dst, a->dl, a->n, a->regions); });
		}

		PFN_vkCmdCopyBufferToImage r_CopyBufferToImage;
		void VKAPI_PTR H_CopyBufferToImage(VkCommandBuffer cb, VkBuffer buf, VkImage img, VkImageLayout layout, uint32_t n, const VkBufferImageCopy* regions)
		{
			struct A { VkCommandBuffer cb; VkBuffer buf; VkImage img; VkImageLayout layout; uint32_t n; VkBufferImageCopy* regions; };
			PS4_DEFER_OR_DIRECT(R8(sizeof(A)) + ArrayBytes(regions, n), r_CopyBufferToImage(cb, buf, img, layout, n, regions));
			A* a = reinterpret_cast<A*>(body);
			uint8_t* cur = body + R8(sizeof(A));
			*a = {cb, buf, img, layout, n, CopyArray(cur, regions, n)};
			End([](uint8_t* p) { A* a = reinterpret_cast<A*>(p); r_CopyBufferToImage(a->cb, a->buf, a->img, a->layout, a->n, a->regions); });
		}
#undef PS4_DEFER_OR_DIRECT
	} // namespace

	static void InstallReplay()
	{
		int deferred = 0, draining = 0;
#define PS4_VK_DEFER(name, local) \
	do \
	{ \
		if (name) \
		{ \
			r_##local = name; \
			name = &H_##local; \
			deferred++; \
		} \
	} while (0)
#define PS4_VK_DRAIN(name) \
	do \
	{ \
		if (name) \
		{ \
			DrainHook<&name>::real = name; \
			name = &DrainHook<&name>::Call; \
			draining++; \
		} \
	} while (0)

		PS4_VK_DEFER(vkCmdDrawIndexed, DrawIndexed);
		PS4_VK_DEFER(vkCmdDraw, Draw);
		PS4_VK_DEFER(vkCmdBindPipeline, BindPipeline);
		PS4_VK_DEFER(vkCmdBindDescriptorSets, BindDescriptorSets);
		PS4_VK_DEFER(vkCmdPushDescriptorSetKHR, PushDescriptorSet);
		PS4_VK_DEFER(vkCmdPushConstants, PushConstants);
		PS4_VK_DEFER(vkCmdSetViewport, SetViewport);
		PS4_VK_DEFER(vkCmdSetScissor, SetScissor);
		PS4_VK_DEFER(vkCmdSetLineWidth, SetLineWidth);
		PS4_VK_DEFER(vkCmdSetBlendConstants, SetBlendConstants);
		PS4_VK_DEFER(vkCmdBindVertexBuffers, BindVertexBuffers);
		PS4_VK_DEFER(vkCmdBindIndexBuffer, BindIndexBuffer);
		PS4_VK_DEFER(vkCmdPipelineBarrier, PipelineBarrier);
		PS4_VK_DEFER(vkCmdBeginRenderPass, BeginRenderPass);
		PS4_VK_DEFER(vkCmdEndRenderPass, EndRenderPass);
		PS4_VK_DEFER(vkCmdClearAttachments, ClearAttachments);
		PS4_VK_DEFER(vkCmdWriteTimestamp, WriteTimestamp);
		PS4_VK_DEFER(vkCmdClearColorImage, ClearColorImage);
		PS4_VK_DEFER(vkCmdClearDepthStencilImage, ClearDepthStencilImage);
		PS4_VK_DEFER(vkCmdCopyImage, CopyImage);
		PS4_VK_DEFER(vkCmdCopyBufferToImage, CopyBufferToImage);

		if (vkBeginCommandBuffer)
		{
			r_BeginCommandBuffer = vkBeginCommandBuffer;
			vkBeginCommandBuffer = &H_BeginCommandBuffer;
			draining++;
		}

		// Every other command-buffer call the GS makes: direct, after the queue is empty.
		PS4_VK_DRAIN(vkCmdResetQueryPool);
		PS4_VK_DRAIN(vkCmdBeginQuery);
		PS4_VK_DRAIN(vkCmdEndQuery);
		PS4_VK_DRAIN(vkCmdBlitImage);
		PS4_VK_DRAIN(vkCmdCopyImageToBuffer);
		PS4_VK_DRAIN(vkCmdCopyBuffer);
		PS4_VK_DRAIN(vkCmdFillBuffer);
		PS4_VK_DRAIN(vkCmdDispatch);
		PS4_VK_DRAIN(vkCmdBeginDebugUtilsLabelEXT);
		PS4_VK_DRAIN(vkCmdEndDebugUtilsLabelEXT);
		PS4_VK_DRAIN(vkCmdInsertDebugUtilsLabelEXT);

		// Calls that end or reuse command buffers, or create/destroy/rewrite objects queued calls may name.
		PS4_VK_DRAIN(vkEndCommandBuffer);
		PS4_VK_DRAIN(vkQueueSubmit);
		PS4_VK_DRAIN(vkQueuePresentKHR);
		PS4_VK_DRAIN(vkResetCommandPool);
		PS4_VK_DRAIN(vkResetCommandBuffer);
		PS4_VK_DRAIN(vkAllocateCommandBuffers);
		PS4_VK_DRAIN(vkFreeCommandBuffers);
		PS4_VK_DRAIN(vkDestroyCommandPool);
		PS4_VK_DRAIN(vkUpdateDescriptorSets);
		PS4_VK_DRAIN(vkAllocateDescriptorSets);
		PS4_VK_DRAIN(vkFreeDescriptorSets);
		PS4_VK_DRAIN(vkResetDescriptorPool);
		PS4_VK_DRAIN(vkCreateDescriptorPool);
		PS4_VK_DRAIN(vkDestroyDescriptorPool);
		PS4_VK_DRAIN(vkCreateDescriptorSetLayout);
		PS4_VK_DRAIN(vkCreatePipelineLayout);
		PS4_VK_DRAIN(vkAllocateMemory);
		PS4_VK_DRAIN(vkFreeMemory);
		PS4_VK_DRAIN(vkBindImageMemory);
		PS4_VK_DRAIN(vkBindBufferMemory);
		PS4_VK_DRAIN(vkCreateImage);
		PS4_VK_DRAIN(vkDestroyImage);
		PS4_VK_DRAIN(vkCreateImageView);
		PS4_VK_DRAIN(vkDestroyImageView);
		PS4_VK_DRAIN(vkCreateBuffer);
		PS4_VK_DRAIN(vkDestroyBuffer);
		PS4_VK_DRAIN(vkCreateBufferView);
		PS4_VK_DRAIN(vkDestroyBufferView);
		PS4_VK_DRAIN(vkCreateFramebuffer);
		PS4_VK_DRAIN(vkDestroyFramebuffer);
		PS4_VK_DRAIN(vkCreateRenderPass);
		PS4_VK_DRAIN(vkDestroyRenderPass);
		PS4_VK_DRAIN(vkCreateSampler);
		PS4_VK_DRAIN(vkDestroySampler);
		PS4_VK_DRAIN(vkCreateShaderModule);
		PS4_VK_DRAIN(vkDestroyShaderModule);
		PS4_VK_DRAIN(vkCreateGraphicsPipelines);
		PS4_VK_DRAIN(vkCreateComputePipelines);
		PS4_VK_DRAIN(vkDestroyPipeline);
		PS4_VK_DRAIN(vkCreateQueryPool);
		PS4_VK_DRAIN(vkDestroyQueryPool);
		PS4_VK_DRAIN(vkGetQueryPoolResults);
		PS4_VK_DRAIN(vkWaitForFences);
		PS4_VK_DRAIN(vkDeviceWaitIdle);
		PS4_VK_DRAIN(vkQueueWaitIdle);
#undef PS4_VK_DEFER
#undef PS4_VK_DRAIN

		Console.WriteLn("GS replay thread: enabled by /data/PS4SX2/vm-gs-replay (%d calls deferred, %d wait first; core 0 unless vm-cores.txt sets GSREPLAY)",
			deferred, draining);
	}

	void Install()
	{
		// The device is created at emulator start, before a game (or PS1 mode) is chosen, so this is not per game.
		// Like the SPU2 worker, the replay thread needs thread affinity to leave the GS core.
		g_replay = access("/data/PS4SX2/vm-gs-replay", F_OK) == 0 && access("/data/PS4SX2/vm-no-thread-affinity", F_OK) != 0;
		if (g_replay)
			InstallReplay();
		else
			InstallCount();
	}

	void FormatStats(char* out, size_t size)
	{
		if (g_replay)
		{
			std::snprintf(out, size,
				"GS replay cumulative: started=%d deferred=%llu drains=%llu drain_ms=%.1f ring_full_waits=%llu direct_fallbacks=%llu worker_sleeps=%llu",
				s_started ? 1 : 0, s_deferred.load(std::memory_order_relaxed), s_drains.load(std::memory_order_relaxed),
				s_drain_tsc.load(std::memory_order_relaxed) / 1.6e6, s_ring_waits.load(std::memory_order_relaxed),
				s_fallbacks.load(std::memory_order_relaxed), s_sleeps.load(std::memory_order_relaxed));
			return;
		}

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
