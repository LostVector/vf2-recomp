# Daytona USA on the Astro City Mini

`daytona-acm` runs the original arcade program from the MAME `daytona`
parent set (Revision A). It shares VF2's i960 recompiler, Model 2 video and
GLES2 frontend. The original Model 2 target adds the main ROM mirror, the TGP's
collision data, the cabinet's Z80 I/O firmware and Model 1 sound through a
68000, YM3438 and two MultiPCM chips. A fresh cabinet starts with single-cabinet
settings. The build uses Daytona-specific objects and i960 coverage.

## Current priorities

The performance target is a delivered-frame p99 of 17.384 ms or less and
at least 99% on-time fresh frames on Beginner, Intermediate, and Expert.
Measure sustained races with full drawing and quiet speaker audio. Keep
loading, warmup, and captures outside the timing window. Capture moving
frames in a separate replay to verify visual output.

- **Beginner:** Reduce packet construction and Mali workload together.
  Matched packet replay exposed more than 2 ms of simulation CPU cost
  from concurrent rendering and 5.79–7.78 ms of GPU wait. Measure both
  costs on heavy race frames and their successors.
- **Intermediate:** Resolve the atlas sampling discrepancy before changing
  texture coordinates. A diagnostic that sampled the center of one
  constant-color tile removed its dependence on surrounding atlas contents.
  A general fix needs to derive the constant-color condition from decoded
  texels and invalidate it on texture RAM writes.
- **Expert:** Reduce clipping, packing, and composition work. Compare
  delivered frame times and on-time freshness alongside CPU time.

Compare core and geometry state, draw order, rendered pixels, and audio
when qualifying a renderer change. Keep clean and candidate launch
settings and clock rates in the run record.

## Build and run

Copy your `daytona.zip` into `roms/`. It must contain the shared CPU board
ROMs and the I/O firmware `epr-14869c.25`. The MAME non-merged Revision A
archive contains these. The loader checks ROM contents against their CRCs.

On Linux with the repository's GLES2, ALSA and zlib build dependencies:

```sh
make -j6 daytona-acm
EGL_PLATFORM=surfaceless MESA_GLES_VERSION_OVERRIDE=2.0 \
  build/bin/daytona-acm roms/daytona.zip --headless --uncapped --frames 3850 \
  --script tools/acm/daytona-race.txt --capture build/daytona-race.png
```

Build the Mini binary in the Cortex-A7 cross-build environment with the
same zlib, stock Mali and ALSA libraries used for VF2. See the [setup
instructions](README.md) for those dependencies.

```sh
sh tools/acm/build-frontend-arm.sh daytona-acm
```

In the RAM USB development session, place `build/acm/bin/daytona-acm`,
`roms/daytona.zip`, and `tools/acm/run-daytona-on-device.sh` in
`/run/daytona`. Call the script `run-game` and make it executable.
The launcher uses the working VF2 environment's ARM loader and ALSA
configuration. With the framebuffer available, run `/run/daytona/run-game`.
Only one game can use the framebuffer at a time. The Daytona launcher stores cabinet settings and
backup RAM in `/run/daytona/cabinet.nv`. To keep them across RAM-session
reboots, use `--nvram FILE` with a path on persistent storage or copy the
file out before rebooting.

The launcher adapts drawing to the available time in each tick. It draws
every frame when there is room and can skip one draw to catch up. The
simulation, input and sound still run each tick. Set `DAYTONA_FULL_DRAW=1`
to draw every frame, or pass `--draw-cadence 1/2` for fixed half-rate drawing.
On a RAM-only setup, `--release-rom` frees the uploaded ZIP after loading.
Use that option with a disposable copy and upload it again before the next launch.

| Control | Mini / keyboard |
| --- | --- |
| Coin, Start | F2 or 5, F1 or Enter |
| Steering | Left / Right |
| Accelerate, Brake | A or Up, B or Down |
| Camera views | C, D, E, F |
| Service menu, service credit | F3, F4 |
| Manual gearbox | Keyboard 0 for neutral, 1–4 for gears |
| Exit | Escape or Power |

Automatic transmission is available using the built-in controls. Arcade link
play, force feedback and emulation checkpoints are outside the current target.
`--audio-capture FILE` writes interleaved 44.1 kHz signed 16-bit PCM for a
headless run with inline sound.

## Timing reference

The 17.384 ms tick budget is not an assumption — it is derived from the Model 2
(Daytona Rev A) screen geometry and the recompiler enforces it exactly:

- `src/host/main.cpp:279` — `FRAME_HZ = 16000000.0 / (656.0 * 424.0)` = **57.52416 Hz**,
  i.e. the 16 MHz Model 2 pixel clock over a 656×424 total (active + blanking) raster.
- `src/host/acm_main.cpp:888` — the ACM frame pacer uses `tick_period = 17384000 ns`,
  exactly `1000 / 57.52416` ms.
- `src/machine/model2a.cpp:809` and `src/net/session.h` both carry the same 57.52416 Hz.

So the budget is the board's own raster rate, and the pacer will not let a tick finish
faster than it. A frame that misses 17.384 ms is a frame the arcade would have shown but
this build did not deliver in time. No better (tighter) figure is available for Rev A: the
pixel clock and raster totals are fixed by the hardware, and the recompiler already uses
the tightest of the two published Model 2 totals (424 lines). Treat 17.384 ms as a hard
ceiling, not a soft target.

## Verification

The bundled replay inserts a coin, chooses a circuit and transmission,
then operates the pedals, steering and view buttons. To regenerate the i960
map, run the replay and a separate cold attract run with `--i960-coverage FILE`.
The option runs the i960 interpreter and records observed control-flow edges.
The checked-in map contains guest addresses. The JIT reads program bytes
from the supplied ROM at startup.

The host i960 comparison matched the interpreter's core hash
`298a97987395b487`, geometry hash `f9d0ad5e3a5a2705`, and PNG SHA-256
`7d6778434ec5b96cb912329db1571bb929c000843ab149e441520eabdd59ca77`
over the first 1,700 replay frames with sound held. On the same host, a
269-frame VF2 Dural replay from a regenerated stage-10 checkpoint matched
baseline `77d6086` byte for byte in the final PNG and reported core hash
`2e7d925031640471` and geometry hash `724d987cdc7cbca5` in both builds.

### Mini driving replay

The render profile and original sound ROMs ran together on the device.
The 3,850-tick replay ended in active driving. The two reduced-cadence
runs used the same binary (CRC32 `78386ce5`) and fresh cabinet settings.
They matched in main core and geometry hashes and submitted all 3,850
audio ticks.

| Drawing cadence | Game ticks/s | Displayed frames/s | ALSA underruns |
| --- | ---: | ---: | ---: |
| Every frame | 49.16 | 49.16 | 85 |
| Two of three | 55.52 | 37.02 | 17 |
| Every other frame | 57.30 | 28.66 | 1 |

Every-other-frame drawing was the initial default based on game speed and
audio continuity. Full drawing on the earlier audio build (CRC32 `7e179aaf`)
averaged 43.10 fps over active-driving ticks 3,001–3,800.
The interactive VF2 release was restarted after each hardware replay and
verified at CRC32 `d3906ad1`.

The commands, launcher contents, checksums, traces and PNGs from these jobs
are under `build/daytona/` in this workspace. The half-rate job is
`mini-half-audio.json`, with the results in `mini-half-audio.log`.
The original Daytona installation is recorded in `mini-playing.json`.
To repeat the half-rate run with the supplied replay on the
device:

```sh
VF2_RENDER_STAGES=1 VF2_FRAME_TRACE=/run/daytona/race.csv \
  /run/daytona/run-game --draw-cadence 1/2 --frames 3850 --script /run/daytona/daytona-race.txt \
  --nvram /run/daytona/test-cabinet.nv --capture /run/daytona/race.png
```

### Full-draw packet experiments

The 3,700-tick runs used the same ARM binary (CRC32 `c099c6b8`) and
full drawing with speaker audio. Measurements below cover race ticks
3,001–3,650. The core and geometry hashes and final PNG matched in all
three runs.

| Configuration | Frame mean / p95 (ms) | Packet median (ms) | Clip median (ms) |
| --- | ---: | ---: | ---: |
| Three render threads | 23.30 / 27.34 | 14.44 | 8.22 |
| Single-cell clipping shortcut | 23.40 / 27.00 | 14.63 | 8.44 |
| Two render threads | 23.88 / 28.74 | 15.21 | 8.56 |

Three render threads and the original clipper are retained. Each
configuration exceeded 17.384 ms in all 650 race ticks. The commands,
binary checksums and phase traces are in
`build/daytona/optimization-packets/`. The preliminary uncapped run
filled the audio queue and is excluded.

### Render-thread CPU sample

The full-draw sample used the ARM binary with CRC32 `15065ff0`. The
sampler collected 2,528 instruction pointers on the main render thread at
5 ms CPU-time intervals over race ticks 3,001–3,600. The capture dropped
zero samples. Render workers and the audio worker have separate CPU clocks.

| Work | Share of render-thread samples |
| --- | ---: |
| Final draw packing | 17.68% |
| Texture packet clipping | 14.99% |
| libc | 21.32% |
| Mali driver | 13.92% |

The hottest application source line constructed the temporary texture
transform for each ordinary vertex in `gpu3d_gles2.cpp` (6.61%).
A generic ARM system-call return accounted for 14.28% of samples.
The sample location leaves its callers unresolved.
The raw samples and resolved reports are in
`build/daytona/optimization-adaptive/`.

Caching the ordinary texture-cell transform reduced median packing from
4.80 to 4.67 ms, while mean race-frame time moved from 23.40 to 23.43 ms.
State, geometry and PNG hashes matched. The original packer remains in
use. Both device commands and their timing data are in
`build/daytona/optimization-pack-reuse/`.

### Adaptive drawing screen

The same 3,700-tick replay on binary CRC32 `15065ff0` compared fixed
half-rate drawing with adjusting the draw decision to the estimated
render time and remaining tick budget.

| Mode | Game ticks/s | Displayed frames/s | Busy race displayed frames/s | ALSA underruns |
| --- | ---: | ---: | ---: | ---: |
| Every other frame | 56.84 | 28.43 | 28.80 | 5 |
| Adaptive | 57.20 | 41.26 | 29.54 | 2 |

Both matched the main core and geometry hashes and submitted every audio
tick. The last image matched within eight RGB levels on 99.9987% of
pixels. The active-race window is ticks 3,001–3,650.

The final launcher passed the 3,850-tick driving replay at **57.21 game
ticks/s and 40.84 displayed frames/s**, with one ALSA underrun and zero
dropped audio ticks. The sustained race window (3,001–3,800) ran at
57.52 game ticks/s and 29.41 displayed frames/s. Core and geometry
hashes matched the original half-rate replay. The first two qualification
attempts hit the Mini's RAM limit. The passing run released the temporary
ROM archive after loading.

Adaptive drawing is the installed default. The verified Daytona binary
has CRC32 `9d97b4b8`. The installed VF2 binary still has CRC32 `d3906ad1`,
and the 269-frame host Dural regression matched the baseline again.
The final replay command, trace and live launch verification are in
`build/daytona/optimization-qualified/`.

### Emulation checkpoints

Daytona reuses VF2's generic `model2a::save_state`/`load_state` machinery
through `acm_checkpoint`. A save at frame 2998 resumes with the race already
running, so profiling work restarts from the beginner-course grid instead of
replaying 2,998 boot/menu ticks.

Checkpoint format: a 40-byte outer header (`VF2ACM1\0`, version 1, pointer
bits, ROM hash, core size, geometry size, CRC32 of core+geometry) followed by
the core state (internal 24-byte `state_header`, magic `0x53324656`) and the
numeric geometry. The core is streamed straight to the file and its CRC is
accumulated during the write; the geometry is a few hundred KB in memory.
A save is refused while the sound board runs, so checkpoint jobs pass
`--no-sound-board` and `--detached-audio ""`. Save jobs release the
renderer's caches before writing, or the kernel kills the process.

A load was initially divergent: the cabinet I/O board Z80
(`model2a::m_original_io`, `original_io_board`) was never serialized, so a
load restarted the I/O firmware from power-on and the game stopped producing
geometry at frame 3012. `original_io_board::state(IO&)` now serializes the
CPU, pins, RAM, ports, address, direction, output, command, ADC, secondary
state and the shared input RAM, and `model2a::state()` includes it under
`MODEL2_ORIGINAL`. The filtered driving latch (`m_driving_analog`,
`m_driving_gear`) is also serialized, and `acm_main` seeds its host steering
and pedal filters from the restored latch.

After the fix, a save at 2998 is byte-exact to a cold-boot state at 2998
(core `84debc192760868f`, geometry `a1ecc40f2591b150`). A load from it
matched the continuous cold run frame-by-frame for frames 2999–3006 (work
hashes `daca3127f4718df4` … `a46c532682fbba4c`), and a load to 3850 matched
the baseline work hash `3eb8fd1f8d9f79da`, IP `00011c74`, and geometry
`df734e1731c57df2`. The rendered PNG differs only on textured 3D surfaces
(HUD matches): the picture-rotation state and atlas history are renderer
caches outside the machine checkpoint, not a state divergence.

A save at frame N resumes at N+3 by default; `VF2_CHECK_WARMUP=0` resumes at
N (first simulated frame N+1) for byte-exact verification. The host frame
counter starts at `machine.frame_number()` on load so the scripted inputs
resume at the right tick. Adding fields to `state()` changes the layout, so
older checkpoints are incompatible and the reference core hash must be
recomputed.

The device has no swap and `/run` is RAM-backed, so the checkpoint buffer,
trace CSVs and PNGs compete with the ~121 MB machine. A 16 MB atlas plus an
8 MB checkpoint buffer is OOM-killed at the margin; delete old
`/run/daytona/trial` artifacts and drop caches before a load (that raised
MemFree from ~148 to ~192 MB and let the loaded run pass). Loaded runs use
`--atlas-mb 8` to match the save run.

#### The memory ceiling stops a full race with audio

A full race with audio and full drawing does not finish. `--frames 3850`
with the sound machine and `DAYTONA_FULL_DRAW=1` is OOM-killed between
Frame 2100 and 2400 (~40 s), with the trace otherwise healthy (54 fps
average, `20261122-113053-audio-race-full`, `20261122-113455-audio-race-a8`,
`20261122-113749-audio-race-a8-compact2`). This blocks acceptance: no
course can produce a complete race until the footprint drops.

It is not a leak and not the atlas. Sampling `/proc/<pid>/maps` through a
race shows the heap flat from ~9 s: `[anon]` 141.3 MB, `/dev/mali` 22.5 MB,
`[heap]` 10.4 MB, VmRSS ~121 MB, unchanged from 9 s to 42 s. An 8 MB atlas
instead of 16 MB gives the identical VmData (175500 -> 205248 kB in both)
and the same kill frame. The interactive release is killed the same way at
~Frame 600 while sitting in attract mode, which warms the atlas across
several demo courses before a race ever starts.

The OOM report is the diagnosis: `Normal free:47084kB` but `free_cma`
~45 MB of it, so **non-CMA free is ~2 MB** against a 2036 kB watermark, and
the victim is the game with `anon-rss:119788kB`. The board has 254 MB total,
a ~51 MB CMA reservation for Mali (43 MB in use), and no swap, so ~120 MB of
game anon plus the kernel leaves essentially nothing for a driver
allocation. Every kill is an order-0 allocation from the Mali path
(`bf002d04`).

`--release-rom` does **not** help a trial run: the runner symlinks
`trial/daytona.zip` to the real archive, so `unlink()` removes the symlink
and the 15 MB stays resident. Only launching with the real path frees it,
and even then the race still died at the same frame.

##### Memory compaction test (negative)

One-shot test with the game stopped, same cleanup as the failed run
(clear `trial`, `drop_caches`). `/proc/sys/vm/compact_memory` **is** present
(mode `--w-------`, so `cat` fails and reports an error; the write is what
matters, and it returned 0).

| | buddyinfo (orders 0..10) | MemFree |
| --- | --- | ---: |
| before | 197 284 292 310 266 143 75 33 14 3 20 | 191440 kB |
| after | 218 288 293 309 265 143 75 33 14 3 20 | 191416 kB |

Memory was already unfragmented at idle (20 order-10 blocks before and
after). The identical rerun was killed in the same window, after Frame 2100
and before 2400, with the same OOM shape (`anon-rss:119788kB`, non-CMA free
~2 MB) and no new Mali messages. Fragmentation is not the cause; the board
simply does not have the memory. Snapshots in `build/daytona/compact/`.

The lever is therefore the steady-state anon footprint (~120 MB), not
cleanup, not the atlas, and not fragmentation.

##### Old-vs-current A/B: this is device state, not a Daytona regression

That last sentence was wrong, and the archived control run proves it.
`build/daytona/optimization-packets/control.json` + `control.log` record a
run that completed **3700 ticks with speaker audio (80 underruns, 0 dropped
ticks) at peak RSS 140768 KiB** — 3.6 MB *more* than the current build ever
reaches — exiting 0. Trial binary `c099c6b8`, launcher `8d982e8d`,
`VF2_THREADS=3`, `race.script`, no `--release-rom`.

That binary is still on disk (`build/daytona/daytona-optimization-acm`,
CRC32 `c099c6b8`, matching the archive). Re-run today under current staging
(same `race.script`, 3700 frames, full draw, audio, `VF2_THREADS=3`,
atlas 8): **it is OOM-killed after Frame 2100 too**, peak RSS 137128 KiB.
The current build (`e18a48a4`) dies in the same place. So the binary is
exonerated, and so is the launcher's cache set: the ablation sweep
(`build/daytona/mem-ablation/`, baseline VmData 205272 kB / VmRSS 121444 kB at
Frame 600) found no candidate worth more than ~3 MB, and three of them *grew*
the heap. `VF2_COMPACT_PACKETS` is not exported by the launcher, so it was
skipped.

| cache off | VmData delta | VmRSS delta |
| --- | ---: | ---: |
| `VF2_TRACK_GEOMETRY_PAGES` | -656 | -436 |
| `VF2_ATLAS_PAGE_CACHE` | -684 | -304 |
| `VF2_PALETTE_SHADE_CACHE` | -188 | -52 |
| `VF2_ATLAS_SNAPSHOT_DECODE` | -12 | +128 |
| `VF2_CACHE_2D_PENS` | +1076 | +288 |
| `VF2_SHARED_PACKET_STORAGE` | +2444 | +2012 |
| `VF2_RETAINED_FANS` | +2836 | +2788 |

Freeing the ROM properly also does not save it: with the real ZIP path and
`--release-rom` (Unevictable 42752 -> 28016 kB, so 14.7 MB really returned)
the race still died, just later, after Frame 2400.

That paragraph was wrong. Both binaries die in the same place, and the OOM
dumps say why. The wall is the **CMA reservation**, not accumulated state.

##### The CMA wall

Every kill has the same shape: non-CMA free pinned at the min watermark while
the CMA region sits mostly idle.

| run | anon kB | `free` kB | `free_cma` kB | **non-CMA free** kB |
| --- | ---: | ---: | ---: | ---: |
| audio on, full draw | 120024 | 47900 | 45864 | **2036** |
| audio off, full draw | 101488 | 65056 | 63020 | **2036** |

`free_cma` is 44-63 MB of the 46-65 MB free, so the non-CMA pool is at exactly
`min` (2036 kB) and the page cache is fully evicted (`active_file:0`). The
failing allocation is `gfp_mask=0x287d2, order=0` from `bf002d04` (libMali
kernel): **no `__GFP_MOVABLE`**, so it can never be served from a
`MIGRATE_CMA` pageblock. Roughly 72 MB is reserved for CMA and only ~28 MB is
used. The board is not short of RAM; it is short of RAM the GPU driver is
allowed to touch.

Levers tried, each one job, all with audio + full draw unless noted:

| change | result |
| --- | --- |
| atlas 8 MB instead of 16 | same kill frame, identical VmData |
| `drop_caches` + clear `trial` | no change |
| `echo 1 > compact_memory` at idle | buddyinfo unchanged, same kill |
| `min_free_kbytes` 2038 -> 640 | died, non-CMA free reached 672 kB |
| old binary `c099c6b8` (the archived one) | died after Frame 2100, same shape |
| `--release-rom` on the real path (frees 14.7 MB) | died after Frame 2400 |
| audio off (`--no-detached-audio`, 16 MB less RSS) | died after Frame 2400 |
| all three safe freeings stacked (`VF2_THREADS=1`, atlas 8, ROM released) | died after Frame 2400 |

Each ~15 MB freed buys ~300 ticks, and the heap is flat from ~9 s (`VmRSS`
121528 -> 121952 kB over 40 s), so this is not a leak: the race dies the first
time Mali needs a burst the non-CMA pool cannot cover. Freeing the remaining
non-essential RAM is the only lever of the right size left, and the two
candidates are not mine to spend: `/run/vf2` (15.6 MB, the VF2 session's
staging) and the CMA reservation itself (a boot/DTB parameter; `/proc/cmdline`
carries no `cma=`, the stock partition is read-only and there is no writable
persistent storage).

Two harness notes from this work. `--release-rom` frees nothing in a trial
run because `trial/daytona.zip` is a symlink and `unlink()` removes the
link. And `run-job.py --binary` used to write *through* the stale
`trial/daytona-acm` symlink and overwrite the release binary; it now removes
that link first (seen as the release coming back as `c099c6b8` at 51 fps).
Also: a helper script that holds the USB RPC connection cannot `subprocess`
`run-job.py` — the device serves one client, so the child fails silently and
leaves an empty job directory. Drive `run-job.py` from the shell instead.


#### First 10 seconds of the beginner course

The checkpoint at frame 2998 lands on the Three Seven Speedway "START" grid
(capture `build/daytona/jobs/20261122-033718-chk21-save2998fix/run.png`).
A loaded run from it to frame 3575 covered 577 ticks (~10.03 s at 57.52 Hz)
with full drawing. Job `20261122-074530-chk-first10` (exit 0):

| Metric | p50 | p95 | p99 | max |
| --- | ---: | ---: | ---: | ---: |
| Work total (ms) | 20.41 | 22.49 | 23.09 | 163.80 |
| Packet preparation | 11.18 | 14.80 | 15.67 | 38.20 |
| 3D submit | 4.27 | 4.94 | 5.14 | 24.28 |
| Camera preparation | 1.81 | 2.28 | 2.83 | 3.23 |
| Picture capture | 0.42 | 0.71 | 1.59 | 49.85 |
| Prior GPU drain | 0.46 | 4.19 | 4.39 | 4.67 |
| Composite | 0.25 | 0.40 | 0.44 | 0.48 |
| Flush | 0.20 | 0.73 | 0.93 | 5.43 |
| Swap | 0.10 | 0.16 | 0.26 | 144.92 |

559 of 577 race ticks exceeded 17.384 ms. The maxima are the first-frame
startup hitch (49.85 / 144.92 ms); every other frame is the steady state.
Packet preparation is the largest component, and its clip phase (~7.3 ms of
the ~11.2 ms) is the next target. Re-analyze with
`python build/daytona/analyze-job.py <jobdir>`; the analyzer now uses every
trace row when the trace begins at loop frame 1 (a checkpoint resume).

#### Clip attribution and the cell-loop optimization

`VF2_PACKET_PHASES=1` breaks packet preparation into clip, plan and pack. On
the 577-tick window (`20261122-075058-chk-clipattr`) clip is 9–11 ms, plan
1–2 ms and pack 2–5 ms per frame. `VF2_CLIP_STATS=1` then divides the clip
cell loop (`20261122-075058-chk-clipattr`, 577 frames): UV cell clipping is
49.1% of the cell-loop time, triangle/fan emission 30.8% and the cell polygon
copy 20.2%. The clip-call counts are plain counters, so they blur under the
row pool; the ratios are the usable signal.

The clip works in `double`. The Mini's Cortex-A7 VFP4/NEON is single
precision, so every `double` operation is a libgcc soft-float call
(`__aeabi_dadd`/`__aeabi_dsub` are linked into the binary), which is why the
clipper dominates despite handling few vertices. Two changes were tried:

1. Reusing the per-vertex signed distance from the count pass in the clip
   pass (byte-exact; halves the distance work for partially clipped cells).
   Binary CRC32 `40463bee`, job `20261122-081258-cellclip-first10`. Work,
   core and geometry hashes match the baseline (`41c2c570901fae0e`,
   `b2a23306453f180c`, `d65ac7ee2f1bb9e0`). Packet preparation moved from
   11.18 to 10.85 ms median and work from 20.41 to 20.38 ms; p99 stayed
   ~23.7 ms. Too small to reach the budget.
2. A single-precision cell-clip path (`VF2_CELL_CLIP_FLOAT`) that computes
   the four UV plane distances and the interpolation in `float`. It is
   expected to change geometry only where a vertex sits within one ULP of a
   cell boundary, so it must be qualified against the baseline PNG and
   geometry hash before it can become a default.

Measured on the same 577-tick window (byte-exact, hashes unchanged):
cell-clip-float is only ~0.2 ms faster than distance reuse (packets 10.68 vs
10.85 ms; work p50 20.27 vs 20.38, p99 slightly worse). So the software-double
distance math is not the dominant clip cost.

#### Vertex-array zero-fill (rejected)

`batch_vertices.resize(nv)` value-initialized the POD vertex vector, zeroing
the whole 52-byte-per-vertex array every frame before the pack overwrote it.
The clean stats run shows **17,820 compact cell vertices per frame** (10.28M
over 577 frames), i.e. ~926 KB of wasted memset per frame. Replacing the
resize with a high-water growth plus tracked `batch_vertex_count` /
`batch_index_count` (binary CRC32 `4b6cdc81`, job
`20261122-084132-nofill-first10`) is byte-exact but **neutral**: work p50
20.30 vs 20.27, packets p50 10.80 vs 10.68. The libc samples in the profile
are therefore not this memset; keep the size-tracking change (it is harmless
and avoids needless zeroing) but it is not the win.

#### Packet phase split (clean, no clip counters)

The earlier `VF2_PACKET_PHASES` numbers included the `VF2_CLIP_STATS` clock
overhead and were inflated. `prepare_staged` now records four phases: atlas
`apply_rams`, packet clip/palette, plan and pack, printed as
`Packet phases N a b c d`. Use `VF2_PACKET_PHASES=1` alone to get the true
split. Clean 577-tick medians (`20261122-085343-phases-first10`): apply_rams
0.01 ms, clip/palette 5.87 ms, plan 1.32 ms, pack 3.28 ms. Palette
preparation is only ~0.2 ms (`VF2_PALETTE_CHURN=1`), so the clip dominates.

The analyzer now folds `apply_rams` into the clip column when four numbers are
printed.

#### Submit is draw-call time, not the vertex upload

`VF2_TRACE_SLOW_SUBMITS=1` now prints a per-phase average. The real submit
(`20261122-093310-submitphases-first10`) is `target-underlay` 0.53 ms,
**`state-vertices` 0.029 ms**, **`draw-calls` 3.56 ms**, `overlay-finish`
0.23 ms, `texture-atlas` 0.05 ms, `palette` 0.04 ms. So `glBufferData` is
free (the driver defers the copy) and the two dozen draws are the cost.

A per-draw timer (`VF2_TRACE_SLOW_SUBMITS`, `Draw calls:` line) shows
**~172–178 µs per call, ~112 ns per index (≈336 ns per triangle)**. The
per-draw spread follows index count, so the cost is proportional to geometry,
not a fixed per-call fee. That matches the launcher's `VF2_CLIENT_ARRAYS=1`:
the driver reads each vertex from client memory at draw time, so draw time
scales with indices × vertex bytes. Shrinking `batch_vertex` (52 bytes) or the
triangle count therefore attacks submit directly.

#### Plane clipping in float is neutral

Only the cell UV planes had float variants. `VF2_CLIP_FLOAT_ALL=1` converts the
four viewport and two depth plane passes to `clip_by_float` too. It is
byte-exact (geometry hash `d65ac7ee2f1bb9e0`) but neutral: clip/palette 6.14 ms
vs 6.03, packets 10.95 vs 10.90 (`20261122-094422-clipfloatall-first10`). So the
clip cost is the loop, per-cell copy and triangle emission, not the soft-float
distance math. Kept env-gated, off by default.

#### Other rejected experiments

- `VF2_FAST_VIEWPORT_BOUNDS=1` (`20261122-094004-vpbounds-first10`): neutral
  (packets 10.80 vs 10.90). The all-inside scan costs what it saves because
  racing polygons straddle the edges. Note the launcher already exports it.
- Finer packet chunking (`threads()*16`, 32 `VF2_DUAL_PACK` ranges,
  `20261122-091820-chunks-first10`): neutral overall (work p50 20.20 vs 20.32).
- Zero-fill elimination (`20261122-084132-nofill-first10`): byte-exact, neutral.

#### Render-thread CPU profile (steady state)

Sample the render thread's own CPU clock with
`VF2_CPU_SAMPLES=/run/daytona/trial/prof.samples`, `VF2_CPU_SAMPLES_THREAD=1`,
`VF2_CPU_SAMPLES_RENDER=1` and `VF2_CPU_SAMPLE_WINDOWS=1`. The `.windows`
file holds per-frame deltas keyed by the absolute machine frame. Aggregate the
steady-state frames (`20261122-094902-prof-render-first10`, machine frames
3200–3575) against `daytona-acm.symbols.txt` (base `0xb6b39000`, from
`prof.samples.maps`):

| share | symbol |
|------:|--------|
| 26.1% | libc.so.6 (unresolved copies/memset) |
| 18.7% | `prepare_batch_direct` pack lambda (`lambda(int)#4`) |
| 11.9% | libMali.so |
| 7.4% | `gles2_submit_projected` |
| 3.7%+2.2% | `prepare_batch_direct` caller / range lambda |
| 3.2% | `prepare_shared_projected` clip lambda |
| 2.7% | `geometry_frame::prepare` |
| 2.4% | `gles2_prewarm_atlas` |
| 1.5% | `prepare_palettes` |

`prepare_batch_reference` shows 8.5% but cannot run while
`VF2_GLES_DIRECT_PACK` is exported; nearest-symbol resolution attributed a
neighbouring function to it. The libc + libMali + submit cluster (~45%) is the
draw path, but it is **not** the vertex byte count: `VF2_SKIP_UV1=1` removes the
8-byte `aUV1` attribute and leaves `draw-calls` unchanged (3.550 ms vs 3.556,
112.9 vs 112.8 ns/index; `20261122-095642-uv1skip-first10`). Draw cost tracks
the **index/triangle count**, not the per-vertex bytes. The probe's real signal
is `prior_gpu` falling 1.21 → 0.30 ms: the second atlas (mip-blend) fetch costs
~0.9 ms of GPU time.

So the draw lever is fewer triangles (cell splits, fan retention), not a
narrower vertex. The pack phase (`prepare_batch_direct` ~24%) and libc remain
the CPU target inside packets.

#### Single-cell copy elision

Most textured polygons keep their UVs inside one texture cell, so the cell
loop's `auto cell=polygon` scratch copy is pure overhead (no clipping runs).
`clip_packet_into` now emits the polygon directly when
`first[0]==last[0] && first[1]==last[1]`. Byte-exact, but **neutral-to-worse**
(clip/palette 6.38 vs 6.03, packets 11.17 vs 10.90;
`20261122-100235-singlecell-first10`): the extra generic lambda hurt the
multi-cell path more than the skipped copy saved. Reverted.

#### Vertex zero-init elimination

`clip_packet_into`'s `make_vertex` used `vertex v{}` and then assigned every
one of the 52 bytes, so the aggregate zero-init was dead. Changed to
`vertex v`. Byte-exact (geometry hash `d65ac7ee2f1bb9e0`), but neutral
(clip/palette 6.16 vs 6.03, packets 10.99 vs 10.90;
`20261122-100708-nomemset-first10`). Most cells take the `ordinary_vertex`
(24-byte aggregate) path, so the memset was not on the hot path. Kept as a
correct cleanup.

#### Where the time actually goes

After the above, every contained micro-optimization is neutral. The 577-tick
window is `packets ~10.9` (clip/palette ~6.0, plan ~1.35, pack ~3.1),
`submit ~4.3` (draw-call index processing), `camera ~1.8`, `prior_gpu ~1.0`,
and ~1 ms of capture/composite/flush/swap. Reaching `WORK p99 <= 17.384`
therefore needs a structural change, not another micro-opt:

1. **Fuse clip and pack.** The clipper writes ~18k intermediate
   `vf2_tiles::vertex` (52 B) per frame; the pack phase reads them and writes
   the same count of `batch_vertex`. The profile shows libc ~26% and the pack
   lambda ~24%, consistent with this double pass. Computing the batch vertex
   directly in the cell loop (or writing into per-bucket arenas) removes one
   full pass over ~926 KB.
2. **Cut triangles.** Draw cost is per-index (112 ns/index, 30k indices a
   frame), so fewer cell splits or more fan retention attacks submit. Note the
   release launcher *already* exports `VF2_RETAINED_FANS=1`,
   `VF2_RETAINED_FAN_CELL_BOUNDS=1` and `VF2_GLES_DIRECT_PACK=1`
   (`run-daytona-on-device.sh` lines 18/41/42), so the 20.41/23.09 baseline
   already includes fan retention — re-testing it is a no-op. The remaining
   triangle lever is fewer cell splits, which changes geometry and must be
   qualified against the reference PNG/hash.
3. **GPU mip fetch.** The `VF2_SKIP_UV1` probe cut `prior_gpu` 1.21 -> 0.30:
   the second atlas sample (mip blend) costs ~0.9 ms of GPU. Baking the blend
   into the atlas would recover it.

None of these alone closes the ~6-9 ms gap (packets p99 17.58 already exceeds
the whole 17.384 budget), so acceptance needs several stacked. The memory
ceiling that previously blocked acceptance runs is now handled by
`extfrag_threshold=0` (see the residency section), so these render-path changes
can be measured on full audio races, not just the no-audio checkpoint window.

#### Open question: draw cost vs vertex stride (probe inconclusive)

The `VF2_SKIP_UV1` probe disabled an attribute but kept the 52-byte stride, so
it could not tell "cost ∝ indices" apart from "cost ∝ indices × stride".
`VF2_STRIDE_PAD=N` padded the `glVertexAttribPointer` stride, but the run is
**confounded**: with a padded stride the driver reads garbage attributes, the
GPU does pathological work, and `prior_gpu` rises 0.92 -> 11.97 ms while even
the pure-CPU `packets` phase rises 10.99 -> 12.62 ms (`20261122-101456
-stridepad16-first10`, WORK p50 32.65 vs 20.26). Submit's +0.41 ms sits
inside that global slowdown, so it proves nothing. A valid probe must pad the
struct itself so the attributes stay correct; that change is not worth the
risk while the memory ceiling blocks every acceptance run.




To resolve the shared libraries, download them from the device
(`/run/vf2/lib/libc.so.6`) and run `arm-linux-gnueabihf-nm -D -S -n` in the
container; the dynamic symbol table does not cover every internal function, so
some libc samples resolve to a neighbouring exported symbol.



#### The GPU is a co-bottleneck

`VF2_FLAT_FRAGMENT_PROBE=1` (the diagnostic shader that skips every texture
and palette fetch while keeping packet construction, uploads and draws) drops
the 577-tick window from work p50 20.32 to **17.58 ms** (f201+ 16.69) and
prior-GPU drain from 1.19 to **0.08 ms**. So the fragment shader's dependent
atlas→palette fetch costs ~2.7 ms of GPU time that the CPU waits on; the CPU
floor itself is ~17.5 ms p50 / 21.8 ms p99. Any CPU-only win is partly
absorbed by a larger prior-GPU wait (four pool threads cut packets by 0.4 ms
but raised prior-GPU by 0.6 ms). Both sides must drop for the p99 budget:
the GPU fragment cost and the CPU packet/serial cost.

The atlas is already `GL_LUMINANCE_ALPHA` (2 bytes/texel), so that win is in
place.

#### Simulation and render share one row pool

`model2a`/`model2_video` call `parallel_rows` for scanline work, and the
simulation job is started during the render (pipeline). Both take the row
pool's single `m_run_mu`, so the simulation's `parallel_rows` blocks behind
the render's packet preparation. That is why `sim_ms` can be ~11 ms with
`sim_cpu_ms` ~0.05 ms; it delays the frame interval but is excluded from the
`WORK` metric. A separate simulation pool (or running the sim's pool phases
outside the render's) is the fix if the interval/tick-delivery target needs
it.

#### Packet chunking experiment

`prepare_shared_projected` / the staged packet path now split polygons into
`row_pool::threads()*16` chunks instead of `*4`, and `VF2_DUAL_PACK` claims
32 ranges instead of 8. This targets the packets p95/p99 tail (steady-state
packets p50 ~9.8–10.3 ms but p99 ~13.6–15.6 ms). Measure with
`VF2_PACKET_PHASES=1`.

Re-analyze with `python build/daytona/analyze-job.py <jobdir>`; the analyzer
uses every trace row when the trace begins at loop frame 1.

#### `extfrag_threshold=0` is the lever, and a race can now finish

The failing allocation is unmovable, and kernel 3.4's default
`extfrag_threshold=500` refuses to give unmovable allocations pages from
`MIGRATE_MOVABLE` pageblocks when fragmentation is high. Setting it to 0 lets
the allocator steal them:

```sh
echo 0 > /proc/sys/vm/extfrag_threshold   # default 500; restore afterwards
```

Same race, same staging, audio on, full draw, `VF2_THREADS=3`, atlas 8:

| run | kill frame |
| --- | --- |
| `extfrag_threshold=500` (default) | 2400 |
| `extfrag_threshold=0` | 3600 |
| `extfrag_threshold=0` | 1800 |
| `extfrag_threshold=0` | **finished, exit 0** |

Variance is large, so this improves the odds rather than guaranteeing a
finish, but it is the first lever that moved the wall by more than a few
hundred ticks. Acceptance runs should set it and expect retries.

#### First complete race with audio and full drawing

`build/daytona/jobs/20261122-132540-extfrag0-race4/` — 3700 frames, 75.152 s,
49.23 fps, exit 0, peak RSS 142532 KiB. Clocks held baseline throughout:
CPU0-3 1344000 kHz, DDR 360000 kHz, 59-69 mC. Sound board 68000/173844
cycles/tick (real time 173840), 60.9 ns/cycle.

Audio: 3700 ticks, 534 MIDI bytes, 2836619 generated and 2836619 submitted
frames, **77 underruns**, 0 errors, 0 dropped ticks. Worker: synth 10.595
ms/tick (max 14.579), write 5.672 ms/tick (max 73.281), queue max 6 of 16.

Whole-race WORK (sum of the render phases, excludes `sim_ms`):

| | p50 | p95 | p99 | max |
| --- | ---: | ---: | ---: | ---: |
| WORK | 22.27 | 25.40 | **26.47** | 26.95 |
| packets | 13.96 | 16.79 | 17.58 | 18.22 |
| submit | 4.58 | 5.09 | 5.45 | 6.12 |
| camera | 2.15 | 2.62 | 3.02 | 4.58 |
| capture | 0.42 | 0.65 | 1.24 | 1.36 |
| prior_gpu | 0.09 | 2.52 | 3.13 | 3.82 |

651/651 sampled frames were over the 17.384 ms budget. This is much worse than
the no-sound checkpoint window (p50 20.26 / p99 23.09), and the reason is
visible in the audio line: the detached sound worker spends **10.6 ms/tick of
synth time** and up to 73 ms in a single write, and it shares the machine.
`packets` alone is 13.96 ms p50, which is 80% of the whole budget before
anything else runs.

#### The 84.7 MB anon VMA is the coalesced ROM regions, and it is already stripped

Sampling `/proc/<pid>/maps` at Frames 300, 900 and 1800 gives byte-identical
region sizes and VmRSS 122308 -> 122584 kB: the footprint is a fixed startup
reserve, not growth. One anonymous mapping of **84772 kB** dominates the process.
A `mincore` probe (`model2a::rom_residency_report()`, gated by
`VF2_ROM_RESIDENCY=1`, binary CRC `87b0ced5`) attributes it and measures its real
RAM cost. In one run (Frame 1500, `build/daytona/residency/`):

```
maincpu          b403c008-b423c008    2048 kB  resident     256 kB
main_data        b203b008-b403b008   32768 kB  resident   16352 kB
copro_data       b183a008-b203a008    8192 kB  resident    3632 kB
polygons         b0839008-b1839008   16384 kB  resident   16376 kB
textures         af838008-b0838008   16384 kB  resident    8064 kB
copro_tgp_tables aefb5008-aeff5008     256 kB  resident     252 kB
total virtual 76032 kB, resident 44932 kB
```

All six regions sit inside the single VMA `aef74000-b423d000` (84772 kB),
separated only by 4 kB allocator headers, so the kernel merges them into one
VMA — that is the whole "mystery" mapping. The decisive number is residency:
**76032 kB virtual but only 44932 kB resident.** `release_unused_roms()` already
`MADV_DONTNEED`s the interior zero-page runs, so ~31 MB of zero padding costs no
physical RAM. The earlier guess that the mapping was "nearly fully resident" was
wrong — `/proc/maps` cannot show residency, only `mincore` can.

What is resident is real data the emulator needs during a race: `polygons` is
16376/16384 kB resident (geometry, fully used), `main_data` 16352 kB and
`textures` 8064 kB (the used halves of those regions). There is no region to
drop and no padding left to free. So shrinking this VMA is **not** a viable
physical-RAM lever; the memory wall is best handled by `extfrag_threshold=0`
above, which lets Mali's unmovable order-0 allocations take pages from MOVABLE
pageblocks. Acceptance runs should set it and expect retries.

Reference traces: `build/daytona/residency/` (residency report + same-run maps),
`build/daytona/jobs/20261122-132540-extfrag0-race4/` (first clean race),
`20261122-132012-extfrag0-3700/` (kill at 3600),
`build/daytona/maps-dump/race-maps.txt` (Frame-1800 maps).

#### Two byte-exact physical-RAM levers: mirror fold and shared-page dedup

The "no padding left to free" conclusion above holds only for *zero* padding.
Three other classes of resident bytes are reclaimable, and two are now done, both
**byte-exact** (core `b2a23306453f180c`, geometry `d65ac7ee2f1bb9e0`, captured PNG
`215217f3` all identical to baseline on the first-10s window):

1. **MAIN_DATA mirror fold** (commit `cc7ba71`, binary `9f5e9964`). `load_daytona()`
   copies the 1 MiB block at `0x800000` into seven mirrors (`0x900000..0xF00000`),
   all nonzero. `romset::result` now records the mirror geometry; `model2a::fold_data()`
   maps any read in `[0x800000,0x1000000)` back to the source, and `MEM_CASES` folds
   it so **both** the bus path and the JIT `rpage` table (they share the macro) see the
   source. The `memcpy` stays so the whole-buffer ROM hash is unchanged; `release_unused_roms()`
   then `MADV_DONTNEED`s the seven mirror pages *after* hashing. A/B on the identical
   window: `main_data` resident 16352 -> **9188 kB**, a **7164 kB (~7.0 MiB)** saving.

2. **Shared-page dedup** (commit `8b5dced`, binary `3bda3343`, env `VF2_ROM_DEDUP`).
   `share_region_pages()` folds exact-duplicate and all-`0xff` 4 KiB pages of a
   read-only region onto one shared frame via a file-backed `MAP_SHARED|MAP_FIXED`
   mapping (kernel 3.4 has no `memfd_create` and the device has no `/dev/shm`, so the
   backing is a tmpfs file in the emulator's `/run` home: create, write one frame per
   group, `mmap`, then `unlink`+`close` — the mappings keep the inode alive). Unlike
   `MADV_DONTNEED` (zero pages only), this preserves `0xff` and arbitrary duplicate
   content, and `MAP_FIXED` keeps every virtual address — so direct ROM pointers and
   the JIT table are untouched. Pages are grouped by 64-bit hash then `memcmp`-confirmed
   (a collision can never fold two different pages); the canonical is folded too, else a
   pair costs one anon + one shared frame (no saving). `PROT_READ` makes any stray write
   fault loudly; the clean `exit 0` also proves `free()`-at-exit over a vector with
   `MAP_FIXED` holes is safe. A/B MemFree on the identical window:

   | run | folded | MemFree delta vs no-dedup |
   | --- | ---: | ---: |
   | `VF2_ROM_DEDUP=1` (polygons) | 3235 pages (12.6 MiB) | **+9.5 MiB** |
   | `VF2_ROM_DEDUP=1,tex` (+textures) | 1255 pages (5.0 MiB) | **+2.6 MiB** |

   Together with the mirror fold that is **~19 MiB** of physical RAM freed, byte-exact.
   copro_data (0.93 MiB dup) is left out: it may be written at runtime, so `PROT_READ`
   would fault — test separately before enabling.

**Measurement caveat:** `mincore` counts a shared frame once *per virtual mapping*, so
it still reports polygons 16380 kB resident after dedup and shows **no** saving. The
physical saving only shows in **MemFree** (now printed by `rom_residency_report()`);
`MemAvailable` is absent on kernel 3.4 so it reads 0. Compare MemFree between a dedup
and a no-dedup run at the same frame, same binary.

**Harness gotcha:** a run's restore step launches the interactive release to verify it;
that game streams stdout onto the single-client USB RPC and desyncs the length-prefixed
framing for the *next* `run-job.py` (symptom: `run.log` line 99 `actual.split()[0]`
IndexError, spurious "uploading ROM"). Kill the game (`/proc/*/cmdline` match on
`daytona-acm`) and open a fresh `Client` before the next job; a fresh connection reframes
cleanly.

#### Full Beginner race qualifies with the levers at default kernel settings

Job `build/daytona/jobs/20261122-150039-dedup-race-beg/` ran the full Beginner replay
(`race.script`, 3700 frames, full draw, speaker audio, `VF2_THREADS=3`, atlas 8) with the
dedup binary `3bda3343` and `VF2_ROM_DEDUP=1,tex`, at **default kernel settings**
(`extfrag_threshold=500`, not the `extfrag=0` the no-lever race needed):

- **exit 0**; core `881d7132fbbe46b5`, geometry `04eab914fed79013` — **identical to the
  no-lever race4**, so the levers are byte-exact on a full race, not just the 10s window.
- **MemFree 50.5 MB** at the end; peak RSS 137124 KiB (RSS is unchanged by design — shared
  frames count per virtual mapping; MemFree is the real headroom). The ~19 MiB freed is
  what lets the race finish without `extfrag=0`.
- Audio: **90 underruns, 0 dropped ticks, 0 errors** (race4 had 77; worker-side variance).
- Clocks steady 1344 MHz / DDR 360 MHz / 59 mC.
- **Timing-neutral**: work-sum p50/p95/p99 16.85/25.42/27.17 vs race4 16.94/25.33/26.96;
  interval p50 20.94 vs 20.58; late 2350/3399 vs 2342/3399; resynced 114 vs 99. The memory
  levers do not move the p99 gap (they change physical backing, not CPU work).

Command (via `run-job.py`): `--frames 3700 --full-draw --atlas-mb 8 --binary
build/daytona/daytona-dedup-acm --env VF2_ROM_DEDUP=1,tex --env VF2_ROM_DEDUP_DIR=/run/daytona/trial
--env VF2_ROM_RESIDENCY=1 --env VF2_THREADS=3`. Analysis: `race-stats.py` (interval from
`swap_end_ns` deltas, work-sum from the serial render stages, warmup 300 excluded).

The p99 gap (~9 ms, 69% of frames over budget) is unchanged and is the target of the next
(structural) experiment. The other two courses have no replay yet: `race.script` cold-boots
straight into Three Seven Speedway (Beginner) with no course-select navigation, so the
Intermediate/Expert replays need the course-select flow worked out first.

#### Course-select flow (findings; menu scripting parked for now)

Probing the boot menu with short captures (`select-probe400`, `steer-test`, `steer-r`,
`steer-r6`, `int-probe430/700`) established the two-screen flow after `coin1`→`start1`:

1. **Circuit Select** — three courses stacked vertically: 初級 Beginner (SHORT×8, top),
   中級 Intermediate (MEDIUM×4, middle), 上級 Expert (LONG×2, bottom). "SELECT COURSE BY
   STEER / STEP TO CHOOSE". `steerright` moves the highlight **down** the list; `steerleft`
   at the top (Beginner) does nothing. Steering is **continuous/hold-based**, not
   one-step-per-tap: a 12-frame `steerright` hold crossed two steps (landed Expert), a
   6-frame hold landed Intermediate. So the intended selection is `press steerright 6`
   (Intermediate) or `press steerright 12` (Expert) at ~frame 400, before the confirm.
2. **Mission / Car Select** — BEGINNER Automatic (max 315 km/h) vs PROFESSIONAL Manual-4
   (max 325 km/h), "STEP TO START". This is a **second** confirm screen the Beginner script
   was silently passing through: its two accelerator presses are 420 = confirm circuit,
   510 = confirm car (Automatic), then 690 = gas.

**Unresolved (parked):** inserting `press steerright 6` at frame 400 before the 420 confirm
did **not** select Intermediate in a full race — the run raced Beginner (geometry hash
`04eab914fed79013` identical to Beginner, HUD showed LAP 1/8), and at frame 700 the
intermediate script was still on Mission/Car Select showing the Beginner circuit preview.
So the confirm sequence is shifted relative to the steer: the steer at 400 either does not
persist to the 420 confirm, or the confirm timing is fragile (the first accelerator after
the steer appears to be absorbed). The scripts `build/daytona/scripts/daytona-race-
{intermediate,expert}.txt` exist and select the highlight correctly in isolation (verified
by frame-415 capture) but need the confirm timing reworked. Parked in favour of the
structural clip→pack experiment, since the Beginner replay already qualifies the memory
work and the p99 gap is the real target.

**Harness gotcha (worse variant of the RPC desync):** when a wedged interactive game is
streaming onto the single-client RPC, a fresh `Client()` can claim the interface but its
first `write()` times out (`USBTimeoutError`), because the server is blocked writing game
stdout to the IN endpoint and is not reading OUT. Recovery: open a `Client`, drain the IN
endpoint with short-timeout `dev.read(inp, ...)` calls until the server unblocks, then send
the kill. `build/daytona/recover-rpc.py` does this. If the game is already dead but the
server stays wedged (drain returns 0 bytes and writes still time out), the fix is a fresh
client that **drains first, then writes** — `build/daytona/recover-rpc2.py` loops fresh
clients doing exactly that and recovers it. **Root cause is `run-job.py`'s restore step**
(line 197) relaunching the interactive game with no `--frames` limit; run experiment jobs
with `--no-restore` and do one final restore+verify at the end.

#### Measured packet-phase split on the first-10s window (drives the structural lever)

Job `build/daytona/jobs/20261122-153436-base-phases/` — frame-2998 checkpoint resumed to
3575 (577 frames), full draw, atlas 8, `VF2_THREADS=3`, dedup binary `3bda3343`, with
`VF2_PACKET_PHASES=1 VF2_CLIP_STATS=1 VF2_GLES_STATS=1`. `packet_phases = {rams, clip,
plan, pack}` (clip = `prepare_shared_projected` + palettes; plan = bucket offsets; pack =
`pack_job`). Absolute ms are **inflated by the `VF2_CLIP_STATS` per-cell timers** (the
uninstrumented full-race `packets_ms` p50 was 8.39 ms), so use them for *relative*
attribution only:

- **clip p50 12.54 / p99 15.79 / mean 12.15** — ~73% of packet CPU, the dominant cost.
- plan p50 1.20 / mean 1.50; pack p50 2.79 / mean 3.05.
- Clip internals (summed across worker threads): **cells 25.2, cclip 9.7, ctri 5.9,
  ccopy 3.9, copy 1.8, outer 1.6**. Counters: calls 684864, cells 2916165, ordinary
  2738236, inside-skip 661809. **Retained fans 147090 vs 1053074 draw jobs (~14%)**.

So the common polygon path is the **cell-expansion path** (`clip_indexed_band` → ordinary
vertices into `r.storage` → pack copies to `batch_vertices`), not the retained-fan path.
The clip cost is cell geometry (split/clip/emit), not the fan math.

**Why a literal clip→pack fuse cannot remove the dominant cost.** Pack needs two things that
only exist after a full clip pass: (a) the atlas page transform from `get_pair` (resolved in
the serial plan, because page allocation is not thread-safe), and (b) the bucket output
offset, which needs every bucket's total clipped size. To emit into `batch_vertices` during
clip you would need sizes first, i.e. a count-only clip pass — but the expensive part *is*
the cell geometry, so a count pass costs about as much as the real clip and the fuse is a
wash or worse. The retained-fan path is already single-pass (clip stores a fan reference,
pack expands once). Conclusion: the clip→pack boundary is not where the 12 ms is; the cell
generation inside clip is. This redirects the first structural lever from "fuse clip→pack"
to **reducing cell-generation cost** (the previously-deferred cell-split lever), which the
objective's "test the largest measured bottleneck first" now points at directly.

#### First cell-generation experiment: hoist the loop-invariant cell multiply (byte-exact, ~2%)

The cell-plane clip distance `double(v.u)-l*p.texw*v.q` recomputes `l*p.texw` (constant
within a cell) for every vertex; Cortex-A7 does double in software, so that is a wasted
software-double multiply per vertex. `VF2_CELL_CLIP_HOIST` hoists it (`lu=l*p.texw`, etc.)
with identical double association, so it is byte-exact. Binary `build/daytona/daytona-hoist-acm`
(sha256 `F2459FC0…`), A/B on the frame-2998→3575 window, `VF2_PACKET_PHASES` only (no
`clip_stats` inflation), matched clocks 1344 MHz/DDR 360, same binary/launcher:

- **Byte-exact**: core `b2a23306453f180c`, geometry `d65ac7ee2f1bb9e0` identical off vs on
  (and equal to the known-good window hashes).
- clip p50 6.36→6.21, mean 6.35→6.23 (~2%); clip+plan+pack p50 11.04→10.91. Consistent
  direction on every percentile but the ~0.13 ms delta is near run-to-run noise.
- Note: the **uninstrumented** clip is ~6.4 ms (the base-phases 12.5 ms was inflated ~2× by
  the per-cell `clip_stats` timers). Real packet CPU ≈ clip 6.4 + pack 3.0 + plan 1.4 ≈ 11 ms.

Conclusion: the hoist is a safe, byte-exact micro-win worth keeping (flag-gated, default off),
but the cell loop is dominated by the Sutherland-Hodgman clip itself (software-double
distance + interpolation), not the hoistable multiply. The next escalation is a **hierarchical
column clip**: clip each column's u-band once and reuse it across the rows in that column
(`clip(clip(poly,u_cx),v_cy)` is order-independent for a rectangle, so byte-exact), removing
the redundant per-row u-clips — a larger cut to the dominant `cclip` cost.

#### Second cell-generation experiment: hierarchical column clip (byte-exact, ~4% combined)

`VF2_CELL_COL_CLIP` precomputes each column's u-band clip once and reuses it across that
column's rows. Rectangle clipping is order-independent, and the per-cell order stays
u-then-v with the same (cy,cx) emission order, so output is byte-identical; only the
redundant per-row u-clips are removed. Guarded `if constexpr(Stack)` (the `Stack=false`
instantiation at gpu3d_gles2.cpp:883 keeps the original `vector_polygon` path untouched),
bounded to grids ≤8 columns and columns ≤32 points (falls back to the in-loop path
otherwise). Added uniform `data()`/`assign()` to both polygon types so the `cell` type
matches `polygon` exactly. Binary `build/daytona/daytona-colclip-acm` (sha256 `B34B72B7…`),
A/B on the frame-2998→3575 window, `VF2_PACKET_PHASES` only, matched clocks 1344 MHz/DDR 360:

| config | clip p50 | clip mean | clip p99 | clip+plan+pack p50 |
| --- | --- | --- | --- | --- |
| col off (baseline) | 6.42 | 6.43 | 8.98 | 11.07 |
| col on | 6.18 | 6.20 | 8.74 | 10.91 |
| col + hoist | 6.15 | 6.14 | 8.79 | 10.84 |

**Byte-exact**: core `b2a23306453f180c`, geometry `d65ac7ee2f1bb9e0` identical across all
three (and equal to the known-good window hashes). The column clip removes ~0.24 ms of clip
mean; the hoist adds ~0.03 ms on top (col already hoists the u multiply internally, so the
hoist now only helps the v clips). Combined ~4.2% of clip, ~2.5% of packet CPU.

**Strategic read (two byte-exact cell-clip levers now measured).** The cell-clip micro-opts
together shave ~0.28 ms off a ~6.4 ms clip stage — real and free (byte-exact, flag-gated,
default off, worth keeping), but they cannot close the ~9 ms p99 gap. The tail analysis
(`build/daytona/tail-analysis.py` on the qualified Beginner race) shows the gap is not clip
*mean*: full-race work p50 is 16.85 ms (already under the 17.384 ms budget) but p99 is
27.17 ms, and the tail is driven by **packets_ms scaling** (median 7.0 → tail mean 18.5 →
max 63.4 ms) plus rare **swap stalls** (max 144 ms) and **capture spikes** (max 22.7 ms).
A proportional ~4% clip cut removes ~0.7 ms from an 18 ms tail frame — directionally right
but an order of magnitude too small. The next real lever must reduce the *cell explosion in
busy frames* (fewer/smaller cell splits — the deferred cell-split lever) or address the
swap/capture stalls, not the per-cell constant.

#### Native-repeat A/B: hardware GL_REPEAT on the top opaque track material (byte-exact at busy frames, −0.9 ms p99)

The native-repeat path (existing, gated to one hardcoded VF2 material) collapses a polygon's
per-cell repeat splits into ONE quad and lets the GPU's GL_REPEAT sampler do the wrap, using a
standalone square power-of-two repeat page (gles2_atlas.h:260) and the transform's centre rebase
(`-floor(centre)`, gles2_tiles.h:302) to keep the wrapped UV near the origin for mediump
sub-texel precision. The split-suppression is `if constexpr(Indexed)`-only, so it needs the
indexed path the race launcher already runs (`VF2_GLES_INDEXED=1`).

Ranking (per-material `VF2_CLIP_STATS` probe, single-threaded Beginner race,
`build/daytona/daytona-matrank-acm`): the top opaque wrap-eligible material by split cells is
atlas **(832,384) 64×64 rend2 wx1 wy1 mx0 my0**, ~600 cells/frame out of ~6400 total (~10%).
A focused band probe (`tguard/tpred/bands`) showed all its indexed splits are at **band 5** (the
2×2 mip pair), which the VF2 route's `band<=4` cap excluded — that was the sole reason the first
predicate never fired (`tguard 15 tpred 0 bands [5:15]`). Widening the track route to band −1..5
made it collapse (`tguard 6 tpred 6 fire 6`). Opt-in flag `VF2_NATIVE_REPEAT_TRACK=1`
(branch-local; the VF2 material predicate is byte-identical). Binary `build/daytona/daytona-natrep-acm`.

Matched 3000-frame Beginner race, cold boot, speaker audio, threads=3, clocks 1344/360
(`natrep-ab-off` vs `natrep-ab-on`):

| metric | track OFF | track ON | Δ |
| --- | --- | --- | --- |
| work p50 / p95 / p99 | 13.96 / 25.51 / 27.55 | 13.97 / 24.62 / **26.63** | p99 −0.92 ms |
| interval p50 / p95 / p99 | 17.93 / 30.31 / 33.23 | 17.87 / 29.58 / **32.26** | p99 −0.97 ms |
| late frames | 32.3% | 30.9% | −1.4 pp |
| on-time presented | 39.30% | 40.33% | +1.0 pp |
| audio underruns | 60 | 54 | −6 |

**Image holds.** Busy-window captures (`--capture-frames`, `natrep-cap-off`/`natrep-cap-on`) at
2421/2434/2447/2460/2473/2477 are **byte-identical (0 px differ)** — far better than
`VF2_CELL_MERGE` (2–5% softening at these same frames). Final frame 3000 differs by only 39 px
(0.010%) in a thin strip `x[261..270]` — the expected mediump rounding on one wide wrapped span.
Baseline-config runs are byte-identical (determinism control), so the delta is entirely native-repeat.

Decision: both gates met (timing improves, image holds). Next: broaden coverage to the other
eligible square-POT opaque wrap-no-mirror materials ((384,384) 128×128, (576,960) 64×64,
(704,128) 64×64, (704,608) 32×32, (1408,448) 64×64; NOT (1024,0)/(960,256) 64×256 — non-square,
the repeat page needs w==h). A general predicate must itself enforce square-POT or a non-square
match would fail `get_pair` and DROP the draw.

**Broad coverage REJECTED on image.** `VF2_NATIVE_REPEAT_ALL` (any opaque square-POT
wrap-no-mirror material, band-capped to tile>=2x2) gave a bigger timing win — matched 3000-frame
Beginner race `natrep-all-off` vs `natrep-all-on`: work p99 27.47→**24.78** (−2.69 ms), p95
25.60→23.35, late frames 32.7%→28.8%, underruns 56→46 (interval p99 31.83→32.39, within noise).
But the busy-window captures (`natrep-cap-off` baseline vs `natrep-all-cap-on`) differ by
**6.8–12.2% of pixels** with a whole-screen bbox (y[80..719] x[0..475], mean |delta| ~1, max ~240)
at 2421/2434/2447/2460/2473/2477 — a real visual regression, not isolated rounding. The broad
route catches non-road wrapping materials (and/or wide spans that break mediump) that the single
(832,384) route never touched. Decision: keep the narrow (832,384) route; do not enable ALL.

**cap64 (broad minus the large material) — REJECTED on image, marginal on timing.** The whole-screen
bbox of the full-broad regression pointed at the large 128×128 material (2nd-most-split, 379 cells),
not the byte-exact 64×64 road. `VF2_NATIVE_REPEAT_CAP64` restricts the broad route to ≤64×64 tiles
(excludes the 128×128). Matched 3000-frame Beginner race `natrep-cap64-timing`: work p99 25.94
(vs 27.47 off, −1.53 ms; but only −0.69 ms better than the narrow route's 26.63). Busy-window
captures (`natrep-cap64-on` vs `natrep-off` baseline) differ by **0.12–0.20% of pixels** (~450–750
px/frame, max per-pixel |delta| up to 226) — a residual from the other ≤64 materials cap64 keeps
beyond the road. That max Δ226 is the same class of fidelity regression that got `VF2_CELL_MERGE`
rejected, and it buys only ~0.7 ms over the byte-exact narrow route. Decision: keep the narrow
(832,384) route as the accepted win (byte-exact at busy frames, −0.92 ms); do not enable cap64.

**Stage breakdown confirms packets_ms is the bottleneck.** `build/daytona/stage-breakdown.py` on the
clean baseline (`natrep-ab-off`, worst 27 frames): packets_ms is **52.7% of the p99 tail** (tail mean
19.19 ms, p99 18.44 ms) — its p99 alone exceeds the whole 17.384 budget. swap_ms tailMax 148 ms is
the single final-frame teardown stall (frame 3000), not sustained. So closing the gap requires cutting
cell-generation CPU; native-repeat is the byte-exact way but only reaches ~10% of cells (the road).
The larger win needs more materials to be byte-exact, which the mediump precision limit blocks.

**Clean post-native-repeat packet-phase split — clip still dominates the tail, but copy/init is not the removable cost.**
`VF2_PACKET_PHASES=1` alone (no `CLIP_STATS` inflation), committed narrow route (`VF2_NATIVE_REPEAT_TRACK=1`,
threads=3, matched 1344/360, full race, job `20261122-204533-natrep-phases`, 2548 frames):

| phase | p50 | p95 | p99 | max | worst-27 mean |
| --- | --- | --- | --- | --- | --- |
| clip | 5.19 | 9.88 | 11.18 | 15.03 | 11.16 |
| plan | 0.88 | 2.32 | 3.13 | 36.12 | 4.65 |
| pack | 1.74 | 5.14 | 5.74 | 7.83 | 4.81 |
| total | 7.63 | 16.48 | 17.91 | — | ~20.6 |

So clip+emit is ~16 of the ~20.6 ms worst-frame packet work (the directive's "roughly half the worst-frame
work" holds). But the **per-cell point-copy / temporary-storage candidate is ruled out**: `ccopy` is ~15% of the
cell loop, the compact `ordinary_vertex` path already covers ~93% of cells (5356/5746), and the per-cell
`stack_polygon` scratch is two empty `std::vector`s (no allocation, ~6 pointer stores per cell). The dominant
`cclip` is the Sutherland-Hodgman clip itself, already shown not soft-float (float-clip neutral) and not
core-scaling (t4==t3). A per-cell scratch-reuse / init-reduction is bounded below ~0.1 ms — below noise, not
worth a device run. **New signal: `plan` is a serial stall (max 36 ms, worst-frame mean 4.65 ms)** — the atlas
page allocation (`get_pair`) is not thread-safe and does not parallelize, which is consistent with the t4==t3
null. Next clip-loop experiment: re-test the committed byte-exact col_clip+hoist on top of the narrow route
(the road collapse changed the cell mix since they were measured), and separately probe the plan-phase stall.

**col_clip+hoist re-tested on the post-collapse cell mix — real byte-exact win, kept opt-in.**
Matched 3000-frame Beginner race, `VF2_PACKET_PHASES` only, threads=3, matched 1344/360, narrow route
(`natrep-phases` off vs `natrep-phases-colhoist` on):

| metric | track only | +col_clip+hoist | Δ |
| --- | --- | --- | --- |
| clip p50 / p95 / p99 | 5.19 / 9.88 / 11.18 | 4.88 / 9.27 / 10.22 | p99 −0.96 ms |
| work p50 / p95 / p99 | 14.12 / 24.74 / 26.50 | 14.00 / 24.44 / 26.13 | p99 −0.37 ms |
| on-time presented | 39.52% | 40.15% | +0.63 pp |
| audio underruns | 52 | 51 | −1 |

Every clip percentile moved down (consistent direction, above clip-stage noise); the road collapse shifted the
cell mix so the column-reuse helps more than the pre-natrep ~4%. Byte-exact (unchanged committed code, geometry
hash identical upstream). **Decision: keep opt-in** — these flags gate the *shared VF2 runtime*, and the work-p99
delta is within run-to-run noise on one matched pair, so flipping the default is not warranted without a
multi-run qualification. The Daytona launcher can export them to capture the ~0.4 ms without touching VF2.
**Next: probe the serial `plan` stall** (worst-frame mean 4.65 ms, max 36 ms) — the largest non-parallelizable
section and the most likely remaining tail lever.

**Atlas-size probe: the plan stall is the serial `get_pair` loop, not eviction — not a lever.**
The earlier phases ran `--atlas-mb 8`; the release launcher uses 16 MB. Matched 16 MB probe
(`natrep-phases-16mb`, narrow route, threads=3, 1344/360): plan p50 0.87 / p99 3.16 (max 23.1 vs 36.1 at
8 MB), clip p50 5.24 / p99 11.43, pack p50 1.69 / p99 5.55; worst-27 means clip 12.01 / plan 3.52 / pack 4.51;
work p99 27.27 (vs 26.50 at 8 MB — within noise). The bigger atlas only shrank the rare cold-decode outlier;
the sustained plan cost is the serial per-draw `get_pair` page-allocation loop (gles2_atlas.h warm path), which
does not parallelize (consistent with t4==t3). **Atlas size is not a lever.** Clip stays 2.7× the next phase.
This closes the last untested packet sub-phase. Every byte-exact lever is now measured and exhausted:
cell-count (native-repeat narrow committed; broad/cap64 rejected on image), copy/init (ruled out), float-clip
(neutral), core-scaling (t4==t3), vertex-shrink (dead end — material varies within a page-keyed bucket),
clip→pack fuse (refuted — needs sizes first), atlas size (this). Architecture floors ~23 ms p99 with the
image-breaking merge, ~26 ms byte-exact. **Reaching 17.384 ms needs an acceptance-bar decision** (fidelity
tradeoff that still misses, GPU-geometry rework past the mediump limit, or clock change) — surfaced to the owner.

**Architecture floor with all cell-count levers stacked — p99 floors at ~23 ms, not 17.4.** Running
merge + native-repeat + col-clip + hoist together (`natrep-floor`, matched 3000-frame Beginner race):
work p50/p95/p99 **11.69 / 21.91 / 23.17**, interval p99 29.97, late frames 17.8%, on-time 47.5%,
underruns 27. So even the image-breaking merge plus every byte-exact lever only removes ~4.4 ms from
the 27.5 ms baseline p99 — it lands at 23.17 ms, still ~5.8 ms over budget, and on-time is 47.5% vs
the 99% bar. **Conclusion: cell-count reduction alone cannot reach acceptance.** The residual p99 is
the per-cell Sutherland-Hodgman clip cost itself (the largest remaining bottleneck, per the stage
breakdown: packets_ms = 52.7% of tail). The next lever must cut per-cell clip arithmetic (float vs
software-double distance/interpolation, or SIMD), not cell count. That is a precision-sensitive change
to the geometry core and needs byte-exactness review before it can be trusted.

**Floor stage-breakdown: the wall is the clip loop, and it is neither core- nor soft-float-bound.**
`stage-breakdown.py` on `natrep-floor` (worst 27 frames): packets_ms is still **49.4% of the tail**
(tail mean 15.80 ms, p99 15.43). swap_ms tailMax 148.82 is the frame-3000 teardown artifact (its p99
is 0.24 ms), not a sustained cost. Combined with two prior nulls — `VF2_CELL_CLIP_FLOAT`/`VF2_CLIP_FLOAT_ALL`
neutral (so not the software-double distance math) and `VF2_THREADS=4` no better than 3 (so not
core-scaling) — the residual clip cost is consistent with **memory bandwidth** (per-cell `cell.assign`
copy of point structs + triangle emission at 360 MHz DDR) or a serial section, not arithmetic. A
render-thread profile (VF2_CPU_SAMPLES_RENDER) is needed to confirm which. **Net: at stock clocks the
current software-clip architecture floors at ~23 ms p99 even with the image-breaking merge, and ~26 ms
byte-exact. The 17.384 ms acceptance bar is not reachable by cell-count reduction alone.** Reaching it
needs either (a) a fidelity tradeoff that still misses the bar (merge → 23 ms), (b) a GPU-geometry
rework (extend the native-repeat idea so the GPU does the per-cell transform — the mediump precision
limit blocked the broad version), or (c) raising CPU/DDR clocks (the baseline held them at stock). This
is a fidelity/acceptance-bar decision for the acceptance owner.

#### Tail characterisation: the p99 gap is sustained busy-scene packets, not stalls

`build/daytona/outlier-analysis.py` on the qualified Beginner race
(`20261122-150039-dedup-race-beg/run.csv`, 3400 in-race frames):

- **swap_ms 143.87 is frame 3699 — the final frame of the run** (teardown/exit), not an
  in-race hitch. The earlier "144 ms swap stall" is an artifact of the run ending; ignore it.
- **capture_ms >3 ms: 19 frames**, clustered early (361–364, 578–590, 826, 876–877,
  1060–1096) and co-elevated with `tile_upload_ms` (7.0, 6.3 ms) — first-view texture
  uploads, partly warm-up. Not the p99 driver.
- **packets_ms >15 ms: 542 frames (16% of the race), and they CLUSTER** in sustained runs
  (consecutive-frame gaps of 1: 353–359, then a long run 2421–2477+). The p99 tail is
  **sustained busy-scene geometry**, not isolated stalls. The single worst in-race frame
  (2693: packets 63.5, submit 17.0, work 83.8) is a maximum-geometry moment.

So the ~9 ms p99 gap is clip/cell-count scaling in busy stretches. The two byte-exact cell
micro-opts (~4% of clip) remove ~0.7 ms from an 18 ms busy frame — right direction, ~10×
too small. The only lever large enough is **reducing the cell count itself** (the deferred
cell-split lever): fewer texture cells per large polygon in busy frames. That changes the
emitted geometry (vertex count, per-cell mip/filter selection), so it **cannot be
byte-exact** — it trades texture fidelity for speed and needs visual + geometry-diff
review, not a hash match. This is the one remaining lever that can move p99 toward
17.384 ms, and it is a fidelity tradeoff the acceptance owner should sign off on.

#### Cell-split merge experiment (`VF2_CELL_MERGE`) — big speed win, subtle fidelity cost

Authorized as a branch-local, opt-in diagnostic (core state must stay identical; geometry
hash may differ; bring the tradeoff back before changing defaults). `VF2_CELL_MERGE` drops
the `force_cells` term on axes where the texture wraps and does not mirror, so adjacent
cells that share one tile merge into a single clip+emit (the reference non-indexed path
already does not split those axes). Default off; the flag-off path is byte-identical to the
qualified release (verified: merge-off reproduces core `881d7132fbbe46b5` / geometry
`04eab914fed79013`). Binary `build/daytona/daytona-merge-acm` (sha256 `0DC7BB74…`).

Matched full Beginner races, cold boot, full draw + speaker audio, dedup levers, atlas 8,
`VF2_THREADS=3` (jobs `20261122-162900-merge-off` / `20261122-163029-merge-on`):

| metric | merge off | merge on | delta |
| --- | ---: | ---: | ---: |
| work p50 | 16.86 | 14.89 | −1.97 |
| **work p99** | 27.22 | **23.91** | **−3.31** |
| packets p50 | 8.70 | 6.60 | −2.10 |
| **packets p99** | 18.17 | **15.54** | **−2.63** |
| submit p99 | 5.24 | 4.76 | −0.48 |
| late frames (>17.384) | 1586/3400 | 1181/3400 | −405 |
| audio underruns | 85 | 59 | −26 |

Cell counters (`VF2_CLIP_STATS`, jobs `…-mergeoff-cnt` / `…-mergeon-cnt`): **cells
11.06M→7.28M (−34%)**, ordinary vertices 10.38M→6.11M (−41%). That is the mechanism for the
~2 ms packets drop.

**Core state identical** (`881d7132fbbe46b5`) — gameplay/emulation unaffected. The geometry
hash is also identical (`04eab914fed79013`) only because it is computed upstream of cell
splitting, so it cannot see this change; the pixel capture is the real check.

**Visual difference (end frame, `png-diff.py`):** 7432/384000 px differ = **1.94%**, all in
the 3D viewport (bbox y80–438 x76–314); 919 px differ by >60 and 92 by >128 (sum-RGB). The
`png-highlight.py` overlay shows the differences as **speckled patches in the crowd/stands
texture and scattered road pixels — not straight lines at cell boundaries (no geometric
seams), no missing geometry or holes**. The scene is recognizably identical (same cars,
road, HUD). The cause is coarser per-region mip/filter selection: the split path picks a mip
per cell from that cell's depth, while a merged span uses one centre-based transform, so
texels far from the span centre sample a slightly different mip.

**Decision (per the authorization): bring the tradeoff back, do not change defaults.** The
speed gain is the first lever with the right magnitude (−3.3 ms work p99, −405 late frames),
but it is not free: it trades per-cell mip/filter locality for speed, visible as subtle
texture softening in detailed repeating regions. It does not by itself reach 17.384 ms
(23.91 ms p99 remains ~6.5 ms over), so it cannot satisfy the full-render goal as-is. The
flag stays default-off. Next options to evaluate before any default change: (a) restrict the
merge to regions where the mip is provably flat across the span (keeps fidelity, less gain);
(b) combine with the byte-exact clip levers; (c) accept the fidelity change as a documented
Daytona-only rendering mode if the user decides the speed is worth it.

#### Fidelity-safe bounded merge (`VF2_CELL_MERGE_SAFE=N`) — dead end, pixels still change

Authorized follow-up (option A): merge only spans where the reference would pick the same
mip/filter and sampling stays equivalent; start with one common case; if it saves little or
pixels change, record and move on. Implemented as `VF2_CELL_MERGE_SAFE=N` (default off,
independent of the full merge): it un-splits a wrapping non-mirror axis only when that axis's
span is at most N cells, on the theory that a small merged span keeps the interpolated texture
coordinate inside the shader's exact 16.8 fixed-point range. Binary `daytona-safemerge-acm`
(sha256 `CCFB723C…`). Added a reversible `--capture-frames N,M,...` host option (writes
`capture.NNNNNN.png` per frame, forces a draw on those ticks) so exact pixels can be compared
at specific busy frames; default path untouched.

Matched full Beginner races, cold boot, full draw + speaker audio, busy interval included
(jobs `20261122-165932-safebase` / `20261122-170057-safe2`, N=2):

| metric | base | safe N=2 | delta |
| --- | ---: | ---: | ---: |
| work p50 | 16.90 | 16.30 | −0.60 |
| work p99 | 27.72 | 27.00 | −0.72 |
| packets p50 | 9.06 | 8.00 | −1.06 |
| packets p99 | 18.34 | 17.96 | −0.38 |
| late frames (>17.384) | 1591/3400 | 1522/3400 | −69 |

Cell coverage (`VF2_CLIP_STATS`, jobs `…-safebasecnt` / `…-safe2cnt`): cells 11.06M→9.92M
(−10.3%), ordinary vertices 10.38M→7.29M (−29.8%). Core state identical (`881d7132fbbe46b5`).

**Pixels change — visibly.** Exact-pixel A/B at busy frames 2421/2434/2447/2460/2473/2477
(`--capture-frames`, `png-diff.py`): frame 2421 2.0% differ, frame 2477 4.9%, frame 2447
**30.2% differ, mean per-pixel delta 50, max 695**. The frames show a clear **checkerboard
aliasing artifact smeared across the road and wall surfaces** in the merged run (baseline road
is smooth). This is a real defect, not subtle softening.

**Root cause.** The split path's integer cell indices map each cell to an atlas tile exactly;
the merged span falls back to the float `transform`, whose larger interpolated texture
coordinate loses sub-texel precision in the fragment shader's `trunczero(vQ.y*z*256)` fixed
point. The merged transform therefore is not pixel-equivalent to the per-cell split even at
span 2. There is no span>1 that is both pixel-exact and reduces cells: **byte-exactness
requires the split.** The fidelity-safe merge is a dead end.

**Consequence for the budget.** Phase breakdown of the baseline (p99 work 27.72 ms): packets
p99 18.34 ms (66% of work), submit 5.34 ms, prior_gpu (GPU-wait) 4.92 ms, camera 2.94 ms.
The packet/clip stage alone exceeds the 17.384 ms budget at p99. The only lever large enough
to cut it (cell-split merge) changes pixels; the byte-exact clip levers (hoist + column clip)
total ~4% (~0.28 ms). The render path cannot reach 17.384 ms p99 without a fidelity tradeoff,
and even the full merge only reaches 23.91 ms. Both merge flags stay default-off.

### Per-course on-device baselines — committed byte-exact narrow route

Matched 3000-frame cold-boot races, full draw + speaker audio, dedup+residency levers,
`VF2_NATIVE_REPEAT_TRACK=1`, `VF2_THREADS=3`, release 16 MB atlas, 1344/360. Beginner is the
`natrep-phases` run; Intermediate/Expert are `inter-base`/`expert-base`.

| course | work p50 | p95 | p99 | worst | late frames | on-time | underruns |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Beginner | 14.12 | 24.74 | 26.50 | 170.65 | 853/2700 | 39.5% | 52 |
| Intermediate | 18.59 | 20.56 | 22.22 | 165.88 | 1451/2700 | 24.9% | 30 |
| Expert | 16.02 | 18.91 | 20.12 | 161.50 | 499/2700 | 47.3% | 10 |

**No course meets the 17.384 ms bar.** But the ordering is NOT monotonic with difficulty, and
that is a measurement defect, not a finding: the Intermediate/Expert replays select the correct
course (verified by HUD lap total + geometry hash) but their **race-line steering is
Beginner-tuned**, so the car leaves the racing line after the first corner and never drives the
dense pack. Expert's low p99 (20.12) and 10 underruns reflect a light off-line scene, not a fast
Expert race; Intermediate's high p50 (18.59) with the lowest on-time (24.9%) is the car grinding
off-track. **These are not valid per-course worst cases.** Honest per-course acceptance needs
per-course race-line tuning (steer/brake events that hold the Expert/Intermediate line through
the field) — a concrete prerequisite before the three-races-per-course acceptance protocol means
anything. Beginner remains the only representative busy-pack measurement.

### Circuit-select replays — solved (verified by in-race frame + geometry hash)

Earlier attempts (int2/int3/exp2) all secretly raced Beginner: their final PNGs were
byte-identical to the Beginner baseline and their geometry hash was `04eab914fed79013`.
The mistake was trusting the pre-confirm highlight capture. A course counts ONLY if the
IN-RACE frame's HUD lap total and geometry hash match it.

Root cause of the failures (the user's insight): the Circuit Select cursor tracks the
STEERING WHEEL POSITION CONTINUOUSLY, and wheel-center = 初級 (Beginner). Releasing the
steer snaps the cursor back to Beginner. So the accelerator confirm must land WHILE the
wheel is held at the target course — a release-then-confirm gap reverts to Beginner. The
menu also auto-starts the highlighted course on a slow timer (~50 frames/unit, ~885).

Working selection (cold boot, `press start1` at 380; menu up ~385). Verified by in-race
capture + geometry hash on a matched 1300-frame window (Beginner control hash
`1375e3896fce119d`):

- Beginner: no steer; confirm with accelerator (default). 8 laps.
- Intermediate: `press steerright 6` at 415, `press accelerator 12` at 420 (confirm while
  the tap is still held). In-race frame shows LAP ×4, geometry `7fff27cbace3ff76`.
- Expert: `hold steerright` at 410, `press accelerator 12` at 424 (confirm while held;
  the cursor rests at the bottom of the list = 上級). In-race frame shows LAP ×2, geometry
  `05412860a8c95163`.

The committed `daytona-race-intermediate.txt` / `daytona-race-expert.txt` must use the
confirm-while-held pattern above. The race-line steering in these scripts is still
Beginner-tuned, so on Intermediate/Expert the car leaves the racing line after the first
corner; course SELECTION is now correct and reproducible, per-course race-line tuning is
the remaining follow-up (an off-line car does not exercise the on-track busy-scene load).

### Thread scaling of the packets stage (t1 vs t3) — was run but unrecorded until now

Matched 3000-frame Beginner races, same binary (`ccfb723c`), same script, clocks pinned
1344 MHz, `VF2_THREADS=1` vs `3` (jobs `174249-t1`, `174411-t3`). Recomputed (frames 300+):

| threads | packets p50 | p95 | p99 | work p50 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 11.76 | 27.60 | 29.64 | 17.03 | 36.82 | 39.29 |
| 3 | 6.90 | 16.89 | 18.50 | 13.99 | 25.57 | 27.36 |

The packets stage DOES scale with threads (1.70× at p50, 1.60× at p99) — sublinear, not
serial. This corrects the earlier "already parallel, nothing left" framing: there is real
parallel headroom (the split is by polygon index, so a single heavy polygon still straggles
on one core). Untested knobs to mine next: `VF2_THREADS=4`, `VF2_ATOMIC_ROW_CLAIMS=1`,
`VF2_POOL_JOIN_SPIN_US` ∈ {0, 2000}, and combining threads with the merge.

**`VF2_THREADS=4` result (job `182707-t4`, matched to t3):** no gain — work p99 27.58 vs
27.37, packets p99 unchanged, underruns 58 vs 57. The 4th thread just contends with the
sound core; the pool is already saturated at 3. `VF2_ATOMIC_ROW_CLAIMS=1` is already set by
the release wrapper. So the env-knob avenue is exhausted; the sublinear scaling is a serial
plan-phase / join cost, not thread count. The remaining large byte-exact lever for the tail
was hypothesised to be intra-polygon: the parallel split is by polygon index, so one polygon
whose cell loop dominates a busy frame would straggle on a single core.

**Straggler-polygon hypothesis — REFUTED (job `183244-clipstats`, `VF2_CLIP_STATS` + a new
per-call cell histogram).** Added `max_cells` + a log2 histogram of cells-per-`clip_packet_into`
call to `clip_counters`. Global max is **165 cells/call**; on the heavy frames the cell mass
sits in the small buckets (bucket 1 ≈800 calls @2–3 cells, bucket 2 ≈490 @4–7) while the top
buckets (5–7, the "one huge polygon" candidates) hold only ~9 calls ≈10% of cell work. A
single polygon is at most 2% of a frame's cell work. The clip time breakdown confirms it:
`cells` (the per-cell loop) is 28–45 ms of the ~35 ms clip total, `copy`/`outer` ~1–2 ms. So
the tail is **aggregate cell load spread across ~970 polygons**, not a straggler. This retires
both the intra-polygon-parallelism lever and the per-polygon UV-span cap.

**Byte-exact clip levers do NOT reproduce on the matched race (job `183958-bexactly` vs
`174411-t3`, both 3000-frame Beginner, same binary).** With `VF2_CELL_COL_CLIP=1
VF2_CELL_CLIP_HOIST=1` ON: work p99 27.08 vs 27.37 (−0.29 ms, within run-to-run noise), on-time
38.70% vs 39.22% (slightly worse), underruns 57 = 57. The earlier "~2% / ~4% combined" was
measured on a different window and does not hold on the matched race. Not worth flipping
default-on.

**Net byte-exact status on the clip stage (the largest bottleneck, ~66% of p99 work):
exhausted.** Threads saturated at 3, atomic-claims already on, straggler/UV-span refuted,
byte-exact per-cell levers in the noise. The only lever that moves the cell loop is
`VF2_CELL_MERGE` (−3.3 ms work p99, 27.21→23.89) but it changes pixels (mip softening) AND
still lands at 23.89 ms — i.e. even the merge does not reach the 17.384 ms bar. The interval−work
gap (~4 ms: interval p99 31.66 vs work p99 27.37) is the sim/geometry worker blocking on the
single `row_pool` `m_run_mu` behind render's packet stage; a separate geometry pool is the one
untested byte-exact structural lever, but it only helps the on-time criterion, not work p99,
and t4's core-saturation result predicts little gain.
