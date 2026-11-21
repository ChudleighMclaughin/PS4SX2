# GS render thread: plan (2026-11-20)

Goal: speed up GS-bound games (Shadow of the Colossus) by moving the Vulkan command-recording cost off the GS thread (core 4) onto a free core. Opt-in marker `vm-gs-replay`, forced off for PS1, like every other perf-test feature.

## Why

Shadow of the Colossus fixed benchmark (new game, standing at the shrine, perf-test11): 28% speed, GS 56–57 ms per frame, GS 0% idle, EE held back by GS ~45%. perf-test6 per-draw profile: 32.5 µs per draw, of which ~13.7 µs is Vulkan recording (`vkCmdDrawIndexed` ~5.5 µs, descriptor binds/pushes ~3–4 µs, the rest barriers, render passes, scissor, constants). On Jaguar most of that is RADV's CPU-side command emission inside the `vkCmd*` calls.

## Approach: defer `vkCmd*` calls, not a GSDeviceVK rewrite

A classic render thread would restructure GSDeviceVK (8,700 lines, 35 command-buffer call sites, state caching everywhere). Instead:

- All Vulkan device functions are global function pointers (`VKEntryPoints.inl`), loaded in `LoadVulkanDeviceFunctions` (GSDeviceVK.cpp ~785), before `CreateAllocator` (~4177).
- With the marker, right after loading, swap the hot `vkCmd*` pointers for **proxies** that copy the call and its arguments (deep copy of barrier/descriptor/region arrays) into a ring buffer. A **replay thread** (core 0) calls the saved real functions in the same order.
- GSDeviceVK logic stays as it is: dirty flags, stream buffers, UBO memcpy and vertex upload stay on the GS thread. Only RADV's recording work moves.
- **Drain points:** any call that must see a fully recorded command buffer, or touches objects the queued calls may still reference, first waits until the ring is empty, then calls the real function on the GS thread: `vkEndCommandBuffer`, `vkQueueSubmit`, `vkBeginCommandBuffer`, `vkResetCommandPool/Buffer`, `vkDestroy*`, `vkUpdateDescriptorSets`, `vkAllocateMemory/FreeMemory`, `vkCreate*`, `vkDeviceWaitIdle/QueueWaitIdle`, `vkWaitForFences`. Swapping these before `CreateAllocator` makes VMA go through them too.
- Rare or complex `vkCmd*` (any call with a non-null `pNext`, debug labels, queries, copies at first) use the same drain-then-direct path: correct, just not parallel.
- Ring wake-up like the SPU2 worker: spin briefly, then condvar. The GS thread blocks when the ring is full.

Estimate: ~9–10 µs of the 32.5 µs per draw leaves the GS thread, and encoding costs ~1 µs. GS work per frame drops to ~70–75%, so the shrine would go from **~28% to roughly 35–38%**. GTA gains little (EE-bound).

## Stages (one build each, unless a fix is needed)

1. **perf-test12, counting only (`vm-gs-replay-count`).** Install the proxies in pass-through mode (no thread) and count per second: deferrable calls, drain-point calls, and drains that would interrupt a run of draws. Zero behaviour change. Decides whether drains are rare enough. Test: Shadow of the Colossus shrine for 60 s, GTA save for 60 s.
2. **perf-test13, replay thread for the draw path (`vm-gs-replay`).** Defer: DrawIndexed, Draw, BindPipeline, BindDescriptorSets, PushDescriptorSetKHR, PushConstants, SetViewport/Scissor/LineWidth/BlendConstants, BindVertex/IndexBuffers, PipelineBarrier, Begin/EndRenderPass, ClearAttachments. Everything else drains. Heartbeat: deferred calls/s, drains/s, GS-thread wait on ring ms/s, replay busy %. Test: both fixed spots, then normal play watching for graphics glitches.
3. **perf-test14+, widen.** Using stage 1/2 counts, defer the next most common drain causes (texture copies, clears, uploads into the init command buffer) if they break up draw runs.
4. Separately: the per-frame GPU fence wait (~7% of GS time).

## Risks

- **PS4 RADV winsys thread safety (biggest unknown).** Recording grows command streams by allocating BOs through the PS4 winsys, while the GS thread may create textures. The drain on `vkAllocateMemory`/`vkCreate*` keeps those from overlapping. If the winsys has other shared state, expect crashes in stage 2. The fallback is to drain on more calls.
- Missing a deep copy (a pointer argument read after the GS thread reused the memory) gives wrong graphics, not crashes. Every proxy copies all arrays it is given.
- An object destroyed while still referenced by a queued call: handled by draining on `vkDestroy*`. PCSX2 defers most destruction until fence completion anyway.
- GPU errors (device lost) surface later than the call that caused them; for debugging, `vm-gs-replay` off gives the old path.
- Core 0 must be free. It is (SPU2 worker on core 1).
