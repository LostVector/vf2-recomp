# Astro City Mini VF2 handoff

Checkpoint from November 20, 2026. Independent priority reviews now run every 4 hours, not every hour (user update, 2026-11-20).

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
