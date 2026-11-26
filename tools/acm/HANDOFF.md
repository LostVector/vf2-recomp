# Astro City Mini VF2 handoff

Current VF2 checkpoint from November 26, 2026. The sections below it retain the November 20 baseline and earlier experiment history.

For Daytona USA, see the [current Daytona priorities](DAYTONA.md#current-priorities).

## Current VF2 performance plan

The performance investigation is paused. The current target is sustained
Shun combat at p99 of 17.384 ms or less on the Mini at a sampled 1.200 GHz.
Use full-rate 496?384 with actual 4? MSAA, 44.1 kHz speaker audio,
and the established worker placement. Evaluate combat ticks 1800?3300
separately from warmup, arena entry, and capture. Keep the ROM ZIP and
benchmark data on the PC between runs.

The cached profiled DSP entry reduced Shun simulation CPU by 0.186 ms
at the paired median. Delivered p50/p95/p99 were 17.951/19.627/20.980 ms
for that candidate. The later linked-register and shared cell-split
prototypes did not show a repeatable improvement. Atlas capacity helped
attract mode when the cap rose from 16 to 32 MiB. In the 18,000-tick
attract replay both 32 and 128 MiB used the same eight pages and had zero
capacity fallbacks. Continue using 32 MiB for attract comparisons.

1. Use the sustained Shun traces to distinguish CPU-heavy ticks from
   renderer waits. Measure cycle and call distributions across whole DSP
   command paths and at mixed i960 register/bus boundaries. Screen
   changes that reduce work across these paths while preserving callback
   order, cycle boundaries, and resumed slices.
2. Profile remaining packet costs at the worker level before changing the
   renderer again. Favor reductions in repeated preparation and emitted
   data across real draws. The measured two-cell sharing variants have
   been screened and are archived.
3. Screen promising changes on Dural. Require two full Shun passes at the
   target and a confirming Dural pass before replacing the interactive
   build. Check state and draws, zero audio faults and skipped frames,
   exact muted PCM, moving captures, and the volume chord. Restore and
   verify the interactive build after each hardware job.


## Goal and current state

Get the Virtua Fighter 2 recompilation running natively on the Astro City Mini. Build a new 3D engine abstraction if it is needed to reach the hardware target. The Mini already runs an interactive ARM build using its built-in controls and GLES rendering. The open performance goal is to sustain the arcade cadence of 57.53 Hz (17.384 ms per frame) with headroom in full-render combat at stock clocks. The user asked to defer audio until gameplay runs fast enough to test. Use combat frame times for optimization decisions and exclude arena entry, warmup, and capture.

- Run the commands below from the repository root.
- Working branch: `astro-city-mini`. The detailed progress log is [README.md](README.md), with the latest findings at its end.
- Firmware backup and inspection: `../backups/20261116-164406/`. See `SHA256SUMS.txt`, `INSPECTION.txt`, and `inspection/system-inspection.json`.
- Installed interactive release: `build/acm/pgo/core/bin/vf2-acm-tgp`. SHA-256 `c5201a5fd74ec2d64b3f0b7f34717242ad93bc515746564573dc730d229c02e5`. The last live verification recorded CRC32 `9b1f5565` in `build/acm/i960-fifo-store-runs/release-verified.json`. The device state may change between sessions, so recheck before relying on that observation.
- Local inputs and build outputs include `roms/vf2.zip`, `build/acm`, and the sibling firmware backup. Keep the current checkout and these local artifacts together for a session handoff. A fresh Git clone alone will not contain them.

## Latest verified timing

The last valid full-draw installed control pair used 1,200-frame Dural replays at stock clocks. Ranges below are the two individual control runs. Deadline counts combine both runs.

| Combat window | Mean | p95 | p99 | Over 17.384 ms |
| --- | ---: | ---: | ---: | ---: |
| All measured combat | 17.25–17.30 ms | 17.85–17.88 ms | 18.29–18.36 ms | 487/2,156 |
| Sustained ticks 302–541 | 17.42 ms | 17.99–18.00 ms | 18.40–18.63 ms | 259/480 |

Raw results are in `build/acm/i960-clear-bit-loop-trained/gate-results.json` under `stage-10-release-1` and `stage-10-release-2`. Full timing runs exclude report and capture activity from the combat window. Simulation averaged 16.94–16.97 ms in the sustained window. Earlier passive samples identified generated i960 code, DSP execution and bus access, and scheduler/FIFO work as the largest simulation costs. See `build/acm/post-vblank-profile/findings.md`.

## Experiment checkpoint

| Local branch | Tip | Evidence |
| --- | --- | --- |
| `acm-i960-clear-bit-loop` | `3e0bf1e` | State, timed FIFO transactions, draw and texture hashes, and viewport matched. The short gain did not repeat in the trained full Dural replay. The shortcut missed 283/480 sustained deadlines versus 238/480 with the same trained binary's shortcut disabled. Kept opt-in. |
| `acm-i960-fifo-store-runs` | `7b74061` | Grouped stores in four hot guest functions. ARM replays matched state and geometry on Dural, Stage 5, and Waterfront. The old hardware proof enabled grouping. A stale run-name check left it disabled in all four timing commands. Treat those four results as controls. |
| `acm-i960-fifo-store-preflight` | `00fc28c` | Checks FIFO capacity in generated ARM code before calling the grouped-store helper. Corrected Mini comparison is complete: control 18.323 ms mean and 474/480 sustained misses, helper 18.353 ms and 470/480, checked helper 18.422 ms and 468/480. No repeatable gain; keep both flags opt-in. |

Measurement only, no new branch: `20261120-003137-671623` ran the installed release on the 269-frame Dural short replay with a per-frame trace at stock clocks. Steady frames (>= tick 30, interval <= 30 ms) measured interval 17.518 ms mean / 18.143 p95 and simulation task 16.987 ms mean / 17.953 p95, so the steady swap-to-simulation gap is 0.531 ms. Five non-simulation hitches (frames 2, 13, 76, 196, 269 at 30-170 ms with normal 14.8-17.1 ms simulation) inflated the raw short-replay mean to 18.317 ms and p99 to 31.525 ms. The simulation-only arm was confounded because `--mode simulation` disables the trained DSP bundle and ran slower. See `build/acm/render-contention/findings.md`.

`acm-dispatch-gap` (`91e9c7f`) was re-screened on 2026-11-20 with two counterbalanced same-binary pairs, jobs `20261120-005116-477f68` (control), `20261120-005154-c9c291` and `20261120-005230-327744` (signed gap on), `20261120-005304-abab27` (control), binary `5e50f8d7...`. Pooled steady interval: control 17.878 ms mean / 18.737 p95 / 432 of 478 over budget; candidate 17.993 / 18.790 / 458. The change raises the mean and p95 and adds 26 misses, so it stays opt-in and is rejected as a release change. See `build/acm/dispatch-gap-screen/findings.md`.

Render-stage attribution on the release, job `20261120-005938-bc0445` with `VF2_RENDER_STAGES=1`: the geometry join is `geometry_wait()` at `T_VBLANK_END` (`model2a.cpp:1585`), inside the 17.12 ms simulation task, and is only 0.317 ms of materialization wait. It cannot be overlapped with palette/atlas packet preparation, which is 6.92 ms on the main thread after the join. `prior GPU drain` is 2.371 ms wall / 0.202 CPU (192 of 269 prior fences still pending at packet time), and EGL swap is 0.765 ms. See `build/acm/join-attribution/findings.md`.

A geometry-worker material-gather/prefetch trial (`build/acm/geometry-material-gather`, four counterbalanced jobs on one binary, SHA `7acb4dd36799...`) measured worker execution 4.008 ms plain against 4.009 ms gathered/prefetched, and pooled steady interval mean 19.187 against 19.225 ms (control spread 0.087), so it is rejected and the source reverted. The trial binary is a full non-PGO rebuild, because the release object set predates later header changes (an incremental mix crashed) and `build/acm/pgo/data` is stale against the current sources. The wait is `worker execution - vblank window`, which is why slower builds show a smaller wait (0.17 ms here against 0.317 ms on the release) and why the wait must only be compared within one build.

The prior-GPU drain was then shown to be off the critical path (`build/acm/gpu-drain-inventory`): on the release binary, jobs `20261120-005938-bc0445` (3 render threads) and `20261120-015211-90f18c` (`--render-threads 2`), removing one worker raised packet preparation 7.15 to 7.67 ms, 3D submit 3.30 to 3.68 ms and the drain 1.58 to 2.25 ms, yet the steady interval moved only 17.548 to 17.605 ms (sim 17.211 to 17.212) because the simulation wait fell from 2.83 to 0.80 ms. Main-thread CPU 13.690 ms plus 1.380 ms of drain against a 17.280 ms simulation task leaves 2.210 ms of slack. Do not build a deeper queue: `gpu-fence-depth-trial` job `20261117-124750-8bd486` moved the wait into a 44.012 ms tile upload at 14.58 fps, and `double-gpu-trial` job `20261117-105006-6f7b45` gained nothing. The interval is `sim_task + gap`, so both the mean and the p95 have to come from the simulation task.

The simulation tail was then attributed with a new per-frame counter trace (`build/acm/sim-tail-attribution`, non-PGO full rebuild, jobs `20261120-021005-04f995` and `20261120-021231-bca165`, 239 steady frames each). Splitting the slowest decile by simulation time from the rest: DSP instructions +4.92% / +4.97% (+3,987 / +4,027 per frame), DSP block runs +6.53% / +6.48%, DSP instructions inside blocks +7.01% / +6.89%, and native DSP helpers/entries zero throughout. Regressing simulation time on the counters puts the DSP at +0.480 / +0.475 ms of a +1.092 / +1.118 ms tail, so at most 44% is DSP instruction work. Combined with 0x32 removing 1,266,261 dispatches with no Dural gain, native DSP command coverage is exhausted for this target; the remaining ~0.6 ms is i960 guest, board scheduler/FIFO and thread scheduling. That part was then measured with the same harness extended with FIFO counters (`build/acm/sim-tail-fifo`, jobs `20261120-022319-a29990` and `20261120-022352-14c695`): failed i960 reads are 41.46% / 41.45% of i960 FIFO reads on ordinary ticks and 40.98% on slow ticks in both runs, while FIFO traffic rises 5-10%, so retries scale with traffic and do not concentrate on the tail. Slow ticks are proportional extra work in every subsystem, and the biggest remaining per-tick cost is the 853 failed i960 reads plus 384 input-FIFO full stalls per frame. That per-tick cost was then priced directly (uild/acm/fifo-empty-price, jobs 20261120-024307-1e81df and 20261120-024411-d5f7b8): bracketing every failed i960 output-FIFO read in host time shows 860.6 episodes per frame, 100% of them leaving the empty-FIFO timer as the immediate scheduler head, with a wall span of 9.3-9.6 us per episode and 8.0-8.2 ms per frame (39.5% of the simulation task). Against the 6.6 us of virtual time an episode should take, that is 1.3-1.5 ms/frame of host overhead, independently confirming the older 1.29 ms/tick figure. A clean simulation-thread profile (uild/acm/sched-glue-profile, job 20261120-030518-6eedf0, 18.96 ms/frame) then named the cost: model2a::timeslice_impl alone is 11.65% = 2.21 ms/frame, and scheduler plus FIFO/timer glue is ~4.07 ms/frame (21.5%); the same replay counted 6,136 CPU dispatches per frame (i960 2,079, TGP 2,165, Z80 1,892), 3,796 passes, ~3,040 timer firings and 1,396 i960 timeslice aborts, with the i960 averaging 209 executed cycles against a 97,608-cycle budget and returning early on 99.6% of dispatches. The lever is therefore per-event host overhead, not the DSP or geometry: the next experiment makes the scheduler inner loop 64-bit when the 128-bit high word is zero, and after that replaces the always-depth-1 m_line_queue std::deque with a single pending slot.

Latest experiment worktree is `build/acm/worktrees/i960-fifo-store-preflight`. Its artifact directory is `build/acm/i960-fifo-store-preflight`. See `proof.log`, the per-arena `*.hash` files and `build.log`. The prebuilt `vf2-acm-diagnostic` SHA-256 is `42f5b9dd8447236161287a950d7c1df9671382ccb68df8be45783d8963b9fe25`. It uses untrained board and i960 objects, so judge it against the same diagnostic binary with the flags off. The release build is a separate control.

## Corrected FIFO comparison result

The corrected comparison is complete. One proof and six counterbalanced
timing runs ran sequentially on the Mini with the untrained diagnostic
binary (`42f5b9dd...`). The audited device commands were: original path
with neither flag, grouped helper with `VF2_I960_FIFO_STORE_RUNS=1`, and
checked helper with both that flag and
`VF2_I960_FIFO_STORE_PREFLIGHT=1`.

| Arm | Sustained mean | p95 | p99 | Over 17.384 ms |
| --- | ---: | ---: | ---: | ---: |
| Original path | 18.323 ms | 19.212 ms | 19.816 ms | 474/480 |
| Grouped helper | 18.353 ms | 19.222 ms | 19.902 ms | 470/480 |
| Helper + ARM queue check | 18.422 ms | 19.243 ms | 20.006 ms | 468/480 |

The differences are inside run-to-run noise: control run means spanned
0.081 ms, more than any arm difference. The proof and all timing runs
matched state, draws, textures, atlas placement and the viewport. There
is no repeatable gain, so both flags stay opt-in and the release defaults
are unchanged. The interactive release was restored and verified at PID
3345, CRC `9b1f5565`. See [README.md](README.md) and
`build/acm/i960-fifo-store-preflight`.

## What to do next

The 2026-11-20 measurement splits the short replay cleanly. Steady
full-render frames have a 16.99 ms simulation mean (p95 17.95) and a
0.531 ms mean swap-to-simulation gap. That gap is close to the VBLANK
tile descriptor (0.175 ms) plus geometry join (0.345 ms), so the
every-frame serial join is the only non-simulation cost of the right
size. Five non-simulation render/present hitches of 30-170 ms also
pollute the short-replay mean and p99, but they are absent from the
302-541 sustained window and are not the path to the cadence target.

An independent review completed on 2026-11-20 placed every remaining
micro-optimisation at 0.03-0.25 ms against a roughly 0.08 ms run-to-run
floor, and concluded that incremental work cannot reach 17.384 ms with a
low p95. Its ranked candidates were DSP `0x4a` block-aware publication
(0.05-0.25 ms), the already-proven signed-gap dispatch (tail only),
inlining the hottest i960 helpers (0.05-0.15 ms), wider native DSP
coverage (0.03-0.10 ms) and FIFO line-path timer cleanup (0.03-0.08 ms).
The signed-gap dispatch has since been re-screened and rejected
(mean +0.115 ms, p95 +0.053 ms, +26 misses of 478), so it is no longer
part of the stack. Treat the rest as small tail improvements, not a
single fix.

Order of work: (1) the 0.317 ms materialization wait is
`worker execution - vblank window`, so it needs about 7.3% off the whole
worker; a worker material-gather/prefetch trial measured 4.009 against
4.008 ms execution (no gain, rejected), and the gather is not a
meaningful share of worker time even though it is the hottest mapped
line; an earlier snapshot is not possible because the list is captured in
`geo_parse()` at `T_VBLANK_START`, the earliest point it exists, and
`frame_end()` must wait because `render_polygons()` reads the parsed
geometry, so a 7% cut must come from the transform/parser bulk and has no
demonstrated path; (2) the 2.371 ms prior-GPU drain is overlapped and must be
dropped: a same-binary comparison removed one render worker, which raised
render CPU and drain by ~1.6 ms but moved the interval by only +0.057 ms
because the simulation wait fell from 2.83 to 0.80 ms, and the earlier
two-frame queue (`20261117-124750-8bd486`) moved the wait into a 44 ms
tile upload while resource duplication gained nothing; the interval is
`sim_task + gap` with sim 17.2 and gap 0.34-0.53, so the mean and p95 must
come from the simulation task; (3) reduce the simulation tail, which is
0.8 ms above its mean and is what keeps p95 above 17.384; the DSP is not
that tail: a per-frame counter trace on the current tree shows slow ticks
carry +4.9% DSP instructions (+4,000/frame) and +6.5% block runs, which
explains at most 42-44% of the 1.09-1.12 ms excess, and 0x32 removed
1,266,261 dispatches with no Dural gain, so native DSP command coverage is
exhausted - attribute the remaining ~0.6 ms to the i960 guest, the board
scheduler/FIFOs and failed FIFO reads; that attribution is now done
(`build/acm/sim-tail-fifo`): failed i960 reads are 41.5% of i960 reads
both on ordinary ticks (41.46%) and on slow ticks (40.98%), so they do not
concentrate on the tail - slow ticks simply carry 5-10% more FIFO traffic.
The remaining 0.6 ms is proportional work everywhere (DSP +5%, block runs
+6-7%, FIFO traffic +5-10%), so no single hotspot removal reaches it.
Pricing and attributing that per-event overhead is now done: the handoff
span is 9.3-9.6 us/episode and 8.0-8.2 ms/frame, i.e. 1.3-1.5 ms/frame of
host cost, and on a clean build `model2a::timeslice_impl` alone is
2.21 ms/frame with scheduler plus FIFO/timer glue at about 4.07 ms/frame
of an 18.96 ms task (6,136 dispatches, 3,796 passes and ~3,040 timer
firings per frame). Two tempting targets are already spent: with
`VF2_MODULAR_CLOCK=1` the loop's `clock_type` is already `uint64_t`
(`timeslice_impl<true,true,true,true,true>`, `clock_type=
std::conditional_t<Modular,uint64_t,atime>`), and `m_line_queue` is a
`std::vector<int>`, not a deque. So the actionable item is event count,
not loop arithmetic: the fusion is already on the assert side
(`assert_fifo_halt`, used by both `fifo_sync_empty` and `fifo_sync_full`)
but missing on the unblock side, because `fifo_push` calls
`set_input_line(..., CLEAR_LINE)` unconditionally, which inserts a
zero-delay timer that becomes the head and aborts the DSP timeslice about
862 times per frame. Fuse that clear symmetrically while keeping the
abort, so the emulated instant and ordering are unchanged (0.1-0.2 ms);
then make the timer queue an indexed heap (0.15-0.25 ms); (4) only then
stack micro-optimisations. Note the guest itself is saturated: 2,079 i960
dispatches x 209 executed cycles = 434,552 cycles per frame against a
25 MHz x 17.384 ms = 434,600-cycle budget, so p95 needs a global kernel
throughput gain, not more glue removal.

The clear fusion was then implemented and tested
(`build/acm/fifo-clear-fusion`, opt-in `VF2_FUSE_FIFO_CLEAR`). It worked
mechanically - `L_I960_HALT` firings fell 1,321.7 -> 828.0 and
`L_TGP_HALT` 302.9 -> 143.7 per frame - but it is **rejected on both
criteria**: the core state hash changed (`2e7d925031640471` ->
`1f72e5857eac3b97`) with DSP instructions -2,127 and block runs -2,033
(pixels identical), and it was slower: jobs `20261120-034641-522bd7`
control vs `20261120-034717-5323a9` fused, +0.238 ms mean, +0.238 p95,
+0.280 ms simulation, +2 misses. The cause is host ordering, not the
timestamp: `timer_sync` records the line change during the DSP write but
applies it only when `execute_timers` fires at `m_base >= expire`, so
delivering it inline sets `m_suspend_changes` one pass earlier and lets
the i960 execute at emulated times the reference forbids. The source was
reverted. This rules out "remove scheduler events" as a cheap direction:
the events mark real emulated instants.

The PGO retrain was then tested and closed (`build/acm/pgo-retrain`):
retrained and release binaries are exactly state-identical and differ by
+0.041 ms mean / +0.045 p95, below the floor. The copro function-port
fast path was also implemented and rejected
(`build/acm/copro-port-fast`): state-identical, but +0.13 and +0.36 ms in
the two pairs, and `fifo_push` is only 0.30 ms/frame in the profile, which
bounds the whole path. Omit-frame-pointer with the retrained PGO profile
is state-identical and makes the simulation task 0.106 ms cheaper, but
moves the interval by +0.020 ms (`build/acm/omit-frame-pointer`).

**That flat interval is the important result, and it supersedes the
`sim_task + gap` assumption above.** Phase accounting shows the frame is
0.27 ms of simulation wait + **16.42 ms of main-thread render** + 0.57
composite + 0.17 swap = 17.44 against a 17.484 interval; the simulation
task is 16.918 with a mean wait of 0.454 ms. The render phase is 94% of
the frame. The pacing deadline is also 69 ms in the past on average
(`sleep_until` never sleeps), so the interval is pure work, not vsync or
sleep jitter. A `--profile-render` sample of the main thread
(`build/acm/render-profile`, 744 samples, 14.10 ms/frame) puts **2.92
ms in libMali, 2.88 ms in libc**, 1.93 in `prepare_batch_direct`, 1.35 in
`clip_packet_into`, 0.83 in `tile_atlas::get` and 0.72 in
`prepare_batch`.

Correction: part of that render phase is GPU wait.
`gpu_queue.drain()` sits inside it (`acm_main.cpp:567`) and the job
reports **3.89 ms/frame of fence wait** - the queue holds exactly one
fence, and the wait only appears at the production cadence (the non-PGO
builds at 18.8-19.3 ms report 0.05 ms). So real CPU render is about
12.5 ms and the frame is **GPU-queue-gated**. `VF2_SYNC_GPU` is dead
under `VF2_GPU_QUEUE=1` (`acm_main.cpp:617-618`), so `--async-gpu` is
inert.

A depth-2 fence ring was built and tested at the real cadence
(`build/acm/pgo-depth`, with a profile recollected for the new header):
it removes the wait (3.50 -> 0.06 ms) and improves the mean 0.124 ms
while halving frames over budget (316 -> 143 of 478), but **p95 rises
18.27 -> 20.01** because queued frames stall 26-28 ms. Rejected on p95.

What pins the mean is still open. The Mali utilization counters read 0%
at every sample (`gpu-runtime.json`, GPU clock 384 MHz), and a VBO
instead of client arrays is much worse (jobs `…-054200-06d4e1`,
`…-054227-9b77d5` against `…-054134-e6667c`, `…-054254-b508a1`: +0.686
and +1.739 ms mean, state and pixels identical), so "the GPU is
saturated" is not established. The stage split
(`build/acm/copy-callers`, job `20261120-053946-83b2e5`) is: packet/
material preparation 6.71 ms wall / 6.28 CPU, prior GPU drain 3.95 / 0.25,
3D submit 3.61 / 3.55, camera preparation 1.70, EGL swap 0.75, picture
capture 0.53, tile upload 0.59 - 14.24 ms of main-thread CPU against 16.9
ms of simulation, with the previous fence pending at the packet stage in
265 of 269 frames. The remaining ~1 ms must come from the 6.28 ms packet/
material stage or from the 14.2 ms whole, not from the drain.

### The two replays are different regimes (2026-11-20, after the shader work)

The short tick-270 replay is GPU-fenced; the 900-frame full Dural replay
from `build/acm/fight.checkpoint` is not:

| | Short (tick 270) | Full (fight start) |
| --- | ---: | ---: |
| Interval mean | 17.53 | **17.377** |
| Fence wait | 3.5-3.9 ms | **0.12 ms** |
| Sim task mean | 17.0 | 14.6 |
| `input/pace` sleep | ~0.01 ms | **5.86 ms** |

So the full replay's mean is ~tick and the main thread has ~5.9 ms of
slack. Its tail is not a slow thread, but the earlier "424 late frames
slept and still overshot" statistic was an **identity, not a cause**
(independent review, 2026-11-20): deadlines advance by exactly one tick,
so `interval(n) - tick == slack(n-1) - slack(n)` holds for every frame
(residual 0.000000 ms). Pacing is `sleep_until(next_tick)` after the swap
(`src/host/acm_main.cpp:630-640`), so a frame presents at
`tick_n + work(n)` and the interval is
`tick_period + work(n) - work(n-1)`. The measured cause is the
frame-to-frame present-offset jitter: 1.438 ms p95 on Dural, 0.803 ms on
Stage 5.

**Fix, qualified and now the default for paced runs:** move the swap
after the pacing sleep (`VF2_PRESENT_AT_TICK`, harness
`--present-at-tick`, default on for paced full runs). The frame is
already complete when the sleep starts, so presents land on the tick
grid. Offset jitter falls to 0.288 ms (Dural) and 0.158 ms (Stage 5), and
over 2 pairs per scene:

| Scene | p95 base -> tick | p99 base -> tick |
| --- | ---: | ---: |
| Dural | 18.392 -> **17.539** | 19.308 -> 17.706 |
| Stage 5 | 17.881 -> **17.503** | 18.835 -> 17.548 |
| Waterfront | 17.930 -> **17.477** | 18.300 -> 17.600 |

State identical in every scene and pair; Dural pixels 100.0000%. The
short tick-270 replay, where the deadline is already in the past, is a
no-op (mean 17.523 -> 17.520). The count of intervals over 17.384 rises
slightly because presents now sit on the grid with symmetric ~0.1-0.3 ms
jitter, so about half fall a fraction above the tick; p95/p99 are what
the target is about. Evidence: `build/acm/tick-present/findings.md`,
`present_offset.py`.

**Correction, same day, after measuring it:** the claim that the remaining
0.15-0.17 ms "is the eglSwapBuffers call" was asserted, not measured. The
frame trace now records `swap_start_ns` and `sleep_end_ns`
(`build/acm/present-times`, plain `-O3` build, 900-frame Dural, 4 arms).
In the grid arm the sleep-end spacing itself is p95 17.760-17.825 and the
swap-start spacing is the same to within 0.07 ms, while the swap call is
only 0.24-0.27 ms mean. So the residual is **`sleep_until` wake-up
latency**, not the swap. `VF2_PACE_SPIN_US` (opt-in; sleep that many
microseconds short of the tick and spin the rest) cuts p95 by ~0.15 ms
(17.825/17.756 -> 17.688/17.572) but leaves p99 18.2-18.5 and max 22-28 ms,
because ~2 of 870 frames overrun the period and set that tail. On the
non-PGO build the strict p95 <= 17.384 target is therefore **not** met;
the release-equivalent figures remain p95 17.539 / 17.503 / 17.477 for
Dural / Stage 5 / Waterfront. Both new knobs stay opt-in until a PGO build
carrying them is qualified, and audio stays deferred.

The textured-transparent shader (`VF2_TEXTURED_TRANSPARENT_SHADER`,
default off) was built for the short replay: mode 0's textured path
without the `vInfo0.x` and checker branches. It is pixel-exact and gives
4/4 pairs mean -0.078/-0.102/-0.080/-0.073 ms with misses 602 -> 467 of
954, but the gain is on the 0.08 ms floor and the full replay does not
reproduce it (-0.004, -0.014 ms mean, p95 +0.171, +0.082). Not installed.
The homogeneous-shader A/B it came from is worth keeping in mind: turning
`VF2_HOMOGENEOUS_ATLAS_SHADERS` off costs **+1.002 / +1.072 ms** mean with
identical pixels, so GPU fragment complexity is a real lever for the
short replay.

**Frame target, with audio off: met to clock resolution.** The qualified
paced default (present-at-tick + 1 ms spin) measures present-grid p95
**17.385 ms** on full Dural, Stage 5 and Waterfront - the 17.384 tick plus
one clock tick - with mean 17.381, and 6 of 6 counterbalanced pairs
negative. Evidence: `build/acm/pgo-spin/findings.md`.

**Audio: the board is nearly free; the tail is the scheduler path.**
With the sound board enabled the game state is bit-identical (core
`59f2ee171fb93ed2`, geometry `a36957b440da7a79`). The first reading
attributed an +8 ms tail to the SCSP mixer; four probes disproved that -
muting the ALSA write, zeroing the SCSP mixer, holding the 68000, and
forcing the lazy aux path all leave sim_task p95 at 24.1-24.3 with the
same 54 spike frames. The tail follows `modular_frame_ready()`, which
returns false whenever `m_sound_on`: disabling that fast path on the
**sound-off** arm reproduces it exactly (sim p95 24.29, tail 25.82, 54
spikes) while sound-off with the path is 16.90 / 17.14 / 2 spikes.
`no-modular` and `sound-on` agree to ~0.05 ms, so the board's own cost is
~0.05 ms, not +0.85. Evidence: `build/acm/pgo-audio/findings.md`.

So audio is affordable in principle. That was implemented and qualified:
`timeslice()` now has a modular-clock pass with the 68000 in the same
loop (`timeslice_impl<false,true,true,...>`, opt-in `VF2_MODULAR_SOUND`),
and the sound-on arms return to 2 spikes of 870 (Dural) and 0 (Stage 5,
Waterfront) with sim p95 17.16-17.90. State and pixels stay identical.
The synchronous ALSA write was then exposed as the next stall (~+2.9 ms
grid p95), so `audio::write()` now enqueues to a writer thread. Grid p95
returns to 17.385-17.40. **Remaining blocker:** the host PCM drains only
~126 of 900 blocks (`Audio: dropped 774`), so audio output is not keeping
up; the frame loop is no longer the cause. Next: inspect the card-1
`hw_params`/period and fix the drain rate, then re-qualify all three
scenes before making audio default. Evidence:
`build/acm/pgo-audio2/findings.md`.

The sound-board guards on the pipeline and the checkpoint loader are lifted
only under `VF2_AUDIO_EXPERIMENT` (harness `--audio-experiment,
--sound-board, --audio-mute`); a checkpoint carries no sound state, so a
load resumes the board from power-on. Next lever: decompose the SCSP mixer
vs DSP cost on the 446+ frames, then either cut it or run the board in
`sound_only` mode (already used for online play) on a worker core - the
board is provably independent of the game state.

The device clock is not a factor: all four cores are governor
`performance` at 1.20 GHz (cap 1.344), 46 C. Keep clocks stock, screen on
the short Dural replay with counterbalanced pairs, and qualify any
repeatable gain with the full Dural replay plus Stage 5 and Waterfront.
Re-run the independent review every 4 hours.

## Resume procedure

1. Check the repo branch and local worktrees, confirm the device is available, and make sure no other process is using the USB benchmark connection. The existing ARM Docker container is `acm-vf2-armv7`, with the repo mounted at `/work/vf2-recomp`. The existing diagnostic binary is ready. If rebuilding is necessary, use `docker exec acm-vf2-armv7 sh /work/vf2-recomp/build/acm/i960-fifo-store-preflight/build.sh`. The script expects the local pinned object sets, generated DSP tables and stock libraries referenced in it.
2. The corrected comparison has already run; its commands remain reproducible if it must be repeated. Repeating from scratch requires removing `build/acm/i960-fifo-store-preflight/timing-jobs.json` first, because the harness skips names already recorded. The launcher interrupts the interactive game and restores the selected release in a `finally` block. Run these from the Windows repo root:

   ```powershell
   python -u build/acm/i960-fifo-store-preflight/run.py
   python build/acm/i960-fifo-store-preflight/verify-release.py
   python build/acm/i960-fifo-store-preflight/analyze.py
   ```

   The launcher expects `build/acm/worktrees/i960-hot-layout/tools/acm/benchmark-device.py`, the tick-270 checkpoint and input file under `build/acm/sustained-dural-checkpoint`, and the ordered atlas seed located via `build/acm/checkpoint-atlas-order/proof-jobs.json`. The analyzer reads the archived matching render proof via `build/acm/active-draw-buckets/colour-proof-jobs.json`. The archive JSONs point to local `build/acm/hardware/iterations` directories.
3. Audit the archived `timing-jobs.json` command for every run. The original path has both experiment flags off. The helper arm has `VF2_I960_FIFO_STORE_RUNS=1`. The ARM check arm adds `VF2_I960_FIFO_STORE_PREFLIGHT=1`. The completed run used one proof and six timing runs in counterbalanced order on the same diagnostic binary. The analyzer compared draw and texture hashes, atlas placement, final state and viewport, and computed mean, p95, p99, and misses over the 240 combat ticks. The interactive process and CRC were verified after the run.
4. If there is a repeatable short-replay gain, qualify the trained build with full Dural replays and check Stage 5 and Waterfront before changing defaults. The 269-frame replay matches game state, draws, atlas placement and pixels in the corresponding full replay. Cadence can differ, so use the full replay for release qualification. Keep hardware clocks stock and benchmark with full drawing and audio deferred.
5. If the gain is inconclusive, keep the trial opt-in and return to the measured simulation bottlenecks. The FIFO accounted for about 56% of traced i960 bus-helper calls in sustained Dural and Stage 5. Use the latest traces to distinguish FIFO traffic from plain ROM/RAM transfers and from scheduler or DSP work before choosing another implementation. See `build/acm/i960-clear-bit-loop/bus-stage-*.json` and `fifo-bus-stage-*.json` in the same directory.

Commit meaningful checkpoints locally and record exact run commands and results in [README.md](README.md). Keep the user informed when a hardware test changes what is running on the Mini.

## Arcade-cadence goal, 1.344 GHz baseline (2026-11-21)

Active objective: p99 frame work < 17.384 ms during steady gameplay, >= 99% of
frame submissions onto their scheduled arcade tick, reported as lateness in
microseconds rather than a rounded 17.385 ms interval.

Baseline: CPU OPP raised to the 1.344 GHz entry the kernel already advertises
(`cpuinfo_max_freq` = 1344000; default `scaling_max_freq` is 1200000) on all
four cores. DDR is NOT changed: it is locked at 360 MHz, and every attempt to
raise it is rejected by the driver (`dmesg: [ddrfreq] ERR:unsupported, please
enable in sysconfig`). Voltage and RAM untouched.

### Method

`build/acm/pgo-audio4/analyze-lateness.py` reads the frame trace and splits
each frame into the four things that can make it late, all in microseconds
against `pace_deadline_ns`:

- `enter_us`: when the render chain finished and entered the pacing wait,
  derived as `swap_end(n-1) + pace(n-1) + sum(stages before pacing)(n)`
- `wake_us`: `sleep_end_ns - deadline` (scheduler/spin overshoot)
- `start_us`: `swap_start_ns - deadline` (when the present began)
- `submit_us`: `swap_end_ns - deadline` (when the present returned)
- `slip`: frames whose `start_us` exceeds one tick (a cadence break)

The pipeline is work -> [enter] -> sleep/spin -> [wake] -> present -> [submit],
so whichever of these is already positive identifies the cause.

### Result: two Stage 2 runs, 900 frames, 1.344 GHz, detached speaker

| job | n | work p50 | work p99 | sim p99 | join p99 | enter p99 | wake p99 | start p99 | submit p99 | present p99 | slip | underruns | dropped |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 842c16 | 868 | 14.828 | 17.715 | 16.537 | 0.626 | 793.9 | 1143.9 | 1144.9 | 1297.8 | 379.6 | 0 | 0 | 0 |
| 639c1e | 867 | 14.666 | 17.921 | 16.464 | 0.778 | 7734.8 | 7930.9 | 7932.1 | 8103.7 | 380.2 | 0 | 0 | 0 |

work/sim/join in ms, everything else in us. `sim p99` 16.46-16.54 ms reproduces
the expected 16.55-16.64 ms baseline.

Conclusions that decide the next step:

- **The delay is work before the wait.** `enter_us` is already positive (794 us
  and 7.7 ms) and `wake_us`/`start_us` sit within 100 us of it, so neither the
  scheduler nor the present is responsible. Present costs 380 us at p99.
- **The simulation is fully hidden.** `join p99` is only 0.63-0.78 ms because
  `next_simulation()` runs during the previous frame's chain, and `sim_task`
  16.5 ms fits inside the 17.384 ms period. The simulation is not on the frame
  critical path.
- **p99 frame work is 17.72-17.92 ms, above the 17.384 ms target.** Median is
  14.7-14.8 ms, so the chain carries ~2.5 ms of slack at the median and loses
  it in the tail.
- **No cadence break**: `slip` = 0 in both runs (no frame ever began presenting
  a whole tick late); 0 underruns, 0 dropped sound ticks.

### Where the chain tail comes from

Per-stage p50 / p99 / mean-over-late-frames (frames whose work > 17.384 ms):

| stage | p50 | p99 | late-frame mean |
| --- | ---: | ---: | ---: |
| packets | 9.34 | 12.44 | 11.26 |
| submit | 2.60 | 3.81 | 3.17 |
| camera | 1.86 | 3.47 | 2.45 |
| tile_upload | 0.14 | 0.58 | 0.43 |
| composite | 0.28 | 0.67 | 0.23 (1.46 in the second run) |
| flush | 0.24 | 0.73 | 0.21 |

`packets` adds 1.4-1.9 ms on late frames and is the dominant term; `submit` and
`camera` add ~0.6-1.0 ms each. Packet sub-phases at 1.344 GHz (p50/p99):
prepare_packets+palettes 4.51/5.89, planning 2.35/2.91, pack 2.05/4.05. Note
`prepare_packets` is ALREADY parallel (it fans out over the row pool at
`gpu3d_gles2.cpp:818`), so its 4.5 ms is parallel wall time; the 2.35 ms
planning step (`prepare_batch_direct`) is the genuinely serial block.

### PGO constraint for any further change

The shipped binary is PGO-trained and the incremental script warns instead of
failing on profile mismatch, so an edit silently drops the profile of the
functions it touches. Measured cost of losing `main()`'s profile: simulation
+0.4 ms (17.05 vs 16.5-16.7 for an inert edit). Use `build-use.sh`
(`-Werror=coverage-mismatch`) and `build-generate.sh` to retrain, or keep edits
out of profiled hot functions.

### Commands and paths

- Baseline jobs: `build/acm/hardware/iterations/20261121-003414-842c16` and
  `.../20261121-003452-639c1e` (ids also in
  `build/acm/pgo-audio4/stage2-baseline.txt`).
- `python tools/acm/benchmark-device.py --frames 900 --paced --fast --frame-trace --profile-render-stages --quiet-monitor --binary build/acm/vf2-acm-detached-O3 --detached-audio speaker --sound-cpu 3 --sound-prio 10 --state build/acm/arena-checkpoints/stage-2.checkpoint --sim-prio 5`
- `python build/acm/pgo-audio4/analyze-lateness.py --skip 30 <job> [job...]`
- Packet sub-phases: add `--profile-packet-phases`.
- Clock baseline: `for i in 0 1 2 3; do echo 1344000 > /sys/devices/system/cpu/cpu$i/cpufreq/scaling_max_freq; done`

### Measurement artifact: the composition stall (2026-11-21)

Some runs of any binary (release or modified) fall into a mode where the
`composite` stage (composition submit, not the GPU drain) takes 21-34 ms on
20-90 frames of the gameplay window, ~21 ms matching one ALSA playback period
(882 frames at 44.1 kHz). Per-frame thread CPU for the stage is far below the
wall time, so the render thread is waiting or preempted, not copying pixels.

Evidence that it is environmental, not the change under test:

- Present in both A/B arms at similar rates (stage 2: A 11 bad frames, B 25;
  stage 5: A 95, B 0; dural: A 68).
- Four consecutive `--detached-audio muted` runs were all clean (composite
  p99 0.575-2.372 ms), while five of eight speaker runs showed the stall.
- Not thermal: trip points are 75/90/110 C, device idles at 43-47 C, governor
  `performance` at 1.344 GHz, DDR fixed at 360 MHz.
- Not stray load: `/proc/loadavg` 1.0 is a single uninterruptible task; a
  3 s per-pid tick delta shows every process at 0 ticks.
- All interrupts, including `Mali_GP`/`Mali_PP0`/`Mali_PP1` and
  `sunxi_dmac`, are delivered on CPU0; the sound worker is pinned to CPU3 at
  FIFO 10 and the main process runs on CPUs 0-2.

Protocol used for every A/B below: a run whose gameplay window contains any
`composite_ms > 5` is treated as contaminated and repeated (up to four
attempts), so a p99 comparison is never decided by the artifact. The repeat
rate is 40-50% under speaker audio, which is itself the strongest clue that
the trigger is the ALSA path; it is not yet explained.

### PGO retrain and qualification of commit e2d1301 (2026-11-21)

The change edits `gpu3d_gles2.cpp`, which invalidates its profile, so the
whole profile set was regenerated: `build/acm/pgo-audio4/retrain-pgo.py`
(`build-generate.sh`, three training runs on `stage-2`, `stage-5` and `fight`
checkpoints with speaker audio, `collect-pgo-audio4.py`, `build-use.sh`,
`build-detached-inc.sh`). New set: 25 files, sha256
`dea429f696af66ac6573ea693e3d81d216c2a7eb25eac29d4c515225f231e5c9`.
`-Wmissing-profile` warnings in this tree are normal (917 in the previous
known-good retrain, 1043 now) and do not mean the profile was rejected.

Qualification, alternating A/B, clean runs only (no composite > 10 ms),
600 frames per scene, 1200 for Dural, 1.344 GHz, detached speaker:

| arm | binary | sha256 (16) |
| --- | --- | --- |
| A | previously qualified release | `D482C0CE882DE149` |
| B | retrained `e2d1301` | `60FB343FA42A0AD4` |

| scene | work p50 A -> B | work p99 A / B (per run) | submit p99 B | misses | <=1ms | <=half tick | <=tick |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| Stage 2 | 14.61 -> 14.23 | 17.69/17.94 vs 15.45/18.98 | 0.42 / 26.6 ms | 0 / 18 | 96.8-100% | 93.8-100% | 96.8-100% |
| Waterfront | 14.58 -> 14.25 | 17.19/16.80 vs 18.53/17.20 | 7.49 / 6.21 ms | 0 / 0 | 94.7-99.8% | 99.3-100% | 100% |
| Stage 5 | 11.66 -> 11.49 | 15.37/16.47 vs 13.97/16.78 | 0.47 ms | 0 / 0 | 96.8-100% | 100% | 100% |
| Dural | 12.55 -> 12.46 | 17.04/17.39 vs 17.10/16.49 | 1.67 / 8.59 ms | 0 / 0 | 96.2-99.1% | 99.0-100% | 100% |

Reading, and why this is not yet a pass on the objective's first bar:

- The p50 gain repeats in every scene with the retrained profile too
  (-0.09 to -0.37 ms), so the optimization survives an honest profile.
- p99 work is still at the boundary and moves with run-to-run noise. Stage 2
  is the worst: one B run reached 15.45 ms and one 18.98 ms with 18 frames
  starting a whole tick late. A scene cannot be called passing while any
  clean run contains deadline misses ("keep every long interval within the
  window"), so the honest status is Stage 5 passing, Waterfront and Dural
  borderline, Stage 2 not yet.
- Audio health is clean in every run: 0 underruns, 0 dropped sound ticks.

Submission lateness model (this is the structural floor, not work):

- `present_at_tick` performs the EGL swap *after* the pacing wake, so the
  present starts on the tick and completes 240 us later (p50) / 390 us (p99).
  Measured `swap_start - deadline` p99 is 474-628 us, `swap_end - deadline`
  p99 594-760 us; 0% of submissions land at or before the deadline in every
  run, by construction, while 99.0-100% land within half a tick and 100%
  within their slot.
- The `swap_ms` stage column contains the pacing sleep plus the present
  (p50 2.7-3.2 ms), so it is not a present cost.

Trace paths for this qualification: job ids in
`build/acm/pgo-audio4/ab3-scenes.txt`; per-run traces under
`build/acm/hardware/iterations/<job>/frames.csv` and `game.log`. Analysis:
`python build/acm/pgo-audio4/report-final.py build/acm/pgo-audio4/ab3-scenes.txt`.

Remaining bottleneck: the serial render chain on the slowest 1% of frames -
packets 9.9-10.1 ms, camera 2.5-3.4 ms, submit 3.1-4.1 ms (`tail-split.py`).
Packet planning (~1.8 ms) is still serial: the atlas is single-threaded state
and bucket append order must be preserved, so parallelising it needs a
per-worker resolution pass and an ordered append.

### Pool join spin: recovering the dispatch tail (2026-11-21)

The packet stage carries a tail of ~9.9-12.5 ms that no stage-level change
had moved. Instrumenting the pack phase (`VF2_PACK_RANGE_STATS`, temporarily
on the pre-spin binary) showed where it goes:

- Pack wall p50 2.48 ms, but the widest of the eight bands finished in
  0.705 ms and the sum of all band durations was 4.59 ms (4.59 / 4 threads
  ~ 1.15 ms of work per thread). The rest was not packing.
- Band start times p50 were `[0.07, 0.12, 0.72, 0.79, 1.25, 1.45, 1.67,
  1.88]` ms: two dispatch rounds on a four-thread pool, with the second round
  starting ~1.2 ms in. Two workers also woke ~0.7 ms late.
- Not contention on the pool's cross-thread run lock: with `VF2_POOL_STATS`
  the whole 600-frame run waited 4.658 ms total (~8 us/frame).

Two candidate fixes were measured and one rejected:

- Weighted pack split (cut points by `work.n + work.ni` instead of job count):
  the imbalance is real (the last equal range is ~1.8x the others) but the cut
  alone did not move the wall (pack p50 3.12 vs 3.02 ms; bands still
  `[0.65, 0.60, 0.40, 0.43, 0.38, 0.33, 0.28, 0.27]`). Rejected as a change.
- Worker wake spin (`VF2_POOL_WAKE_SPIN_US=800`): workers spinning on CPUs 0-2
  starve the render and sound threads; 3-5 audio underruns per run and on-tick
  submissions collapsed to 0.2-0.4%. Rejected.

The fix kept is a bounded spin on the join counter in the *caller* only
(`VF2_POOL_JOIN_SPIN_US`, default 800 us; the condvar remains the fallback).
Phases finish a few hundred microseconds apart, and parking the render thread
for the last band costs about a millisecond of wake-up.

Same-binary A/B, arm A spin=0 vs arm B spin=800, Stage 2, 600 frames,
1.344 GHz, detached speaker, contaminated runs (any `composite_ms > 5`)
repeated, seven clean runs per arm:

| metric (median of clean runs) | park | spin 800 us |
| --- | ---: | ---: |
| work p99 | 17.94 ms | **17.52 ms** |
| submissions on tick | 95.1% | **96.8%** |
| work p50 | 14.52 ms | 14.51 ms |
| audio underruns / dropped ticks | 0 / 0 | 0 / 0 |

Mechanism, slowest 1% of frames (`joinspin-tail.py`): `submit` 4.52 -> 3.26 ms
and `flush` 1.10 -> 0.30 ms, while `camera` (2.96 -> 2.87) and `clip`/`plan`
are flat, i.e. the gain is the GPU-wait/submit tail, not the work itself. The
state signature is identical in both arms and matches the qualified
Stage 2/Waterfront signature (`game frame 2703, IP 00027eb4, TGP 55095657,
work 61492d4bd92bdb66`), so the change is scheduling-only.

Related negative result: cutting one band per thread (`VF2_PACK_RANGES=4`)
instead of eight is worse (pack p99 4.90 vs 4.44 ms, work p99 18.50 vs
17.65 ms). With the join spin in place, pack p50 is ~2.0 ms in *both* band
arms, so the dispatch overhead was the whole gap and the band count no longer
matters.

Commands: `build/acm/pgo-audio4/joinspin-ab.py` and `joinspin-ab2.py` (gated
alternating runs), `joinspin-report.py`, `joinspin-stages.py`,
`joinspin-tail.py`, `joinspin-state.py`.
Binary: `build/acm/vf2-acm-joinspin` = `2B80428CE983AAB4` (reproducible from
this source).

### Qualification: present lead + join spin vs shipped release (2026-11-21)

Honest PGO retrain against the changed `parallel.h` (it is inlined into many
profiled translation units): `retrain-pgo.py generate|train|collect|use`,
profile set 25 files sha256
`087f4fd141334140d27070ccbe718157f088e934277cdb902f5a370ec2b8841a`.
`build-detached-inc.sh` reported "up to date" after the collect because `make`
does not track the profile data, so `obj-detached` was cleared and rebuilt
before hashing; that rebuilt binary is the one qualified.

| arm | contents | sha256 (16) |
| --- | --- | --- |
| A | shipped release | `D482C0CE882DE149` |
| B | present lead 500 us + join spin 800 us, retrained PGO | `370FF652E8F2A497` |

Alternating A/B, clean runs only (gate: no gameplay frame with
`composite_ms > 5`, up to six attempts), 600 frames per scene, 1200 for Dural,
1.344 GHz, detached speaker (`ab4-scenes.txt`):

| scene | work p50 A -> B | work p99 A / B | submit on tick A -> B | misses A / B | audio |
| --- | --- | --- | --- | --- | --- |
| Stage 2 | 14.52 -> 14.39 | 17.12/17.29 vs 17.58/17.79 | 0% -> 96.8/97.2% | 0 / 0 | 0/0 |
| Waterfront | 14.59 -> 14.38 | 18.21/17.23 vs 17.23/16.74 | 0% -> 96.6/97.5% | 30 / 0 | 0/0 |
| Stage 5 | 11.91 -> 11.82 | 14.42/16.39 vs 16.52/17.31 | 0% -> 97.5/97.2% | 0 / 0 | 0/0 |
| Dural | 12.61 -> 12.31 | 17.00/18.46 vs 16.48/16.49 | 0% -> 97.8/97.7% | 13 / 0 | 0/0 |

What this qualification does and does not establish:

- Established: the present lead is what moves submissions onto the tick (A is
  at 0% by construction because it wakes on the tick and presents after it);
  with the lead, every arm is at 96.6-97.8%, and **no B run had a deadline
  miss** where the shipped release had 43 across the four scenes. Audio is
  clean everywhere (0 underruns, 0 dropped ticks).
- Established: pixels and emulation are unchanged. Every B run's `screen.png`
  matches its A scene exactly (`d4ff6a4cd0bb328b` Stage 2 and Waterfront,
  `1bbed5bb36193e7d` Stage 5, `4620c0ee16377af3` Dural) and the state
  signatures are identical.
- Not established: the objective's 99% bar. On-tick is 96.6-97.8%, short of
  99%, and Stage 2's work p99 is 17.58/17.79 vs the 17.384 bar. The binding
  constraint is now the work tail, not the lead: `max(work_end, tick - lead) +
  present <= tick` needs work p99 <= 16.88 ms, and Stage 2 is ~0.7-0.9 ms over.
- Stage 2's B work p99 (17.58/17.79) is slightly above A's (17.12/17.29) in
  this sample while the other scenes improve; n=2 per arm and same-arm p99
  variance is ~1 ms, so that difference is not attributable to the change.

Trace paths: job ids in `build/acm/pgo-audio4/ab4-scenes.txt`, per-run traces
under `build/acm/hardware/iterations/<job>/`, analysis with
`python build/acm/pgo-audio4/report-final.py build/acm/pgo-audio4/ab4-scenes.txt`
and `python build/acm/pgo-audio4/visual-check.py ab4-scenes.txt ab4-scenes.txt A`.

Remaining bottleneck after this change: the packet stage's parallel wall
(`clip` ~4.5 ms + `plan` ~2.1 ms + `pack` ~2.0 ms p50), which is what the work
tail is made of; the join spin already removed the dispatch/join overhead
inside it. Further work has to reduce packet CPU or move it off the critical
path.

### Bilinear display default (2026-11-21)

Decision: bilinear magnification of the final canvas is the display default;
nearest-neighbor is retained only as a pixel-comparison override. This is an
output/default change, not a cadence claim. The earlier captures already
established that bilinear changes the visible pixels, so no new pixel test was
needed.

Source: `src/host/acm_main.cpp`, compositor member
`linear_upscale = std::getenv("VF2_NEAREST_UPSCALE") == nullptr`.
The frontend override is `VF2_NEAREST_UPSCALE=1`; `VF2_LINEAR_UPSCALE` is
obsolete. Harness: `benchmark-device.py --linear-upscale` is an explicit no-op
for old scripts, and `--nearest-upscale` sets the override.

Binary: `build/acm/vf2-acm-linear`, sha256 prefix `178381BA1078133E`.
The detached play binary `build/acm/vf2-acm-detached-O3` is byte-identical
(same sha256 prefix, CRC32 `d3906ad1`), so the installed interactive release
already has the bilinear default. `install-play.py` drops all scaling flags
from the launcher environment and relies on the engine default. Verified on
the Mini: `/run/vf2/run-game-audio` contains no `UPSCALE` export, and
`/run/vf2/vf2-acm` has CRC32 `d3906ad1`.

The 2930-frame demo-fight linear-vs-nearest A/B was stopped and is not a
qualification: it was measuring timing cost, which was not the request, and
every run was contaminated by the environmental composition stall. It does not
change the cadence bottleneck above.

### Demo-fight intrinsic tail and simulation profile (2026-11-21)

A 600-frame checkpoint run from the demo fight, muted detached audio, job
`20261121-113137-4c3dd4`, binary `vf2-acm-linear` sha256 prefix
`178381BA1078133E`, 1.344 GHz, measured 56.06 fps. Work p99 was 24.72 ms with
86/600 frames over 17.384 ms; `composite_ms > 5` appeared on 60 frames,
concentrated at 498-516 and 560-600. Excluding those, the clean subset was
538 frames with work p99 24.28 ms and 83 frames over the tick. So the demo
fight is still not qualified, and the display default is not the cause.

The simulation-thread CPU profile (1655 samples, 8.359 CPU seconds, thread 661)
attributed 69.43% to `/run/vf2/vf2-acm`, 25.80% to native JIT code and 4.77% to
libc. Top samples: `vf2_tiles::packet_into<false,false>` 9.18%; the m68000
`divu_w_*` variants together about 23.6%; `gles2::prepare_batch_direct` 3.81%;
`vf2_batch::tile_atlas::get` 1.69%. The next candidate is to test one divu or
packet-tail reduction, with the criterion that state hashes stay identical and
demo work p99 falls below 16.88 ms with zero misses. No such candidate has been
implemented or qualified yet.

### Full attract loop baseline and transition attribution (2026-11-21)

The attract goal (zero late or dropped frames across every character 3D
segment, with speaker audio) is not met. One uninterrupted loop from cold boot
was recorded: job `20261121-134251-a61878`, binary `178381ba1078133e`, 12,000
frames in 209.175 s (57.37 fps), 0 dropped sound ticks and **3 audio
underruns**. Command:

```
python -u tools/acm/benchmark-device.py --frames 12000 --paced --fast \
  --frame-trace --profile-render-stages --quiet-monitor \
  --binary build/acm/vf2-acm-linear --boot \
  --detached-audio speaker --sound-cpu 3 --sound-prio 10 --sim-prio 5
```

`tools/acm/analyze-attract.py JOB` segments the trace into 3D scenes. Over
10,133 3D frames (final capture excluded): **81.81% submitted on or before the
exact tick**, 92.47% within one slot, work p50 12.49 ms and **p99 20.46 ms**
against a 16.88 ms target. **25 ticks were skipped** in four deadline jumps of
5.96, 5.97, 6.30 and 6.62 ticks. Per-scene table:

```
scene frames       n  work50  work99   worst early%   late  >tick
  989- 3917   2929   17.35   23.19   74.57  65.1%   1023    411
 4242- 6293   2052   17.35   17.86   73.16  87.8%    251     66
 6540- 6991    452   17.36   24.69   78.97  76.5%    106     35
 7007-10036   3030   17.36   18.76   81.55  90.5%    288    160
10102-12000   1899   17.36   21.94  152.47  87.8%    231    107
```

(The `work` column there includes the pace/swap wait; the per-frame stage sum
used for the 12.49/20.46 figures excludes it.)

Column semantics, previously misread: `sim_wait_ns` (and the `sim_ms` stage) is
the render thread's join wait; `sim_task_ns` is the simulation job duration
(mean 12.26 ms over the loop). The pipeline already launches the next
simulation at `acm_main.cpp:934`, before camera and packet preparation, so
launch placement is not a remaining lever.

Two independent deficits. First, the present cost: a frame that sleeps to the
target starts at tick-496.7 us and submits on time only if the present costs
under 497 us, but 3.29% of 3D frames present in more than 500 us (p50 252,
p99 578, p99.9 822, max 1,750 us). The 500 us lead therefore caps the on-tick
share near 96.7% by itself; covering p99.9 needs about a 900 us lead and moves
the work target to about 16.48 ms. Second, the work tail at transitions.

The 22-84 ms packet spikes at character and scene changes are the serial
**planning/atlas remainder**, not packing. Job `20261121-143531-b5b9a2` ran to
frame 7,512 with `--profile-packet-phases` (muted, since a checkpoint cannot be
combined with detached audio) and `tools/acm/align-packet-phases.py` aligns the
phase stream with the trace:

```
phase      p50     p99     max
clip      2.14    4.45    8.89
rest      0.82    2.59   47.98
pack      0.77    2.87    3.89

   frame  packets    clip    rest    pack
    6294    84.21    6.13   47.98    1.48
    2935    53.82    8.13   35.20    1.53
    6618    45.37    8.89   34.82    1.60
    4242    22.53    4.11   17.11    1.25
```

The phase columns are `capture->clipped`, `clipped->completed minus pack`, and
`pack` (`gpu3d_gles2.cpp:1840`). Planning spikes 20-58x its median exactly on
the frames that overrun a whole tick, while `pack` stays at 1.5 ms and `clip`
at 6-9 ms.

A follow-up run to frame 6,600 with `VF2_GLES_STATS=1` (job
`20261121-153451-70fd37`) showed **two** phase records for the one frame 6294:

```
(4985, clip 4.25, rest 22.92, pack 0.00)
(4986, clip 6.35, rest 47.64, pack 1.50)
```

That is 82.7 ms of phases against 84.7 ms measured, and the same job reports
`Projected stream: 4984 frames, 2 plan fallbacks`. The extra records are the two
frames where the projected plan failed and the host re-planned from expanded
polygons (`acm_main.cpp:977-983`); the two fallback frames are 2935 (50.0 ms) and
6294 (84.7 ms).

Instrumentation then named the failing call (`VF2_PLAN_FAIL_STATS=1`, job
`20261121-190135-062431`, default 12 MiB cap):

```
Plan fail 2 (get_pair worker): frame 1948 at=56/1504 band=1 level=6 tex=128x128 page_count=16 resident=1947
Projected plan FAILED at staged frame 1948 (falling back to expanded polygons)
Retry 0: clearing the atlas at staged frame 1949
Plan fail 2 (get_pair worker): frame 4984 at=54/1541 band=0 level=7 tex=256x256 page_count=16 resident=1708
Projected plan FAILED at staged frame 4985 (falling back to expanded polygons)
Retry 0: clearing the atlas at staged frame 4986
Projected stream: 4984 frames, 2 plan fallbacks
```

So the trigger is an **atlas page claim failure**: `claim_backing` refuses
`allocated+bytes>12 MiB` (`gles2_atlas.h:93`). It is not an index overflow, and
it is not the texture-RAM-change clear. The sequence on a transition frame is:

1. the projected `prepare_batch(nullptr,...)` plans the frame, reaches a polygon
   whose tile needs a new 520-side page, and `get_pair` returns false with
   `pack=0`;
2. `prepare_projected` returns false and the host increments
   `projected_fallbacks`, expands the stream and calls `prepare_frame_cpu`;
3. the second `prepare_staged` re-clips everything and fails at the same
   polygon; `index_overflow` is false, so the retry loop calls
   `batch_atlas.clear()` (the clear counter does increment; the earlier
   `VF2_GLES_STATS` window simply ended before this frame) and re-plans;
4. with `frame_indexed` now false the draw lists are gone, so the retry takes
   the serial non-indexed path over expanded polygons: the 47.6 ms record.

A 16 MiB cap run (job `20261121-191034-64af37`) confirms the cap is the trigger:
the first transition stops failing (`Projected stream: ... 1 plan fallbacks`),
and the remaining failure lands 12,416 bytes short of the cap:

```
Plan fail 2 (get_pair worker): frame 4984 at=90/1541 band=0 level=7 tex=256x256 page_count=22 resident=2518 allocated=16764800/16777216
```

A new 520-side LUMINANCE_ALPHA page costs 540,800 bytes, so that frame needs
about 17.3 MiB. The atlas keeps every scene's pages, so raising the cap only
defers the failure: it moved from page 16 to page 22.

The diagnostic binary is built with `-DVF2_SCHED_DIAGNOSTICS`, so its absolute
timings are not comparable with the release; use same-binary A/B runs for
diagnosis and qualify gains only on a retrained release.

`gpu3d_gles2.cpp` and `gles2_atlas.h` are both in the profiled build, so any
change there needs an honest PGO retrain before qualification.

Short replays from a mid-attract checkpoint are **not** representative of a
transition: `build/acm/attract-7512.checkpoint` carries no GPU atlas state, so
the replay runs with `prior_gpu_ms` 3.1-5.0 against 0.08 in the full loop, a
palette switch on all 40 frames, and it resumed at game frame 7515 rather than
7512 (job `20261121-151528-0fd5f4`). Use full attract runs for atlas/planning
changes; short replays remain valid for pipeline and pacing changes.

Audio is a separate failure: the detached worker reports synth 14.862 ms/tick
plus write 2.115 ms/tick against a 17.384 ms tick, with a 72.8 ms worst write.
That 17.0 ms of work is where the three underruns come from.

Harness addition: `benchmark-device.py --save-state-out PATH` asks the run to
save a checkpoint at its last frame and downloads it with a CRC check. It is
rejected together with detached audio by the engine.

Detailed notes: `build/acm/pgo-audio4/attract-baseline.md`.

## Stopping point (2026-11-21 19:21): the transition trigger is an atlas page claim failure

### What is on disk

- `src/gpu/gpu3d_gles2.cpp` and `src/gpu/gles2_atlas.h` carry diagnostic
  instrumentation (committed, default behaviour unchanged):
  - `VF2_PLAN_FAIL_STATS=1` prints which of the five `prepare_batch_direct`
    failure returns fired, plus the projected-skip / retry-loop path, with the
    frame ordinal, polygon index, band, texel size, page count, resident tiles
    and `allocated/cap`.
  - `VF2_ATLAS_LIMIT_MB=N` overrides the 12 MiB atlas cap; the default is still
    12 MiB.
  - `tile_atlas::claim_failures` counts refused page claims;
    `allocated_bytes()` / `capacity_bytes()` are public accessors.
  - This changes the profiled `gpu3d_gles2.cpp`, so the release binary on disk
    is stale with respect to source: **no timing from the current source is
    qualified until a PGO retrain and a release rebuild**.
- Diagnostic binary `build/acm/vf2-acm-diag` was built by
  `build/acm/pgo-audio4/build-detached-diag.sh`. It carries
  `-DVF2_SCHED_DIAGNOSTICS`, so its absolute timings are not comparable with the
  release; compare same-binary A/B runs only.
- `build/acm/vf2-acm-linear` and `build/acm/vf2-acm-detached-O3` both remain
  sha256 `178381ba1078...` (unchanged).
- The interactive release was restored and verified after the diagnostic runs:
  PID 1619, `/run/vf2/vf2-acm` CRC `d3906ad1`, 57.5 fps, detached speaker
  configured. The wrapper launcher is `/run/vf2/run-game-audio`.

### Evidence recorded this session

| job | binary | frames | cap | finding |
| --- | --- | ---: | ---: | --- |
| `20261121-153451-70fd37` | release | 6,600 | 12 MiB | 2 `projected_fallbacks`; 6294 = 84.70 ms, 2935 = 50.01 ms |
| `20261121-190135-062431` | diag | 6,300 | 12 MiB | 2 fallbacks; `get_pair` refused a page at `page_count=16`, `resident` 1947 / 1708 |
| `20261121-191034-64af37` | diag | 6,300 | 16 MiB | 1 fallback; first transition clean; remaining failure at `allocated=16764800/16777216`, `page_count=22` |

The 16 MiB run's first-transition spike disappeared (next-worst frames after
6294 are 1320 = 23.89, 3918 = 23.64, 4242 = 21.98), which is direct evidence
that the cap, not the plan shape, decides whether the frame falls back.

### Next steps, in order

1. **Make the projected retry reuse the packet plan** (smallest change, in
   `prepare_staged`): after a failed projected `prepare_batch(nullptr,...)`, if
   `batch_atlas.claim_failures` changed, call `batch_atlas.clear()`, set
   `packets_ready=true` (the packet/draw-list data is unchanged by the failed
   plan), and retry `prepare_batch(nullptr,...)` once. That skips the serial
   re-clip and the expanded-polygon path, which is where ~30 ms of the 47.6 ms
   record goes. Sketch:

   ```cpp
   if(palettes_ready&&packets_ready&&draw_lists_ready){
       const uint64_t claims=batch_atlas.claim_failures;
       staged_plan=prepare_batch(nullptr,count,f,staged_rows,false);
       if(!staged_plan&&batch_atlas.claim_failures!=claims){
           batch_atlas.clear();
           packets_ready=true;
           staged_plan=prepare_batch(nullptr,count,f,staged_rows,false);
       }
   }
   ```

   Acceptance: `Projected stream: ... 0 plan fallbacks` on a 6,300-frame boot,
   the 6294 packet time drops well below 84.7 ms, geometry/state hashes and the
   capture are unchanged versus job `20261121-190135-062431`.

2. **Reclaim atlas pages instead of failing** (durable fix). The atlas keeps
   every scene's pages, so the cap is hit again at the next transition. Dropping
   the oldest page means removing every `cached_tile` that points at it.
   `entries` is a `std::deque<cached_tile>` so individual erasure invalidates
   the `index_slot::entry` pointers; rebuild `entries` + `lookup_slots` in one
   pass over the live pages instead (n is ~2,500, so the pass is cheap) and
   delete the dropped page's GL textures while decrementing `allocated`.
   Acceptance: `claim_failures` stays zero across a full attract loop and
   `allocated` never hits the cap.

3. **Get the transition frame under one tick.** Even a clean single plan costs
   ~20-30 ms, so the transition frame still misses. Two candidates: split the
   first-frame plan across ticks (check `VF2_FRAME_BUDGET_DIVISION`), or prewarm
   the incoming scene's tiles during the preceding 2D screen (the baseline has
   ~246 non-3D frames between scenes). Prewarming must be verified to keep
   geometry/state hashes identical.

4. **Retrain and qualify.** Run
   `python build/acm/pgo-audio4/retrain-pgo.py`, rebuild the release with
   `build/acm/pgo-audio4/build-detached-inc.sh`, then three consecutive full
   attract loops with the baseline command (12,000 frames, `--paced --fast
   --frame-trace --profile-render-stages`, speaker audio). Report per-segment
   frames, work p50/p99/worst, late/skipped ticks, audio underruns and dropped
   ticks. Three clean loops is the exit criterion.

5. **Audio is still a separate failure** and is not addressed by any of the
   above: synth 14.862 ms/tick plus write 2.115 ms/tick against a 17.384 ms tick
   (worst write 72.8 ms), which is where the 3 underruns in the baseline came
   from.

6. **Present lead.** The 500 us lead caps on-tick near 96.7% by itself (3.29% of
   3D frames present in over 500 us; p99.9 822 us). Covering p99.9 needs ~900 us,
   which moves the work target to ~16.48 ms. Re-evaluate only after the work
   tail is cut; do not change the lead and the work target in the same arm.

### Reproduce commands

```
# diagnostic build (container acm-vf2-armv7 must be up)
docker exec acm-vf2-armv7 sh /work/vf2-recomp/build/acm/pgo-audio4/build-detached-diag.sh

# 6,300-frame boot with the branch counters
python -u tools/acm/benchmark-device.py --frames 6300 --paced --fast \
  --frame-trace --profile-render-stages --quiet-monitor \
  --binary build/acm/vf2-acm-diag --boot --sim-prio 5 \
  --env VF2_PLAN_FAIL_STATS=1 --env VF2_GLES_STATS=1

# the cap experiment that isolated the trigger
... --env VF2_ATLAS_LIMIT_MB=16

# restart the interactive release after any benchmark upload
python build/acm/pgo-audio4/install-play.py
```

One device job at a time; restore and verify the interactive release
(`/run/vf2/vf2-acm` CRC `d3906ad1`, game running) after the last run.
