// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// (AI-assisted) perf-test38: opt-in DMA worker (/data/PS4SX2/vm-dma-worker). See the PS4DmaWorker block in R5900.h.
//
// With Instant DMA, a VIF1 or GIF DMA used to run to its end inside the EE's event test, ~20% of the EE thread in GTA
// San Andreas and Shadow of the Colossus (perf-test36 time profile). Games start such a DMA only ~60-250 times a
// second and rarely look at it while it runs (perf-test37), so here the EE hands the pending VIF1/GIF/MTVU events to a
// thread on another core and keeps running the game. The worker runs the same handlers until the transfers are done
// (or stop on something only the EE can do), then the EE raises their interrupts at its next event test.
// Needs proper testing.

#include "Common.h"
#include "GS.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "VUmicro.h"
#include "Vif_Dma.h"
#include "common/Threading.h"
#include "common/Timer.h"
#include "ps4/core/ThreadAffinityPS4.h"

#include <immintrin.h>
#include <thread>

u64 g_ps4_dma_jobs, g_ps4_dma_giveups, g_ps4_dma_ee_waits; // EE thread
double g_ps4_dma_ee_wait_ms, g_ps4_dma_busy_ms; // EE thread / worker thread
// perf-test39: the EE's waits by reason (PS4DmaWorker::SyncReason, R5900.h).
constexpr int kSyncReasons = 16;
u64 g_ps4_dma_wait_n[kSyncReasons];
double g_ps4_dma_wait_ms[kSyncReasons];

namespace PS4DmaWorker
{
	bool g_enabled = false;
	std::atomic<int> g_state{0};

	namespace
	{
		constexpr u32 kJobMask = (1u << DMAC_VIF1) | (1u << DMAC_GIF) | (1u << VU_MTVU_BUSY) | (1u << VIF_VU1_FINISH);
		constexpr double kGiveUpNs = 2'000'000.0; // a job stuck this long goes back to the EE

		thread_local bool t_on_worker = false;
		thread_local bool t_on_ee = false;

		u32 s_job_mask = 0; // EE -> worker, before the kick
		u32 s_pending = 0; // worker only, during a job
		u32 s_return_mask = 0; // worker -> EE: events handed back
		std::atomic<u32> s_dmac_irqs{0}, s_intc_irqs{0};
		std::atomic<bool> s_ee_waiting{false}; // perf-test39: the EE is in SyncEE
		Threading::UserspaceSemaphore s_kick;
		std::atomic<bool> s_started{false};

		u64 Signature()
		{
			// Anything that moves while a VIF1 or GIF transfer makes progress.
			u64 s = vif1ch.madr;
			s = s * 31 + vif1ch.tadr;
			s = s * 31 + vif1ch.qwc;
			s = s * 31 + vif1ch.chcr._u32;
			s = s * 31 + gifch.madr;
			s = s * 31 + gifch.tadr;
			s = s * 31 + gifch.qwc;
			s = s * 31 + gifch.chcr._u32;
			s = s * 31 + vif1.tag.size;
			s = s * 31 + vif1.cmd;
			s = s * 31 + vif1Regs.num;
			s = s * 31 + vif1Regs.stat._u32;
			s = s * 31 + gifRegs.stat._u32;
			s = s * 31 + VU0.VI[REG_VPU_STAT].UL;
			return s;
		}

		void RunJob()
		{
			s_pending = s_job_mask;
			s_return_mask = 0;
			u64 signature = Signature();
			Common::Timer::Value stall_start = 0;
			while (s_pending)
			{
				const u32 p = s_pending;
				s_pending = 0;
				s_return_mask |= p & ~kJobMask; // anything else scheduled from here is the EE's
				// The EE's TESTINT order.
				if (p & (1u << VU_MTVU_BUSY))
					MTVUInterrupt();
				if (p & (1u << DMAC_VIF1))
					vif1Interrupt();
				if (p & (1u << DMAC_GIF))
					gifInterrupt();
				if (p & (1u << VIF_VU1_FINISH))
					vif1VUFinish();

				const u64 now_signature = Signature();
				if (now_signature != signature)
				{
					signature = now_signature;
					stall_start = 0;
					continue;
				}
				if (!s_pending)
					break;
				// No progress: waiting on VU1, the GS or something only the EE can do. Keep trying for a while
				// (VU1/GS waits end by themselves), then hand the rest back.
				// perf-test39: if the EE is already waiting for this job, a stalled job goes back at once. In GTA San
				// Andreas VIF1 waited on a GIF transfer the EE was about to start, while the EE waited on the job:
				// ~20 give-ups a second, each after the full 2 ms, ~20% of the EE waiting (PKG-44).
				const Common::Timer::Value now = Common::Timer::GetCurrentValue();
				if (!stall_start)
					stall_start = now;
				else if (s_ee_waiting.load(std::memory_order_relaxed) ||
						 Common::Timer::ConvertValueToNanoseconds(now - stall_start) > kGiveUpNs)
				{
					s_return_mask |= s_pending;
					s_pending = 0;
					++g_ps4_dma_giveups;
					break;
				}
				_mm_pause();
			}
		}

		void WorkerMain()
		{
			t_on_worker = true;
			PS4ThreadAffinity::PinCurrentThread("DMAWORKER", 5); // the light audio core (vm-cores.txt DMAWORKER=N)
			for (;;)
			{
				s_kick.Wait();
				const Common::Timer::Value t0 = Common::Timer::GetCurrentValue();
				RunJob();
				g_ps4_dma_busy_ms += Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - t0) / 1e6;
				g_state.store(2, std::memory_order_release);
			}
		}

		bool CanOffload()
		{
			// Instant DMA only (no cycle timing to keep), MTVU, DMA on and not suspended, no MFIFO, no stall control.
			return CHECK_INSTANTDMAHACK && THREAD_VU1 && dmacRegs.ctrl.DMAE && !(psHu8(DMAC_ENABLER + 2) & 1) &&
				   dmacRegs.ctrl.MFD == NO_MFD && dmacRegs.ctrl.STD == 0;
		}

		// EE thread, with g_state == 2.
		void ApplyFinished()
		{
			const u32 dmac = s_dmac_irqs.exchange(0, std::memory_order_acq_rel);
			const u32 intc = s_intc_irqs.exchange(0, std::memory_order_acq_rel);
			const u32 back = s_return_mask;
			s_return_mask = 0;
			g_state.store(0, std::memory_order_release);
			for (int n = 0; n < 32; n++)
			{
				if (dmac & (1u << n))
					hwDmacIrq(n);
				if (intc & (1u << n))
					hwIntcIrq(n);
				if (back & (1u << n))
					CPU_INT(static_cast<EE_EventType>(n), 4);
			}
		}
	} // namespace

	bool OnWorker() { return t_on_worker; }
	bool OnEE() { return t_on_ee; }
	void Schedule(int n) { s_pending |= 1u << n; }
	bool WorkerScheduled(u32 mask) { return (s_pending & mask) != 0; }
	void DeferDmacIrq(int n) { s_dmac_irqs.fetch_or(1u << n, std::memory_order_relaxed); }
	void DeferIntcIrq(int n) { s_intc_irqs.fetch_or(1u << n, std::memory_order_relaxed); }

	void Start()
	{
		if (!g_enabled || s_started.exchange(true))
			return;
		std::thread(WorkerMain).detach();
	}

	void EEService()
	{
		t_on_ee = true;
		int state = g_state.load(std::memory_order_acquire);
		if (state == 1)
			return;
		if (state == 2)
			ApplyFinished();
		const u32 mask = cpuRegs.interrupt & kJobMask;
		if (!mask || !s_started.load(std::memory_order_relaxed) || !CanOffload())
			return;
		cpuRegs.interrupt &= ~mask;
		cpuRegs.dmastall &= ~mask;
		s_job_mask = mask;
		++g_ps4_dma_jobs;
		g_state.store(1, std::memory_order_release);
		s_kick.Post();
	}

	void SyncEE(int reason)
	{
		if (g_state.load(std::memory_order_acquire) == 1)
		{
			const Common::Timer::Value t0 = Common::Timer::GetCurrentValue();
			s_ee_waiting.store(true, std::memory_order_relaxed);
			for (u32 spin = 0; g_state.load(std::memory_order_acquire) == 1; spin++)
			{
				if (spin < 4096)
					_mm_pause();
				else
					std::this_thread::yield();
			}
			s_ee_waiting.store(false, std::memory_order_relaxed);
			const double ms = Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - t0) / 1e6;
			++g_ps4_dma_ee_waits;
			g_ps4_dma_ee_wait_ms += ms;
			const int r = (reason >= 0 && reason < kSyncReasons) ? reason : 0;
			++g_ps4_dma_wait_n[r];
			g_ps4_dma_wait_ms[r] += ms;
		}
		if (g_state.load(std::memory_order_acquire) == 2)
			ApplyFinished();
	}
} // namespace PS4DmaWorker
