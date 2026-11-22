# PS4 Slim 12.52 kernel dump: what it told us (handoff)

Written 2026-11-22 for a Claude Code session continuing the PS4SX2 performance work. Companion to `HANDOFF.md`. This explains what the kernel dump is, what was learned from it, and how that shaped the project's decisions.

**Scope:** performance research only. The user's rule is no vulnerability or exploit work. The dump was used only to read the console's own logs and power-state readouts. No kernel code was disassembled or probed for bugs, and none should be.

---

## 1. The dump itself

| Item | Value |
|---|---|
| File | `kernel.bin` (44 MB, 44,040,192 bytes). The user also sent an identical copy named `PS4.bin`. |
| Console | The user's PS4 Slim CUH-2115B, firmware 12.52 (build J02690760), GoldHEN |
| Tool | iHaiDeeZ `ps4-kernel-dumper` v1.1.0 (released for FW 14.02), with 12.52 support requested by the user. Runs as a GoldHEN payload and writes to USB. |
| Date | 2026-11-19 |
| Where it is | Only with the user (and uploaded to the claude.ai chat). **Not in git.** |

**Privacy:** the dump contains the kernel's log ring buffer, which includes this console's account IDs. Never commit it, attach it to a GitHub issue, or paste raw log lines from it without removing IDs. Long digit strings are the giveaway; strip anything 10+ digits long.

## 2. How it was read

Only the printable log text was used:

```
strings -n 8 kernel.bin > kstr.txt
grep -aE 'SceSystemStateMgr|ExpandCpuSet|radv/ps4|Power Mode' kstr.txt | sed -E 's/[0-9]{10,}/<id>/g'
```

That gives four useful kinds of lines:
- `[SceSystemStateMgr]` power-mode changes and a clock/power table (header row plus one row per mode).
- `[Syscore App] ExpandCpuSetMode : enable/disable` (whether system services may use the app's CPU cores).
- `radv/ps4: submit N ...` and `radv/ps4: va ...` from PS4SX2's own Vulkan driver (Mesa RADV port).
- Memory-allocation messages from PS4SX2's sessions (title ID `PCSX20007`). Two real PS4SX2 runs were in the buffer.

## 3. What it showed

### Power modes recorded on this console

The `SceSystemStateMgr` table (columns: CPU MHz, HLT, ..., CU count, ACLK, DCLK, ECLK, LCLK, SCLK, SAMCLK, VCLK, DispCLK, NCLK, MemCLK, MemP, GMC, DPM level, VDD, VDDNB, SOC):

| Mode | CPU MHz | CPU halt | GPU CUs | SCLK (GPU) MHz | Mem MHz | DPM level |
|---|---|---|---|---|---|---|
| NAVIGATION (home screen) | 1600 | allowed | 8 of 18 | 711 | 1375 | 2 |
| FULL_GAME (any app in front) | 1600 | off | 18 of 18 | 800 | 1375 | 3 |

- Opening any app (the payload launcher, PS4SX2) switches to FULL_GAME. PS4SX2 relaunches itself through LoadExec from its menu, which keeps FULL_GAME. The mode only drops back to NAVIGATION when the app closes.
- `ExpandCpuSetMode : disable` appears while an app is in front: system services don't spread onto the app's cores. The exact core mask is not printed.
- PS4SX2's RADV driver logs only the first ~40 GPU submits and then every 1000th, so its logging costs nothing measurable. About 15,000 submits were seen in one session.
- In one session PS4SX2's allocator used 512 MiB of system flexible memory plus a 105 MiB executable (JIT code) mapping.

## 4. How it helped the project

1. **It ruled out the "free speed" route.** While PS4SX2 runs, the Slim is already at its maximum shipped clocks: CPU 1600 MHz with halt disabled, GPU at 800 MHz with all 18 CUs. No power-mode, DPM or launch-mode trick will speed up the emulator. Faster clocks would mean editing Sony's power tables, which was rejected (hardware and thermal risk, and out of scope). That settled the project's direction: **all gains must come from the emulator's own CPU work.**
2. **It confirmed the profiling picture from the other side.** The emulator's logs show the GPU busy only ~2 ms per frame while the EE, GS and VU1 threads are over budget. The dump shows the GPU at full clocks with all CUs, so the GPU idles because it has little work, not because it is throttled. The bottleneck is the CPU threads, which is why every perf-test build (perf-test2 to perf-test8) targets EE- or GS-thread CPU time.
3. **It informed core planning.** Because system services stay off the app's cores while PS4SX2 is in front, pinning the emulator's threads (EE 2, VU1 3, GS 4, audio 5) is not fighting the OS, and the spare cores 0–1 are worth using. perf-test8's SPU2 worker defaults to core 1 on that basis. This is supporting evidence, not proof: the exact core mask isn't in the log, so measure before relying on it.
4. **It gave the memory numbers that matter for bug-report finding 1.** The JIT code map (~105 MiB executable) comes from flexible memory alongside the 512 MiB flexible pool. A future "contiguous memory reservation" fix (one reserved range, `MAP_FIXED` data + code) must take the code map from flexible memory too (see `HANDOFF.md` §7, finding 1).
5. **It removed one suspect.** RADV driver logging is throttled and is not a performance cost, so it was never made a target.
6. **It backed the user's hardware and firmware answers.**
   - A PS4 Pro would help only through its higher CPU clock (~2.1 GHz vs 1.6 GHz, at most ~33%, and only in Pro mode or Boost Mode). Its GPU doesn't matter here.
   - Updating firmware gives no speed gain, because clocks are already at maximum; it only risks losing the jailbreak. Advice: stay on 12.52.
7. **For the paused EmuFive work** (PS5 emulator on PS4), the same table shows the GPU already at 800 MHz / 18 CUs in FULL_GAME, so EmuFive can't gain from GPU clocks either.

## 5. What it did not tell us (limits)

- No per-thread or per-core CPU usage, no timing of the emulator's own code. Per-thread timing comes from PS4SX2's own profilers (`vm-profile-perf`, `vm-profile-gs`, `Thread waits` heartbeat).
- No exact CPU core mask for apps.
- One snapshot of one console on one firmware. A PS4 Pro or different firmware may log different tables.
- Nothing about Jaguar cache/cluster behaviour (bug-report finding 4: EE/VU1 on cluster 0, GS on cluster 1). That needs an on-console experiment with `vm-cores.txt` (e.g. `GS=1`).

## 6. Using it in future (performance only)

- Re-dumping isn't needed for PS4SX2 work. The questions the dump can answer are answered.
- If clocks or power modes are ever questioned again (for example, does PS4SX2 drop out of FULL_GAME during some flow?), ask the user for a fresh dump after that flow and grep `Power Mode Change` and the `SceSystemStateMgr` table rows. Strip account IDs before quoting anything.
- Do not use the dump for symbol hunting, patching, or anything exploit-related.

## 7. Where the findings are recorded

- claude.ai Project "PS4 Emulator Project":
  - `claude/PS4 Slim 12.52 kernel dump – performance findings.md` (the original findings)
  - this handoff
- `HANDOFF.md` in this repo (§4 "Where the time goes", §8 "clock/power-state changes" as a dead end).
