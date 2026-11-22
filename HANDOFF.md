# PS4SX2 performance work: handoff

Written 2026-11-20 to let a new Claude Code session continue exactly where the claude.ai session stopped. Nothing here is a redesign; it records what exists, why, and what is next.

---

## 1. Purpose and requirements

**Goal:** make PS4SX2 (a PCSX2 port that runs natively on a jailbroken PS4, no Linux) run PS2 games faster on the user's console. This is performance work only.

| Item | Value |
|---|---|
| Console | PS4 Slim CUH-2115B, firmware 12.52, GoldHEN |
| Emulator | PS4SX2 v1.00 alpha by Phoenixx1202 (`github.com/Phoenixx1202/PS4SX2`, single squashed commit `cbd9176`) |
| User's fork (all work goes here) | `github.com/hubwallpaper858-tech/ps4sx2` |
| BIOS | SCPH-90001 v2.30 (USA), in `/data/PS4SX2/bios/` |
| Resolution | 1x internal |
| Test game 1 | GTA San Andreas (USA, SLUS-20946). **EE-bound.** Route: start of game, then cycling down Grove Street. |
| Test game 2 | Shadow of the Colossus (USA, SCUS-97472). **GS-bound.** Title screen, shrine, riding, a colossus fight. |
| Also available | God of War |
| User's PC | HP Z220 (builds are done by GitHub Actions, not locally) |

The user tests on the console and sends logs back. They are not a developer: give plain-language results and exact step-by-step instructions (which artifact to download, which marker folders to create, what route to play, which logs to send).

---

## 2. Hard constraints (do not break)

1. **No secrets in git.** The user's instruction, verbatim: *"Make sure you're API key is not detected in any of the code you are putting on git"*. Before every commit/push, scan the diff for tokens (`ghp_`, `gho_`, `ghs_`, `github_pat_`, `sk-`, `AKIA`, `-----BEGIN`, `GH_TOKEN=`, `Authorization:` etc.). Never write a token into code, workflow files, docs or commit messages.
2. **No firmware vulnerability hunting.** Performance only. Power-state/clock changes are also out of scope (see 6).
3. **Every new feature is opt-in** through a marker file or folder in `/data/PS4SX2/` (a folder with the marker name works the same as a file). Default behaviour of the official build must not change.
4. **PS1 mode forces new EE/SPU2 features off** (block in `ps4/vm/main.cpp` under `if (platform == GamePlatform::PS1)`). Keep doing that for anything new.
5. Mark new code with `// (AI-assisted) perf-testN: ...` comments, matching the repo's existing convention.
6. Bump the build tag in `ps4/vm/main.cpp` (`BUILD: PS4SX2-EMU-1.00-CHD-SMC-TELEMETRY+perf-testN`, ~line 1100) for every test build. It is line 2 of the boot log and is the only reliable way to tell which build the user actually ran.
7. Commit attribution lines the user's environment asks for go at the end of commit messages.

---

## 3. Workflow (how a test cycle runs)

1. Branch from the previous test branch: `perf-test-N` → `perf-test-(N+1)`. Each branch stacks on the last. `main` is untouched upstream (`cbd9176`).
2. Make the change, bump the build tag, add a marker, add a heartbeat/log line so the effect is measurable.
3. Syntax-check what you can (see 3a). The full build only happens in CI.
4. Commit, scan for secrets, push: `git -c push.negotiate=true push -u origin perf-test-N`.
5. Trigger CI (it only auto-runs on pushes to `main`; `*.md` changes are ignored):
   `gh workflow run build.yml --ref perf-test-N -R hubwallpaper858-tech/ps4sx2`, then poll `gh run list` / `gh run view <id>`. A build takes ~6–10 minutes (Linux compile, then Windows PKG packaging).
6. The installable artifact is named **`PS4SX2-PKG-<run_number>`** (run number, not run id). perf-test8 = run 9 → `PS4SX2-PKG-9`. Earlier notes wrongly said "PKG-2"; tell the user the exact name every time.
   - PKG artifacts are kept 30 days; executable/symbol artifacts 14 days.
7. The user installs the .pkg with the GoldHEN package installer (deleting the old app first if needed; `/data/PS4SX2` settings stay), creates the marker folders, plays, **relaunches the emulator once** (so the session's logs are archived), then sends logs from `/data/PS4SX2/logs/last-game-PS2/`:
   - `*-PCSX2-vm-boot.log` (the main one: build tag, settings, `Performance:` lines every ~5 s, heartbeat counters, `[hwprof]`/`[drawsec]` printf output)
   - `*-emulog.txt`, stdout, stderr
8. Analyse, update the project doc (`claude/PS4SX2 – test builds and results.md` in the claude.ai Project; a copy of its content is summarised in section 5 here), report to the user in plain words.

**Log sanity checks before analysing:**
- Line 2 must show the expected `+perf-testN`. (The last logs the user sent were perf-test7, not perf-test8.)
- The user has once re-sent old logs; compare md5 with earlier uploads.
- Check `Serial:` to know which game.
- GTA stretches at 90–100% may be an empty scene (the user confirmed 242–345 s in a perf-test7 run was an empty parking lot). Compare like-for-like route segments (128–237 s on the Grove Street route).
- A log that ends without "VM destroyed" means the user closed it from the PS menu or it crashed; ask which.

**Parsing `Performance:` lines:**
```
grep "^Performance:" LOG | sed -E 's/.*seconds=([0-9.]+).*Speed_pct=([0-9.]+).*EE_ms=([0-9.]+).*/\1 \2 \3/'
```

### 3a. Syntax checking without the SDK
The PS4 SDK (OpenOrbis + sysroot + Mesa) is only fetched in CI (`tools/ci/fetch-sdk.py`, release `Phoenixx1202/PS4SX2-SDK` `sdk-v2`). Locally, the previous session used `clang++ -fsyntax-only -std=c++20` with host headers plus a tiny stub directory `/tmp/stub/orbis/` (empty-ish `libkernel.h`, `AudioOut.h`, `Pad.h`, `Sysmodule.h`, `SystemService.h`, `UserService.h`, `VideoOut.h`) and filtered errors down to the edited line ranges. Files that include many orbis headers (`GSDeviceVK.cpp`, `ps4/vm/main.cpp`) cannot be fully checked this way; CI is the real check. The stubs are not in the repo; recreate them if needed.

---

## 4. Architecture and technical facts (decided / measured)

### Threads and cores (Jaguar, 8 cores in two 4-core clusters)
| Thread | Core |
|---|---|
| EE (main emulation) | 2 |
| VU1 (MTVU) | 3 |
| GS (MTGS, CPU side of graphics + Vulkan recording) | 4 |
| Audio output | 5 |
| SPU2 worker (perf-test8, opt-in) | 1 by default (`SPU2=` role in `vm-cores.txt` overrides) |
| Free | 0, 1 |

`/data/PS4SX2/vm-cores.txt` (perf-test1) can override roles, e.g. `GS=1`.

### Where the time goes
- **PS4 CPU/GPU are already at max clocks** while PS4SX2 runs (FULL_GAME: CPU 1600 MHz, GPU 800 MHz, 18 CUs; from the 12.52 kernel dump). No free performance there.
- **The PS4 GPU is nearly idle** (~2 ms per frame). All cost is on CPU threads.
- **GTA SA is EE-bound.** EE waits on GS/VU1 0.0% of the time; GS idle 30–45%, VU1 50–60%. EE phase shares in gameplay: guest code ~39%, IOP ~26% (IOP JIT + iopEventTest, includes SPU2 mixing), DMA handlers ~20% (mostly VIF1), exceptions ~5%, schedule ~4%, VU ~3%, timers ~3%. ~60% of EE time is emulator overhead.
- **SotC is GS-bound.** GS 0% idle, EE stalled on GS up to ~49%. ~4,000 draws/frame, ~32.5 µs per draw on Jaguar (Vulkan recording ~13.7 µs of it). No single hotspot. Even with a perfect GS, SotC's EE would cap heavy scenes at ~40%.
- **SPU2 mixing** costs 60–190 ms/s of EE time (GTA ~154 ms/s at ~53% speed). SoundTouch (`audio_write`) is only 0–2 ms/s.
- **VIF1 DMA:** 150–350 ms/s, 90k–190k `vif1Interrupt` calls/s, ~1.6–2.3 µs each.

### Agreed direction (2026-11-20)
EE-side work first (helps both games): PS5 fast paths (done, perf-test7) → SPU2 mixing off the EE thread (perf-test8, awaiting test) → VIF1 DMA cost. The big later project for SotC is a **GS render thread** (record Vulkan commands on a second core, ~40% of GS work), then the GPU fence wait (~7%).

### Key mechanisms
- **EE event-test profiler** (`ps4/core/EEEventProfilePS4.*`, marker `vm-profile-perf`): phases sampled 1/1024 via `EventScope`; perf-test5 added `SubScope`/`SubPhase` (IOPExecute, IOPEventTest, SPU2Mix, AudioWrite, DmaMTVU, DmaVIF1, DmaGIF, DmaSIF0, DmaSIF1, DmaOther). Output: "EE sub-phase profile (estimated wall ms per second)" and "EE sub-phase calls".
- **GS profiler** (`ps4/core/GSProfilePS4.*`, marker `vm-profile-gs`): hot scopes sampled 1/64, `IsHot()` selects them; "GS timing" lines include `est_ms_per_s`. `vm-profile-gs-draw` enables the PS5 port's per-draw timing (`OrbisFlag("drawprof")`, rdtsc at 1596 ticks/µs), printing `[hwprof]`, `[drawsec]`, `[drawgap]` into the boot log.
- **`OrbisFlag(name)`** (`ps4/core/OrbisCompatPS4.cpp`) returns false on PS4 except `"drawprof"` → `vm-profile-gs-draw`.
- **PS5-port EE fast paths** (compiled in but off on PS4 before perf-test7): GetThreadId cache (`OrbisEEHle.h`), `eret_fast`, `vif_fast`, `vif1_fuse` (needs the Instant-DMA loop, which neither test game reaches), `hle_flush`, `pfw`.
- **SPU2 timing:** `TimeUpdate` mixes one tick per 768 IOP cycles; `SPU2async` is called from `psxRcntUpdate`; IRQs go `SetIrqCall → spu2Irq` (IOP INTC); `ReadInput` can raise DMA4/DMA7 IRQs; `IopCounters` reads `lClocks`.

---

## 5. Implementation state: builds on the fork

All branches stack. `git diff main..perf-test-8` = 27 files, +1316/−83.

| Build | Branch / commit | Run id (run #) | Change | Markers | Result |
|---|---|---|---|---|---|
| perf-test1 | `perf-test-1` / `626c7b9` | 44653925815 (2) | crash context in crash.log (`ps4/vm/CrashContextPS4.cpp`), keep 5 archived log sessions (`SessionLogs.h`), GS perf defaults actually applied after `MaskUserHacks` (`Pcsx2Config.cpp`), effective-settings log, `vm-cores.txt` core overrides (`ThreadAffinityPS4.*`), probe logger additions | `vm-no-gs-perf-defaults`, `vm-cores.txt` | No speed difference vs official |
| perf-test2 | `perf-test-2` / `edf94ec` | 44763399442 (3) | page protection for EE code pages instead of per-entry manual SMC checks; removed per-compile debug (leaprobe, printf) in `iR5900.cpp`; `HostSysPS4.cpp` mprotect counters | `vm-smc-pageprot` | Works on 12.52, 13 protected-page writes/3 min, small gain (37–52%) |
| perf-test3 | `perf-test-3` / `ace0d33` | 44794027294 (4) | pure EE wait-loop skip (`ps4/core/IdleLoopPS4.h`, hook in `iR5900.cpp`) | `vm-ee-idle-skip` | Fires ~150k/s, no crash, **no speed gain** |
| perf-test4 | `perf-test-4` / `eb4d733` | 44798183878 (5) | heartbeat `Thread waits` (EE blocked on GS/VU1, GS/VU1 idle) | always on | Proved GTA is EE-only bound |
| perf-test5 | `perf-test-5` / `2647a8c` | 44804449038 (6) | EE sub-phase profiler (IOP/DMA split); SoundTouch quick seek | `vm-audio-quickseek` | SPU2 mix is the audio cost; SoundTouch negligible |
| perf-test6 | `perf-test-6` / `0974c86` | 44814063216 (7) | finer GS scopes (SetupIA, EmulateBlending, VK apply state/bind pipeline/upload/send draw/begin pass/OM set), RenderHW tick accumulation, `drawprof` flag | `vm-profile-gs`, `vm-profile-gs-draw` | SotC per-draw breakdown (above); no single hotspot |
| perf-test7 | `perf-test-7` / `09057e7` | 44818524726 (8) | markers for PS5 fast paths; heartbeat `EE PS5 fast paths cumulative` | `vm-ps5-fastpaths` (sets hle-tid, eret-fast, vif-fast, vif1-fuse), plus individual `vm-hle-tid`, `vm-eret-fast`, `vm-vif-fast`, `vm-vif1-fuse`, `vm-hle-flush`, `vm-pfw` | Safe; small EE gain (GTA ~49→53% on route; SotC exceptions −20%). **Keep on.** |
| **perf-test8** | `perf-test-8` / `43deb27` | **44826762251 (9) → `PS4SX2-PKG-9`**, build succeeded | SPU2 mixing on a worker thread | `vm-spu2-thread` | **Not yet tested** (user's last logs were perf-test7) |

Recommended marker set for current tests: `vm-spu2-thread`, `vm-ps5-fastpaths`, `vm-profile-perf`, `vm-smc-pageprot`, `vm-ee-idle-skip` (add `vm-profile-gs` for SotC GS numbers).

### perf-test8 design (SPU2 worker), files touched
- `pcsx2/SPU2/spu2.h`: `namespace SPU2Thread { g_enabled; g_core (=1); Sync(); AsyncUpdate(u64); struct Stats{posts, worker_ticks, sync_ticks, waits, wait_tsc, irqs, dma4, dma7}; GetStats(); }` and `SPU2_SYNC()` macro (real under `PCSX2_PS4_VM_BOOT`, no-op otherwise).
- `pcsx2/SPU2/defs.h`: `SPU2RaiseDMA4Irq()`, `SPU2RaiseDMA7Irq()`.
- `pcsx2/SPU2/ReadInput.cpp`: direct `spu2DMA4Irq/spu2DMA7Irq` calls replaced with the Raise wrappers.
- `pcsx2/SPU2/spu2sys.cpp`:
  - `thread_local bool t_spu2_worker`; deferred atomics `s_spu2_deferred_irq/dma4/dma7`. `SPU2RaiseIrq/RaiseDMA4Irq/RaiseDMA7Irq` defer when called on the worker; `SetIrqCall` uses `SPU2RaiseIrq`.
  - `TimeUpdate` split into `static TimeUpdateMix(u64)` (sanity check + mix loop; SPU2Mix SubScope only when not on the worker).
  - `SPU2Thread`: atomics `s_target`/`s_done` (IDLE = ~0), `s_sleeping`, mutex + condvar. `WorkerMain` pins to `g_core`, spins 20,000 pauses then sleeps on the condvar. `Sync()` (no-op if not started or on worker) spin-waits until done == target (counts waits/wait_tsc) then applies deferred `spu2Irq`/`spu2DMA4Irq`/`spu2DMA7Irq` (guarded by `s_applying`). `AsyncUpdate(c)`: Sync, CheckDMAProgress(0/1); first call mixes on the EE thread and starts a **detached** `std::thread`; later calls post the target and notify if sleeping.
  - `TimeUpdate(c)`: `SPU2_SYNC(); TimeUpdateMix(c); stats; CheckDMAProgress(0); CheckDMAProgress(1);`
- `pcsx2/SPU2/spu2.cpp`: `SPU2async` calls `AsyncUpdate` when `g_enabled && !s_psxmode`; `SPU2_SYNC()` at the top of 17 entry points (interruptDMA4/7, CreateOutputStream, UpdateSampleRate, SetOutputVolume, SetOutputMuted, UpdateOutputVolume, SetOutputPaused, SetAudioCaptureActive, InternalReset, Reset, OnTargetSpeedChanged, Open, Close, CheckForConfigChanges, SPU2read, SPU2freeze).
- `pcsx2/IopCounters.cpp`: `SPU2_SYNC()` before `spu2_delta = (psxRegs.cycle - lClocks) % 768`.
- `ps4/vm/main.cpp`: include `SPU2/spu2.h`; marker `vm-spu2-thread` → log `SPU2 worker thread: enabled/off`; heartbeat `SPU2 worker cumulative: posts= worker_ticks= ee_sync_ticks= ee_waits= ee_wait_ms= deferred_irq= dma4= dma7=`; forced off for PS1; tag `+perf-test8`.

What to look for in perf-test8 logs: the `SPU2 worker thread: enabled` line; `SPU2 worker cumulative` growing (`posts` ≈ `worker_ticks`, low `ee_wait_ms`); the `SPU2Mix` sub-phase dropping toward 0 ms/s on the EE; IOP phase and speed vs perf-test7 on the same route; `input_short` in `PS4 audio output` (perf-test7: GTA 300, SotC 2189); and the user's audio report (crackle, missing music/voices, stutter, freezes).

---

## 6. Remaining tasks (in order)

1. **Get a valid perf-test8 test.** User installs `PS4SX2-PKG-9` from run 44826762251, creates `vm-spu2-thread` (plus the recommended set), plays GTA route + SotC, reports audio, sends boot logs. Verify line 2 says `+perf-test8`.
2. Analyse perf-test8 vs perf-test7 (speed on the 128–237 s GTA route segment, EE phases, SPU2 worker counters, audio). If audio breaks or it hangs, look first at IRQ deferral timing and the Sync points.
3. Update the project doc `claude/PS4SX2 – test builds and results.md` (claude.ai Project) with the perf-test8 build row and results. Also fix its "PS4SX2-PKG-2" wording to "PS4SX2-PKG-<run number>".
4. Next EE candidate: **VIF1 DMA cost on the EE thread** (150–350 ms/s, ~2 µs per `vif1Interrupt`).
5. Later, larger: **GS render thread** (Vulkan recording on a second core) for SotC; then the GS fence wait.
6. Bigger structural items from the repo scan still open (see 7): contiguous memory reservation (finding 1), emitter TLS cost (finding 5).

---

## 7. Known bugs and risks

From the original repo scan (`claude/PS4SX2 (Phoenixx1202) – repo scan and bug report.md` in the Project):

1. **Code and PS2 data memory mapped ~56 GiB apart** (`ps4/core/MemoryMapPS4.cpp:43,51`); the x86 emitter silently truncates absolute memrefs it can't encode (`common/emitter/x86emitter.cpp:329-340`, prints `[sibwarn]` once). Patched site-by-site by the port authors; unpatched sites break games. Fix: one reserved range, `MAP_FIXED` both maps (code map must come from flexible memory). **Open.** GTA logs show no `[sibwarn]`.
2. GS perf defaults discarded by `MaskUserHacks`. **Fixed in perf-test1**, no measurable effect.
3. Every EE block re-checked on each entry (forced `ProtMode_Manual`). **Addressed opt-in** by `vm-smc-pageprot` (perf-test2).
4. Thread placement splits EE/VU1 (cluster 0) from GS (cluster 1). `vm-cores.txt` can test `GS=1`; no recorded result yet.
5. Emitter state fetched via `pthread_once`+`pthread_getspecific` on every emitted byte (`ps4/core/JitThreadStatePS4.cpp`, `x86Ptr` macro). **Open.** (Per-compile leaprobe/printf removed in perf-test2.)
6. EE cycle rate 75% by default (`EECycleRate=-1`), marker-only override. **Open**, left as is.
7. Multi-block DMA wait fast-forward overshoots timeout by one iteration and charges raw cycles. **Open**, low.
8. `TimedWaitForEmpty` stale wake-up with `vm-verbose-log`. **Open**, low.
9. Small: missing-BIOS message only in log; `ps4_sampler.cpp` dead code with DolphinPS4 paths; BIOS search fallback is PS5-only; leftover `[dbg]` printfs and `orbis_probe_rsp` on every recompiled-code entry.

perf-test8-specific risks:
- The SPU2 worker thread is **detached and never stopped**; it lives for the process. If the VM is shut down and restarted in one process, check it does not mix against freed state.
- IRQs raised on the worker are deferred until the EE's next `Sync()`; a game that is sensitive to SPU2 IRQ timing could glitch.
- Both perf-test7 logs (GTA `6f06da3a`, SotC `45290168`) ended without "VM destroyed"; unknown whether the user quit or it crashed.

Upstream issues on Phoenixx1202/PS4SX2: #2 (returns to library after PS2 logo), #9 (CE-34878-0 crash with SCPH-90001), #17 (language option).

---

## 8. Approaches that failed or are dead ends; do not repeat

- **`vm-skip-smc`** (skip SMC checks entirely): +18 points in God of War per the original author but **breaks GTA SA**. Use `vm-smc-pageprot` instead.
- **GS perf defaults** (paltex, preload frame, disable partial invalidation): applied in perf-test1, **no difference** in GTA.
- **Idle-loop skip** (`vm-ee-idle-skip`): works, harmless, but **no speed gain** in GTA; the spin was already cheap. Keep as opt-in; don't invest further.
- **`vm-ee-hotspots`** counts where *events* land, not CPU time. Don't use it to attribute CPU cost.
- **SoundTouch / `vm-audio-quickseek`**: SoundTouch costs 0–2 ms/s; not worth pursuing. The audio cost is SPU2 mixing.
- **Shader compiles** are not SotC's slowness (cache works; ~10 compiles on second play, same speed).
- **Micro-trimming GS per-draw steps**: no single hotspot; each step is 0.3–5 µs. Gains of a few % at most. The real lever is threading (GS render thread).
- **`vm-vif1-fuse`** never fires in GTA or SotC (needs Instant-DMA loop). Don't build on it for these games.
- **Clock/power-state changes**: already at max FULL_GAME clocks; raising them means altering Sony's power tables. Out of scope (risk).
- **Firmware/kernel exploit work**: forbidden by the user.
- Process mistakes to avoid: trusting the artifact name "PKG-2" (it's `PKG-<run_number>`); analysing logs without checking line 2's build tag; comparing GTA numbers from different scenes (empty areas run ~100%).

---

## 9. Related claude.ai Project docs

Kept in the claude.ai Project "PS4 Emulator Project" (not in this repo):
- `claude/PS4SX2 – test builds and results.md` (build table, results, SotC profiles; updated through perf-test7)
- `claude/PS4SX2 (Phoenixx1202) – repo scan and bug report.md` (findings 1–9 above, first measurements)
- `claude/PS4 Slim 12.52 kernel dump – performance findings.md` (power modes; the dump contains account IDs, never attach it publicly)

See also `KERNEL-DUMP-HANDOFF.md` in this repo: what the 12.52 kernel dump showed and how it shaped these decisions.
