# Astro City Mini native target â€” development in progress

For the current checkpoint and resume commands, see [HANDOFF.md](HANDOFF.md).

The target is the original Astro City Mini: ARMv7 Cortex-A7, 256 MiB RAM,
Mali-400/OpenGL ES 2.0, Linux 3.4.113. The native i960 code uses SLJIT's
Thumb-2 backend. The scheduler retains exact 128-bit attoseconds using a
two-word integer implementation on hosts without `__int128`, checked against
native arithmetic. The previous Boost implementation remains a test oracle.

`vf2-acm` is a small EGL/fbdev, evdev and ALSA frontend. It does not depend
on SDL3 or a desktop. `gpu3d_gles2.cpp` implements the existing `gpu3d`
interface with hardware triangle rasterization, Model 2 indexed texture
sheets, luma/colour/gamma tables, mip/microtexture sampling and checker/
transparent coverage. It is a **fast** rasterizer, not the desktop compute
renderer's pixel-exact mode. Supersampling and the desktop edge filter are
explicitly unsupported and rejected.

## Evidence and outstanding work

As of this development checkpoint:

- ARMv7 binaries build and run under Cortex-A7 QEMU user emulation.
- The ARM and x86-64 headless runners match all 1,200 per-frame state hashes
  with sound enabled. The run crosses the point where a 64-bit attosecond
  counter would overflow. Two snapshots are identical; the third differs
  at 11 of 190,464 pixels in the CPU renderer.
- The portable 128-bit type passes 10,000 native-vs-portable arithmetic,
  overflow, division, comparison and serialization comparisons.
- GLES2 synthetic tests cover ordering, owner IDs, viewport clipping,
  checker parity, texture/luma/palette/gamma lookup and transparent texels.
- Four actual VF2 scenes on Mesa/GLES2 highp match CPU polygon ownership
  by about 99.988%, and colours within eight levels per channel by 99.813%.
- The mediump path clips polygons into mip bands and mirror/repeat cells,
  caches decoded indexed tiles (eight MiB between polygon submissions), and
  sends vertex UVs directly to the texture samplers. This preserves Mali-400's
  higher precision interpolator-to-sampler path. Four sampled scenes retain
  99.988% polygon ownership and 97.032% colour agreement within eight levels,
  up from about 71% with fragment coordinate arithmetic. Filtering and
  transparent-edge colour substitution are approximate. Mesa results do not
  prove Mali correctness or performance.
- The full native frontend renders the attract scene with separate tile
  and 3D layers under Mesa. The ARM binary's dependencies resolve against
  its bundled runtime and the device's stock Mali/ALSA libraries.

The physical Mini now executes the native build and displays textured VF2
gameplay. Mali reports two PP cores, GLES2, no fragment highp, eight texture
units and a 4096 maximum texture size. LCD rotation 270 degrees is confirmed
upright. Start, Select/coin and the stick work through character selection.
The speaker route works, but audio underruns while the game runs slowly;
audio work is deferred at the user's request. Fight performance still needs
substantial improvement, and a persistent stock-menu launch is unfinished.

The first hardware baseline used the bit-level reference scheduler and ran
at about six fps. The frontend now uses the upstream desktop's byte link,
batched vertex-buffer uploads, mixed 64-bit material hashes on ARMv7 and
front-to-back depth rejection. Interactive speed varies substantially by
scene. Use the fixed fight checkpoint below for comparable measurements.
Palettes occupy one 256x1024 RGBA texture with capacity for 2,048 palettes.
Each row contains two 128-colour palettes. Their normalized coordinates
still address the original pairs of identical logical entries, and dirty
rows upload before drawing.

## Build

Build the Docker image with this Dockerfile and mount the parent
`astro-city-mini-tools` workspace at `/work`. The source checkout is
`/work/vf2-recomp`. The sibling workspace contains the downloaded ACM-CFW
Windows package and the user's firmware backup. No original game data or
firmware is committed to this repository.

The current local builder container is `acm-vf2-armv7`. Its shell commands:

```sh
sh tools/acm/build-headless.sh
sh tools/acm/build-reference.sh
sh tools/acm/check-gles2.sh
sh tools/acm/compare-scene.sh
sh tools/acm/build-frontend-host.sh
```

The headless build needs the zlib 1.3.1 release tarball at
`build/deps/zlib.tar.gz`; its SHA-256 is checked before building.

For the hardware frontend, run `extract-stock-libs.py` on Windows to read
Mali/ALSA from the saved rootfs, then build inside the container:

```sh
sh tools/acm/build-frontend-arm.sh
```

The frontend uses the supplied glibc 2.36 ARM loader/runtime from
`build/acm/runtime` (Linux minimum 3.2). It does not replace the stock
glibc 2.29. The runtime loader explicitly searches the RAM package and
read-only `/stock` libraries.

## RAM development session

`pack-dev-image.py` rebuilds the vendor RAM initramfs with the installer,
restore logic and replacement NAND images removed. It adds a small
FunctionFS USB transport (`usb-rpc.c`) and mounts the stock rootfs
`ro,noload`. Uploads are limited to `/tmp` or `/run`. This is a local USB
development tool with shell access; there is no network listener.

`patch-memboot.py` makes a RAM-only bootloader copy with the primary
display set to LCD instead of HDMI. It verifies and recalculates the
Allwinner additive image checksum. It does not write NAND or saved boot
environment. Internal LCD presentation defaults to 270-degree rotation;
`--rotate 0/90/180/270` overrides it at runtime.

On Windows, after building:

```text
python tools/acm/pack-dev-image.py
python tools/acm/patch-memboot.py
python -u tools/acm/develop-device.py --boot
```

The last command waits for one physical restart: disconnect micro-USB,
wait ten seconds, reconnect with the Mini switch ON. It catches the
startup handshake on the endpoint the device reports, loads the RAM
image through FEL, waits for RPC, uploads the binaries/runtime and the
user's `roms/vf2.zip`, checks each upload's CRC, and records hardware
benchmarks under `build/acm/hardware/`.
After successful benchmarks it starts an interactive game with sound and
cabinet controls enabled. Use `--no-launch` for a benchmark-only session.

Jobs run in the background on the device with PID/exit/log records. The
controller polls those records; an observation deadline does not restart
or declare a live device job finished. A normal power cycle leaves this
RAM environment and returns to the unchanged stock firmware.
Each run has unique log/exit paths, and the controller checks process start
times and terminal state as well as the exit record to avoid stale results.

The stock analog audio control is initialized on card 1 by its gameapp
script. The frontend uses that same modest lineout level only when the RAM
kernel starts muted, preserving an existing nonzero setting. The private
`asound.conf` chooses that card. The user confirmed intermittent/distorted
speaker audio; audio work is deferred until frame rate improves.

Controls map arrows to movement, A/X to punch, B/Y to kick, C/Z to guard,
F1 to Start and F2 to Select/coin. The factory arcade setting needs two
coin presses per credit. Presses are latched for a frame so brief taps
survive slow frames. Escape or the power key exits the frontend.

## Short checkpoint iterations

`build-checkpoint.sh` creates an ARM checkpoint at game frame 2400 of
`tools/scripts/start_game.txt`: Pai versus Shun at the stone-tower stage.
It uses QEMU on the PC, then verifies the save in a separate ARM process.
After twenty resumed frames both the core-state hash and full CPU-rendered
image must match the uninterrupted reference exactly. The file includes
numeric geometry state, a ROM identity check and CRC; it contains no process
pointers. It is a development format for compatible builds on the same
architecture. The sound board must be held.

```sh
sh tools/acm/build-checkpoint.sh
sh tools/acm/build-frontend-arm.sh
```

With the RAM USB session connected, on Windows:

```text
python tools/acm/benchmark-device.py --frames 30 --profile --sync-gpu
python tools/acm/benchmark-device.py --frames 120 --profile --sync-gpu
python tools/acm/benchmark-device.py --interactive --sync-gpu
```

Each iteration resumes at frame 2403 after three unmeasured frames rebuild
the display pipeline. A thirty-frame hardware run takes about four seconds
of measured gameplay, about ten seconds including upload/startup. Screens,
logs, samples, matching symbols and binary/checkpoint hashes are saved under
`build/acm/hardware/iterations/`. The script stops only its previously owned
game process before replacing the RAM payload. No power cycle is needed.
`--sync-gpu` bounds pending GPU work while memory/performance are investigated.

Checkpoints use `--no-sound-board`; `--mute` alone still emulates sound.
The uploaded ROM ZIP and checkpoint file are released after decoding to
avoid keeping duplicate copies in initramfs. `release_unused_roms()` also
releases unused auxiliary regions and decommits zero-filled ROM pages on
Linux, preserving the padded regions' virtual addresses and byte values.
On Linux, the Mini frontend also shares byte-identical nonzero pages in
main data, polygons and textures. Guest addresses and the original ROM
hash stay the same, and ROM patches use private copy-on-write pages.
Set `VF2_ROM_DEDUP=0` to use the original allocations for a comparison.
The loader falls back to those allocations if RAM backing is unavailable.
The kernel reserves 64 MiB as CMA; Mali allocations can still fail with
apparently free CMA pages, so bounded submission and memory measurements
remain necessary.
The 120-frame checkpoint run also completes, taking 13.7 seconds of measured
gameplay at 8.73 fps, about twenty seconds including startup and transfer.
The game is running natively but is still far below real-time speed.

Resolve an iteration's CPU profile with its matching symbol file:

```text
python tools/acm/analyze-cpu-samples.py PATH/cpu.samples --symbols PATH/symbols.txt
```

The generated native binary, ROMs, screenshots, copied stock libraries,
backups and logs remain under ignored `build/` or `roms/` paths.

Renderer diagnostics: `VF2_GLES_MEDIUMP=1` forces the Mali precision path;
`VF2_GLES_TILES=1` selects the tile sampler on a highp device;
`VF2_GLES_MANUAL=1` keeps the older manual atlas sampler for comparison.
Arm documents the direct-varying sampling precision requirement in its
[numerical precision guide](https://documentation-service.arm.com/static/63935ba11d698c4dc521b9d3).

## Performance experiments, November 16

These are diagnostic builds, below the target 60 fps. The fixed fight
checkpoint makes comparisons repeatable; all runs hold the sound board.
`--mode simulation` omits rasterization/display but retains geometry work.
`--mode graphics` freezes simulation and repeatedly renders the same scene.
Reported gameplay time excludes checkpoint warmup and PNG compression.

- Exact portable clock arithmetic raised simulation-only throughput from
  26.99 to 29.80 fps with the same final state signature.
- `vf2-acm-tgp` specializes the existing TGP instruction semantics into
  native ARM handlers for the user's loaded microcode. Each instruction
  checks its live word and falls back to interpretation if it changed.
  Fresh-process checkpoint verification matches both state and pixels.
  Simulation-only measured 31.32 fps, versus 28.30 when forcing interpretation
  in the same binary. Both execute the same instruction count and final state.
- `--batched` selects the experimental paged renderer. Texture pages keep
  Mali interpolation precision better than the previous single 2048 atlas;
  a 30-frame hardware image comparison improves from 90.91% to 98.51% within
  eight colour levels versus individual tiles. Some texture error remains.
  Vertices are 52 bytes instead of 108; page-pair batches use 23â€“39 draws
  instead of roughly 1,600 in this fight. Clipping/wrap preparation runs
  across the existing CPU worker pool; GL calls stay on the submitting thread.
- Parallel packet preparation raises frozen-scene throughput from 19.57 to
  22.20 fps with an identical hardware image. Full simulation/rendering with
  TGP handlers and paged batches measures 12.07 fps over 120 fight frames.
- Kernel debug counters sampled during frozen rendering report 384 MHz GPU,
  about 26% pixel and 2% geometry utilization. These coarse driver counters
  suggest CPU preparation/submission dominates; they are not GPU timer queries.

Build the specialized frontend from Windows with the configured builder:

```text
python tools/acm/build-tgp.py
python tools/acm/benchmark-device.py --binary build/acm/bin/vf2-acm-tgp --batched --frames 120 --profile
python tools/acm/benchmark-device.py --binary build/acm/bin/vf2-acm-tgp --mode simulation --frames 120 --tgp-interpreter
```

A single-pass screen compositor was tested and removed: 21.76 fps versus
22.20 for the original, with minor pixel differences.
Profiles save matching binary symbols, hashes and optional kernel GPU runtime
samples alongside each iteration. The microcode include is ROM-derived and
must remain under ignored `build/gen/`; it is never a source asset to publish.

## GPU tiles and frame pipelining

`--gpu2d` enables `gpu2d_gles2.cpp`, an implementation of the small host
`gpu2d` interface. System 24's existing scroll/window/split logic exports
rectangles instead of drawing pixels. The renderer caches four indexed maps,
uploads changed 8x8 cells and applies palette, transparency, priorities and
window masks on the GPU. Native 496x384 rendering is supported; widescreen,
smooth-motion and custom HUD-name cases retain CPU drawing. The CPU renderer
remains the reference. A hardware screen comparison is 99.0605% identical and
99.9564% within eight levels: native layers match, while final screen scaling
has a few different nearest-neighbour boundary choices.

`check-gpu2d.sh` compares all three native-resolution layers in 24 cases:
normal/line scroll, vertical/horizontal splits, disabled maps, window masks,
priority, palette changes, dirty graphics/cells and version-gap recovery.
The desktop test and ARM test on the Mini pass with exact pixels. Build the
ARM test with `build-check-gpu2d-arm.sh` and use the bundled ARM loader/runtime.

`--pipeline` overlaps next-frame simulation with current-frame presentation.
`--pipeline-render` also overlaps 3D preparation/submission: the host copies
the polygon list and captures texture/luma/colour/gamma RAM before allowing
the board to advance. Texture snapshots are copied again only on generation
changes. All OpenGL calls remain on the main thread; the CPU worker is joined
before the next frame's board data is inspected. Inputs are set before each
worker task. These experiments currently require the sound board held.

The same 120-frame fight gives:

| Configuration | fps |
| --- | ---: |
| Compiled TGP, paged batches, CPU tiles | 12.07 |
| Initial GPU tiles | 13.69 |
| GPU tiles, presentation overlap | 16.15 |
| GPU tiles, render preparation/presentation overlap | 19.45 |

Both pipeline variants match game-state signatures; their 30- and 120-frame
hardware images are identical to each other. CPU phase timing in pipeline
runs is remaining join wait, not the entire worker's time. A separate line
reports worker-task duration. Driver utilization rose from roughly 26% to
51-52% pixel utilization as CPU feeding improved; those remain coarse counters.

```text
python tools/acm/benchmark-device.py --binary build/acm/bin/vf2-acm-tgp --batched --gpu2d --pipeline-render --frames 120 --profile
```

Longer tests exposed full-atlas invalidation after small texture RAM writes.
The atlas now retains allocations and exact packed source words per tile, validates
on the relevant sheet generation changing, and uploads only changed content.
Capacity is still bounded; changing source RAM pointers invalidates it fully.
This avoids relying on hashes for pixel-cache correctness.
Over 600 frames this reduced atlas uploads from 88,817,336 to 4,555,272 bytes
and clears from 37 to zero. The first implementation compared decoded bytes;
checking packed words instead gives an identical final hardware image, with
20.61 fps overall versus 20.01. Peak RSS is 152,964 KiB. The unmodified
simulation signature remains frame 3003, IP 00028274, TGP 49173603,
work RAM 4fa73f2e21d72f68. CPU worker tasks average 40.53 ms under simultaneous
rendering, so simulation still needs major optimization for real-time play.

## SIMD and exact scheduler arithmetic

The original ARM `-O3 -mfpu=neon-vfpv4` build auto-vectorizes some copies,
clears and structure-field work; a compiler report and disassembly showed
scalar VFP geometry arithmetic, with no NEON floating arithmetic in that unit.
`geometry_math.h` now provides an **opt-in** matrix prototype selected by
`--geometry-neon` / `VF2_GEOMETRY_NEON=1`. It uses vector multiply/add across
XYZ with the original operation order and no fused rounding. Matrix/vertex
operands outside a bounded normal range use scalar VFP, preventing NEON's
denormal flushing from changing the result. `check-geometry-arm.sh` checks
200,000 random/cancellation matrices and edge fallbacks; it passes bit-exactly
under QEMU and on the Mini. The checkpoint's state/pixels also match, as do
the 120-frame hardware picture and full serialized core hash.

This prototype is disabled by default: simulation-only measured 32.72 fps
with it versus 33.17 scalar in the same binary. Per-point SIMD with guards
and register traffic did not help this workload; bulk vertex processing is
a better future SIMD target than enabling relaxed floating-point globally.

Specialized TGP helpers are inlined so constant register/ALU fields can be
folded. The measured simulation-only change was small, 32.00 to 32.46 fps.
The scheduler's repeated 64/32 divide now uses a precomputed floor reciprocal,
high multiplication and one exact remainder correction. It preserves the
old shifted-dividend calculation, without changing clock precision or timing.
1,190,114 divisor/edge/random cases pass on host, QEMU ARM and the Mini.
Simulation-only measured 33.67 fps versus 33.19 with `--reference-division`
in the same binary. Both have full core hash `65347bfab44ab67a` after 120
resumed frames; the canonical checkpoint hashes remain unchanged.

Finite frontend tests now print a full serialized core hash after timing and
profiling stop, in addition to the shorter IP/TGP/work-RAM signature. The
full pipeline remains around 19-20 fps; these CPU changes are small gains.

## Shared native frame target

`--canvas` / `VF2_GPU_CANVAS=1` draws the tile underlay, 3D and HUD into the
same 496x384 framebuffer, then presents it once. `gpu2d::prepare` owns the
uploaded map/palette/mask data and a single vertex buffer for all three tile
passes. `gpu3d_frame` underlay/overlay callbacks run while the 3D framebuffer
is bound, avoiding intermediate tile-layer targets and framebuffer switches.
Menus without 3D draw the same tile passes into the shared target directly.

Depth clears happen before colour drawing; depth is discarded after use
when `GL_EXT_discard_framebuffer` is available (`VF2_KEEP_DEPTH=1` disables
that optimization for diagnostics). The final opaque canvas blit disables
blending. Native layer/canvas composition tests pass exactly on Mesa and on
the Mini. The final LCD comparison to separate layers is 97.9284% identical,
98.9915% within eight levels: different sampling boundaries affect some HUD
edges. Core state is unchanged.

Vertex packing now computes material/depth/colour once per polygon, page
scales once per band, and looks up each page-pair batch once per band. UV
normalization uses exact power-of-two reciprocals where applicable. This
produces an identical hardware image to the previous packer.

The 120-frame fight measured 21.53 fps after moving depth clear, 22.06 after
the opaque-blit/depth-discard changes, and 23.00 after material hoisting.
The 600-frame run averages **24.18 fps**, 24.814 seconds, peak RSS 152,916 KiB;
full core hash `277230da285bb5f5`, work-RAM hash `4fa73f2e21d72f68`.
The 120-frame core hash remains `65347bfab44ab67a`.

LTO builds use a separate object directory and binary:

```sh
sh tools/acm/build-frontend-arm.sh vf2-acm-tgp-lto
sh tools/acm/build-checkpoint.sh verify-existing vf2checkpoint-tgp-lto
```

The LTO checkpoint matches, but it measured 33.52 fps simulation-only versus
33.67 without LTO, and 22.76 versus 23.00 in the full 120-frame pipeline.
It grows the binary from 4.16 MB to 5.67 MB; the regular build stays preferred.

The benchmark helper's `--fast` preset selects the current regular TGP build,
batched textures, shared canvas, snapshot render pipeline and bounded GPU
queue (full-render mode). `--no-gpu-queue` selects the finish-per-frame reference:

```text
python tools/acm/benchmark-device.py --fast --frames 120 --profile
```

CPU/GPU/DRAM clock and voltage settings were not changed for these experiments.

## TGP fetches, internal RAM and scheduler queues

The board now maintains an ordered intrusive queue of enabled timers, ordered
by expiry and sequence number. Reset and state load rebuild the queue from
serialized timer data. `--reference-timers` retains the original scan for
comparisons. The optional `vf2-acm-tgp-stats` target counts dispatches and
timer activity; the regular build contains no diagnostic counter fields or
updates. The 120-frame test has about 477,000 scheduler passes, with frequent
FIFO stalls and TGP halt/reset transitions.

The TGP bus exposes its two side-effect-free internal RAM arrays directly.
Only the fixed internal data address map uses this path; FIFO, register-file
and external I/O still use the original callbacks. `--slow-tgp-ram` selects
the virtual data bus reference. Individual compiled instructions measured
34.44 fps simulation-only with direct RAM versus 33.20 with virtual RAM.

Microcode uploads, reset and state load advance a bus revision. A nonzero
revision guarantees the program view remains stable for a complete CPU run.
The native TGP compares all 4,096 words with the generated image when that
revision changes. Matching images execute individual native handlers without
virtual program reads or per-instruction opcode checks. Other images retain
the checked-handler/interpreter fallback; debugger hooks also use that path.
`--guarded-tgp` forces the per-instruction checks for comparison. On the same
binary, simulation measured **37.43 fps versus 35.14 guarded**. Both finish
with full core hash `65347bfab44ab67a` after 120 frames.

`check-tgp-arm.sh` compares four execution paths across boot, microcode
mutation, restoration, debugger callbacks and resumption. All serialized CPU
and bus states match. `VF2_TGP_BLOCKS=1` / `--tgp-blocks` enables experimental
eight-word blocks; those averaged only 3.23 instructions per entry and ran
slower, so the default remains individual native handlers. `--single-tgp`
forces those handlers if block execution was otherwise selected.

Polygon clipping now uses an explicit wrap comparison instead of a variable
integer remainder on every edge, and alternates vertex buffers between planes
instead of copying each intermediate polygon. The ARM checkpoint keeps core
hash `d46dd64a8dbc0653` and CPU-rendered pixel hash `1e01b6bd873de77f`.
The 120-frame hardware image is identical before and after this change.

A material shader specialization experiment was removed: 600-frame speed
was effectively unchanged (24.47 versus 24.45 fps), and a targeted Mali test
found differences in fractional mip blending. The retained renderer uses
the original batch shader.

The benchmark collector tolerates a process exiting between checking and
reading `/proc/PID/stat`. CPU sample reports also group the many individual
TGP handlers, geometry functions and scheduler functions into subsystem totals.

The retained configuration completes 600 frames in **24.303 seconds, 24.69
fps**, peak RSS 152,944 KiB. Overlapped simulation tasks average 34.71 ms.
The full core hash remains `277230da285bb5f5`; the final hardware viewport
is pixel-identical to the preceding regular configuration. Evidence is in
`build/acm/hardware/iterations/20261116-213510-776e71`.

## Bounded GPU overlap and staged uploads

The Mini advertises `EGL_KHR_fence_sync` and `GL_OES_EGL_sync`. The frontend's
`--gpu-queue` benchmark option uses a fence after presentation commands and
drains the preceding frame before modifying shared GPU resources. CPU packet
preparation overlaps the preceding GPU frame. The final fence is drained
inside the measured interval. Failed/timeout waits terminate the iteration;
unsupported drivers reject this mode instead of running an unbounded queue.
The synchronization contract follows
[EGL_KHR_fence_sync](https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_fence_sync.txt).

Waiting only at frame submission was slower (8.43 fps) because resource
updates stalled inside the driver. The retained arrangement separates tile
capture (`stage`) from GPU upload, and adds optional `gpu3d::prepare_cpu`
for geometry packets. Tile staging owns encoded changed map rectangles,
palette/mask bytes and pass vertices. The board can advance after staging;
uploads use only those owned values. The 24 tile cases remain pixel-exact
on Mesa and the Mini when source maps, flags, palette and masks are all
altered between staging and upload. `paint` rejects a staged, unuploaded frame.

The initial bounded pipeline measured 25.09 fps over 600 frames. Changing
render preparation to two threads measured 25.19; one thread measured 24.78.
Pinning simulation to CPU 3 measured 25.20. The default remains three render
threads with normal scheduling; `--render-threads` and `--sim-cpu` are retained
as diagnostic controls. CPU/GPU clocks are unchanged.

Texture snapshots now accept optional per-4KB-page revisions. Every texture
write advances its page; whole-bank invalidation, reset and state restoration
refresh all pages. The snapshot owns both bytes and page metadata. Sources
without page revisions retain whole-bank copies. `--full-texture-snapshot`
forces that reference path. `check-snapshot-arm.sh` tests page boundaries,
dirty copies, invalidation, source replacement, legacy inputs and ownership;
both sparse and reference paths pass under QEMU and on the physical Mini.

With sparse copies, the 600-frame run copies 6,139,904 texture bytes and
measures **25.29 fps**, 23.720 seconds, peak RSS 152,944 KiB. The full state
hash remains `277230da285bb5f5`, and the viewport is pixel-identical to the
preceding 24.69 fps configuration. Evidence:
`build/acm/hardware/iterations/20261116-215043-6c976f`.
Preparation timing now separates tile capture, scene capture, remaining board
work, geometry packets and GPU submission. Profiling also records `/proc/meminfo`
alongside GPU utilization to detect memory growth in longer tests.

The same binary with whole-bank copies transfers 159,383,552 bytes and runs
at 24.95 fps; its final state and image match the sparse version exactly.
The 2,400-frame queue run completes in 92.117 seconds (**26.05 fps**), peak
RSS 152,944 KiB. Late-run free memory stays around 64 MiB; 2,400 fences finish
with an average wait of 0.06 ms. Its final core hash is `41ab1dc3d8a38d84`,
work-RAM hash `0d15ccf7ccdf4310`, frame 4803. The texture atlas remains at
nine pages with no clears. Evidence:
`build/acm/hardware/iterations/20261116-215206-86ef05`.

## Geometry command batches and exact packet math

The four geometry polygon parsers now hand complete three-coordinate or
five-word lighting/distance/coordinate groups to the rasterizer together.
The grouped path operates only within the initial polygon header or a valid
triangle/quad payload. It processes the polygon at the same completion word
and preserves linking and command indices. Groups crossing another command
boundary use the existing per-word port. `--reference-geometry-push` selects
the original port for every word. The initial simulation comparison measured
38.22 fps with grouping versus the preceding 37.44 fps.

Texture LOD boundaries have the form `exp2(-k/256)` for integer `k`.
`gles2_packet_math.h` caches 256 fractional results and adjusts the exponent
bits for normal doubles in the validated range; other values use libm.
Microtexture scale uses the four exact powers of two for its two-bit input,
and integer mirror parity avoids a floating remainder. The reference remains
available with `--reference-packet-math`.

`check-packet-math.sh` checks all 131,072 boundaries in the fast range directly
against libm, microtexture levels, parity edges, range fallbacks and 1,000
generated polygons. The 29,325 generated vertices have the same hash on x86-64,
QEMU ARM and the physical Mini, in both reference and optimized modes:
`5e8e40c260fbe83f`. GLES2 raster tests pass on Mesa at highp and forced mediump.

The retained configuration measures **25.43 fps** over 600 frames versus
**24.95 fps** with both reference options in the same binary. Packet CPU
preparation averages 7.01 ms versus 7.71 ms. Both finish with core hash
`277230da285bb5f5` and geometry hash `e0c188d55df8df32`; their final viewport
pixels are identical. Finite tests now print geometry state hashes separately
from core state, covering raster command buffers and geometry registers.
Evidence is in `build/acm/hardware/iterations/20261116-221410-dbee39` and
`build/acm/hardware/iterations/20261116-221238-3c62a8`.

An all-inside clipping shortcut and early material-read culling were tested
and removed because they did not improve the full run. The prior clipping
algorithm and culling sequence remain in use.

The final configuration, after those two experiments were removed, measures
37.75 fps simulation-only over 120 frames, with core hash `65347bfab44ab67a`
and geometry hash `7db96aa7cf93a2fa`.

## Short cycle products and reusable indexed packets

`cycle_duration.h` computes short nonnegative cycle products in 64 bits only
when the result fits. Larger and signed products use the original 128-bit
arithmetic, and absolute clocks remain 128-bit. The cache is initialized from
each CPU's fixed clock rate and is not serialized. `--reference-duration`
selects the original multiplication. 400,107 signed, overflow-boundary,
clock-carry and random cases pass on native x86-64, portable x86-64, QEMU ARM
and the Mini. Simulation measured 38.97 versus 38.78 fps in the same binary;
the corresponding full 600-frame runs measured 25.67 versus 25.43 fps.

The renderer can share fan vertices with 16-bit element indices. It retains
only vertices referenced by nondegenerate triangles; adjacent triangles reuse
their fan origin and common edge. The first fight frame uses 6,866 vertices
and 9,807 indices rather than 9,807 duplicated vertices. Scenes exceeding
65,536 vertices retry as regular array batches, preserving batching and
avoiding index truncation. A 16,385-quad test checks a visible final quad past
that limit, including ownership and colour, on Mesa and the physical Mali.

Packet output vectors retain their capacity between frames. Spare retained
storage is limited to 4 MiB above current frame data; larger spare allocations
are compacted without dropping data. Optional stack clipping uses two inline
32-point buffers and swaps their pointers between planes. Unusual larger
polygons spill to vectors. Golden packet tests check vector/stack clipping,
indexed expansion, output reuse, spilling and copying byte-for-byte on host,
QEMU and physical ARM. The previous packet stream hash remains unchanged.

In the current binary, the 600-frame configurations measured:

| Configuration | FPS | CPU packet preparation |
| --- | ---: | ---: |
| Array batches, vector clipping | 25.58 | 7.33 ms |
| Array batches, stack clipping | 25.72 | 5.95 ms |
| Indexed batches, stack clipping | 25.85 | 7.05 ms |

All finish with the same core/geometry hashes and identical viewport pixels.
The fast preset selects indexed batches and stack clipping; use `--no-indexed`
and/or `--no-stack-packets` for the other paths. The simulation worker still
averages about 35 ms under simultaneous rendering. Further CPU work should
be profiled by thread before choosing larger scheduling/TGP changes.

The updated fast preset completes 2,400 frames in **90.409 seconds, 26.55
fps**, peak RSS 152,972 KiB. Late-run free memory remains around 58 MiB.
Its core hash `41ab1dc3d8a38d84` and final viewport match the earlier long
run exactly; geometry hash is `5c4a0b75827d6659`. The final rebuilt executable
matches the tested binary's SHA-256. Evidence:
`build/acm/hardware/iterations/20261116-225936-8e5e8a`.

## Guest execution diagnostics

`benchmark-device.py --fast --mode simulation --frames 120 --guest-counts`
records native i960 instruction entries and interpreter fallbacks, plus TGP
instruction attempts and consecutive same-PC attempts. Runs are bounded to
1..1,200 frames. The i960 counters are emitted into native code only when
enabled; ordinary builds emit the original code. This preserves native idle
loop skipping. The TGP debugger hook uses checked individual handlers during
this diagnostic, so its elapsed time is not a performance baseline.
`analyze-guest-counts.py <iteration-directory>` lists hot functions and PCs.

The resumed 120-frame fight has 3,810,758 entries into i960 instructions with
inline native paths, 644,398 helper-only entries and 40,195 interpreter
fallbacks. Inline-capable memory instructions can still call helpers for bus
accesses. Leading helper-only opcodes are floating arithmetic and multiword
memory transfers. The TGP has
8,349,225 instruction attempts, including 1,020,952 consecutive same-PC
attempts. Core and geometry hashes match the uninstrumented run. Evidence:
`build/acm/hardware/iterations/20261116-231950-bb8ca1`.

A single-thread simulation CPU profile over 600 frames puts about 28% in the
board/buses/scheduler, 23% in TGP execution, 19% in geometry processing and
16% in emitted i960 code. These are CPU sample shares with display disabled,
not percentages of full rendering wall time. Evidence:
`build/acm/hardware/iterations/20261116-231555-f49a37`.

A hardware-repeat handler experiment passed state/cycle-boundary tests but
was removed. Looping every opcode slowed simulation to 34.25 fps versus
37.53 fps in its reference path. Restricting loops to plain transfers gave
39.07 versus 38.28 fps simulation-only, but full rendering measured 25.66
versus 25.78 fps. Both full runs matched core/geometry hashes and pixels.
Evidence: `20261116-231823-655573`, `20261116-231914-f90510`,
`20261116-232138-03bba4`, `20261116-232210-784866`,
`20261116-232245-571c1f` and `20261116-232322-ac9ff3` under the iteration
directory. The ordinary TGP dispatch remains in use.

## Owned camera-space geometry

The native fast preset now hands camera-space polygons to the rendering
thread before clipping and projection. Geometry commands, texture-header
addresses, vertex linking, culling and depth-state updates retain their
original order in the simulation. The packet captures vertices, UVs, clip
planes, window/depth keys and material settings. Palette/gamma colours and
CRTC offsets are resolved before the next simulation frame starts; the
renderer then reads only its own data. Clipping and projection use the row
worker pool, with the reference arithmetic and rounding order. The clipped
survivors retain the hardware's window/depth/link order and 32,767-polygon
limit. This removes intermediate software-renderer polygon preparation from
the simulation's critical path.

VF2 reads the polygon-count register zero times during both the 600- and
2,400-frame fights. The handoff still answers an actual register read by
clipping/counting its pending list. `check-camera-packets.sh` compares this
feedback before and after presentation, all meaningful render-packet fields
for 177,125 polygons over 120 fight frames, and packet ownership after the
machine advances another full frame. Synthetic cases cover crossing-plane
texture interpolation, empty scenes, window/depth/link order, and capacity
before hidden-window filtering. All pass on ARM under QEMU.

In the same binary, the 600-frame handoff measures **28.64 fps** versus
**25.72 fps** for the original path. Simulation tasks average 31.60 versus
35.28 ms; camera-space preparation moves to render workers at 3.43 ms/frame.
Both runs have identical core/geometry hashes and viewport pixels. Evidence:
`build/acm/hardware/iterations/20261116-233410-abb330` and
`build/acm/hardware/iterations/20261116-233537-5d79d1`.

The final fast preset completes 2,400 frames in **82.244 seconds, 29.18 fps**,
versus the preceding 26.55 fps. Peak RSS is 152,972 KiB. Core hash
`41ab1dc3d8a38d84`, geometry hash `5c4a0b75827d6659` and final viewport pixels
match the previous long run exactly. Evidence:
`build/acm/hardware/iterations/20261116-234152-1453b5`.

Use `--no-camera-packets` with `--fast` for the original path. The handoff is
a startup option for snapshot rendering with the normal arcade camera and
no picture delay; smooth/free-camera/CPU-renderer paths retain their existing
implementation. This stage uses CPU render workers; GPU vertex processing
and reusable object meshes are further opportunities.

## Simulation-thread CPU sampling

`benchmark-device.py --fast --frames 600 --profile-simulation` uses a Linux
thread CPU-clock timer directed to the simulation thread (`SIGEV_THREAD_ID`).
It samples that thread every 5 ms of CPU time, independently of rendering
threads. `cpu.samples.thread` records its TID and measured CPU time; the
sample analyzer reads this metadata. Ordinary `--profile` keeps process-wide
sampling. The device's kernel supports both paths.

The resumed 600-frame fight records 3,811 samples over 19.073 simulation-thread
CPU seconds. Sample shares are 26.84% board/buses/scheduler, 24.22% TGP,
18.13% emitted i960 code and 15.80% geometry. The worker averages roughly
32 ms per frame and spends essentially all of that task on CPU work.
Evidence: `build/acm/hardware/iterations/20261116-234553-4f9f4f`.

The trusted individual-handler TGP path now accumulates its instruction count
in a host register and updates the 64-bit diagnostic total at the end of each
slice. This preserves the total on return; checked/debug paths retain their
individual updates. `--reference-tgp-counter` selects the previous update on
every instruction. All five ARM modes match both machine state and diagnostic
counts across boot, microcode mutation/restoration, debugger entry/exit, and
short slices. The same-binary 600-frame runs measured 28.51 and 28.29 fps,
with identical state hashes and instruction totals. Evidence:
`20261116-234811-3036e8` and `20261116-234914-a8d5bb` under the iteration folder.

Two native-page experiments were removed after passing state/count checks
but running slower: 256-opcode switch pages measured 27.20 fps, and pages
with direct links between known PCs measured 24.30 fps. Both executed the
same 39,698,315 DSP instructions and ended with the reference core/geometry
hashes. Evidence: `20261116-235258-c63ac0` and `20261116-235628-ed7d41`.
The small, shared opcode handlers remain the preferred implementation.

## Scheduler diagnostics outside the hot loop

Scheduler logging used to keep formatting, floating conversions and string
cleanup in `timeslice()` even when logging was disabled. Debugger hook
installation also constructed `std::function` objects in that function.
Both now live in separate cold, non-inlined helpers. Normal scheduling retains
the existing guards, cycles, timer order and callback behavior. The local
stack allocation falls from 428 to 108 bytes, and the hot function no longer
needs the saved floating registers used by logging.

The first 20 ms of boot produce identical scheduler traces before and after:
37 lines, including 18 CPU dispatch records. Fresh checkpoint verification
keeps core hash `d46dd64a8dbc0653` and CPU-rendered pixels
`1e01b6bd873de77f`. A separate verification enables and then removes an i960
instruction hook, observes 979,337 callbacks and retains those same hashes.
Use `VF2_CHECK_SCHED_HOOKS=1` with the checkpoint verifier for that check.

The 600-frame hardware runs measure 28.73 versus 28.39 fps, with worker tasks
31.45 versus 31.91 ms. Core hash `277230da285bb5f5`, geometry hash
`e0c188d55df8df32`, and final viewport pixels match. The new profile has 3,776
samples over 18.887 thread CPU seconds; `timeslice()` accounts for 451
samples (11.94%). Evidence:
`build/acm/hardware/iterations/20261117-000448-cb9bc3` and
`build/acm/hardware/iterations/20261116-235930-183ee0`.

## Native DSP kernel trial, archived

A native REP-copy kernel preserved address increments, overlap order, repeat
counts, slice boundaries and instruction totals. It passed 1,344 fixtures
covering both copy directions, invalid spans, zero/255 repeat counts and
partial resumes. In a 600-frame fight it replaced 3,669,508 instruction steps
with 314,517 copies, but measured 28.60 versus 28.62 fps in the reference path.

A second prototype combined 92 pure internal-RAM multiply/accumulate chains
of two to four instructions. Arithmetic, pipeline registers, status flags,
PC and cycle accounting remained exact. It passed 29,808 fixtures including
subnormals, infinities and NaNs; NaN arithmetic results used the original
handler to preserve payload selection. All seven DSP execution modes matched
state and instruction counts through revision changes and debugger hooks.
The fight used 1,420,301 math batches covering 4,149,050 instruction steps.

After removing per-kernel statistics from the normal path, combined kernels
measured 28.58 fps versus 28.23 fps with both kernels disabled in that binary.
The preceding fast build measured 28.73 fps. The prototype was therefore
archived and removed from the active runtime and generator. Its source,
matching binary, tests and an apply-checked patch remain in
`build/acm/native-kernel-trial/`. Evidence is in iterations
`20261117-001439-0fba55`, `20261117-001540-0069fe`,
`20261117-002614-778cf8`, `20261117-003248-c37a7e` and
`20261117-003620-11d6cc`.

The restored executable matches the SHA-256 of the tested scheduler-cleanup
build (`20261117-000448-cb9bc3`). It was uploaded again and completed a fresh
30-frame fight with core hash `1f0999ee12ec0bd8`. Restore evidence:
`build/acm/hardware/iterations/20261117-003855-a6dea0`.

The earlier scheduler diagnostics show 477,373 passes over a 120-frame
fight, with the halted 68K visited each pass and advanced 271,792 times
without executing instructions. The dummy Z80 was visited each pass and
advanced 184,052 times. These and FIFO synchronization are the next targets
for reducing hardware-model coordination work.


## Halted sound CPU and FIFO storage (2026-11-17)

With the sound board disabled, the 68000 remains halted. The optional
`VF2_LAZY_AUX_CLOCKS=1` path omits its per-pass scheduler visits and materializes
its clock at frame/run-until boundaries, save-state writes and cycle-counter
reads. It uses full-period division for larger gaps and retains 128-bit time.
Sound-enabled and sound-only operation use the original scheduler. The dummy
Z80 stays in the original schedule: delaying it changed game state and was
rejected. `--fast` now selects the halted-68000 path; use
`--no-lazy-aux-clocks` for the original per-pass advancement.

Same-binary 600-frame profiles with and without lazy clock advancement measured
29.22 versus 28.56 fps and 30.80 versus 31.68 ms per simulation task. Both core
and geometry hashes matched the reference, and viewport pixels were identical.
The lazy path's 2,400-frame run measured 30.30 fps / 29.54 ms per simulation task
and also matched all reference hashes and pixels. Evidence:
`20261117-005906-7914b2`, `20261117-005941-805e76`,
`20261117-010027-ec3a6e`.

The eight-word processor FIFOs now use an inline ring instead of a deque;
overflow staging retains its unbounded deque. Interrupt delivery returns the
processed vector's capacity to its queue when no callback has queued another
event. Logical ordering and the checkpoint byte format are unchanged.
`docker exec acm-vf2-armv7 sh tools/acm/check-word-fifo.sh` checks one million
operations against deque, including wraparound, clears, serialized bytes,
restore and rejected oversized lengths. The fresh-process 20-frame verifier
matches core `d46dd64a8dbc0653` and pixels `1e01b6bd873de77f`.

Together these changes measured 29.72 fps / 29.06 ms per simulation task over
600 frames, and 30.56 fps / 28.33 ms over 2,400. Core, geometry, instruction
counts and final viewport pixels match the original reference at both lengths.
Evidence: `20261117-010212-866a7a`, `20261117-010256-70a80f`.
The 600-frame simulation profile measured 17.440 thread CPU seconds, versus
18.488 with lazy clocks alone. These remain below the real-time target.

An explicit `--swap-interval 0|1` benchmark option sets `VF2_SWAP_INTERVAL`.
The selected fbdev EGL configuration permits intervals 0..10. Interval zero
measured 29.96 fps over 600 frames with identical game state and viewport;
this did not reveal a large presentation cap, so normal presentation retains
interval one. Evidence: `20261117-010452-efb5ec`.

The benchmark uploader also ignores disappearing /proc entries while finding
old jobs. `VF2_CHECKPOINT_DUMP=path` on the checkpoint verifier writes the core
state plus section offsets for diagnosing timing differences. No firmware,
clock or voltage settings changed. Audio remains deferred.

Next profiling work should resolve the i960 JIT samples to guest functions
using the already-generated native label addresses (rather than counting all
JIT samples as one bucket), and distinguish renderer CPU/driver time from GPU
execution now that simulation has become faster. The latest 600-frame profile
still attributes roughly 26% of simulation samples to TGP, 23% to board/buses,
20% to native i960 code and 18% to geometry.

The final default `--fast` smoke/profile run (`20261117-010536-6d887f`) uses
the selected lazy clock path automatically and measured 29.25 fps with a
29.13 ms simulation task. It preserves the 600-frame reference hashes and
viewport. Thus short full-frame measurements vary around 29?30 fps even
though simulation work consistently falls from about 31.7 to 29.1 ms.


## Direct final-buffer packing and render-thread profiles (2026-11-17)

`--profile-render` samples the main rendering thread's CPU clock, excluding
sleeping/fence time and the simulation worker. It is mutually exclusive with
`--profile-simulation`. `analyze-cpu-samples.py --module-symbols MODULE=NM_FILE`
also accepts `nm -D -S -nC` shared-library symbols and only resolves addresses
inside an exported symbol's recorded extent. Internal stripped functions stay
unresolved rather than being attributed to the preceding exported symbol.

The original 600-frame render profile (`20261117-010805-c66be7`) measured
15.094 thread CPU seconds: 20.20% batch assembly, 12.18% packet construction,
23.22% libc memory/runtime functions and 10.57% Mali driver code. The existing
"GPU submit" wall time includes CPU batch assembly and texture/palette work;
it is not a GPU execution timer. The stock driver has no disjoint timer-query
extension, and its utilization counters report zero during rendering.

The new packing path resolves atlas uploads and assigns per-bucket spans on
the GL thread, then lets the renderer workers write final vertices and indices
directly into disjoint spans. It preserves polygon depth, bucket/draw order,
material values and the original floating-point operation order. It retains
work descriptors across frames and caches exact atlas scale/bias values.
The original bucket-copy path remains available with `--no-direct-pack`;
`--fast` now selects direct packing. No shaders, filtering or resolution changed.

`--check-direct-pack` generates both paths and compares all packed vertex bytes,
indices and draw records before drawing. A 120-frame fight passed this check
with reference core `65347bfab44ab67a` (`20261117-011440-17ac5e`).
`docker exec acm-vf2-armv7 sh tools/acm/check-direct-pack.sh` checks array and
indexed paths with normal and mediump shaders, including ownership, ordering,
clipping, transparency and the 16,385-quad index-capacity fallback. All passed.

Same-binary 600-frame runs measured 30.46 fps direct versus 29.99 fps reference
(`20261117-011217-6c8abe`, `20261117-011302-6e2823`). Main-thread CPU time fell
from 15.101 to 14.227 seconds. Reusing descriptors and atlas transforms measured
14.144 CPU seconds / 30.38 fps with a 10.05 ms submit phase
(`20261117-011459-4fd673`); final pixels and reference hashes match.

The 2,400-frame direct run (`20261117-011535-272059`) measured 31.44 fps over
76.348 seconds, versus 30.56 fps for the preceding FIFO/clock build's long run.
Peak RSS was 152,828 KiB. Core `41ab1dc3d8a38d84`, geometry
`5c4a0b75827d6659`, all guest instruction counts and every final viewport pixel
match the reference. The simulation task still takes 28.69 ms; reducing native
i960/TGP execution is necessary for a larger jump toward real time.


## Per-function JIT samples and TGP command attribution (2026-11-17)

CPU profiles now save an allocation-specific `cpu.samples.jit` map from ARM
instruction ranges to guest instruction/function addresses. Label addresses
are sorted by native address before forming non-overlapping ranges. The
analyzer automatically loads this map, rejects overlapping ranges, reports
native i960 function bodies and lists hot guest instructions. C++ helper
callee time remains in the helper's own bucket. Samples now retain halfword
PC resolution, and thread metadata reports dropped hash-table samples.
No guest counters are inserted for ordinary CPU sampling.

`20261117-012130-34e1f1` collected 3,527 simulation-thread samples over 17.644
CPU seconds, with zero drops. The map contains all 45,877 generated guest
instructions. Subsystem shares were 25.97% TGP, 22.97% board/buses/scheduler,
19.11% native i960 bodies and 16.61% geometry. The largest individual i960 body
was `0x16504` at 65 samples / 1.84% of the simulation total; the rest is spread
across many functions. The 600-frame core and geometry match the reference.
The initial map in `20261117-012029-4b413e` used unordered label iteration;
it is explicitly marked invalid and rejected for overlapping ranges. Use the
corrected iteration above, never that initial map, for function attribution.

The bounded `--guest-counts` diagnostic also writes `guest.commands`. It checks
the known microcode's 112-entry direct-branch table at `0x3f..0xae`, then tracks
the active dispatch slot until the dispatcher returns to `0x35`. Slot `ffff`
contains dispatcher work and any initial partial command. The file records
calls, instruction attempts and consecutive same-PC attempts separately;
consecutive attempts may include productive REP work as well as FIFO retries.
These are guest counts, not CPU-time estimates.

The 120-frame trace `20261117-012400-e80dda` accounts for all 8,349,225 TGP
attempts and 124,099 dispatches, with the same game core/geometry as reference:

| Dispatch slot | Entry | Calls | Attempts | Attempt share |
| --- | --- | ---: | ---: | ---: |
| `3a` | `08f6` | 120 | 1,296,669 | 15.53% |
| dispatcher/partial | ? | ? | 1,254,353 | 15.02% |
| `37` | `028b` | 8,040 | 775,800 | 9.29% |
| `4a` | `0cfa` | 240 | 622,180 | 7.45% |
| `6b` | `073e` | 958 | 513,176 | 6.15% |

The next command-level target is slot `3a` (`0x08f6`): identify its input/output
protocol, called subroutines and state/timing effects before replacing it.
Combining this with dispatcher/FIFO work can remove more machinery than a
single scattered i960 function body. No additional frame-rate improvement is
claimed from these diagnostic changes; the last long fast-render run remains
31.44 fps, below the real-time target.


## Command-region trial and DSP CPU-time attribution (2026-11-17)

Disassembly using the [MAME MB86233 decoder](https://github.com/mamedev/mame/blob/master/src/devices/cpu/mb86233/mb86233d.cpp)
identified slot `3a` as three-axis overlap-mask construction for 30 objects.
The decoder and ROM-derived disassembly stay under ignored `build/acm/`.

A native control-flow region covering `0x08f6..0x099d` removed indirect opcode
handler dispatch within that command, preserving every instruction's semantics,
cycle debit, yield point and observable memory operation. 9,408 ARM/QEMU
fixtures compared every entry PC under short/long budgets, arbitrary register
bits, repeats, I/O stalls and aborts against single handlers; complete state,
memories, cycle counts and instruction counters matched. The 20-frame checkpoint
also matched. Hardware 600-frame native/reference runs were 30.11/30.09 fps,
with simulation tasks 29.81/29.67 ms. Both matched reference core, geometry and
pixels. The extra region gave no useful gain and was removed from active
source, generator, Makefile and benchmark flags.

The tested source, fixture checks, executable/symbols, results and a patch
checked against the pre-sampler baseline remain in
`build/acm/command3a-trial/`. Valid paired evidence:
`20261117-013401-b289aa` and `20261117-013451-75cf22`.

The first hardware attempt (`20261117-013112-c7f229`) was OOM-killed; its kernel
log is archived. Old captures, guest traces and JIT maps accumulated in the RAM
filesystem. 410 files with CRC-verified local copies were removed, freeing
21,333,230 bytes. `benchmark-device.py` now CRC-checks each successful download
against the remote file and removes that temporary copy only after archival.
Failed-run files and local evidence are retained. This does not touch stock
firmware or change physical clocks/voltages.

`--profile-tgp-pcs` now enables simulation-thread CPU sampling plus an atomic
DSP location tag in an explicitly separate diagnostic loop. It writes
`cpu.samples.tgp` and `cpu.samples.tgp-commands`; the analyzer reports them
automatically. Samples include time in DSP callees such as bus/FIFO helpers,
not just the flat `mb86233_device` symbol bucket. `ff` covers dispatcher work
and any initial partial command. Other modes do not tag each DSP instruction.

The 600-frame trace `20261117-013935-4eb38b` collected 1,179 inclusive DSP
samples with zero drops. Command shares of those DSP samples were:

| Slot | Samples | DSP CPU sample share |
| --- | ---: | ---: |
| `4a` | 123 | 10.43% |
| dispatcher/partial | 115 | 9.75% |
| `37` | 112 | 9.50% |
| `6b` | 94 | 7.97% |
| `3a` | 73 | 6.19% |

This corrects the instruction-count priority: `3a` has many cheap operations;
it is not the dominant CPU-time command. Tagging has measurement overhead:
30.70 ms per simulation task versus 29.56 ms with tagging disabled in the same
binary (`20261117-014027-815067`). The ordinary run measured 30.29 fps over
600 frames. Both runs and the fresh-process 20-frame verifier preserve the
reference state and pixels. Do not use the tagged run as a speed baseline.

The next target is slot `4a` at `0x0cfa`. Its buffered command stream dispatches
subcommands through the program table at `0x0d08`, loads matrix data, performs
matrix/vector calculations and returns data through `rf2` (output FIFO). The
stream decoder/FIFO boundary is the candidate abstraction for replacing useful
work and coordination together. Removing dispatch alone has now been tested
and did not supply a useful gain. The full-speed objective and deferred audio
remain outstanding; the last long normal run remains 31.44 fps.


## Native matrix arithmetic (2026-11-17)

Two complete DSP matrix routines now have native arithmetic replacements:
`0x0bc3..0x0bf9` (55 instructions / 95 cycles) and
`0x0bfb..0x0c34` (58 instructions / 100 cycles). They compose column-major
3x4 matrices into internal RAM `0x2c..0x37`. The first multiplies the left
translation by the DSP's unit value; the second adds that translation directly.
This replaces their virtual-register/ALU instruction sequence with direct
multiply/add expressions, preserving separate float rounding and the final
DSP registers, flags, PC and instruction/cycle totals.

The generator installs special entry handlers only when the corresponding
user-microcode span matches its recognized SHA-256. Runtime whole-image
revision validation, full remaining cycle budget, side-effect-free direct RAM,
non-overlapping windows/inputs/outputs, base-word zero and repeat/stall guards
are required. Nonfinite inputs/results use the original handlers to preserve
NaN payload behavior. Debugger hooks and DSP-PC sampling use original execution.
Original handlers also handle short slices; no visible state is committed until
the complete native calculation succeeds.

`docker exec acm-vf2-armv7 sh tools/acm/check-native-matrix.sh` passes 40,000
ARM/QEMU differential fixtures against guarded single handlers. Both variants
actually execute (4,582 / 3,875 native fixture calls). Tests include arbitrary
register/RAM bits, exact/short budgets, alias fallback, subnormals, signed zeros,
infinities and NaNs. Four additional mutation/restoration cases confirm that
unknown microcode disables both replacements and restoration re-enables them.
The 20-frame fresh-process checkpoint retains core `d46dd64a8dbc0653` and pixels
`1e01b6bd873de77f`. Evidence: `build/acm/check-native-matrices.log`,
`build/acm/checkpoint-native-matrices.log`.

The first-layout 600-frame same-binary comparison measured 30.45 versus
29.98 fps, with simulation tasks 29.37 versus 29.96 ms. It performed 20,079
native compositions, replacing 1,104,345 DSP instruction steps. Evidence:
`20261117-014832-37d4cd`, `20261117-014913-7efdfc`. Adding the second layout
measured 30.30 fps / 29.50 ms, with the same native call count in this fight
(`20261117-015252-14617e`). Do not infer that both layouts were exercised by
that hardware scene; both are covered by the differential fixture tests.

The final same-binary 2,400-frame comparison:

| Path | FPS | Simulation task | Native compositions |
| --- | ---: | ---: | ---: |
| Native matrices | 31.42 | 28.72 ms | 80,281 |
| Original DSP execution | 31.01 | 29.25 ms | 0 |

Evidence: `20261117-015519-480eaa`, `20261117-015720-c7a263`. Both have core
`41ab1dc3d8a38d84`, geometry `5c4a0b75827d6659`, original guest instruction
counts and identical final viewport pixels. Native peak RSS was 152,852 KiB.
The paired gain is modest and is not a new large frame-rate milestone over
older runs under different conditions. The fast preset now selects these
replacements; use `--no-native-matrix` for the original path. A final default
30-frame smoke run (`20261117-020003-eedbb4`) used 1,095 native compositions
and preserved the reference core `1f0999ee12ec0bd8`.

The next larger target is the per-word FIFO handoff between i960 and DSP.
Investigate bounded cooperative DSP service when an i960 FIFO read blocks or
its write fills the queue, then native command transactions that bypass the
queued halt/resume/timer machinery. Use the original board model as a reference
for game-visible results, command ordering, input and 60 Hz timing while
replacing its internal scheduling mechanism. Full-speed gameplay and deferred
audio remain outstanding; the current native-matrix long run is 31.42 fps.

## Cooperative DSP service experiment (2026-11-17)

`--cooperative-fifo` replaces the i960/DSP queued FIFO halt/resume timers
with bounded, demand-driven DSP execution on the simulation thread.
`--timed-cooperative-fifo` additionally timestamps output readiness and
charges i960 virtual wait cycles. Both remain opt-in; `--fast` retains the
original board scheduler. This is a prototype of a direct coprocessor
interface, not yet a complete native command implementation.

The 600-frame timed trial `20261117-022647-9eea46` measured 32.02 fps and
24.55 ms per simulation task, versus approximately 29 ms for the original
board path. It made 1,648,442 DSP service calls, with maximum queued input
one word. Renderer CPU work and GPU waits now limit the overall gain.
The untimed 30-frame trial `20261117-020719-e5fc7f` retained the reference
geometry hash and identical viewport pixels. Longer trials diverge in combat
decisions and private state. This was initially mistaken for faster game
pacing: the 120-frame guest-count comparison actually has identical counts
for the per-frame (120), per-fighter (240) and other checked game routines
(`20261117-021430-84b38d` versus `20261117-012400-e80dda`). Those counts do
not establish complete behavioral equivalence.

Pending output timestamps are derived runtime state and are not serialized;
loading an original checkpoint initializes existing FIFO words as ready.
Shared DSP RAM writes and input availability do not yet have complete virtual
timing. Keep the experiment separate from the default build configuration
until command ordering, cold start, input, game clock and checkpoint resume
are covered. Output starvation and unbounded DSP service fail explicitly.

Cold-start controller testing is now available without loading a checkpoint:

```powershell
python tools/acm/benchmark-device.py --fast --timed-cooperative-fifo --boot --input-script tools/scripts/start_game.txt --frames 2400
```

The first run (`20261117-023558-4fb8cf`) completed reset, coin/Start and
character selection, reaching Pai versus Lau in Round 1 at timer 18.02.
It completed 2,400 frames without a FIFO abort, with peak RSS 152,836 KiB
and maximum queued input one word. Its 38.21 fps average includes menus;
the final gameplay windows were approximately 31 fps. `shot` directives in
the input script are currently ignored by this frontend; the final screenshot
is archived normally. Audio remains disabled during these benchmarks.

The original-scheduler cold-start reference (`20261117-023720-e23019`)
also reaches Pai versus Lau, Round 1, timer 18.02 at frame 2,400. Both
characters' poses and damage differ, so this validates selected character,
round and countdown progression rather than identical combat. The reference
averaged 37.73 fps including menus and 15.30 ms per simulation task, versus
38.21 fps and 13.41 ms for cooperative service. Its renderer-thread sampling
was enabled, so this is not a clean paired performance benchmark. Both runs
are archived with command lines, binary/script hashes and final captures.

The reference renderer profile contains 5,717 samples over 29.099 CPU seconds
for all 2,400 frames, including menus. Main-thread `packet_into` accounts for
11.81%, direct batch preparation 10.70%, 2D staging 5.42%, and the Mali driver
15.83%. These are CPU samples, not GPU execution time; worker-thread geometry
and packet work are excluded. This reinforces the need to replace CPU texture
packet construction as well as DSP coordination. Keep fight-checkpoint traces
as the relevant gameplay performance comparison.

A separate three-program shader specialization trial preserved final pixels
but measured 32.22 versus 32.02 fps on the timed FIFO path. It was removed;
the source, patch and artifact references remain under
`build/acm/blend-shader-trial`. Disabling presentation synchronization measured
32.36 fps and was not selected. Full-speed gameplay remains outstanding.

## Shared viewport-clipping trial (2026-11-17)

Preparing one viewport-clipped polygon for all mip bands was tested and
removed. Host and ARM/QEMU packet fixtures preserved the 29,325-vertex
golden stream and byte-identical indexed/nonindexed packets. The normal
600-frame hardware comparison (`20261117-024118-5c7dd6` versus
`20261117-024149-2824fa`) measured 30.70 versus 30.55 fps with identical
core/geometry hashes and viewport pixels. With timed cooperative FIFO service,
the same-binary render-thread-profile comparison measured 32.42 versus
32.16 fps, but CPU packet preparation was effectively unchanged at 6.46
versus 6.49 ms. Final viewport pixels and machine/geometry hashes were
identical there too (`20261117-024236-6ed5f5` versus
`20261117-024323-27083a`). This does not justify another active path.

The prototype, apply-checked patch, build log and results remain under
`build/acm/shared-view-clip-trial`. Original packet construction is restored.
The larger rendering experiment should move wrap/mirror cell handling to
native per-texture GPU sampling and measure the increased texture-pair draw
count against the eliminated CPU splitting. Atlas coordinates currently
require that splitting to preserve the Mali interpolator-to-sampler path;
merely changing shader branching or clipping reuse has not removed the cost.

## Native GPU repeat/mirror trial (2026-11-17)

An individual-texture path was implemented and tested on the Mini, then
removed. Power-of-two wrapping tiles used GLES repeat/mirrored-repeat and
direct interpolator-to-sampler UVs. This eliminated CPU cell splitting for
eligible polygons while retaining mip-band clipping, indexed filtering,
palette lookup, painter-depth ordering and the atlas fallback. A broader
variant also repeated textures whose arcade filtering clamps cell edges;
that variant deliberately approximates filtering at those boundaries.

All runs below are 120 fight-checkpoint frames with renderer-thread CPU
sampling, so their averages include the initial upload/capture costs and
are not the established long gameplay benchmark:

| Path | FPS | CPU packets | GPU submission CPU/wall phase | Final draw calls |
| --- | ---: | ---: | ---: | ---: |
| Atlas reference | 27.69 | 7.44 ms | 12.04 ms | 39 |
| GPU wrapping tiles only | 27.85 | 6.86 ms | 11.15 ms | 44 |
| GPU repeat for all power-of-two tiles | 23.60 | 5.33 ms | 18.29 ms | 366 |

Evidence: `20261117-024900-d52a03`, `20261117-024716-b809fb`,
`20261117-024816-addb23`, respectively. All retain the reference core
`65347bfab44ab67a` and geometry `7db96aa7cf93a2fa`. Relative to the atlas
capture, wrapping-only pixels are 84.7021% identical / 97.5531% within eight
channel values; the broader variant is 78.0876% / 96.7044%. GPU repeat changes
Mali UV interpolation and the broader variant changes seam filtering, so
neither has pixel-equivalent rendering. Moving the work to native texture
sampling saves packet work but loses more to texture-pair draw submission.
It is not selected for the port.

`build/acm/native-wrap-trial` retains the source, apply-checked feature patch,
matching ARM binary/symbols, logs and results. The original atlas is restored.
The new patterned repeat/mirror regression remains in `check-gles2.cpp`:
negative and positive UV cells, both axes and all four mirror combinations
are checked against analytic texel colours/ownership under normal and forced
mediump Mesa shaders. Direct/reference vertex packing is also checked.
The next renderer design must preserve batching while reducing packet work;
individual textures do not meet that requirement on the stock Mali driver.

## Repeated atlas and source-line profiling (2026-11-17)

A 2x2 repeated/mirrored tile atlas was also implemented and removed. It
retained batching while doubling CPU cell boundaries for wrapping textures
up to 256x256. Patterned repeat/mirror and direct-packing fixtures passed.
The 120-frame hardware trial (`20261117-025310-96db77`) retained core and
geometry hashes, but measured 27.41 fps / 7.22 ms CPU packet preparation,
versus the preceding atlas reference's 27.69 fps / 7.44 ms. Final vertices
fell from 6,785 to 6,205, while atlas pages increased from eight to eleven
and uploaded bytes from 3,889,984 to 5,438,848. This did not justify keeping
another renderer path. Source and an apply-checked patch are archived under
`build/acm/repeated-atlas-trial`.

ARM builds now retain minimal source-line debug information in the local
unstripped ELF; the uploaded binary is still stripped. Profiling iterations
archive the matching ELF as `symbols.elf`, alongside the existing symbol map.
`analyze-cpu-lines.py` resolves sampled ARM PCs, accounting for mapping offsets
and Thumb addresses, and can attribute inlined STL operations to the nearest
project call site. Shared-library samples remain in percentage denominators;
missing executable line information is reported explicitly. The relocation,
Thumb-alias aggregation and denominator check is archived under
`build/acm/line-profile-check`.

```powershell
python tools/acm/analyze-cpu-lines.py build/acm/hardware/iterations/20261117-025550-eecb8c/cpu.samples --elf build/acm/hardware/iterations/20261117-025550-eecb8c/symbols.elf --project-root /work/vf2-recomp/ --addr2line "docker exec -i acm-vf2-armv7 arm-linux-gnueabihf-addr2line"
```

That 600-frame reference trace retains the golden core/geometry signatures
and contains 2,735 renderer-thread CPU samples. Project call-site attribution
places 146 samples (5.34%) in the spare-capacity walk and 132 (4.83%) in the
nonempty mip-band search. Those serial vector-descriptor walks, rather than
just the clipping arithmetic, are the next measured target. Other threads
and GPU execution are not included in these percentages.

## Cached packet occupancy and spare storage (2026-11-17)

Workers now compute a nonempty-band bit mask and spare-capacity byte count
while packet vectors are already hot. The GL thread sums one cached count
per polygon and iterates only active mip bands instead of walking all vector
descriptors twice. The existing four-MiB spare-storage trim remains enforced
each frame; geometry, palette/atlas data and packed vertices are unchanged.
The fast preset selects `VF2_COMPACT_PACKETS`; use `--no-compact-packets`
with `benchmark-device.py --fast` for the original descriptor walks.

`check-direct-pack.sh` passes with compact metadata for indexed and array
batches, forced-mediump and ordinary Mesa shaders, repeated/mirrored textures
and 16-bit capacity overflow. The Mini's 120-frame comparison mode also
compares packed vertex/index/draw buffers against the original assembly every
submit (`20261117-025841-dda1db`), retaining the reference state hashes and
identical viewport pixels.

The same-binary, 600-frame timed-cooperative-FIFO comparison:

| Path | FPS | CPU packet preparation | Submission phase |
| --- | ---: | ---: | ---: |
| Cached metadata | 33.14 | 5.25 ms | 9.00 ms |
| Original scans | 32.27 | 6.32 ms | 9.71 ms |

Evidence: `20261117-025912-00a5eb` and `20261117-030003-61b22e`.
Both have core `2cf7070a4c237908`, geometry `92b9afd5ceb58513`, identical
final viewport pixels and the same draw/vertex counts. Both enable
renderer-thread CPU sampling. This modest gain is retained; it does not
validate the separate experimental FIFO timing model or achieve full speed.
The compact-metadata result/source-profile summaries are archived under
`build/acm/compact-packets-trial` and the corresponding hardware artifacts.

The paired traces measured 12.667 versus 13.823 renderer-thread CPU seconds
over 600 frames, a saving of approximately 1.93 CPU ms/frame. The final default
fast-preset run with the original board scheduler completed 2,400 frames at
31.43 fps, 28.95 ms per simulation task and 152,852 KiB peak RSS
(`20261117-030152-ce506f`). It retains core `41ab1dc3d8a38d84`, geometry
`5c4a0b75827d6659` and identical final viewport pixels to the earlier native
matrix long run (`20261117-015519-480eaa`). Overall speed on the original
scheduler remains essentially unchanged because simulation still limits it.
The selected source/patch and result record are under
`build/acm/compact-packets-trial`; the FIFO experiment remains opt-in.

## Atlas source-page validation (2026-11-17)

Cached tiles now retain the revisions of the source pages actually visited
by their packed-word snapshot. When a texture bank's generation changes,
unchanged page revisions allow reuse without allocating/copying/comparing
all source words. Changed pages still use the existing exact word comparison
and decode/upload path; unchanged words avoid a texture upload. The revision
array's identity must match before stamps are trusted. Missing or replacement
metadata uses source-word validation. The fast preset selects this path;
`--no-atlas-page-cache` restores the original word snapshots.

New GLES fixtures cover unrelated-page reuse, actual changed-page pixel
updates, revision changes with identical data, replacement revision arrays,
missing metadata and restored metadata. Both page and word paths pass with
indexed/array batches, normal/forced-mediump shaders, reference packing,
texture transparency and repeat/mirror fixtures.

The same-binary 600-frame timed-FIFO comparison retained identical final
pixels, core `2cf7070a4c237908` and geometry `92b9afd5ceb58513`:

| Path | Renderer CPU seconds | Source snapshots | Source words read | FPS |
| --- | ---: | ---: | ---: | ---: |
| Source-page validation | 12.133 | 2,206 | 390,110 | 33.19 |
| Original word snapshots | 12.620 | 11,264 | 7,174,271 | 33.26 |

Evidence: `20261117-030734-a9e667` and `20261117-030938-8bec7b`.
The CPU saving is approximately 0.81 ms/frame; no overall frame-rate gain
was measured. Early frames have no page hits because the existing bank
generation check already handles them. The skips occur during later texture
updates. GPU submission averaged 8.41 versus 8.98 ms; both traces sampled
only the renderer thread and had no dropped samples.

The final fast-preset original-scheduler run (`20261117-031059-7f7220`)
completed 600 frames at 30.35 fps with 7,374 page skips. Core
`277230da285bb5f5`, geometry `e0c188d55df8df32` and final viewport pixels
match the original reference. The selected source, reverse-checked patch,
fixture/build log and results are under `build/acm/atlas-page-cache-trial`.
Full-speed gameplay and the separate FIFO timing work remain outstanding.

## Shader constant trial and flat-colour diagnostic (2026-11-17)

Moving quantized per-polygon LOD division and inverse microtexture scale
from the fragment shader to the vertex shader was implemented and tested,
then removed. New adjacent-mip fixtures cover eight fractional blend values;
microtexture fixtures cover all four usual scale selectors with distinct
source-sheet indices and analytically expected palette colours. Original
and modified shaders pass indexed/array, normal/forced-mediump and reference
packing fixtures. These blend regressions remain in `check-gles2.cpp`.

The 600-frame same-binary timed-FIFO hardware pair measured 33.20 versus
33.01 fps, with identical viewport pixels and state hashes. Queue waits were
6.35 versus 6.12 ms/frame; this did not supply a useful gain. Evidence:
`20261117-031444-be7715` and `20261117-031619-9b1e8b`. Source and an
apply-checked patch are under `build/acm/vertex-lod-constants-trial`.

`--flat-fragment-probe` is a diagnostic only. It removes all 3D texture,
palette, checker and transparency-discard work, emitting flat polygon colours.
CPU geometry/texture-packet construction, uploads and draw submissions still
run. The shader compiler can also remove now-unused vertex attributes, and
occlusion differs without discard; this is a coarse rendering bound rather
than an isolated measurement of fragment ALU time. The flag is never selected
by `--fast` and intentionally produces incorrect graphics.

The 600-frame timed-FIFO probe (`20261117-031800-cefd1a`) reached 36.38 fps,
with queue waits down to 0.06 ms/frame and simulation tasks at 24.38 ms.
Draw counts, packed vertex counts, core `2cf7070a4c237908` and geometry
`92b9afd5ceb58513` are unchanged. Even this reduced workload remains below
full speed, so pixel-shader changes alone cannot account for the remaining
gap in the current pipeline. CPU simulation and render preparation remain
necessary targets. This result is diagnostic and is not a gameplay milestone.

The same-binary textured reference after the probe completed at 33.18 fps
and 6.40 ms/frame queue wait (`20261117-032017-71b0e3`). It retains the
same core/geometry hashes and final viewport pixels as the earlier normal
shader. Textured rendering is restored; the probe remains opt-in. Its source,
reverse-checked patch and paired result record are under
`build/acm/flat-fragment-probe`. The next simulation target is the experimental
FIFO service boundary: it still enters DSP execution 1,648,442 times in this
600-frame scene. Bounded servicing at input-full/output-read boundaries can
be tested against the existing per-word path before replacing whole commands.

## Bounded cooperative input batching (2026-11-17)

`--fifo-batch 8` with `--timed-cooperative-fifo` defers DSP service until
eight input words are queued, an output is needed, the i960 accesses shared
buffer RAM, or its CPU slice ends. The threshold defaults to one; batching
and the FIFO backend remain explicit experiments. Thresholds 1/2/4/8 are
accepted for investigation. Shared-buffer pages are removed from the JIT's
direct-page map only for a batched cooperative run, so both reads and writes
pass through dependency barriers. Partial batches flush before timer callbacks
can parse a frame. DSP budget, queue-overflow and output-starvation guards
remain active. Output timing still inherits the prototype's limitations.

The same-binary 120-frame comparison with simulation-thread sampling:

| Path | DSP entries | Simulation task | Simulation CPU seconds |
| --- | ---: | ---: | ---: |
| Threshold 8 | 73,168 | 23.87 ms | 2.850 |
| Eager threshold 1 | 337,305 | 25.13 ms | 3.000 |

Evidence: `20261117-032616-e0d614`, `20261117-032921-8bd642`.
Both retain geometry `d893e598f2968cbd`, 11,193 native matrix compositions
and identical final viewport pixels. Private core/work hashes and virtual
wait counts differ. The reduction removes empty-input retry instructions as
well as run-entry overhead; it is approximately 78% fewer DSP entries and
1.25 CPU ms/frame saved in the matching segment.

The batched 600-frame run (`20261117-032720-cd2eba`) completed at 33.00 fps
and 23.66 ms per simulation task, with 350,448 DSP entries and maximum queued
input eight words. Combat diverges after the initial matching segment, so
this is not a matched whole-run speed comparison. Its simulation-thread
profile has 2,829 samples over 14.203 CPU seconds: TGP 25.17%, geometry
24.18%, i960 native code 22.59%, and board/buses/scheduler 11.81%.
`model2_3d_process_polygon<4>` and `geo_parse_np_s` account for 9.90% and
8.80% respectively; those high-level geometry paths are substantial remaining
CPU targets. Full function attribution is archived with that trace.

The cold-start script completed 2,400 frames without an abort
(`20261117-033048-f08d89`), selecting Pai and reaching Pai versus Lau,
Round 1, timer 18.02, matching the prior cold-start clock/selection check.
Combat poses/damage differ. It used 569,250 DSP entries and queued at most
eight input words. The 39.07 fps average includes menus and is not a new
gameplay performance milestone. The ordinary fast preset then passed the
30-frame reference smoke (`20261117-033334-efe599`), retaining core
`1f0999ee12ec0bd8` and geometry `f5abdd014bf463be`.

The opt-in source, reverse-checked patch, build log and result record are under
`build/acm/fifo-batch-trial`. Native full-speed gameplay and robust cooperative
timing/checkpoint serialization remain unfinished.

## Compact native geometry handoff (2026-11-17)

The fast preset now keeps camera geometry in a 176-byte owned record instead
of a 300-byte decoded record. Original 16-bit UVs and texture headers remain
packed; positions, planes and centres retain float precision. Palette,
lighting/build-colour and CRTC state resolve before the next simulation starts.
The compact records cross the ownership boundary intact, and the existing
geometry workers expand and project them. No worker reads mutable board RAM.
The original record path remains available with `--no-compact-camera`.

An initial 164-byte internal-only version saved simulation work but expanded
on the render thread, increasing capture cost from 1.98 to 2.78 ms. That
intermediate design is retained only under `intermediate164` in the trial
archive. The selected 176-byte handoff also carries resolved palette data and
full-precision centres, moving expansion into the parallel stage.

The ARM/QEMU independent-renderer comparison passes 177,125 polygons over
120 fight frames, comparing meaningful projected/material fields exactly,
visible-polygon feedback and ownership across the entire next board frame.
Additional synthetic coverage checks compact clipping, window/depth/link
ordering, hidden windows and the first-32,767-survivor limit. The checker now
retains `-g1` like the frontend, preserving source-line metadata in shared
objects. Evidence: `build/acm/compact-camera-trial/check-worker-handoff.log`.

The same-binary 600-frame pair using the experimental timed FIFO with batch
threshold eight and renderer-thread sampling:

| Path | FPS | Simulation task | Capture | Projection |
| --- | ---: | ---: | ---: | ---: |
| Compact handoff | 33.83 | 22.49 ms | 1.33 ms | 3.41 ms |
| Original records | 33.12 | 23.14 ms | 1.96 ms | 3.42 ms |

Evidence: `20261117-034909-9616a0`, `20261117-035033-45b4f2`.
Renderer CPU time is 11.912 versus 12.512 seconds over 600 frames, about
1.00 CPU ms/frame saved on that thread. Both retain core `70c397a10113fa5a`,
geometry `201401334f195582`, draw/vertex counts and identical final pixels.

The final default fast-preset pair uses the original board scheduler and no
CPU sampling, over 2,400 identical fight frames in the same binary:

| Path | FPS | Simulation task |
| --- | ---: | ---: |
| Compact handoff | 32.75 | 27.82 ms |
| Original records | 31.44 | 28.89 ms |

Evidence: `20261117-035140-f5714a`, `20261117-035504-fede7e`.
Both retain core `41ab1dc3d8a38d84`, geometry `5c4a0b75827d6659`, guest
instruction counts, 80,281 native matrix compositions and identical final
viewport pixels. Peak RSS was 152,864 KiB for both; the record-size reduction
is not a claim of measured whole-process RSS savings. This gain is selected
independently of the experimental FIFO paths, which remain opt-in.

The source, reverse-checked selected patch, intermediate prototype, verifier
and build logs and result record are under `build/acm/compact-camera-trial`.
The full-speed objective remains outstanding. Remaining geometry parsing
and model transforms can now feed a smaller native handoff representation.


### Bounded geometry reader trial (rejected)

`build/acm/bounded-geometry-trial` preserves the opt-in prototype and patch.
It validated object spans once for immutable ROM, then omitted per-word bounds
checks in the specular-normal parser. ARM/QEMU matched 177,125 projected
polygons, and the hardware pair matched complete state and final pixels.
However simulation CPU time was 13.6004 versus 13.6377 seconds over 600 frames,
only 0.062 ms/frame saved. The parser/cache complexity was removed.
Evidence: `20261117-041117-84eb36`, `20261117-041256-328288`.


### Direct native polygon submission

The specular-normal object parser can now submit complete polygons directly,
without re-entering the emulated rasterizer command-port state machine for
attributes, lighting and point groups. It retains the original precision
truncation, command buffer contents, texture address advances, depth/link
updates and termination state. Entry requires a complete polygon command at
its expected initial field index; other cases retain the original parser.
The fast preset selects this path; `--no-direct-geometry` selects the reference.
This removes transport overhead, not the geometry transforms or all emulated
hardware state.

ARM/QEMU compares all serialized geometry registers/RAM on every verification
frame, 177,125 projected polygons, visible-polygon feedback and ownership
across the next board frame. The independent legacy path retains command-port
submission. It passes with 452,225 direct polygon submissions. Evidence:
`build/acm/direct-geometry-trial/check-registers.log`.

The same-binary 600-frame timed-FIFO/batch-eight pair measured 13.1990 versus
13.6933 simulation CPU seconds (0.824 CPU ms/frame saved), and 33.99 versus
33.45 fps. Evidence: `20261117-041906-c255ed`, `20261117-041937-f90386`.
The normal scheduler, without sampling, measured 33.04 versus 32.68 fps over
2,400 identical fight frames, with simulation tasks at 27.29 versus 28.09 ms.
Evidence: `20261117-042101-236575`, `20261117-042237-3abe43`.
Both retain core `41ab1dc3d8a38d84`, geometry `5c4a0b75827d6659`, guest
instruction counts and exactly identical final viewport pixels. The direct
path submitted 8,464,700 polygons. The selected source, reverse-checked patch,
binary, symbols and results are in `build/acm/direct-geometry-trial`.


### Skip unused material reads for rejected polygons

The native camera path determines culling before fetching texture coordinates,
texture headers and log-RAM LOD adjustments. Rejected polygons still advance
both texture addresses and update depth and link registers exactly as before.
The legacy renderer retains the full material-fetch reference. The fast preset
selects this optimization; `--no-early-material` disables it.

The ARM/QEMU 120-frame independent comparison passes all geometry registers
and RAM, projected/material fields, feedback and next-frame packet ownership.
The same-binary timed-FIFO/batch-eight pair saved 0.409 simulation CPU ms/frame:
12.9619 versus 13.2073 seconds over 600 frames. Both measured 33.84 fps and
matched full state and final pixels. Evidence: `20261117-042427-4dc098`,
`20261117-042507-f85fbc`.

The final same-binary normal-scheduler pair retained core `277230da285bb5f5`,
geometry `e0c188d55df8df32` and identical final viewport pixels. Simulation
tasks measured 27.07 versus 27.44 ms, while overall FPS measured 32.26 versus
32.48. This is a CPU-work reduction, not a claim of improved overall FPS.
Evidence: `20261117-042635-efe803`, `20261117-042712-1ca223`.
Sources, patch, build/check logs and result record are retained under
`build/acm/early-material-trial`.


### Bulk geometry transform trial (rejected)

The ROM-object prototype decoded immutable normal/vertex streams into cached
arrays and transformed four independent vertices with ARMv7 NEON. It retained
scalar operation order and translation masks, guarded matrices on each use,
cached safe-input groups, and used scalar arithmetic for exceptional operands.
RAM objects and truncated ROM records retained the original parser. Logical
cache entries were bounded by four MiB of vector capacity.

ARM/QEMU arithmetic checks passed 680,000 random/cancellation vertices in both
guarded and trusted modes (1,280,000 SIMD vertices), exceptional values, signed
zero, subnormal cached groups, truncated objects and explicit-count changes.
The independent 120-frame renderer comparison passed all serialized geometry
registers/RAM, 177,125 projected polygons, feedback and next-frame ownership.

Hardware was exact but slower. The final same-binary timed-FIFO/batch-eight
pair measured 22.53 versus 21.55 ms per simulation task and 13.5229 versus
12.9332 simulation CPU seconds over 600 frames. Overall FPS was 33.51 versus
34.02. Both retained core `70c397a10113fa5a`, geometry `201401334f195582` and
identical final viewport pixels. The bulk path handled 48,606 objects and
6,281,313 points, including 6,157,952 SIMD points. The extra transformed-array
pass outweighed arithmetic savings, even after caching individual SIMD-group
bounds and hoisting the consumer pointer. Evidence: `20261117-043816-be2ac2`,
`20261117-043854-3ee164`; initial pair `20261117-043508-c8e952`,
`20261117-043552-38239b` showed the same regression.

The prototype, reverse-checked patch, arithmetic tests, earlier variant, line
profiles, symbols and results are archived in `build/acm/bulk-geometry-trial`.
It was removed from active sources. The restored frontend matches the prior
selected SHA-256 `623159e01d9335a07bf834d82f09e29f278e5099aa07d47b398671a2bfda6c3b`.


### Native multiword memory trial (rejected)

The prototype emitted ldl/stl, ldt/stt and ldq/stq directly for aligned accesses
contained in one plain-RAM page. Other accesses retained the bus helpers,
including burst flags and stalled transfers. It kept exact destination-group
alignment, address-before-register-write ordering and instruction cycle costs.

The independent ARM/native-versus-MAME fixture passed 18,432 cases: all six
transfers, every destination field, three addressing modes, register overlap,
page edges, unaligned/wrapped addresses, MMIO burst/nonburst behavior, partial
stalls/resume and both short and multi-instruction budgets. The fresh-process
fight checkpoint retained core `d46dd64a8dbc0653` and pixels
`1e01b6bd873de77f`.

The same-binary 600-frame hardware pair was exact but showed no useful gain:
21.83 versus 21.77 ms per simulation task, 13.0984 versus 13.0319 simulation
CPU seconds, and 33.68 versus 33.98 fps. JIT code grew from 4,401,940 to
4,485,228 bytes. Both retained core `70c397a10113fa5a`, geometry
`201401334f195582` and identical final viewport pixels. Evidence:
`20261117-044704-8d7ca1`, `20261117-044759-1875ba`.
The prototype, independent fixture, build/check logs, symbols, result record
and reverse-checked patch are under `build/acm/native-multi-trial`.
All three original source files were restored byte-for-byte, and the rebuilt
frontend was re-uploaded and verified against the 30-frame state/pixel golden.
The rebuilt binary hash is recorded in that trial's result record.

The next recompiler target is repeated PC bookkeeping. `gen::insn` stores PIP
and next IP before every guest instruction, including pure arithmetic. Such
instructions can potentially defer those stores until an external helper or
slice exit. The optimization must preserve the PC observed by helpers, faults,
interrupts, debugger hooks and complete per-slice state; branches and device
accesses should retain eager bookkeeping until explicitly covered.


### Native i960 bookkeeping (selected)

The fast preset now selects `--lazy-pc` and `--register-cycles`.
`--no-lazy-pc --no-register-cycles` restores the previous bookkeeping.
Normal integer operations (test, integer arithmetic/compare/move and lda)
commit PIP/IP before helper fallback, leaving compiled code, or in a cold
cycle-exit stub. Branches, memory accesses and interrupt-sensitive operations
retain eager PCs. The remaining cycle budget lives in callee-saved S1 during
native execution; it is committed/reloaded around helpers (including spin and
indirect-dispatch helpers) and committed before returning to the scheduler.
Device callbacks therefore observe the same PCs and elapsed cycles.

`check-i960-pc.sh` passes 102,665 native-versus-MAME cases in each of three
configurations: PC deferral only, register cycles only, and both. Four slices
per case compare full CPU state, cycles returned, RAM, device-observed PCs and
elapsed cycles. Coverage includes helper fallbacks, eight-byte instructions,
plain/signed/unaligned/page-edge loads, branches, stalls/resume, explicit
abort_timeslice, stable RAM polling/spin helpers, debugger hooks and an
illegal-instruction exit. Fresh-process game checkpoint verification retains
core `d46dd64a8dbc0653` and pixels `1e01b6bd873de77f`.
Evidence: `build/acm/lazy-pc-trial/check-aborts.log`, `checkpoint-cycles.log`.

The same-binary 600-frame timed-FIFO/batch-eight pair measured 20.76 versus
21.71 ms per simulation task, and 12.4420 versus 13.0054 simulation CPU seconds
(0.939 CPU ms/frame saved). Overall FPS was 33.70 versus 33.59. JIT code shrank
from 4,401,940 to 4,063,424 bytes (7.69%). Core `70c397a10113fa5a`, geometry
`201401334f195582`, instruction counts and final pixels are identical.
Evidence: `20261117-050546-51d1ec`, `20261117-050650-3915cc`.

The normal scheduler pair, without CPU sampling, measured 33.31 versus 33.09
fps over 2,400 identical fight frames, with simulation tasks at 26.21 versus
26.90 ms. Both retain core `41ab1dc3d8a38d84`, geometry `5c4a0b75827d6659`,
all guest counts, 80,281 native matrix calls, 8,464,700 native polygon
submissions and exactly identical final viewport pixels. Evidence:
`20261117-050833-95b322`, `20261117-051029-bbdf46`.

An arithmetic-only intermediate saved 0.56 simulation ms/frame. Extending PC
deferral into loads did not improve that timing and grew JIT code to
4,735,482 bytes, so that extension is archived and not selected. Cold PC
stubs have explicit non-overlapping profile intervals; the final combined
map has 61,356 intervals for 45,877 guest instructions. Sources, variants,
reverse-checked patch, tests, symbols and results are in
`build/acm/lazy-pc-trial`.

The new normal-scheduler renderer profile (`20261117-051227-35ba18`) measured
12.461 CPU seconds over 600 frames. About 15.49% of main-thread CPU samples
are packet generation, 15.86% are direct batch preparation and its packing
worker, 5.43% atlas lookup/validation, 21.94% libc, and 11.68% libMali.
Full symbols and source-line attribution are archived alongside the trial.
The host currently drains the GPU fence before set_rams and submit; submit
still performs CPU palette/atlas/batch preparation. Its 8.84 ms/frame label
is not a pure GPU execution measurement. The next renderer change should
separate CPU planning from GL uploads, allowing material and batch preparation
to overlap the prior GPU frame while retaining the fence before mutation of
textures still in use. Existing set_rams can clear/delete/decode GL resources,
so it cannot simply be moved ahead of the fence unchanged.


### CPU material planning before the GPU fence (selected)

The fast preset selects `--stage-materials`; `--no-stage-materials` retains
the original post-fence preparation. `gpu3d::prepare_frame_cpu` is a CPU-only
phase with the immutable RAM snapshot as input. Its default implementation
retains the old geometry-only behavior for other backends. GLES batches now
resolve palette rows, validate/decode atlas tiles, allocate page metadata and
pack final VBO/IBO data before the host waits for its prior-frame fence.
GL page allocation, texture updates and retirement of old textures occur
only in submit after that wait. Old GPU texels remain intact during planning.
The atlas keeps its 12 MiB page limit and frees retired pages before creating
replacements. Planning/allocation failures retain the original fallbacks;
index overflow can still select array batches, and mismatching submit input
invalidates the staged geometry plan. Individual-tile fallback textures also
have deferred retirement when a subsequent staged frame changes their source.

`check-staged-materials.sh` passes highp/mediump and indexed/array cases with
direct-versus-reference packed-buffer comparison. It checks CPU-only GL error
and binding preservation, real atlas texture samples before/after flush,
retired texture lifetime, cancelled allocation and capacity, texture/luma/
colour/gamma and RAM-pointer changes, palette updates, individual-tile LOD
fallback, mismatched geometry, and the large indexed-scene fallback. Evidence:
`build/acm/staged-materials-trial/check-atlas-boundary.log`, `check-fallback.log`.
The Mini's 120-frame packed-buffer check retains core `65347bfab44ab67a` and
geometry `7db96aa7cf93a2fa`: `20261117-053134-e042a5`.

The same-binary 600-frame timed-FIFO/batch-eight pair, with main-thread CPU
sampling, measured 38.75 versus 34.08 fps (13.70% higher). Fence wait fell from
7.87 to 2.70 ms/frame, and post-fence submit from 8.39 to 3.60 ms/frame. The
CPU-prepare label grows from 5.30 to 11.02 ms because it now includes material
and batch planning. This is overlap, not reduced total CPU work: main-thread
CPU time was 12.5513 versus 12.0481 seconds over 600 frames. Both retain
core `70c397a10113fa5a`, geometry `201401334f195582`, all guest/render counts,
4,801,480 atlas upload bytes, 153,324 KiB peak RSS and identical final pixels.
Evidence: `20261117-053219-59377f`, `20261117-053332-17f0b2`.

The normal-scheduler pair, without sampling, measured 34.79 versus 33.38 fps
over 2,400 identical fight frames. Fence wait fell from 6.88 to 0.06 ms/frame;
simulation task time was 26.45 versus 26.11 ms. Both retain core
`41ab1dc3d8a38d84`, geometry `5c4a0b75827d6659`, 197,828,015 TGP instructions,
80,281 native matrix calls, 8,464,700 native polygon submissions, 152,628 KiB
peak RSS and identical final viewport pixels. Evidence:
`20261117-053558-365e6e`, `20261117-053925-d31024`.
The cooperative FIFO remains opt-in; this renderer improvement is selected
independently. The default is now chiefly waiting on simulation rather than
the prior-frame GPU fence. The full-speed objective and deferred audio remain
outstanding. Sources, reverse-checked patch, tests, symbols and results are in
`build/acm/staged-materials-trial`.


### Exact ARM scheduler difference and ordering (selected)

`--scheduler-gaps` is selected by the fast preset; `--no-scheduler-gaps`
retains the prior scheduler comparisons. `scheduler_gap.h` computes the
128-bit clock difference and its unsigned ordering in one four-word ARM
subtraction with a carry output. Most nonnegative gaps then need only the
64-bit period comparison. Absolute time, periods, cycle budgets, timer order
and CPU order retain their original values. Backwards clocks, large gaps and
both limb and full-width wrap boundaries have no narrowed-time assumption.
Other architectures use the original full-width operations.

`check-scheduler-gap.sh` verifies 2,200,726 cases each on host native128,
host portable128 and ARMv7: all subtraction limbs, backwards clocks, near-full
wrap, random full-width clocks and exact period boundaries. The fresh ARM
checkpoint retains core `d46dd64a8dbc0653` and pixels `1e01b6bd873de77f`,
including instruction-hook enable/remove with 979,337 hook calls.

The same-binary normal-scheduler 600-frame simulation-profile pair measured
16.0070 versus 16.2659 CPU seconds (0.431 CPU ms/frame saved), 26.68 versus
27.15 simulation ms/task, and 33.88 versus 33.45 fps. Both retain core
`277230da285bb5f5`, geometry `e0c188d55df8df32`, all guest counts and exactly
identical viewport pixels. Evidence: `20261117-055140-1cf49d` and
`20261117-055212-66461c`.

The final-source 2,400-frame pair without CPU sampling measured 34.90 versus
34.75 fps, and 26.37 versus 26.45 simulation ms/task. Both retain core
`41ab1dc3d8a38d84`, geometry `5c4a0b75827d6659`, 197,828,015 TGP cycles,
80,281 native matrices, 8,464,700 native polygons and identical final viewport
pixels. Evidence: `20261117-055330-158858`, `20261117-055456-9a6c0a`.
This is a small retained CPU improvement; the full-speed objective requires
larger gains. Sources, reverse-checked patch, symbols and results are archived
under `build/acm/scheduler-gap-trial`.

Normal-scheduler DSP command sampling `20261117-055644-1e8c10` collected 1,161
DSP samples: command 0x37 11.80%, 0x4a 9.91%, dispatcher 8.44%, 0x3a 6.37%,
0x6b 6.03%, and 0x46 5.08% of tagged DSP samples. Tag stores add overhead
and native matrix replacement is disabled for per-PC visibility; these are
candidate-selection data, not FPS results. The run retains the normal
600-frame core and geometry hashes. No single command dominates DSP work.


### Rejected direct DSP instruction links and page functions

`build/acm/tgp-chain-trial` preserves two measured instruction-dispatch trials.
Both retain precise instruction/cycle boundaries, complete register/RAM state,
external bus timestamps, native matrix entry points, microcode-revision guards
and debugger fallback. One generates direct tail links between 4,096 specialized
functions; the other uses 32 functions with 128 instruction cases and native
fallthrough within each page. Counts accumulate locally and return at most every
64 dispatched instructions. The ARM linked variant has no calls to other link
functions, only tail jumps. Larger-function FP register allocation can commute
NaN operands; a canonical-handler fallback for two non-finite operands fixes the
initial random-state regression. Neither trial is selected.

Both pass 65,536 differential fixtures, three slices per fixture, starting from
every instruction address with random state and bus abort/stall behavior;
96,316 FP edge fixtures; six-path mutation/revision/debugger/resume checks;
and the normal 600-frame Mini core/geometry hashes and exact viewport pixels.
The linked version also passes the fresh-process 20-frame checkpoint.

The same-binary linked 600-frame simulation-profile pair measured 29.42 versus
33.81 fps, 31.40 versus 26.70 simulation ms/task, and 18.8513 versus 16.0351
simulation CPU seconds: `20261117-060737-4cfaa5`, `20261117-060811-7e1d12`.
The page pair measured 30.27 versus 33.64 fps and 30.41 versus 26.85 simulation
ms/task: `20261117-061056-ef09d2`, `20261117-061148-8d0159`.
All retain 38,614,049 dispatches plus the same native matrix bypass counts.
Lower central dispatch samples do not translate into less total DSP time.
Both prototypes, fixtures and patches are archived and removed from the runtime.
Original sources are byte-identical and rebuilt. Further instruction-dispatch
reshuffling is a poor priority; the next native work should eliminate the
instruction-level operations themselves.


### Resumable native DSP matrix arithmetic (selected)

The fast preset now selects `--extended-matrix` and `--resumable-matrix`.
Their `--no-...` forms retain the earlier entry coverage and whole-slice path.
The BFC reverse-composition entry keeps a variable left operand in low RAM;
BFB first sets that operand to 0x20. Both now have native implementations,
with original bounds, output-alias and exceptional-float fallbacks. Their
original totals are 57 instructions/99 cycles and 58/100 respectively; the
forward BC3 entry remains 55/95. Microcode hashes recognize every native entry.

The resumable path computes a complete private matrix result once, then
advances through generated original instruction-cost prefixes across later
scheduler slices. It preserves the one-cycle overshoot of a two-cycle DSP
instruction and counts only the instructions issued so far. It requires a
bus opt-in asserting that the low/high internal RAM is isolated from other
devices between CPU runs; the Model 2 implementation qualifies, generic buses
default to false. It performs no FIFO or shared-memory accesses ahead of time.

While suspended, cached registers/output are private. State save/load, PC
inspection, debugger attachment, reset, program mutation or program-pointer
change materialize the exact intermediate architectural state from the saved
inputs and original constant handlers. Pending execution is then cancelled.
GPIO changes survive this reconstruction. Checkpoint format stays unchanged:
CPU state is materialized before its internal RAM arrays are serialized.
The pending cache is placed after the original hot DSP fields, preserving
compact ARM load offsets for existing instruction handlers.

`check-native-matrix.sh` passes 60,000 independent entry fixtures, including
short budgets, alias/bounds cases, arbitrary float bits and image invalidation;
native fixture calls are 4582/4137/2514 across the three entries.
`check-resumable-matrix.sh` passes 10,500 fixtures and 188,709 exact state/cycle
comparisons, with 27,705 live pending prefixes inspected through independent
CPU/RAM clones without cancelling live execution. It covers every initial
cycle cut, single-cycle continuation, checkpoint save/load, debugger attach/
remove, reset, past/future program mutation, program-pointer identity changes,
GPIO changes, isolation fallback and PC inspection. The reference-counter
configuration also passes. The final fresh-process checkpoint retains core
`d46dd64a8dbc0653`, pixels `1e01b6bd873de77f`, and 979,337 enabled/removed
instruction-hook calls.

Initial-layout, same-binary 600-frame simulation CPU sampling:

| Path | FPS | Simulation ms/task | CPU seconds | Native matrices |
| --- | ---: | ---: | ---: | ---: |
| Extended + resumable | 34.52 | 26.10 | 15.6751 | 71,591 |
| Earlier coverage, whole slices | 33.34 | 27.27 | 16.3622 | 20,079 |
| Extended only | 33.97 | 26.69 | 16.0366 | 37,293 |

The combined path saves 1.145 simulation CPU ms/frame in that pair. All retain
core `277230da285bb5f5`, geometry `e0c188d55df8df32`, every guest count and
identical final viewport pixels. Evidence: `20261117-062616-d4b0d0`,
`20261117-062712-1c35cd`, `20261117-062849-0e5b20`.

The final layout's 2,400-frame normal-scheduler run measured **36.38 fps**
versus **35.04 fps** for the archived preceding selected binary, and 25.22
versus 26.31 simulation ms/task. This comparison includes all new code/layout
costs; the shorter pair above isolates the runtime switches within one binary.
Final and reference retain core `41ab1dc3d8a38d84`, geometry
`5c4a0b75827d6659`, 197,828,015 TGP cycles, 159,891,146 compiled instructions,
8,464,700 native polygon submissions, and exactly identical viewport pixels.
Native matrix computations rise from 80,281 to 286,202, including 136,231
resumable computations. Peak RSS is 152,628 versus 152,624 KiB. Evidence:
`20261117-063428-9d703e`, `20261117-063153-0e506d`. The initial-layout long
run was 35.86 fps (`20261117-063018-d68300`), with the same state and pixels.

The final default-preset 600-frame simulation profile (`20261117-063617-de70c5`)
retains the normal state/geometry hashes. Matrix helpers now account for only
29 of 3,110 samples (0.93%); board/buses/scheduling are 25.95%, remaining DSP
execution 25.43%, i960 JIT 20.19% and geometry 14.41%. Further matrix SIMD is
therefore a small target. Existing detailed DSP PC sampling disables native
matrix execution for visibility, so its command ranking must not be mistaken
for this new native configuration. A coarse command profile that leaves native
kernels active is the next useful DSP measurement. Full-speed play and the
deferred audio work remain outstanding. Sources, reverse-checked patch,
matching symbols, tests and structured results are in
`build/acm/resumable-matrix-trial`.


### DSP command sampling with native kernels enabled

`--profile-tgp-commands` samples only the simulation thread and tags recognized
command-table entries plus the dispatcher. It retains native and resumable DSP
matrix execution. Bit 30 distinguishes command-only tags from instruction-PC
tags, so no false instruction-PC histogram is produced. Tags clear on every
CPU return, including native pending early exits. The command mode is limited
to the compiled individual-handler path; conflicting interpreter/block options
are rejected. Detailed `--profile-tgp-pcs` retains its earlier per-instruction
mode, which disables native matrix bypasses for visibility.

`check-command-samples.sh` verifies 112 command identities, dispatcher/stall
scope exits, all three resumable native entry points and 172 observed DSP
scopes. The enabled mode also passes 188,709 resumable CPU comparisons and
five-path core mutation/debug/resume state regression checks.

The 2,400-frame sample (`20261117-064417-bf292e`) collects 3,791 tagged DSP
samples while retaining 286,202 native matrices and 136,231 resumable calls.
It retains core `41ab1dc3d8a38d84`, geometry `5c4a0b75827d6659` and identical
final viewport pixels. Of tagged DSP time, dispatcher/initial partial context
is 12.82%, command 0x4a 11.50%, 0x6b 7.04%, 0x3a 6.15%, 0x29 5.20%,
0x0c 4.91%, and the former large target 0x37 only 4.17%. Sampling is diagnostic,
not a performance comparison. Source/line reports, matching symbols, tag tests,
source patch and results are archived under `build/acm/native-command-profile`.


### Checked low-limb scheduler comparisons (selected)

The fast preset selects `--modular-clock`; `--no-modular-clock` retains the
previous scheduler. Absolute CPU/timer clocks and their serialized format stay
128-bit. Within an eligible frame, nearby deadlines use signed differences of
their low 64-bit limbs. The initial absolute head deadline reconstructs the
final target once per CPU pass, including low-limb rollover. Short positive
cycle products advance the low clock limb directly; the high limb changes
only on carry. No clocks, cycle budgets or timer ordering are approximated.
The full-width timer queue remains responsible for long future deadlines.

Eligibility requires the halted-auxiliary configuration, no cooperative FIFO,
sound, tracing/logging or external execution hooks, an enabled near-future
frame timer, nearby active CPU clocks, bounded nonzero CPU periods, and no
old expired timer outside the checked window. The full-duration reference
switch also selects the original scheduler. Frame timing bounds all compared
gaps below the signed low-limb half-range; invalid or unusual configurations
fall back to the original full-width code. Derived eligibility/count fields
are excluded from snapshots. The optimized path omits redundant sound/coop/
instruction-hook branches only after those conditions have been checked.

`check-frame-clock.sh` passes 900,196 ordering/window cases, 300,000 target
reconstructions and 600,000 advance/carry cases on each of host native128,
host portable128 and ARMv7. `check-modular-clock.sh` passes 31 explicit guard
and fallback cases, including distant CPU clocks, missing/past/distant frame
timers, old expired timers, long future timers, wide/zero/slow periods, sound,
tracing and large absolute epochs. All 20 fresh-checkpoint frames actually use
the new path and retain core `d46dd64a8dbc0653` and pixels
`1e01b6bd873de77f`. Hook enable/remove retains those hashes with 979,337
callbacks; the full-duration reference configuration also passes.

The comparisons-only intermediate saves 0.543 simulation CPU ms/frame in its
600-frame same-binary pair: `20261117-065649-41f74e` versus
`20261117-065802-41214d`. Adding short clock advances raises the measured saving
to 0.714 CPU ms/frame (15.3075 versus 15.7358 CPU seconds over 600 frames),
25.48 versus 26.23 simulation ms/task and 35.31 versus 34.20 fps. That pair
retains core `277230da285bb5f5`, geometry `e0c188d55df8df32`, all guest counts,
71,591 native matrices and exactly identical viewport pixels. Evidence:
`20261117-070639-aba5f2`, `20261117-070820-ef6106`.

The final-source, same-binary 2,400-frame pair without sampling measures
**36.89 versus 36.03 fps**, 65.067 versus 66.606 seconds and 24.80 versus
25.54 simulation ms/task. Both retain core `41ab1dc3d8a38d84`, geometry
`5c4a0b75827d6659`, 197,828,015 TGP cycles, 159,891,146 compiled instructions,
286,202 native matrices, 136,231 resumable matrices, 8,464,700 native polygons,
152,636 KiB peak RSS and exactly identical final viewport pixels. The selected
run reports 2,403 optimized frames including warm-up. Evidence:
`20261117-071049-c774cc`, `20261117-071212-9c841b`.
The final default-preset smoke (`20261117-071418-b78d67`) reports 33 optimized
frames, core `1f0999ee12ec0bd8` and geometry `f5abdd014bf463be`.
Sources, reverse-checked patch, symbols, tests and structured results are in
`build/acm/modular-clock-trial`. Full-speed gameplay remains outstanding;
the existing cooperative FIFO remains opt-in.


### FIFO timing trace

`sh tools/acm/build-trace-fifo.sh` builds a headless ARM diagnostic using the
same compiled DSP core. Run `qemu-arm -cpu cortex-a7 build/acm/bin/trace-fifo
roms/vf2.zip build/acm/fight.checkpoint 2 OUTPUT.trace` with the desired runtime
environment, then compare two traces using
`python tools/acm/analyze-fifo-trace.py REFERENCE.trace CANDIDATE.trace`.
Each record includes completed i960 FIFO traffic, instruction PC, virtual time,
DSP scheduled cycles and clock phase. Diagnostic callbacks select the existing
wide scheduler in both modes; these runs are not performance measurements.

The two-frame investigation in `build/acm/fifo-clock-trial` found 5,051 empty DSP
input attempts in eager cooperative mode versus 202 in the reference. Its 6,876
completed transactions match the common prefix of the reference's 6,982 when
frame numbers are ignored, but cross a frame boundary differently at transaction
3,389. Preserving whole DSP clock periods fixes phase accounting without fixing
that divergence, so the trial change and deeper temporary instrumentation were
removed. The selected core remains unchanged. This evidence points to input
availability and scheduler slice visibility as necessary parts of a native
service replacement; clock rounding alone is insufficient.


### Native i960 arithmetic

`--fast` now enables `--native-arithmetic` and `--scalar32`. The former emits
integer `mulo`/`muli` and single-format floating add/subtract/multiply/divide as
ARM/VFP instructions instead of opcode helper calls. Floating sources in the
four double FP registers, literal zero/one, FP-register destinations and division
retain double intermediates. GPR-only binary32 add/subtract/multiply use VFP
binary32 operations and canonicalize NaNs to the existing negative pattern.
For multiplication the exact finite product fits 48 significand bits, so the
reference double intermediate loses no precision. For addition/subtraction, an
exponent gap small enough to affect rounding fits within double precision;
a larger gap leaves the already representable larger input unchanged when
rounded to binary32. Division stays on the wide path.

`--no-native-arithmetic --no-scalar32` selects the reference helper path in the
same executable. Neither changes board clocks, memory access ordering, DSP
scheduling or save-state layout. `VF2_REC_NATIVE_ARITHMETIC=1` and
`VF2_REC_SCALAR32=1` are the corresponding runtime variables. These are scalar
VFP operations, rather than NEON vector operations.

`sh tools/acm/check-native-arithmetic.sh` checks both wide and binary32 forms
against the independent MAME interpreter: 1,470 operand combinations and
849,660 / 1,292,028 complete state comparisons across slice boundaries.
It asserts that every arithmetic instruction is actually emitted natively,
and covers GPR/FP/literal inputs, aliases, NaNs, signed zeros, subnormals,
overflow and debug-hook fallback. The existing CPU fixture suite also passes
with the feature active, and the 20-frame whole-game state/pixel golden passes
with both precision paths. Sources, binaries, reverse-checked patch, tests and
hardware records are under `build/acm/native-arithmetic-trial`.

The selected binary is `4b03fe50c41be6b8748e1019ac9eae076f9bc1030b53528cfb3234a244538c15`.
Matched 600-frame simulation-thread profiles measure 15.041909 versus
15.258798 CPU seconds (0.36148 ms/frame saved), 35.73 versus 35.38 fps and
identical core/geometry/viewport pixels (`20261117-073119-de1c17`,
`20261117-073150-01a358`). The earlier double-intermediate-only pair measured
15.214817 versus 15.399992 CPU seconds and also matched state/pixels.
The longer 2,400-frame pair measures **36.90 versus 36.98 fps**, with simulation
24.66 versus 24.77 ms/task: no sustained overall FPS gain is established.
Both retain core `41ab1dc3d8a38d84`, geometry `5c4a0b75827d6659`, exact viewport
pixels and all existing DSP/geometry counters. Evidence:
`20261117-073219-396a59`, `20261117-073336-7b91aa`. The final default smoke
`20261117-073509-485449` matches the 30-frame golden. This is selected for its
CPU reduction and direct native execution; full-speed gameplay remains open.


### Experimental native DSP translator

`--tgp-native-jit` (`VF2_TGP_NATIVE_JIT=1`) enables an ARM/Thumb2 SLJIT
translator. It emits private-RAM moves, supported integer/floating ALU operations,
immediate loads, conditions, direct/indirect jumps, calls and returns. A/B/D/P,
status, cycle budget and instruction count remain in host integer registers.
The generated loop calls canonical handlers for I/O, repeat control and other
unsupported operations, spilling architectural state before callbacks and
reloading afterward. Constant private-memory addresses use direct loads/stores;
indexed addresses retain the exact 16-bit wrapping and unmapped-window behavior.
PCs are materialized at exits and canonical helper entry. Instruction cycles,
FIFO callback times and matrix/resumable-kernel accounting are preserved.

The complete microcode must match the existing revision-validated image.
Changed images, debugger hooks and unavailable direct private RAM retain the
existing fallback. DSP guest-PC/command tracing and alternate block/interpreter
modes cannot be combined with this option. The ordinary `--fast` preset leaves
it disabled. The implementation currently generates ARM code only.

Build the frontend first, then run `sh tools/acm/check-tgp-native.sh`. It compares
786,440 cases against the selected compiled handlers, using a debugger hook on
the reference CPU to disable native translation. Cases cover all 4,096 starting
PCs, randomized raw registers/RAM, finite/nonfinite values, valid/unmapped/wrapping
addresses, two successive slices at budgets 1/2/3/7/23/67, I/O callback times and
PCs, aborts/stalls, exact compiled instruction totals, image mutation/restoration,
hook attach/removal, program-view pointer changes and a bus without direct RAM.
The 20-frame whole-game state/pixel golden also passes with translation active.

A separately compiled generic interpreter exposed a **preexisting** NaN
payload/sign difference at PC `2a7`, reproduced with native translation disabled.
That diagnostic and its generated reference sources are archived under
`build/acm/tgp-native-jit-trial`; they are not claimed as a passing generic
interpreter comparison. The native translator falls back to canonical handlers
for arithmetic with two nonfinite inputs, preserving the selected baseline.

The prototype improved across iterations: 32.09 fps for separate native regions,
34.05 fps for an integrated fallback loop, then about 35.2 fps with native
branches. The final same-binary 600-frame pair is **35.15 versus 35.51 fps**,
25.66 versus 25.29 simulation ms/task, with identical core
`277230da285bb5f5`, geometry `e0c188d55df8df32`, viewport pixels and all matrix/
geometry counters. It translates 25,152,570 instructions and calls 10,342,671
fallback handlers in 795,443 runner entries. Existing matrix kernels account
for additional instruction work independently. Generated code occupies 257,390
bytes. Evidence: `20261117-080129-ca7cb7`, `20261117-080218-f3f026`.
The final normal-preset 30-frame smoke `20261117-080336-c81728` matches its golden.

This is a tested foundation, not a selected speed improvement. The next candidate
is keeping floating values in saved VFP registers and address registers in
integer registers, reducing cross-register-file transfers and helper spills.
Current sources, baseline snapshots, reverse-checked patch, matching binary/ELF,
test logs and structured results are in `build/acm/tgp-native-jit-trial`.


### Experimental DSP VFP register allocation

Add `--tgp-vfp-cache` to `--tgp-native-jit` to keep DSP A/B/D/P in four saved
VFP registers. Integer instructions preserve their raw bit patterns using
explicit transfers, while arithmetic reads VFP operands directly. Supported
private-memory transfers use direct VFP loads/stores when this preserves the
original transfer/ALU ordering; the integer-post-operation D overlap retains its
conservative path. Saved floating registers follow the ARM hard-float ABI, and
all architectural values still spill before canonical handlers or slice exits.
The corresponding startup flag is `VF2_TGP_VFP_CACHE=1`.

The VFP path uses ordered/unordered tests to retain canonical fallback when both
operands are NaNs. This avoids moving each operand through an integer register
just to classify it. Single-NaN, infinity, invalid-operation, subnormal, signed
zero and integer reinterpretation cases are included in the existing full-state
comparison. No emulated state format or DSP cycle accounting changes.

After building the frontend, `sh tools/acm/check-tgp-vfp.sh` verifies both integer
and VFP allocation using the 786,440-case selected-baseline fixture. The VFP
20-frame whole-game state/pixel golden also passes. Hardware runs preserve the
600-frame core `277230da285bb5f5`, geometry `e0c188d55df8df32`, all existing
matrix/geometry counters and exactly identical viewport pixels.

The direct VFP path measures 15.267173 simulation-thread CPU seconds versus
15.374954 for integer allocation, approximately **0.18 ms/frame saved** over
600 frames. FPS is 35.30 versus 35.12. The normal path remains faster at 15.155768
CPU seconds and 35.78 fps, so native translation remains opt-in. These three
runs use the same executable: `20261117-081406-fb3599`,
`20261117-081539-63192d`, `20261117-081642-edf929`. Generated VFP code occupies
254,968 bytes. The final normal-preset smoke is `20261117-082248-86b4e5`.

An additional trial retained intermediate arithmetic results in VFP scratch
registers. It did not improve CPU time (15.276789 seconds); that bookkeeping was
removed, and its source and test results are archived. Rebuilding the simpler
path reproduced the exact previously tested binary SHA-256
`e6a8c617d4ef60a9f8e0e61bb059134a56e3cd3f2d6ab4647fce6624448e0061`.
Current sources, baseline binary/ELF, reverse-checked patch, all trials, matching
hardware records and structured results are in `build/acm/tgp-vfp-trial`.

The next target is generated DSP I/O transfers with narrow bus callbacks, reducing
whole-instruction fallback and repeated datapath spills. This requires an
explicit bus capability describing callback visibility, plus the existing
conservative path for other buses and debugging. The current port still needs
substantial work to reach full-speed gameplay.


### Native DSP I/O and repeat execution

`--tgp-native-io` extends the experimental `--tgp-native-jit` backend with direct
RF and external-memory/table transfers. It works with either integer or VFP
allocation (`--tgp-vfp-cache`). The startup variable is `VF2_TGP_NATIVE_IO=1`.
The Model 2 bus explicitly opts into `native_control_callbacks()`: callbacks may
observe PCs/cycle timing and change GPIO, abort a slice, or stall a read, while
cached datapath/address/status registers and CPU state snapshots are not visible
inside the callback. Reset/re-entry/program mutation is excluded during a run.
Buses without that capability retain complete instruction-handler callbacks;
normal state inspection remains supported there. Debugger hooks retain the
existing conservative path.

Generated transfers preserve read-before-postincrement order, integer ALU update
before the destination write, floating ALU update afterward, and each original
callback's PC and cycle count. Reads that stall retry the same opcode, clear the
stall latch, charge the original single stall cycle and leave the repeat counter
unchanged. Read/write aborts complete the remaining architectural side effects.
ARM callee-saved registers keep cached data resident across narrow callbacks;
caller-saved ALU intermediates are restored from architectural scratch fields.

Linear repeated instructions now have separate generated bodies. Ordinary
instructions acquire no per-instruction repeat-state check. Repeated bodies
update the byte counter, rewind to the same PC until its final iteration and
rejoin normal code afterward. Unsupported repeated control flow stays in the
reference handlers. This preserves zero-to-255 wrap, memory aliasing, unmapped
windows and arbitrary slice boundaries without bulk-copy reordering.

After building the frontend, `sh tools/acm/check-tgp-native-io.sh` checks both
register allocations: **786,506 full-state/RAM/callback comparisons per mode**.
It covers GPIO changes, stalls with and without abort, read/write aborts without
stall, capability rejection, complete CPU snapshots inside fallback callbacks,
program revisions/view changes, hooks, slow buses, and repeat counts 0/1/2/255
at budgets crossing 255/256/257. The native-I/O 20-frame game golden also passes.

On hardware, direct I/O reduces full-handler calls from 10,342,671 to 6,131,642
per 600 frames; adding repeat bodies reduces them to 1,479,397, with 34,316,387
native instruction dispatches. The same-binary pair is **35.66 versus 35.62 fps**
and 25.27 versus 25.24 simulation ms/task: effectively performance parity,
with exact core `277230da285bb5f5`, geometry `e0c188d55df8df32` and viewport pixels.
Evidence: `20261117-084052-3b0b48`, `20261117-084318-654406`.

The 2,400-frame native regression (`20261117-084847-68aaa5`) runs at 37.04 fps,
retains core `41ab1dc3d8a38d84`, geometry `5c4a0b75827d6659`, 159,891,146 compiled
instructions, 197,828,015 TGP cycles, 286,202 native matrices, 136,231 resumable
matrices and 8,464,700 geometry submissions, and matches the saved reference's
viewport pixels exactly. This longer run verifies fidelity, rather than a paired
speed improvement. Generated code is 863,820 bytes. Normal-preset smoke
`20261117-085218-291ba6` matches its 30-frame golden, and `--fast` still leaves
DSP translation and native I/O disabled.

Sources, baseline snapshots, before-repeat implementation, matching binary/ELF,
reverse-checked patch, all test logs, profiles and results are archived in
`build/acm/tgp-native-io-trial`. Current binary SHA-256:
`d4b6f16d543bf316b91f60cd8501364e8134350f7fb33d27f910a8a56d8a4c7b`.
Fine-grained instruction translation is now largely native without a material
speed gain. The next target is a geometry-job abstraction that moves model
processing outside the simulation thread and supports worker/GPU backends,
with materialization only where actual readbacks or debugging require it.
Full-speed native gameplay remains outstanding.


### Owned geometry jobs (2026-11-17)

`--fast` now selects an owned display-list worker when camera packets are enabled.
`--no-geometry-worker` retains synchronous geometry for comparisons. A job owns
all 128 KiB of display RAM, the start address, frame number and floating-point
environment. The existing geometry engine executes it during vblank; end of
vblank, geometry feedback, checkpoint save/load, reset and register changes
materialize its result. Palette/texture resolution stays with presentation.
Previous camera pictures have separate packet ownership and remain usable while
the next list is being built. Alternate camera/smoothing configurations use the
synchronous engine.

The ARM independent renderer comparison passes 120 fight frames and 177,125
output polygons, with exact serialized geometry registers/RAM and feedback.
A deferred backend additionally verifies 16 matrix lists, including display-list
jumps, after every byte of the source buffer has been overwritten.
Run with `VF2_GEOMETRY_WORKER=1 VF2_DIRECT_GEOMETRY=1 VF2_COMPACT_CAMERA=1
sh tools/acm/check-camera-packets.sh` inside the ARM build container.

Same-binary hardware pair: worker **38.75 fps**, synchronous **36.15 fps**
(600 frames, +7.2%). Simulation tasks fall from 24.95 to 21.91 ms; viewport
pixels and whole-game core/geometry hashes match exactly. Evidence:
`20261117-091044-70cddc` and `20261117-091116-e54339`.
The longer worker run `20261117-091152-e5d85c` completes 2,400 frames at
**39.37 fps**, with all 2,400 lists dispatched, core `41ab1dc3d8a38d84`,
geometry `5c4a0b75827d6659`, and pixels identical to the saved reference.
Snapshot cost is 0.295 ms/list, worker execution 5.450 ms/list (wall time,
including contention), and materialization wait 0.025 ms/list.
Two render threads are slower (36.30 fps, `20261117-091309-212f2e`), so the
existing three-thread render pool is retained.
This overlaps existing native geometry work; it does not remove its arithmetic
or move transforms to the GPU. Full-speed gameplay remains outstanding.
Sources, matching binary/ELF, reverse-checked patch, logs and result records are
archived in `build/acm/geometry-worker-trial`.


### Renderer profile and rejected packet trials (2026-11-17)

With owned geometry jobs enabled, renderer-thread profile
`20261117-091441-edfac0` records 2,512 samples over 12.688 thread CPU seconds.
Packet construction takes 11.73 ms/frame wall time, camera projection 3.55,
scene capture 1.37, and GPU submission 3.66. Samples include packet clipping
15.09%, direct batch planning 9.16%, final packing 7.40%, atlas lookup 6.05%,
and Mali driver 14.57%; these percentages cover the sampled renderer thread,
not the additional preparation workers or the simulation/geometry threads.

An exact-input per-slot packet cache found almost no reuse while the camera
moved; its whole-input diagnostic completed at 37.40 fps with exact state.
It is removed. Archived sources and reason: `build/acm/packet-reuse-trial`.
A second implementation shares viewport clipping between mip bands. Host and
ARM tests match the legacy 29,325-vertex byte stream and all 1,000 randomized
polygon fixtures. Hardware `20261117-091943-5af317` preserves state, reduces
CPU packet preparation from 11.73 to 11.43 ms/frame, but both profiled runs
remain at 38.50 fps. It is also removed. Sources, patch, matching ELF and
profiles: `build/acm/shared-viewport-trial`.
The selected binary remains the geometry-worker build; restored-preset smoke
`20261117-092043-aa46c5` passes the 30-frame golden. Further work should target
larger pipeline costs rather than retaining these low-return changes.


### Profile-guided compiler experiment (2026-11-17)

`tools/acm/build-pgo-arm.sh generate` builds an atomic-counter instrumented
frontend in `build/acm/pgo/generate/bin`. Train with the usual fast 600-frame
checkpoint run and that binary, then `python tools/acm/collect-pgo.py` from the
repository. The collector downloads all 25 GCC profiles with CRC verification;
1,033,332 bytes of ROM-derived training data remain in ignored `build/`.
`sh tools/acm/build-pgo-arm.sh use` applies profiles to the complete frontend;
`sh tools/acm/build-pgo-arm.sh core` applies them only to model2a, the TGP AOT
core, i960_device, i960_rec and mame_shim, preserving ordinary renderer code.
Builds use separate object directories, reject coverage mismatches, and require
profiles for the major translation units. Compiler warnings about unused
functions discarded by the linker are expected; whole-file profile misses are
not. All trained builds have zero whole-file profile misses.

Updated simulation profile `20261117-092130-e5f05a`: 2,681 samples / 13.412 CPU
seconds; TGP 31.93%, board/buses/scheduler 27.42%, native i960 24.92%.
The native DSP translator recheck with geometry jobs enabled
(`20261117-092435-71b03b`) still shows no material gain: 22.43 versus 22.38
simulation ms/task. The command-tag profile is `20261117-092233-e2132e`.
All analyses/results are in `build/acm/simulation-worker-profile`.

Compiler PGO reduces simulation from roughly 22.4 to 19.4-19.5 ms/task.
Full PGO reaches 38.23 fps (`20261117-093218-8c1bb7`), core-only 38.53 fps
(`20261117-094102-62f376`), with exact 600-frame state and viewport pixels.
The mixed/core optimization's 2,400-frame run (`20261117-093512-9280bd`)
reaches 40.41 fps, core `41ab1dc3d8a38d84`, geometry `5c4a0b75827d6659`,
and pixels identical to the reference. The reproducible core build and the
mixed long-run binary have identical executable `.text` bytes (SHA-256
`268aa6ea306edd822a9aa7bec5b28073e44f32b624dcee88a85c05ca36fcb0b3`).
This is a real reduction in simulation cost, but not a clear overall speed gain
in the 600-frame comparisons. The normal fast preset therefore stays unchanged;
use `--binary build/acm/pgo/core/bin/vf2-acm-tgp` to test the optimized core.
Profiles, compiler logs, source build tools, matching binaries/ELFs and structured
results are under `build/acm/pgo`.

A separate opaque/masked shader experiment (`20261117-093711-514d13`) preserves
pixels and state but reaches only 38.59 fps, so its active code was removed.
Sources and patch are archived in `build/acm/opaque-pass-trial`. The rationale
was to allow early depth rejection on the opaque pass, as described by Arm:
https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/accelerating-mali-gpu-analysis-using-arm-mobile-studio
Full-speed gameplay remains outstanding. The next renderer target is repeated
vertex/material copying during final buffer packing, rather than more DSP
instruction dispatch tuning.


### Copy caller profile and direct projection (2026-11-17)

`--profile-copy-callers` defaults to the rendering thread and also works
with `--profile-simulation-windows`. It records return addresses while
execution is in the first 1 KiB of the bundled ARM memcpy.
The implementation signature is checked at startup; matching disassembly shows
LR is preserved throughout this range. This is a bounded caller diagnostic,
not a general stack unwinder or a complete accounting of every copy routine.
`cpu.samples.copy-callers` uses the ordinary address/count format; analyze it
with `analyze-cpu-samples.py --maps cpu.samples.maps --symbols symbols.txt`.
For `20261117-094826-8744c8`, all 395 caller samples match the copy-PC sample
count: 218 application, 175 Mali and two libc callers. The largest application
caller was the final projected-polygon gather (116); camera capture accounted
for 67. The largest Mali caller (138) resolves to `_gles_gb_buffer_data` at
return offset 0xa771c. This identifies vertex-buffer upload copying as a concrete
remaining driver-side target. Evidence: `build/acm/copy-caller-profile`.

A preliminary direct vertex-store change passed the independent final-buffer
comparison for 120 frames but produced 38.57 versus 38.55 fps, with 11.66 versus
11.64 ms packet planning. Disassembly confirmed the original structure copies
were already inline. That change was removed; source and results are archived
in `build/acm/vertex-store-trial`.

The selected fast preset now sorts source polygon indices and projects directly
into the final ordered vector (`--no-direct-projection` selects the former
project/sort/gather path). Under the hardware's 32,767-polygon capacity, hidden
or culled input polygons cannot displace another survivor. Larger source lists
retain the original capacity-aware path. Empty/cull/window/depth/capacity checks
cover 1,024, 32,767 and 32,768 inputs; the independent old-renderer comparison
passes all 120 fight frames and 177,125 output polygons with exact geometry RAM.
Run the camera checker with `VF2_DIRECT_PROJECTION=1` to exercise this path.

The same-binary 600-frame comparison is 38.53 versus 38.33 fps, with camera
preparation reduced from 3.57 to 3.00 ms. The 2,400-frame regression completes
at 39.77 fps, core `41ab1dc3d8a38d84`, geometry `5c4a0b75827d6659`, and pixels
identical to the reference. Evidence: `20261117-095127-96036f`,
`20261117-095213-dc063a`, `20261117-095335-e51780`.
Default-preset smoke `20261117-095454-28991f` passes its 30-frame golden.
Sources, matching binary/ELF, reverse-checked combined diagnostic/projection
patch and logs are in `build/acm/direct-projection-trial`.
The saved PGO binaries predate direct projection; rebuild `build-pgo-arm.sh core`
to combine later renderer changes with the same trained CPU profiles.
Full speed remains outstanding. Next investigate mapped/ring vertex-buffer
storage to remove driver upload copies, using the current indexed atlas path
as the fidelity reference.


### Client vertex arrays and preferred PGO core (2026-11-17)

The installed Mali driver exposes neither GL_OES_mapbuffer nor
GL_EXT_map_buffer_range. The finite `probe-gles2-buffers.cpp` test confirms exact
first/last vertex rendering and measures 0.7525 CPU ms for each 360,000-byte
BufferData upload (240 repetitions, no in-flight readers). Its log is in
`build/acm/buffer-map-trial`. No buffer-mapping path was enabled.

The GLES2 batch backend can instead use client vertex/index arrays. It unbinds
both buffer targets, supplies pointers into the owned final CPU arrays, and
uses the same indexed/nonindexed draw ranges. The driver consumes these arrays
through GLES2 draw calls; this is not a claim that the driver performs no copies.
`--fast` now selects this mode for full rendering; `--no-client-arrays` retains
the VBO/IBO path for comparisons. Raster fixtures on hardware pass ownership,
checker, clipping, indexed colours, transparency, staged resource lifetime,
RAM changes, and large-scene fallback.

The same-binary profiled pair is 38.97 versus 38.51 fps (client arrays versus
VBOs), with exact 600-frame state and viewport pixels. GPU submission falls
from 3.66 to 2.99 ms. In the bounded memcpy caller diagnostic, Mali samples fall
from 151 to 37; `_gles_gb_buffer_data` return samples fall from 117 to three.
The total renderer-thread CPU times are 12.461 versus 12.354 seconds, so this
primarily changes driver work/overlap rather than proving a total CPU reduction.
The unprofiled regular 2,400-frame client-array run reaches 40.84 fps with exact
state and pixels (`20261117-100401-a83646`).

With the rebuilt PGO core, the 600-frame client/VBO pair reaches 40.57 versus
37.78 fps (`20261117-100526-ed32c9`, `20261117-100742-b344b4`). The combined
2,400-frame run reaches **41.77 fps**, simulation 19.30 ms/task, core
`41ab1dc3d8a38d84`, geometry `5c4a0b75827d6659`, and pixels identical to the
reference (`20261117-101015-57fcba`). The preferred 30-frame preset smoke
`20261117-101210-95124a` passes its golden.

`--fast` now prefers `build/acm/pgo/core/bin/vf2-acm-tgp` when its binary and
source SHA-256 stamps match. `build-pgo-arm.sh core` writes those stamps after
building. Missing/stale stamps select the regular compiled-TGP build;
`--pgo-core` explicitly requires a valid PGO build, `--no-pgo-core` selects the
regular build, and an explicit `--binary` takes precedence. Six checks verify
matching inputs, changed source, changed binary, empty stamps, outside paths,
and the actual current build. These are development-build selection checks;
normal runtime rendering has no source hashing.

Sources, matching regular/PGO binaries and ELFs, reverse-checked patch, all logs,
profiles and result records are archived in `build/acm/client-array-trial`.
Full-speed gameplay and deferred audio remain outstanding. The next larger
rendering target is the camera-space pipeline: reduce repeated clipping/copies
or move projection into the highp vertex shader while retaining the texture
atlas's repeat/mip behavior and the existing CPU reference.


## Camera-space GPU projection experiment (2026-11-17)

Archived in `build/acm/gpu-camera-trial`; not selected. The prototype accepts
owned raw camera packets, projects eligible polygons in the vertex shader, and
retains CPU preparation for viewport, texture-cell and mip-band crossings.
786,297 of 831,019 polygons in the 600-frame fight use the GPU path (94.62%).
Multiple camera centers initially split page-pair batches: 38.44 versus 39.04 fps
for the same executable. Folding the center into homogeneous vertex coordinates
and carrying the camera flag in the unused material byte restores batch counts
without changing the 52-byte vertex format or adding an attribute.

The packed PGO pair is 39.99 versus 39.82 fps (GPU versus CPU projection), with
packet preparation 11.39 versus 12.87 ms, but fence wait 3.22 versus 1.81 ms.
This does not establish a useful overall speed improvement. Core and geometry
hashes match the 600-frame golden. The viewport differs at 34/307,200 pixels,
maximum channel difference 12/255, reflecting changed projection/interpolation
rounding. Hardware fixtures pass mixed camera centers, projected/native packets,
mip/microtexture, viewport/UV fallback, ownership, and staged resource lifetime.
The source prototype, matching regular/PGO binaries and ELFs, tests, logs and
results are preserved in the archive; production sources retain CPU projection.


## Camera-plane buffer reuse (2026-11-17)

`gpu3d_geometry_frame` now caches plane distances, keeps the current vertex
buffer when all points are inside, and rejects an all-outside polygon without
copying it through the remaining planes. Crossing-plane interpolation retains
the reference float operations and vertex order. `--reference-camera-clip`
selects the preserved original implementation for same-binary comparisons.

`check-camera-packets.sh` also runs the independent preserved clipping reference
against 100,000 cases (inside, crossings, boundaries, NaN/infinity). All match
bit for bit. The existing 120-frame/177,125-polygon comparison against the old
renderer and serialized geometry feedback/lifetime checks also pass.

The regular profiled pair is 39.33 versus 39.56 fps, camera preparation 2.82
versus 3.04 ms. The rebuilt PGO 2,400-frame pair is 41.82 versus 41.66 fps,
camera preparation 2.90 versus 2.93 ms. These results do not establish a material
FPS gain. Both long runs retain exact state hashes and viewport pixels; the
copy elimination is retained, without claiming it solves the speed gap.
Evidence and matching binaries/ELFs are in `build/acm/camera-clip-trial`.
The preferred PGO source/binary fingerprints match the current production code.


## Render stages and resource-generation experiment (2026-11-17)

`--profile-render-stages` enables wall/thread-CPU accounting at host boundaries
and samples prior fence readiness after capture, camera and packet preparation.
It does not enable statistical PC sampling. With the selected PGO renderer,
600 frames measured 40.46 fps. Wall/CPU ms per frame: capture 1.91/1.89,
camera 2.96/2.84, packets/materials 12.09/10.43, prior drain 2.83/0.24,
tile upload 0.52/0.52, submit 2.75/2.71. At the packet boundary the previous
fence was pending in 524 frames, complete in 75, absent in the first.
State and viewport match the golden. Evidence: `20261117-105042-c2897f`.

`--serialize-gpu-passes` adds diagnostic glFinish calls after submit and after
composition, changing overlap deliberately. The measured waits are 0.03 and
19.00 ms respectively, with 26.43 fps. These are wait locations, not isolated
GPU pass execution times: deferred offscreen producer work can be accounted
at its consumer. Do not conclude that 3D is free or composition costs 19 ms.
Evidence: `20261117-104518-497565`.

Two independent 3D atlases/palettes/canvases and accumulated tile uploads were
tested behind alternating fences, then removed. Hardware tile fixtures pass
24 cases plus pending consecutive dirty/palette/mask updates. The 600-frame
pair is 40.33 versus 40.46 fps; explicit queue waits drop to 0.01 from 2.69 ms,
but submit rises to 3.51 from 2.75 ms, capture to 2.13 from 1.91 ms, and
simulation tasks to 20.32 from 19.46 ms. State is exact; changed atlas placement
also changes the final viewport (97.19% identical). No useful throughput gain.
Prototype sources, fixture, patch, binary/ELF and results are retained in
`build/acm/double-gpu-trial`; normal resource handling is restored.

Repeating the flat-shader bound with the faster simulation gives 43.91 fps
versus 40.46, queue wait 0.01 ms. The probe intentionally removes indexed
texture/palette/discard work and changes rendering; it is not a playable build
or an isolated fragment ALU timer. Evidence: `20261117-105115-d20133`.
Normal rendering is restored by `20261117-105347-f02df1`, with exact 120-frame
state. Diagnostic results and matching final binaries live in
`build/acm/render-stages-trial`.

The live vertex-capability probe reports 256 uniform vec4s, 16 attributes,
and zero vertex texture samplers. The larger engine direction is retained
object-space GPU meshes and uniform matrix palettes, preserving atlas batching;
validate matrix indexing and ROM-object reuse before committing to that path.
Both simulation and rendering still exceed the full-speed budget.


## Retained mesh foundation (2026-11-17, not yet game-integrated)

`probe-gles2-mesh.cpp` validates the required Mali path on hardware: a single
immutable VBO upload, 64 dynamically indexed 3x4 uniform matrices, 32 matrix
palette updates, and 131,072 pixel comparisons. Every pixel matches. Build it
with `build-probe-mesh-arm.sh`. This is a capability/correctness fixture, not a
game speed measurement or the textured game renderer.

`mesh_geometry.h` decodes immutable ROM topology into an owned catalog keyed
by address/count/mode. It retains attributes, raw normals and their generation
mode, triangle/quad vertex order, and all four link types. Truncated input
rejects partial meshes; decoded polygon storage is capacity-bounded to 4 MiB
by default. `check-mesh-geometry-arm.sh` checks topology, winding, all normal
modes, count/termination, truncation, owned data, cache keys and allocation bounds.

The existing smooth geometry recorder now also captures object count/mode,
texture point/header addresses, and focus alongside each matrix. The normal
renderer does not enable recording. `profile-mesh-reuse.cpp` consumes these
owned records with synchronous geometry; its timings are not gameplay timings.
Build with `build-profile-mesh-arm.sh` and use the archived command for matching
fast simulation options. The ARM/QEMU 600-frame fight inventory sees 66,411
object instances: 65,211 ROM and 1,200 RAM. There are 447 ROM source addresses,
64,764 cache hits (99.31%), at most 114 objects per frame, and at most one
instance of each ROM address per frame in this trace.

The decoded catalog uses 2,328,320 bytes of polygon capacity, with no rejected
ROM meshes. CPU transformation of cached vertices matches 1,029,152 raw camera
polygons bit for bit (99.19% of the compared camera stream); 8,410 remain
unattributed/unverified by this ROM-only comparison. They must retain the
existing fallback. These are vertex/topology checks, not texture/lighting,
clipping, painter-order or complete-render equivalence checks.
The 600-frame core/geometry golden hashes match. The independent 120-frame
camera/old-renderer comparison and geometry ownership/feedback checks pass.
After rebuilding regular/PGO executables, the normal hardware 120-frame run
`20261117-111034-ac74f3` retains exact state and viewport pixels.

Next: connect the catalog to a native mesh-frame handoff, preserving per-polygon
material/sort metadata and the fallback. Assign stable mesh matrix slots so
static VBOs do not need vertex rewrites when active objects change. This trace
would need seven 64-matrix groups across all encountered ROM meshes; atlas-page
batching must be retained. UV cell splitting can be cached in object space for
immutable texture recipes; dynamic mip crossings require the existing path.
GPU transform rounding/Model 2 coordinate truncation still need real-scene
verification. No game speed gain is claimed yet. Source, matching binaries,
logs and inventory are under `build/acm/retained-mesh-trial`.


### Bilinear display scaling

Bilinear magnification of the final canvas is now the engine default, so no
flag is needed. `--linear-upscale` is retained as an explicit no-op for old
scripts; use `--nearest-upscale` to force nearest-neighbor magnification for
pixel comparisons. The frontend's override is likewise `VF2_NEAREST_UPSCALE=1`;
`VF2_LINEAR_UPSCALE` is obsolete. This changes only magnification at
presentation. The 496 ? 384 game canvas and the 480 ? 640 centered viewport
keep their dimensions.

The 600-frame hardware comparison was 38.55 fps with bilinear and 39.51 fps
with nearest-neighbor on the same ARM executable. Core and geometry state
hashes match, the viewport shows filtered edges, and the borders are exact.
See `build/acm/linear-upscale/results.json` for the capture IDs.


### Mesh packet inventory

The mesh profiler now runs the renderer?s indexed packet builder over the
recorded compact camera polygons and reports packet eligibility in
`packet_inventory`. Attribution uses unique bit-matched camera vertices from
the ROM catalog. Duplicate source positions, duplicate camera positions and
repeated object instances are excluded from attribution. A retained candidate
also needs immutable UV and header addresses, no frustum clipping, and a
single mip band with the original projected triangle fan.

In 600 fight frames, 744,354 of 830,600 textured polygons meet those conditions
(89.62%). They account for 74.74% of the textured vertex stream. The replay
sees 24,435 distinct object/polygon/material/mip combinations. 87.98% of
eligible occurrences use a combination also eligible in the preceding frame.

The diagnostic groups meshes into stable 64-matrix slots. It reports distinct
texture recipes with and without those groups. Those numbers do not measure
actual GPU draws because the runtime batches multiple recipes onto each atlas
page. Lighting, palette selection, ordering, and transforms still need
per-frame inputs. The eligibility inventory is a guide to the next prototype,
not a frame-time measurement.

Evidence is under `build/acm/retained-mesh-trial/packet-inventory-*.json`.

The 2,400-frame replay measures 89.53% polygon eligibility, 76.03% vertex
eligibility and 89.86% consecutive reuse. Both replays match the checkpoint
core and geometry hashes.


The Mini benchmark can archive the final atlas key-to-page map with
`--fast --atlas-layout`. Supply its `atlas-layout.txt` as the final argument
to `vf2mesh-profile-tgp` for the identical replay. This mode rejects maps
with atlas clears or missing keys.

In 600 frames, the replay reproduces all 20 sampled Mini draw counts. The
current atlas averages 32.58 batches per frame. Splitting eligible polygons
into separate retained 64-matrix groups and preserving the original
fallback batches would average 72.35. A shared vertex format that puts
fallback polygons in a retained draw whenever both use the same atlas
pages has a lower bound of 52.35 with the same matrix groups. Both retained
figures are projections. The actual cost depends on how the renderer
streams dynamic attributes and submits the mixed buffers.

See `build/acm/mesh-atlas-layout/results.json` for evidence and peak counts.


### Live mixed mesh projection

`--fast --mesh-render` enables the experimental shader on eligible ROM
polygons with unchanged single-band fans. Fallback vertices can share the
same atlas texture pair and draw with a zero matrix slot. This experiment
still builds projected CPU packets and streams all vertices. It exercises
live provenance and shader projection under gameplay load.

The 600-frame checkpoint pair measured 35.60 versus 38.86 fps. The GPU
projected 777,116 polygons. Game and geometry hashes match. The experimental
viewport has projection differences, so the normal renderer remains selected.
See `build/acm/live-mesh-render/results.json` for two screenshot comparisons
and the sampled draw counts.

### Atlas batch depth order

The projected renderer was tested with near-first and far-first ordering of
atlas page-pair draws, weighted by triangle index count. Atlas placement,
painter-depth values and packing stayed the same. The four interleaved
600-frame hardware runs measured 39.51 fps baseline, 39.56 near-first,
38.85 far-first and 39.39 baseline repeat. All core and geometry hashes and
all pixels in the final viewport capture matched. Prior GPU drain averaged
2.19, 2.08, 3.46 and 2.85 ms in run order. Near-first fell within the spread
of the reference runs, so the selected renderer keeps its original order.
The trial sources, binary, ELF, captures' job IDs and stage logs are recorded
under `build/acm/atlas-depth-order-trial`.

### Shared texture transforms per clipped cell

The indexed packer was tested with one UV transform pair per clipped cell
and a separate position/UV point stream. A 120-frame device run compared
packed vertices, indices and draw descriptors against the reference each
frame. The Mesa renderer and packet math checks passed as well.
The 600-frame hardware sequence measured 39.27 fps baseline, 39.25 shared
cells and 39.41 baseline repeat. The packet/material stage took 11.90,
12.36 and 11.92 ms respectively. Core and geometry hashes matched across
all three runs. Their final viewport captures matched pixel for pixel.
The original packer remains selected. See `build/acm/shared-cell-trial`
for the sources, binary, ELF and measurements.

### Current simulation and renderer thread profiles

Two 1,200-frame runs on the selected PGO build sampled the simulation and
main renderer threads separately. The simulation trace has 4,757 samples
in 23.883 CPU seconds. The scheduler loop has 9.40%, the DSP dispatch loop
8.77%, and emitted i960 code 27.33% of simulation samples. One inlined DSP
RAM read line has 3.05% of the trace.

The renderer trace has 4,775 samples in 24.139 CPU seconds. Indexed polygon
packet generation accounts for 14.51%, atlas batch planning for 11.54%,
final vertex/index packing for 9.63%, and camera sort's introsort body for
2.72%. The sort's inlined comparator also appears in the source profile.
These are main-thread samples. GPU and render-worker execution are outside
that trace. The two runs match game and geometry hashes and every final
viewport pixel. See `build/acm/current-thread-profile/results.json` and the
function and source-line reports alongside it.

### Contiguous DSP internal RAM read view

A contiguous read view skipped the bank-selection comparison by keeping the
unmapped 0x100..0x1ff region zero-filled. Stores still bypassed that region,
and checkpoint serialization included only the two mapped RAM banks. The
five-path ARM DSP fixture matched state and counts. Three 600-frame hardware
runs measured 38.56 fps baseline, 38.94 contiguous view and 38.87 baseline
repeat. Simulation tasks averaged 22.68, 22.51 and 22.65 ms. Core and geometry
hashes and final viewport pixels matched. The small change did not justify
switching the selected RAM path. Trial sources, binary, ELF and results are
under `build/acm/linear-dsp-ram-trial`.

### Packed camera sort keys

Direct projection now packs the window, depth and reverse source index once
per polygon. The sort compares contiguous keys. The original order still
applies for equal depths and hidden windows, and the existing projection-first
fallback handles lists above the hardware capacity. ARM fixtures match the
original polygon stream over 120 fight frames plus capacity, clipping,
window and tie cases. Use `--reference-camera-sort` for an A/B comparison.

A packed-key radix sort and the simpler integer comparison sort performed
similarly in 600-frame trials. The integer sort is selected. In 2,400-frame
runs on the rebuilt PGO executable, camera preparation fell from 2.85 to
2.33 ms and average frame rate was 41.65 and 41.48 fps respectively. The
prior GPU drain absorbed the time saved. Both runs have identical core and
geometry hashes and final viewport pixels. See `build/acm/camera-sort-trial`
for the builds, stage logs and `results.json`.

### Draw-local client-array indices

Rebased indices and per-draw GLES attribute pointers passed the direct pack
comparison and matched the game state and final viewport. In the three
600-frame runs, submission took 2.95 ms baseline, 3.05 ms draw-local and
3.02 ms baseline repeat. Frame rates were 39.10, 39.43 and 39.08 fps.

The Mini's isolated 32-draw probe rendered the same triangles from indices
near zero and near 60,000, with identical pixels. Both absolute-index cases
used about 0.367 ms of thread CPU per frame. Rebinding the eight attribute
pointers per draw took 0.426 ms per frame. This configuration shows no
penalty for high absolute indices. The original draw path remains selected.
The fixture, binaries and measured results are in
`build/acm/rebased-client-arrays-trial`.

### Reusing effective addresses for scalar i960 bus accesses

The hottest emitted i960 routine was `0x16504`, with 145 CPU samples in
the 1,200-frame profile. Single-register loads and stores accounted for 91
of those samples, excluding time spent inside the C++ bus functions.

The generated scalar load/store path now passes its computed address to
bus fallbacks. The helper skips a second page lookup and operand decode.
It preserves the byte accesses used for unaligned words, stalled-read
writeback, original guest PC and bus-visible cycle timing. `--fast` enables
this path and `--no-i960-bus-address` restores opcode helper calls.
The expanded ARM fixture passes 143,689 four-slice interpreter comparisons
in each of three lazy-PC/register-cycle configurations.

The 2,400-frame PGO comparison reaches 41.79 versus 41.31 fps and 18.89
versus 19.48 ms per simulation task. Both game state and geometry hashes
match, as do all pixels in the final viewport. See
`build/acm/i960-bus-address-trial/results.json` and `hot-function.json`.

### Reduced draw cadence for gameplay testing

`--draw-cadence D/N` draws D of every N game ticks, with draw attempts spread
as evenly as the fraction permits. Inputs and board simulation still advance
on every tick. The fast preset draws every frame unless this flag is passed.
The last tick of a finite capture is drawn even when the cadence would omit it.
Use `--paced` for a finite comparison at the normal frame deadline.

```powershell
python tools/acm/benchmark-device.py --fast --interactive --draw-cadence 1/3
```

The 2,400-tick paced fight replay averaged 56.13 game ticks/s and 18.73 shown
frames/s at 1/3, and 56.27 and 14.09 at 1/4. Some 60-tick gameplay windows fell
to about 50 ticks/s. Both have the original full-render game and geometry
hashes. The final captures match over 99.99% of viewport pixels. The scripted
cold start at 1/3 reached Pai versus Lau in Round 1 at 55.55 game ticks/s and
18.54 shown frames/s. The results and capture IDs are under
`build/acm/render-cadence-trial`.

### Shared reciprocal for texture coordinates

Sharing the projected Q reciprocal between U and V did not shorten the
Mini's packet preparation stage. The same-binary 600-frame cadence was
39.88 fps baseline, 39.81 double reciprocal, 39.56 single reciprocal
and 39.73 baseline repeat. The double variant matched every packed buffer
in a 120-frame comparison and the final viewport in the longer run.
The single variant had small texture differences and the same simulation
and geometry hashes. The original division path remains selected. Sources,
binaries, and comparisons are under `build/acm/shared-uv-reciprocal-trial`.

### Two outstanding GPU frames with shared resources

A second fence allowed two submitted frames on the existing context and
kept every atlas and canvas unchanged. All 600-frame state hashes and final
viewport pixels matched. Throughput fell to 14.58 fps, versus 39.44 and
39.84 fps in the one-fence reference runs. Explicit fence wait fell from
3.28 to 0.09 ms, while tile upload rose from 0.71 to 44.01 ms per frame.

In 120-frame split timing, the map texture upload rose from 0.35 to 84.88
ms. Palette, mask and vertex calls stayed below 0.2 ms each. Client arrays
removed the vertex buffer upload, but the map upload still took 82.16 ms
and the run averaged 9.51 fps. The original fence ordering is selected.
The sources and measurements are under `build/acm/gpu-fence-depth-trial`.

### Alternating tile-map textures and selective GPU waits

Alternating the two mutable 2D map textures kept the existing 3D atlas
layout. Mesa and Mini tile fixtures matched their software reference. In
600-frame Mini runs, the alternating maps with two outstanding frames
cut tile upload from 44.01 ms in the preceding shared-map experiment to
0.71 ms. 3D submission rose to 11.19 ms and throughput was 31.23 fps.
The 120-frame split attributes 8.24 ms to texture atlas upload and 11.13
ms to palette atlas upload on this path.

A selective fence drains outstanding draws only for staged shared 3D
texture mutations or the conservative fallback. Across 600 frames there
were 261 shared-update frames and 339 read-only frames. Fence wait fell
from 3.46 to 1.01 ms. Simulation wait, capture and submission increased,
and the candidate averaged 39.48 fps against 39.86 and 39.59 fps in the
same-binary reference runs. State and final viewport pixels matched.
The original queue remains selected. See `build/acm/alternating-tile-maps-trial`
for the fixture, source, binaries, and results.

### Shared immutable camera pictures

The compact camera path can retain an immutable picture while simulation
publishes its successor. A writer reuses a spare only when it owns the
buffer exclusively. Capture copies the mutable palette and colour tables.
Projection workers resolve the colours and CRTC offsets using that capture.
The mesh diagnostics retain their existing packet path. `--fast` selects
sharing, and `--no-shared-camera` selects the capture-copy reference.

The ARM camera fixture compares 120 fight frames against the independent
renderer and verifies that the first captured view still projects exactly
after later pictures replace it. The same-binary 2,400-frame Mini runs
measured 42.31 fps with sharing and 41.85 fps with copying. Capture time
fell from 1.86 to 0.68 ms per frame. Projection and prior GPU wait rose
slightly. Game and geometry hashes and final viewport pixels were identical.
See `build/acm/shared-camera-trial/results.json` and the stage logs.

### Fusing camera projection and texture-packet preparation

The projection workers built texture packets before camera compaction. The
packet descriptors moved with surviving polygons. The ARM alignment fixture
covered clipped polygons and the capacity boundary. The Mini's 600-frame
comparison matched game and geometry hashes and all final viewport pixels.

The fused run averaged 40.16 fps, versus 40.48 and 40.57 fps in the baseline
runs. Combined projection and packet/material time increased from 14.27 to
16.31 ms per frame, while prior GPU wait fell. The separate worker passes
remain selected. See `build/acm/fused-camera-packets-trial`.

### Clipping directly from compact camera records

The alternate projection clipped the immutable compact record before
expanding color and material fields. The independent ARM camera fixture
matched 120 fight frames, including retained picture lifetime checks.
Camera preparation fell from 2.77 to 2.52 ms in the first 600-frame pair.
Throughput was 40.56 fps with direct clipping, versus 40.43 and 39.94 fps
in the reference runs. Game state, geometry and the final viewport matched.
The fast preset retains the shared camera path with its existing clipper.
See `build/acm/compact-direct-projection-trial`.

### Early dispatch of aligned DSP port accesses

The board bus was tested with a direct range check for aligned TGP command
and FIFO addresses before the general memory-map scan. It keeps the original
FIFO callbacks and per-region burst flags. A 12-frame ARM trace matched all
41,908 completed transactions, virtual timestamps and DSP clock fields.

In three 600-frame runs, simulation tasks took 21.33 ms baseline, 21.21 ms
direct and 21.44 ms baseline repeat. Frame rates were 40.10, 40.36 and 40.42 fps.
Game state, geometry and the final viewport matched. The existing bus path
remains selected. See `build/acm/direct-tgp-port-trial`.

### Specialized dispatch for common scheduler orders

The modular scheduler was tested with direct CPU calls for its common saved
orders. The general loop handled other permutations. The 31 modular clock
guards and 20-frame ARM state and pixel golden passed. The 600-frame Mini
comparison matched all state hashes and final viewport pixels.

The specialized run averaged 40.28 fps and 21.25 ms per simulation task.
The reference runs measured 40.49 and 40.10 fps and 21.02 and 21.26 ms.
The general dispatch remains selected. Evidence and sources are under
`build/acm/static-scheduler-trial`.


### Rechecking the atlas lookup cache on the shared-camera renderer

A 256-entry direct-mapped cache held references to the atlas map's stable
entries and checked texture generations before reusing them. Atlas clears
invalidated the cache. Mesa passed at both shader precisions, and the Mini
passed the 120-frame direct-pack comparison. Three 600-frame runs on the
same ARM build measured 40.52 fps reference, 40.42 fps cached, and 40.48 fps
reference repeat. Core and geometry hashes and the final viewport matched.
This confirms the outcome of the earlier 1,024-entry trial on the renderer
before shared camera pictures. Both variants remain archived in
`build/acm/atlas-recent-lookup-trial` and `build/acm/atlas-lookup-trial`.


### Material traffic and single-sample reuse

Across 2,400 fight frames the renderer submitted 7,192,634 triangles in
81,332 3D draws. Single-sample textured triangles accounted for 39.15% of
triangles and 87.31% of projected area. The area sums triangles before depth
testing and discard, so it is only a proxy for fragment work. Splitting the
same atlas-page batches into opaque and masked variants would produce 122,322
draws. Splitting by one versus two samples would produce 97,353 draws.

For batches that use one sample, reusing the first UV transform and atlas
lookup preserves packet bytes, core and geometry hashes, and final viewport
pixels. It reduced packet/material wall time to 11.55 ms from 11.81 and
11.77 ms in the reference runs, with 40.44 fps versus 40.40 and 40.44. The
selected renderer was restored. See `build/acm/material-traffic` for the
inventory, logs, ARM packet check and both experimental source snapshots.


### Shared mip-band depth bounds

The packet builder was tested with one reciprocal-depth scan per polygon,
one calculation per shared mip boundary, and integer maximum mip selection.
ARM/QEMU matched the reference packet bytes in 1,000 polygon fixtures, with
additional cases at mip boundaries. Integer mip selection matched all 65,535
positive texture widths. The Mini passed 120 frames of direct-pack checks.

The 600-frame comparison measured 40.44 fps with the sweep and 40.46 and
40.38 fps for the reference runs. Packet preparation fell to 10.77 ms from
11.59 and 11.62 ms. The prior GPU fence wait rose to 5.11 ms from 4.38 and
4.07 ms. Core and geometry hashes and final viewport pixels matched. The
sweep was archived and the selected renderer restored. See
`build/acm/mip-band-sweep/results.json`.


### Reserving a simulation core with the current renderer

Using the selected PGO build, pinning simulation to core 3 reduced mean
simulation task time to 18.09 ms from 18.23 and 18.24 ms. Restricting renderer
and geometry workers to the other cores increased packet preparation to
13.21 ms from 12.29 and 12.28 ms. The pinned run averaged 40.66 fps versus
41.09 and 40.89 fps. State hashes and final viewport pixels matched.
Default scheduling remains selected. See
`build/acm/current-affinity-test/results.json`.


### Forced canvas resolve measurements

The timed read ran while the completed canvas FBO was still bound, with a
separate serialized window after the final display blit. The existing
glFinish-only probe let offscreen work carry over into that window. Two
read sizes on the same fights give the following mean wall times:

| Read size | Textured | Repeat | Flat shader | 2D only | Final blit drain |
| --- | ---: | ---: | ---: | ---: | ---: |
| 496 x 384, 300 frames | 143.20 ms | 143.13 ms | 136.85 ms | 135.65 ms | 1.63 ms |
| 1 x 1, 120 frames | 19.90 ms | 19.91 ms | 13.50 ms | 12.24 ms | 1.55 ms |

The absolute resolves include driver synchronization, framebuffer resolve
and readback. The paired delta of about 6.3 to 6.4 ms covers the removal
of texturing, indexed palette lookup and discard, alongside changes in
varying work and occlusion. The flat and 2D-only images intentionally differ.
State hashes match across each run series, and the ordinary textured reads
match the corresponding uninstrumented final viewport pixel for pixel.
The main renderer was restored. Diagnostic sources, binaries, logs and
per-window data are under `build/acm/gpu-resolve-probe`.


### Shader cost sweep and compact palette atlas

The 300-frame forced one-pixel resolve measured 19.80 and 19.82 ms in the
textured references. Replacing palette lookup with grayscale measured
18.20 ms, skipping the second mip/microtexture sample 18.42 ms, removing
discard 18.12 ms, and removing only checker masking 19.49 ms. State was
identical. Checker removal matched viewport pixels in this fight, where
the material inventory saw no checker triangles. The other probes
intentionally change output. Differences include changes in shader
compilation and fragment coverage, so they cannot be added as pass times.

Adjacent logical palette indices already map to the same luma and colour.
The nearest-filtered palette now stores one texel per pair, reducing its
dimensions from 512 x 1024 to 256 x 1024 and keeping the original normalized
coordinates. Mesa fixtures passed for both shader precisions, including
batched indexed rendering and the existing direct pack comparison.

On the identical trial executable the compact mode reached 40.98 fps
over 600 frames against 40.46 and 40.07 fps, with matching state and final
viewport pixels. The 120-frame forced resolve comparison was 19.53 vs
19.87 ms with exact output. Peak process RSS dropped 1,024 KiB in the
600-frame trial. The compact atlas is selected. Shader probes and
comparison runs are archived under `build/acm/shader-cost-sweep`.

The selected core-PGO binary ran 2,400 frames at **43.12 fps**, versus
the prior selected build's 42.31 fps. The core hash `41ab1dc3d8a38d84`,
geometry hash `5c4a0b75827d6659` and every viewport pixel matched.
Peak RSS was 151,384 KiB. The long-run evidence is
`build/acm/shader-cost-sweep/selected-2400.json`.


### Alpha quantization and single-channel opaque tiles

Removing floor before the single-sample alpha cutoff measured 19.40 ms
in the canvas resolve probe, against 19.42 and 19.41 ms. Calculating alpha
only for transparent materials took 19.71 ms. State and viewport matched
in all four 300-frame runs. Neither variant remains selected.

Separate one-channel opaque pages reduced cumulative atlas upload from
4,555,272 to 3,140,644 bytes and reached 41.93 fps against references of
41.07 and 40.77 fps. Atlas coordinates moved, and the viewport was 92.84%
pixel-identical (99.07% within eight levels per channel). A second version
kept original logical page placement and put opaque texels in companion
textures. It matched the viewport, but the final draw count grew from
33 to 52 and the 600-frame throughput was 41.16 fps against 40.86 and 40.95.
All state hashes matched. Both variants passed normal and mediump staged
GLES2 fixtures. The selected atlas was restored. Archives are under
`build/acm/alpha-quantization-trial`, `build/acm/opaque-index-atlas-trial`
and `build/acm/opaque-index-same-layout`.


### Compact GLES2 vertex layouts

The baseline sends 52 bytes per vertex. A 44-byte layout packs the material
flags, mip selector and palette row and decodes them in the vertex shader.
It measured 40.96 fps against 41.05 and 41.08 fps. Packet preparation was
11.43 ms against 11.63 and 11.59 ms.

A 48-byte layout uses ten scalar/vector attributes instead of eight and
avoids GLSL bit-field arithmetic. It measured 41.23 fps against 40.58 and
40.69 fps in its immediate comparisons. An independent reference series
above reached 41.08 fps. Both formats passed staged Mesa fixtures, 120
Mini packing-check frames, and 600-frame state and final viewport checks.
The relative gain of the ten-attribute variant is small, and the layout
needs more than the eight attributes guaranteed by GLES2. The selected
renderer retains the original layout. Sources and results are under
`build/acm/packed-vertex-trial` and `build/acm/packed-vertex-attributes-trial`.


### Texture-cell traffic and horizontal clip reuse

The 600-frame inventory saw 5,782,810 requested mip bands and 4,923,915
early depth rejects. Of 858,895 surviving bands, 347,408 crossed a cell
boundary and emitted 50.07% of the triangles. 366,846 surviving bands
(42.71%) fit the origin cell with unit steps and no microtexture. Positive
depth, viewport/mip treatment, and output pixels match the reference run.

Caching X-clipped polygons for up to four columns and keeping the original
X-then-Y sequence and row-major emission passed the ARM packet golden
and the 120-frame Mini direct-pack comparison. The checked run reuses
138,286 horizontal clips across 240 packet preparations (two preparations
per frame for the comparison). Over 600 frames, the cache reached 40.80 fps
against 40.99 and 40.92 fps, and stage preparation increased to 12.43 ms from
12.10 ms. State and final viewport pixels matched. The renderer was restored.
Evidence is in `build/acm/texture-cell-traffic/results.json` and
`build/acm/reuse-horizontal-cell-clip/results.json`.


### Division-free origin-cell packet classification

After the original viewport/mip clipping, ordinary origin cells can be
recognized by comparing UV numerators with the same binary32 denominators
used in the reference divisions. These polygons use implicit identity
transforms and retain the existing indexed fan emission. The ARM fixture
matches the golden stream and signed-zero and adjacent-float UV boundary
cases. The Mini passed 120 packing-check frames.

The shortcut took 11.34 ms in packet preparation against 11.73 and 11.60 ms.
The prior GPU drain rose to 4.09 ms from 3.87 and 3.88 ms. The three 600-frame
runs reached 41.03 fps shortcut, 40.76 and 40.97 fps reference. Core state,
geometry state and viewport pixels matched. The selected renderer was
restored. Results and sources are under `build/acm/origin-cell-trial`.


### Nearest palette quantization and selected TGP profile

The compact palette test passes the ordinary and mediump Mesa fixtures.
On the Mini, letting nearest sampling replace the final index rounding
takes 19.16 ms in the one-pixel canvas probe, against 19.40 and 19.41 ms.
The final viewport is 99.2064% identical, and maximum RGB error is 84.
Retaining texel centering for the upper half recovers the reference
viewport but takes 19.93 ms. Both retain game and geometry state. The
selected shader retains explicit quantization and texel centering.
Evidence is under `build/acm/palette-nearest-quantization-trial`.

A new 2,400-frame run of the selected core-PGO build retained the native
matrix paths and matched the selected reference's game/geometry hashes
and final viewport. It recorded 9,016 simulation-thread samples, including
3,210 with a DSP command tag. Dispatcher/partial context accounted for
4.58% of simulation samples, command 0x4a 3.33%, 0x6b 2.52%, and 0x3a
2.43%. The scheduler loop had 9.52% of samples and native i960 code 27.14%.
The tag profile includes diagnostic overhead. See
`build/acm/selected-tgp-command-profile/results.json`.


### DSP address samples for commands 0x4a and 0x6b

The targeted diagnostic adds DSP address tags for the two commands while
retaining the native and resumable matrix paths. Other commands keep
coarser tags. The ARM fixture covers 112 command identities, exact address
tags for the two selected commands, and all three resumable native entries.

The 2,400-frame Mini trace recorded 430 address samples for command 0x4a
and 299 for 0x6b. No sampled addresses overlapped. For 0x4a, 343 samples
were in 0x0cfa through 0x0e04 and 24 landed on the repeated FIFO write at
0x0dc6. That guest instruction writes each of 12 words separately.
For 0x6b, 78 samples were in the three rotation helpers at 0x0b74 through
0x0bc2. Counts include time in host callbacks or native regions attributed
to the current guest address. All expected core and geometry hashes, both
native matrix counters and the final viewport matched the selected
reference. The probe is archived under `build/acm/hot-dsp-pc-profile`.


### Native DSP rotation arithmetic trial

The model's three 20-instruction rotation tails at 0x0b79, 0x0b93 and
0x0bad were replaced with native arithmetic when the 33-cycle private
region fits. The original sine/cosine accesses and return instruction
run at their original cycle positions. Program revision, RAM ranges,
overlap, repeat state and exceptional floating-point results are guarded.
ARM/QEMU matched complete CPU state, RAM, instruction totals and timed
I/O in 36,000 fixtures. A 12-frame FIFO trace matched all 41,908 records
byte for byte.

With rendering enabled, 74,621 native rotations ran in 600 frames.
Simulation averaged 20.72 ms/task against 21.09 and 21.17 ms. The three
throughput measurements were 41.14 fps and 40.82/41.05 fps, respectively.
The Mini game state, geometry state and final viewport matched.
The 1,200-frame simulation-only measurements were 20.74 ms/frame with
147,024 native rotations, and 20.91/21.35 ms/frame in the references.
State matched in all three. The kernel is archived for combined CPU
work and the selected PGO build remains active. See
`build/acm/dsp-rotation-trial/results.json` and `simulation-results.json`.


### Native repeated FIFO output at DSP address 0x0dc6

A direct loop grouped the dispatch of the repeated low-RAM-to-FIFO MOV.
Each word retains the original RF callback, guest PC, post-increment,
repeat count and cycle debit. The loop stops on a callback-induced slice
abort. Unsupported RAM ranges, unusual repeat counts and untrusted
program views use the original handler.

ARM/QEMU passed 26,880 state and callback fixtures, including short
budgets, address wrap, disabled bus capabilities, and read stall and write
abort behavior. Independently, 41,908 FIFO trace records matched byte
for byte. The Mini used 115,776 native words over 600 frames and reached
41.03 fps, against 40.90 and 41.00 fps. Simulation took 21.24 ms/task,
between the references of 21.33 and 21.11 ms. Game and geometry hashes and
final viewport pixels matched. The selected core was restored.
Sources and evidence are under `build/acm/dsp-fifo-repeat-trial`.


### Combined native rotation and mip-band sweep with fresh core PGO

The two archived variants were combined after the compact palette change.
Both ARM fixtures passed, and the Mini produced 25 CRC-verified profiles
in an isolated 600-frame training session. The new core-PGO binary was
trained with both variants enabled. The selected profiles were preserved.

At 2,400 frames it reached 43.07 fps against a fresh 43.08 fps run of the
selected binary. Packet preparation improved from 11.92 to 11.25 ms
and simulation from 18.35 to 18.11 ms/task. Prior GPU wait increased from
3.31 to 4.04 ms. It executed 292,805 native rotations. Switching both
variants on and off in the same trained binary reproduced the result at
1,200 frames: 42.19 vs 42.20 fps, with preparation 11.74 vs 12.26 ms and
GPU wait 4.05 vs 3.55 ms. Game and geometry hashes and final viewport pixels
matched in each pair. The selected source and binary were restored.
Training data, fixtures, source snapshots and results are archived under
`build/acm/combined-pgo-trial`.


### Opaque depth prepass

An opaque-only depth draw used a small positive depth offset before the
original colour pass. Alpha and checker triangles were excluded, and
colour depth comparison and draw order were preserved. Mesa staged
fixtures passed at both shader precisions. On a frozen fight scene the
240-frame one-pixel resolve decreased from 19.44 to 18.80 ms, with a
pixel-identical capture. Total throughput fell from 28.30/28.36 to 27.87 fps.

The moving 600-frame comparison achieved 39.14 fps versus 40.91/41.11 fps.
Submission cost rose from about 2.83 to 4.26 ms. Preparing depth indices
alongside the ordinary indices reduced submission to 3.74 ms but reached
39.87 fps against a 41.12 fps client-array reference. Using buffer-backed
vertices reached 38.91 fps against 38.87 fps. All corresponding captures
were identical, and moving runs matched game and geometry state. The
renderer was restored. Evidence is in `build/acm/opaque-depth-prepass-trial`
and `build/acm/prepared-depth-prepass-trial`.


### Coalescing consecutive draws across three atlas pages

The existing atlas placement, UVs, index order and vertex stride were
preserved. Each coalesced draw used up to three bound pages and passed the
two texture slots per primitive. Geometry and final viewport captures
matched in both frozen and moving runs. The original renderer was restored.

In 240 frozen frames, draw count fell from 5,520 to 2,160. Fence/flush rose
from 19.25 to 20.59 ms and throughput fell from 29.51 to 28.57 fps. In 600
moving frames, draw count fell from 19,545 to 10,542 and 3D submit fell from
2.84/2.89 to 2.58 ms. Prior GPU drain grew from 3.95/4.00 to 5.09 ms, and
throughput was 39.70 fps against reference runs of 40.77 and 40.64 fps.
Sources, logs and results are in `build/acm/multi-atlas-draw-trial`.

### Selected bilinear checkpoint baseline

The selected core-PGO build rendered 2,400 moving frames in 55.876 seconds
at 42.95 fps with bilinear LCD presentation and the sound board held.
The earlier 43.12 fps result used nearest-neighbor presentation. Game and
geometry hashes matched. Logs, launch options and stage measurements are in
`build/acm/bilinear-selected-baseline`.

### Skip empty tile regions by priority

A 180-frame checkpoint probe suppressed 3D draws and forced a one-pixel
read with the canvas FBO still bound. All 2D passes took 12.24 to 12.29 ms
including readback. Clear-only controls took 1.23 to 1.28 ms. Removing
only the above-3D pass reduced the resolve by about 5 ms. The foreground
changed 9.57% of the final viewport in the sampled foreground-only capture.
These timings isolate the 2D work with a forced resolve.

The renderer tracks which decoded 8x8 cells contain visible pixels at each
priority. It trims transparent rectangles to covered bands, keeping
neighboring cells as a margin for texture interpolation and leaving window
tests in the shader. Opaque backdrop rectangles stay intact. The original
scroll mapping and draw order are preserved. Desktop Mesa and the Mini
passed the 24 existing tile cases and four new sparse cases. Fight viewport
captures matched pixel for pixel at 120, 300, 600 and 2,400 frames.

The 2,400-frame core-PGO runs with bilinear presentation reached 46.71 and
46.63 fps with coverage enabled. Matched runs with coverage disabled reached
43.02 and 42.96 fps. Prior GPU drain fell from about 3.4 ms to
0.08 ms per frame. Each coverage run found only one pending prior fence
after packet preparation. The sound board was held for these runs.
The full-render `--fast` preset enables the change. Use
`--no-clip-empty-tiles` to compare with the original geometry and shader.
Evidence is in `build/acm/tile-pass-probe` and
`build/acm/tile-coverage-trial`.

The gameplay target is locked 57.53 Hz with headroom above the native
rate. Report frame-time distribution and missed 17.38 ms deadlines in
representative paced gameplay alongside uncapped throughput.

### Frame deadlines and simulation samples by window

`--frame-trace` archives simulation task time, render-loop wait for
simulation, EGL swap return time and pacing deadlines for each tick.
`analyze-frame-trace.py` reports the full run and a steady-state view
excluding the first 120 ticks and the final framebuffer capture. Swap
timestamps measure submission cadence at EGL return. Scanout timing would
need separate display-side measurement.

With bilinear presentation and expensive tile regions skipped, the
2,400-frame uncapped reference reached 46.49 fps. The steady-state swap
interval was 21.07 ms median, 27.26 ms p95 and 31.16 ms p99. Simulation
tasks took 26.54 ms at p95. In the paced run, 2,277 of 2,279 steady-state
swaps missed the configured 17.384 ms deadlines. The pacer rebased its
schedule 84 times after startup. All three reference traces matched the
game state, geometry state and final viewport pixel for pixel.

`--profile-simulation-windows` preserves CPU sample deltas for each
60-frame report interval. Pass repeated `--window-end` arguments to
`analyze-cpu-samples.py` to combine intervals. All 9,363 samples were
accounted for across 40 windows. The four windows ending at 600, 660,
2,160 and 2,220 held 1,034 samples and had 7.9 to 11.9 ms per frame of
simulation wait. Within those windows, board, buses and scheduling
accounted for 27.76% of simulation CPU samples, TGP work for 27.27%,
and recompiled i960 code for 24.85%. The generic 64-bit divide took 61
samples across those windows, or 5.90%, versus 0.89% across the fight.
The ongoing split between rendering and simulation can place work for
the next tick at an interval boundary. Evidence is in
`build/acm/deadline-baseline`.

### Deferring i960 program-counter stores on mapped memory accesses

The mapped-RAM trial deferred program-counter stores on scalar read/write
page hits and committed the PCs before device callbacks or on cycle exits.
It passed the 143,689 interpreter comparisons in each of the three ARM
fixture configurations. The 2,400-frame comparison matched game and
geometry state and all viewport pixels. The trial ran at 46.19 fps
against references at 46.50 and 46.51 fps, with simulation p95 increasing
from about 26.47 to 26.78 ms. The original path remains selected.
Evidence is in `build/acm/i960-memory-pc-trial`.

### Exact elapsed-cycle division for i960 timer reads

The 25 MHz board timer period factors exactly into a power of two and
the cached scheduler divisor. Timer reads divide the shifted elapsed
attoseconds using that exact reciprocal whenever the shifted dividend
fits in 64 bits. The wide path covers longer intervals.

The matched diagnostic eliminated all 49 wide-division samples across
the four slow windows and reduced simulation p95 from 26.49 to 24.33 ms.
After verified profile-guided retraining, the matched 2,400-frame runs
measured 46.59 vs 46.98 fps. Simulation p95 fell
from 27.09 to 23.95 ms, and EGL swap interval
p95 fell from 27.83 to 24.91 ms. The state
hashes and final viewport matched the previous bilinear baseline.
The fast preset enables `--i960-timer-reciprocal`. Evidence is in
`build/acm/i960-timer-division-trial`.

### i960 multiword store stalls and direct RAM burst trial

The expanded native-vs-interpreter fixture exposed a fallthrough after
a stalled multiword store in the generated helper path. The translation
now checks for a stall after `stl`, `stt` and `stq` helpers, so the
saved burst resumes before the following guest instruction. The default
build passes 145,609 four-slice cases in each of three ARM configurations.

A separate test emitted aligned multiword accesses inside a plain BURST
page directly. Page crossings, unaligned operations and devices used
the existing helper. The generated path matched game state, geometry
and all viewport pixels across 2,400 frames. Its 46.90 fps result was
slightly below the 47.02 and 46.94 fps references, with slightly
higher p95 frame and simulation intervals. The direct access path was
discarded. Evidence is in `build/acm/i960-multiword-trial`.

### i960 cycle-budget checks across arithmetic instruction runs

The compiler trial grouped contiguous callback-free instructions behind
one budget check. Individual entry labels and the original execution path
covered short budgets and entry into the middle. The combined fixture
passed 145,929 four-slice cases in all three ARM configurations. The
2,400-frame comparison matched all viewport pixels and both state hashes.
The trial reached 46.65 fps versus 47.08 and 46.90 fps for the controls,
with a p95 EGL interval of 24.93 ms versus 24.75 and 24.88 ms. The original
per-instruction checks remain selected. Evidence is in
`build/acm/i960-cycle-run-trial`.

### Rendering thread after timer and tile changes

The current Mini profile covered 2,400 frames and matched the game,
geometry and final viewport of the selected bilinear build. Packet and
material preparation averaged 13.20 ms wall and 11.66 ms main-thread
CPU per frame. Camera preparation averaged 3.02 ms wall. Prior GPU drain
averaged 0.078 ms, with one pending prior fence after packet preparation.
Main-thread samples attributed 16.73% to indexed tile packet construction,
13.50% to direct batch planning and 9.57% to the main thread's direct
packing loop. Evidence is in `build/acm/post-timer-render-profile`.

### Screen coverage and frame-local atlas lookups

A read-only 600-frame inventory processed the existing draw order with
8-by-8 screen blocks. It expanded the candidate bounding boxes and eroded
occluders by two pixels, respected arcade windows, and required a four-entry
depth separation. It flagged 2,346 of 858,895 active textured bands as
potentially fully covered when unmasked textured polygons counted as
occluders. Solid-only occluders covered zero bands. This is a screen-space
estimate. Textured polygon seams would need further verification before
actual rejection. The 0.27% opportunity under these rules did not justify
a culling path. Evidence is in `build/acm/3d-coverage-inventory`.

The atlas trial checked a 512-slot lookup cache per batch, short-circuiting
the existing atlas map only for an exact key already resolved against the
same RAM snapshot. It hit 5,711,813 of 6,923,986 lookups in 2,400 frames.
Packet preparation remained at about 13.1 ms per frame, and throughput
was 46.76 fps versus controls at 46.76 and 46.70 fps. Game state, geometry
and the final viewport matched. The original lookup remains selected.
Evidence is in `build/acm/atlas-frame-lookup-trial`.

### Packet worker bands and mip sweep on the current pipeline

Splitting packet work into 8 or 16 bands per thread gave 45.44 and 45.70
fps over 600 frames against the 4-band controls at 45.56 and 45.72.
Viewport captures and both state hashes matched. Evidence is in
`build/acm/packet-work-bands-trial`.

The earlier mip-band sweep matched 131,072 ARM boundary values and the
packet fixture's 29,325 vertices. On the current render pipeline, the
sweep reduced packet and material preparation from 12.92 to 13.05 ms
in the controls to 12.56 ms. Simulation wait grew from 1.23 ms in the
first control to 1.59 ms. Prior GPU drain stayed close to 0.08 ms.
Over matched 2,400-frame runs, it averaged 47.09 fps against 47.28 and
46.85 fps. All game state, geometry state and final viewport comparisons
matched. The interactive build retains the original packet path.
Evidence is in `build/acm/mip-sweep-recheck`.

### CPU pool and geometry worker on the current build

At three render threads, asynchronous geometry sustained 45.62 and
45.48 fps over 600 frames, against 41.80 fps with synchronous geometry.
Two and four render threads with the worker enabled ran at 43.40 and
45.47 fps. With synchronous geometry they reached 42.39 and 41.91 fps.
All seven runs matched state, geometry and final viewport. Results and
per-frame traces are in `build/acm/cpu-overlap-trial`.

### Palette planning alongside packet work

The staged GLES renderer uses one existing worker-pool task to produce
CPU palette rows while the other tasks build polygon packets. All jobs
read the same RAM snapshot. Palette ordering stays serial within that
task, and uploads still run on the GLES context thread after the fence.

The 2,400-frame trial reached 47.42 fps against references at 47.08 and
46.91. Mean packet and material preparation fell from 13.07 and 13.11 ms
to 12.66 ms. The p95 EGL frame interval remained about 24.8 ms. All game
state and viewport comparisons matched exactly. The full `--fast` preset
enables this work split. Use `--no-overlap-palettes` for the serial
reference. Evidence is in `build/acm/palette-overlap-trial`.

The staged GLES2 fixtures with palette overlap enabled passed in Mesa and
on the Mini, in highp and forced mediump modes. They check the CPU-only
GL boundary, source changes, deferred resource lifetime and fallback.

### Inline i960 literal folding

An independent trial used ARM immediate forms for five-bit operands in
native i960 logical, add/subtract and shift operations. The fixture
matched all 145,609 interpreter comparisons in all three configurations.
Over 2,400 frames, generated code shrank from 3,943,192 to 3,900,626 bytes.
The trial reached 46.94 fps against references at 46.95 and 46.79 and
matched their state and viewport. The original emission remains selected.
Evidence is in `build/acm/i960-literal-fold-trial`.

Production validation with palette overlap and pacing reached 47.16 fps
over 2,400 frames. The viewport and both state hashes matched the prior
checkpoint reference. EGL frame intervals measured 24.83 ms at p95.
The pacing trace missed 2,275 of 2,279 deadlines after warmup and before
the final capture. See `build/acm/palette-overlap-trial/selected-paced.json`.

### Mip-band sweep with palette overlap enabled

The sweep and first control each reached 47.18 fps over 2,400 paced frames.
The second control reached 47.24 fps. Packet preparation fell from 12.54
and 12.59 ms to 11.92 ms, while p95 EGL intervals remained close to 24.9 ms.
The final viewport and both state hashes matched. The sweep remains
disabled. Evidence is in `build/acm/mip-palette-combination`.

### Geometry worker CPU profile

`--profile-geometry` selects the owned geometry worker's CPU clock on
a finite run. The sampler starts on that worker and uses the same
per-process symbol map as the renderer and simulation samplers.
The 2,400-frame profile matched game state, geometry state and the final
viewport, and collected 2,090 samples in 10.63 worker CPU seconds.

The direct normal/specular parser accounted for 40.81% of samples, and
triangle and quad packet construction together accounted for 41.20%.
Resolving libc symbols attributed 7.94% to `memset`. ARM source mapping
identifies calls in the compact and legacy camera-record constructors.
Sample reports are in `build/acm/geometry-thread-profile`.

Selecting the material source once and loading UV and header words in
contiguous spans retained the masked per-word path at a RAM/ROM edge.
The 600-frame variant reached 45.35 fps versus 45.37 and 45.69 for the
controls. Its state and viewport matched. The original reads remain
selected. Evidence is in `build/acm/geometry-material-span`.

Constructing compact camera records as complete aggregates avoided
the explicit zero-and-fill sequence, but introduced a copy of the
record. The 600-frame experiment reached 45.64 fps versus 45.54 and
45.31, with p95 EGL intervals of 27.54 ms versus 27.48 and 27.75 ms.
State and viewport comparisons matched. The original producer remains
selected. Evidence is in `build/acm/geometry-camera-construction`.

### Camera record contexts and projection

Constructing the full camera record directly in its vector slot reached
45.87 fps over 600 frames, with 45.78 and 45.63 fps for the controls. Its
p95 interval was 27.45 ms versus 27.39 and 27.34 ms. Core and geometry
hashes and final viewport pixels matched. Evidence is in
`build/acm/camera-in-place-trial`.

The same trial logged 1,042,859 compact records across 603 publications,
including startup. Each publication used one consecutive clip-plane,
viewport and projection-center context. The context prototype reduces
the ARM record from 176 to 100 bytes and stores 80 bytes per context.
It captures changes in the raster viewport command and projection-center
selection, and applies the CRTC offsets once per context at capture.

The direct projection prototype clips compact vertices against that
context, resolves the material of surviving polygons, and writes the
projected result without staging a camera polygon. The ARM checks pass
three alternating contexts with different clipping, centers and viewports,
both projection paths, the 120-frame independent renderer comparison
over 177,125 polygons, pending feedback and retained-picture lifetime.
The 600-frame hardware trial averaged 2.29 ms for camera preparation and
46.64 fps. The same binary's two inline-record controls averaged 3.01
and 3.07 ms and ran at 46.00 and 45.46 fps. Sharing contexts with the
original projection averaged 2.66 ms and 46.82 fps in that trial.
All four runs matched the state hashes and final viewport.

The prototypes, results and check logs are in `build/acm/camera-context-trial`
and `build/acm/camera-context-project-trial`.

The 2,400-frame paced comparison reached 48.16 fps and 24.19 ms p95
with direct projection. The controls reached 47.18 and 47.10 fps with
25.00 and 25.02 ms p95. Camera preparation fell from about 3.01 ms to
2.24 ms. Both 2,400-frame state hashes and all final viewport pixels
matched across the three runs. See `build/acm/camera-context-project-trial/paced`.

The fast preset now uses context-indexed camera packets and direct compact
projection. The rebuilt production binary reached 48.42 fps with
24.08 ms p95 over 2,400 paced frames. Camera preparation
averaged 2.25 ms. Both state hashes and the final
viewport matched the inline reference. In the 2,279-frame analysis window,
2,274 pacing deadlines were missed and 1,880 simulation tasks exceeded
17.384 ms. Shared and legacy ARM checks passed. Logs and results are in
`build/acm/camera-context-selected`.

### Simulation profile, idle SCSP mixer and private TGP repeats

The 1,200-frame CPU sampling run on the shared-camera build collected
4,625 samples in 23.30 simulation-thread CPU seconds. The TGP accounted
for 31.26%, emitted i960 game code 29.10% and board/bus/scheduler work
25.99%. The central scheduler and TGP execution loop accounted for
10.62% and 9.21% individually. Reports and frame-window sample records
are indexed under `build/acm/post-context-simulation-profile`.

A separate 16 KiB trusted DSP handler pointer table matched state and
the final viewport over 600 frames. It measured 45.86 fps with 46.02 and
46.16 fps for the same-binary controls. That layout remains experimental.
Evidence is under `build/acm/tgp-pointer-table-trial`.

The SCSP emits silence in one batch when all 32 voices are inactive,
the DSP is stopped with clear effect returns, and probes, group gains
and FM delay are inactive. It advances the FM cursor and clears external
inputs as before. The existing stream call still updates its clock and
the slot monitor. The ARM reference comparison matched samples and mixer
state through 4,096 updates, including active slots, DSP, effect returns,
group gains and probes. The 2,400-frame same-binary experiment reached
48.77 fps versus 48.16 and 48.17, with simulation averaging 18.69 ms
versus 19.14 and 19.18 ms. The production build reached 48.73 fps and
23.97 ms p95 with matching state and final viewport. It missed 2,273 of
2,279 measured deadlines. Evidence is under `build/acm/scsp-idle-trial`
and `build/acm/scsp-idle-selected`.

The native private-copy trial grouped four incrementing internal-RAM MOV
instructions within the exact scheduler slice budget. ARM/QEMU matched
complete DSP state, RAM, cycles and instruction counters over 18,144
two-slice cases. The 600-frame trial matched hashes and viewport at
46.86 fps compared with 45.98 and 46.42 fps. It remains experimental
while the generated dispatch entries are separated from the existing
PGO-profiled handlers. Evidence is under `build/acm/tgp-private-repeat-trial`.

The private MOV wrappers now occupy separate generated dispatch entries.
The production PGO build passed the existing coverage-mismatch check
and 18,144 two-slice ARM cases. With idle SCSP enabled, the 2,400-frame
same-executable comparison ran at 48.86 fps versus 48.36 and 48.41 fps.
Mean simulation time was 18.73 ms versus 19.04 and 19.00 ms. The deployed
binaries measured 48.74 versus 48.58 fps and 23.97 versus 24.11 ms p95.
Game state, geometry state, and final viewport matched. The default fast
build keeps the native MOV path. Pair results are in
`build/acm/tgp-repeat-wrapper-trial/results.json` and
`build/acm/tgp-repeat-selected/deployed/results.json`.

Attributing the existing simulation window samples to i960 instruction
addressing modes found 827 scalar memory samples among 1,317 mapped
samples, including 154 constant-address loads or stores. Base-relative
loads accounted for 315. Reports are under
`build/acm/post-context-simulation-profile`.

### i960 memory access trials and per-frame stage attribution

Constant-address scalar operations were tested with a direct lookup of
the live page slot. The 9,600 new cases per ARM configuration cover the
constant displacement forms, direct and remapped RAM, alignment, bus
read stalls and write aborts. The full 155,209 four-slice fixture passes
in all three configurations. The 2,400-frame paced run measured 48.91 fps
versus 48.83 and 48.94, with 86,270 fewer emitted bytes and exact state and
viewport. The existing emitter remains selected. Evidence is in
`build/acm/i960-static-address-trial`.

A single native work-RAM span and a combined rotate-based alignment and
range guard also retained the game and viewport in 600-frame tests.
The span emitted 373,740 more bytes and ran at 46.48 fps versus 47.35
and 47.43. The rotate guard measured 46.52 fps versus 47.12 and 47.21.
Both are archived under `build/acm/i960-work-ram-trial` and
`build/acm/i960-rotated-guard-trial`. A static inventory found 59 sampled
scalar memory operations in sequences of at least three same-base operations
with the supported immediate instructions between them. See
`build/acm/post-context-simulation-profile/i960-scalar-page-runs.json`.

The 1,200-frame diagnostic added render-stage wall and main-thread CPU
durations to the frame trace. The final viewport and both state hashes
matched. After warmup and excluding the capture, the slowest 108 frame
intervals averaged 27.01 ms versus 19.64 ms for the other 971. Simulation
wait rose by 7.20 ms, tile upload by 1.04 ms, and picture capture by
0.71 ms. Packet preparation was 1.42 ms shorter on these frames. The
unaccounted mean time was under 0.02 ms in both groups. Evidence and
the standalone diagnostic sources are in `build/acm/frame-stage-trial`.

### Batched i960 countdown polling

The 60-frame combined sample windows diluted the simulation spike. A
second diagnostic tags individual ARM CPU samples with frame numbers
and records per-frame simulation-thread CPU and render-stage wall time.
The 2,400-frame trace has 9,056 tagged samples without overflow and
matching state and final viewport. The slowest 225 timed frames use
4.44 ms more simulation-thread CPU than the other 2,016 and spend only
0.17 ms more of the simulation task off-CPU. Estimated excess board,
bus and scheduler time is 2.77 ms. The timer reads in the i960 loop
ending at 0x4cba0 account for a substantial part of that difference.
Raw traces and analysis are in `build/acm/stamped-simulation-profile`.

After an actual iteration takes the poll's back edge, the native helper
can execute whole waiting iterations in a batch. It debits all 23 guest
cycles per iteration and stops before the timer exit, masked sentinel
or slice budget. The board's timer latch and i960 register match the
last batched sample. The exact code signature, stable work-RAM flag,
active 25 MHz timer, modular scheduler and instruction-counter checks
gate the fast path. Partial rounds still execute the normal instructions.
The three ARM configurations each matched the interpreter through 22,680
three-slice timer tests and the existing 145,609 four-slice cases.

The 2,400-frame paced comparison measured 49.52 fps and 22.41 ms p95
against 48.74 and 48.85 fps at 24.21 and 24.04 ms p95. Simulation
task p95 fell from about 23.23 ms to 19.85 ms in that comparison.
The rebuilt production preset measured 49.49 fps, 22.40 ms p95 and
24.05 ms p99. It missed 2,274 of 2,279 paced deadlines. State hashes
and all final viewport pixels matched. Evidence is in
`build/acm/i960-timer-poll-trial/paced` and
`build/acm/i960-timer-poll-selected`.

### Packet timing after timer batching and grouped tile updates

A new 2,400-frame, frame-tagged diagnostic matched state and the final
viewport. The slowest decile added 2.62 ms of packet wall time, 1.02 ms
of tile upload and 0.69 ms of picture capture. Simulation wait added
0.27 ms. The packet stage's main-thread CPU time grew by 0.90 ms. See
`build/acm/post-timer-tail-profile/stage-excess.json`.

The previous mip-band sweep reduced packet preparation by about 0.8 ms,
while simulation wait increased by about 0.7 ms. Its 49.32 fps and
22.61 ms p95 did not improve on the two same-binary controls. Skipping
viewport clips for inside polygons and eliding the single-cell copy
matched 5,000 byte-level packet cases but did not improve frame timing.
Reserving or sharing the pinned simulation core did not establish a
repeatable p95 improvement over unpinned execution. These three
experiments remain disabled.

Dirty tile cells now upload as connected rectangles for small updates
and as bounding rectangles per changed map for large updates. On ARM,
NEON packs eight tile indices and their flags into the unchanged RGBA
layout. All 32 layer, canvas and dirty-update fixtures pass on Mesa and
on the Mini's Mali driver. In the 2,400-frame same-executable comparison,
the number of tile-map uploads fell from 16,897 to 3,615 and encoded
bytes fell from 21,098,752 to 11,113,216. The combined path achieved
49.69 fps and 22.21 ms p95 against controls at 49.31 and 49.66 fps
with 22.49 and 22.36 ms p95.

The rebuilt default has 49.59 fps, 22.28 ms p95 and 23.33 ms p99, with
the same state hashes and exact final viewport. It missed 2,245 of
2,279 paced deadlines. See `build/acm/post-timer-tile-combination` and
`build/acm/post-timer-tile-selected` for both comparisons.

### Packet preparation breakdown and texture atlas index

The 2,400-frame packet diagnostic matched the game state, geometry state,
and final viewport. Ordinary frames averaged 6.16 ms for clipping and
texture packet construction, 4.24 ms for atlas and batch planning, and
2.17 ms for vertex and index packing. The slowest tenth added 1.25 ms in
packing and 0.85 ms in clipping and packet construction. In packing, the
extra 1.14 ms between the main thread's last task and the worker join
suggests inspecting how packing work is distributed. That interval can
include active worker computation and scheduling delay. The trace and
frame-tagged samples are in `build/acm/packet-substages-profile`.

Replacing the atlas key-to-record map with a power-of-two slot table and
stable tile records reduced 2,400-frame uncapped packet preparation to
12.68 ms from 12.85 and 12.74 ms in the two same-binary controls. The run
reached 49.89 fps against 49.40 and 49.60 fps, with p95 swap intervals of
22.33 ms against 22.59 and 22.64 ms. Core and geometry hashes and the final
viewport matched. The GLES2 fixture also checks tile-address stability
across table growth and lookup after clearing the atlas. Raster and staged
resource fixtures pass with highp and forced mediump on Mesa.

The production paced comparison matched the same state hashes and final
viewport. Packet preparation fell from 12.77 to 12.48 ms per frame.
Throughput rose from 49.64 to 49.80 fps and p95 swap intervals fell from
22.410 to 22.292 ms. The selected build missed 2,274 of 2,279 sampled
deadlines. Hardware results are in `build/acm/atlas-flat-index-trial`.

### Packing workload and direct buffer writes

Two same-binary comparisons used 600-frame runs from the fight checkpoint.
Partitioning packing by estimated per-job cost took 13.38 ms in packet
preparation against 12.86 and 12.95 ms for the controls. The p95 swap
interval was 22.47 ms against 22.52 and 22.64 ms. Direct writes of vertex
fields plus ARM NEON index rebasing took 12.98 ms in packet preparation
against 12.99 and 13.25 ms. Its p95 was 22.36 ms against 22.61 and 22.34
ms. Both sequences matched core and geometry hashes and the final viewport.
The direct-write path also passed the Mesa byte-comparison fixture with
array and indexed packing. The selected build uses its original packing
path. See `build/acm/weighted-packing-trial` and
`build/acm/direct-packing-stores-trial`.

### Simulation CPU and packet timings on the selected atlas build

The 2,400-frame tracked run recorded 8,915 simulation-thread CPU samples
with zero overflow, matching both state hashes and the final viewport.
The slowest tenth of displayed frames added 4.15 ms in swap intervals,
including 2.80 ms more packet wall time and 0.40 ms more simulation wait.
Packing accounted for 1.57 ms of the packet excess.

Ranking frames by simulation CPU time isolates a separate limit. The
busiest tenth used 20.59 ms, against 18.47 ms for the other frames.
Off-CPU task time rose only 0.17 ms. Sample-based attribution of the
extra CPU time was about 0.82 ms for TGP code, 0.66 ms for recompiled
i960 game code and 0.49 ms for the board, buses and scheduler. These
figures are sampling estimates. The saved captures, frame groups and
guest attribution are in `build/acm/atlas-selected-simulation-profile`.

### Frame-tagged TGP command samples on the atlas build

Two 2,400-frame captures recorded both the executing host PC and TGP
command context per CPU sample. The repeat used a shorter timer
interval and collected 13,699 frame-tagged samples, of which 5,105
carried a command tag. Both captures matched both state hashes, the
final viewport, and the two native matrix counters.

Command 0x3a led the first simulation-heavy group by nominal excess
sample time. In the repeat it occupied 1.95% of all simulation CPU
samples in the ordinary group and 2.05% in the busiest tenth
(221 of 11,346 and 29 of 1,415). The largest repeat excesses were
distributed across 0x34, 0x0c and 0x4a. The repeat host symbols show
changes across compiled i960, board/scheduler and TGP samples.
The command data does not isolate one TGP handler to optimize.
Raw traces and reports are in `build/acm/tgp-frame-command-profile`
and `build/acm/tgp-frame-command-profile-dense`.

### i960 direct memory and bus fallback layout

The current Mini samples map 37 of 55 emitted-code samples in function
0x16504 to scalar loads and stores on simulation-heavy frames. The
inventory is in `build/acm/i960-lazy-memory-pc-trial/guest-op-inventory.json`.

The deferred-PC trial kept the original PC commit before bus callbacks,
exceptional exits and slice boundaries. It passed 145,609 four-slice
interpreter comparisons in each of the three register-cycle and lazy-PC
configurations, including callbacks that inspect both PCs and cycles.
It grew emitted code from 3,943,248 to 4,451,450 bytes. The trial reached
47.81 fps against 48.72 and 48.56 fps in two same-binary 600-frame controls.
Both state hashes and final viewport pixels matched. The existing PC
commit policy remains selected. See `build/acm/i960-lazy-memory-pc-trial`.

The outlined-bus trial let direct accesses fall through and placed the
bus callbacks in a separate code region, with profiler attribution for
the outlined instructions. It passed the same interpreter fixture in
all three configurations, with exact device state hashes and viewport.
The 600-frame controls reached 48.67 and 48.66 fps, versus 48.37 fps
with outlined bus callbacks. Generated code grew by 219,830 bytes.
See `build/acm/i960-outline-memory-trial`.

### Wrap eligibility on the selected atlas renderer

The 600-frame inventory found 347,408 textured mip bands crossing
one or more texture cells. Of these, 21,821 crossed only wrapping
axes without mirroring, emitting 862,104 of the 2,716,365 multicell
indices. The other 325,587 bands crossed a mirrored or clamping axis.
No plain-wrap candidate used a base texture within 128 by 128, so
doubling small repeating tiles would skip none of these splits.
The run matched both state hashes and the final viewport. Counts are
in `build/acm/repeat-atlas-inventory/counts.json`.

The exact texture-cell clipping trial cached signed plane distances.
Host and ARM/QEMU retained the 29,325-vertex packet hash. On the Mini,
packet preparation took 13.19 ms against 13.24 and 13.29 ms in the
600-frame controls. Overall throughput stayed within the controls'
spread. The selected path is unchanged. The packet fixture and Mini
captures are in `build/acm/cached-cell-distances-trial`.

The by-material inventory assigns 691,119 of the plain-wrap indices to
one opaque 256-square, two-axis-repeat material at texture RAM address
(1792, 0) on sheet 0. Its mip band 3 alone accounts for 192,105 indices.
These are submitted geometry counts, which also include the triangles
a native sampler would still need to draw. The 600-frame inventory
matched both state hashes and the final viewport. See
`build/acm/repeat-material-inventory/results.json`.

### Native wrapping for one VF2 texture

The isolated experiment uses ordinary GLES2 repeat sampling for the
32/16 mip pair of the 256-square texture on sheet 0 at (1792, 0).
The matching polygons retain viewport and mip clipping. Atlas batches
continue to hold the remaining material. The selected mip pair uses
two small standalone textures with the same generation and upload
lifetime as the atlas.

Over 600 fight frames the isolated pair removed 184,875 indices and
added 487 draws. Packet preparation took 13.02 ms against 13.31 and
13.37 ms in the two controls. The final viewport matched at frames
120, 300 and 600, and each pair matched both state hashes. A patterned
Mesa fixture validates expected colour and owner on two-axis seams
with ordinary and forced-mediump shaders.

Expanding to six mip bands for the same material saved 637,842 indices
and 1.10 ms of preparation against the first control. It added 2,057
draws and changed 0.3525% of final viewport pixels. The broader mode
remains experimental. The source and test archives are
`build/acm/native-repeat-single` and `build/acm/native-repeat-material`.

The isolated pair also matched the 2,400-frame viewport and state at
arcade pacing. It removed 547,632 indices and added 1,794 draws.
Preparation took 12.76 ms against 12.95 and 12.99 ms for the controls.
Throughput was 49.66 fps against 49.66 and 49.47 fps, with a p95 swap
interval of 22.33 ms against 22.27 and 22.13 ms. The selected renderer
continues to use the atlas for this pair. See
`build/acm/native-repeat-material/paced/results.json`.

A six-band rerun reserved the original atlas slots before choosing the
standalone mip pair, so other texture placements were preserved. Its
600-frame viewport was 99.9642% identical, compared with 99.6475% without
the reservations. The largest channel delta fell from 126 to 11.
Preparation fell from 13.21 to 12.27 ms in this pair, and throughput
rose from 47.98 to 48.98 fps. Both state hashes matched. This remains
experimental. See `build/acm/native-repeat-preserved-atlas/results.json`.

### Compare native repeat and the atlas against scanline rendering

On the longer 2,400-frame comparison, the six-band GPU repeat mode
with original atlas reservations reached 50.09 fps, against 49.65
and 49.42 fps in the controls. Packet preparation fell by 0.68 ms
versus the first control. The capture matched the atlas exactly on
94.88% of pixels, and 99.54% of pixels were within eight channel
values. The test retained the game and geometry hashes.

The crossing-only variant keeps single-cell polygons in their original
atlas regions and uses standalone textures for selected bands that
cross a seam. The ARM fixture verifies that single-cell and mirrored
packets retain their original path. Over 2,400 paced frames, the variant
reached 50.49 fps and 22.00 ms p95 with identical game and geometry
state to the atlas runs. Its 11.88 ms packet stage is compared with the
13.13 ms of the preceding atlas control, built from the earlier diagnostic.
The crossing mode still missed 2,277 of 2,279 paced deadlines.

The software reference used the board's scanline renderer and the same
bilinear presentation on the Mini. Its game and geometry hashes match
each GPU capture at frames 120, 300 and 2,400. Rendering only the final
frame for the long reference was verified at frame 120 against rendering
every frame. Across the viewport, mean absolute RGB-channel error of
the atlas and six-band mode respectively was 6.248 and 6.247 at frame
120, 5.754 and 5.756 at 300, and 4.658 and 4.665 at 2,400. The crossing-only
capture gave 4.666 at frame 2,400. Comparisons include error restricted
to pixels changed between GPU paths. The atlas is the performance
baseline. The scanline images are the fidelity comparison.

The artifacts are in `build/acm/native-repeat-preserved-atlas/paced`,
`build/acm/native-repeat-crossing-only` and
`build/acm/model2-software-reference/comparisons.json`.

The sampler-only diagnostic leaves the original splits and submitted
index count in place. At frame 2,400 the standalone textures and UV
scaling alone changed 4.33% of atlas pixels, and reduced mean channel
error against software from 4.658 to 4.639. Removing the splits and
recentering UVs then changed 5.02% of the sampler-only pixels, with
mean error 4.666 versus software. The two changes affect overlapping
pixels, so the percentages describe separate comparisons. See
`build/acm/native-repeat-sampler-only/source-isolation.json`.

### Native repeat for seam-crossing packets in the fast preset

The full-game fast preset enables native repeat only for the profiled
opaque 256x256 material and its selected mip bands when a packet
crosses a texture seam. The original atlas placements are reserved
before binding the standalone textures. Single-cell, mirrored and
non-indexed packets use their original path. Run with
`--no-native-repeat-material` to select atlas sampling throughout.

The controlled 2,400-frame experiment measured 50.47 fps and 22.02 ms
p95 with native repeat, versus 49.38 and 49.54 fps with p95 at 22.50
and 22.45 ms in the two same-binary atlas controls. The mean packet
stage dropped from 12.85 and 12.84 ms to 11.89 ms. The rebuilt fast
preset averaged 50.21 fps, 22.14 ms p95 and 11.88 ms in the packet
stage. It matched the prototype's final viewport and both state hashes.
The atlas and native repeat are compared for fidelity separately against
the scanline software renderer as described above. Mesa passed the
patterned seam fixture with highp and forced mediump. The normal and
reference packet hashes also match on x86 and ARM/QEMU.

The separate cold-start sequence ran through attract and select into
a fight in both presets and matched game and geometry state. The
native-repeat capture was 99.42% identical to the atlas and 99.95%
within eight channel values. Cold-start throughput was 53.24 fps
with repeat and 53.14 fps with the atlas. The standard ARM and PGO
binaries also matched both state hashes and every viewport pixel in
the 30-frame fight smoke test. Results and logs are under
`build/acm/native-repeat-integrated`.

### Packet substage profile with native repeat

The two instrumented 2,400-frame runs matched their corresponding
production state and viewport captures. Over 2,241 paired fight frames
after warmup and excluding periodic sample dumps and final capture,
the average packet stage fell from 13.04 to 12.44 ms. Clipping fell
from 6.44 to 5.85 ms, atlas and batch planning rose from 4.00 to
4.11 ms, and packing fell from 2.28 to 2.19 ms. The average submitted
vertex count fell from 6,245 to 5,683.

With native repeat, the slowest tenth of swap intervals added
1.08 ms of packing wall time and 1.07 ms of simulation wait. Packing
recorded 1.10 ms more elapsed time after the main thread's last
packing task, with 0.20 ms more summed packing CPU. Simulation
tasks averaged 18.53 ms wall and exceeded 17.384 ms in 1,746 of
the same 2,241 frames. The times were measured with a concurrent
renderer. Paired stage data and frame-tagged samples are in
`build/acm/post-native-repeat-profile`.

### Persistent texture-pair lookup experiment

A 600-frame inventory recorded 858,895 requests. A bounded table found
a valid pair for 648,578 requests from the same batch and 166,552 from
an earlier batch (94.90% combined). Full keys and both texture
generations guard reuse. Clearing the atlas invalidates held placements.
Mesa passed texture-sheet changes and atlas clearing while using the
pair cache, at both shader precisions.

The 2,400-frame paired trace held geometry and viewport pixels exact.
Lookup reuse cut mean atlas and batch planning by 0.18 ms and the full
packet stage by 0.21 ms. Simulation gained 0.06 ms of CPU time and had
0.13 ms more visible wait. Throughput reached 49.17 vs 48.85 fps, with
p95 intervals at 22.64 vs 22.76 ms and 2,247 missed deadlines in both
cases. The shorter runs had mixed frame-time results. The selected
preset retains the simpler lookup path.
See `build/acm/persistent-pair-cache-trial` and
`build/acm/persistent-pair-cache-profile/results.json`.

The same-tile shortcut avoided the second validation on 345,836 of
858,895 requests in the short inventory. The 600-frame run spent
11.76 ms preparing packets against 11.99 and 11.83 ms in controls.
It reached 49.21 fps against 49.24 and 49.07 fps with exact state
and viewport. The shortcut was archived in `build/acm/single-tile-pair-trial`.

### Simulation cost with and without concurrent rendering

The selected binary ran the 2,400-frame fight at 75.62 simulation
ticks per second with drawing disabled and the geometry worker
enabled. Both game and geometry hashes matched the full-render run.
Across the same 2,241 frame numbers, mean simulation wall time was
13.15 ms with drawing disabled and 18.51 ms with full rendering.
The respective p95s were 14.23 and 20.16 ms. Two simulation-only
frames exceeded 17.384 ms, versus 1,809 with full rendering.

A diagnostic with every other draw disabled and the same paced
simulation averaged 16.21 ms for simulation following drawn
frames and 14.53 ms following skipped frames. The corresponding
frames in the full-render control averaged 18.53 and 18.48 ms.
Both state hashes matched. This is an overlap measurement. The
selected preset continues to draw every frame. Traces are in
`build/acm/post-native-repeat-simulation-only` and
`build/acm/post-native-repeat-draw-contention`.

### Full-cadence scheduling trials

A first sweep capped clipping and packing separately at two threads.
The trial compiled the board translation unit without PGO because
the shared pool header had changed. The pack-only cap improved
throughput in that executable, with p95 intervals within the controls.
Limiting clipping increased packet preparation from 10.95 to 13.53 ms
and p95 from 22.49 to 23.22 ms. Both hashes and final viewport matched.
See `build/acm/render-phase-worker-trial`.

The next comparison held the normal core PGO build and moved the
next simulation launch after camera projection or after packet
preparation. All runs still drew every frame. The early-launch
controls reached 49.29 and 49.25 fps. Launch after camera projection
reduced simulation task time from 18.51 to 17.69 ms but reached
46.46 fps. Launch after packet preparation reduced simulation time
to 16.00 ms and reached 35.86 fps. Both state hashes and final
viewport matched. The existing overlap remains selected. See
`build/acm/render-overlap-launch-trial`.

A renderer-local packing worker allowed a two-thread packing test
without changing the shared pool or core PGO. The Mesa staged and
byte-comparison fixtures passed at both shader precisions. In the
600-frame series, two-thread packing reached 49.74 fps and 21.65 ms
p95, with the two controls at 48.96/49.31 fps and 21.92/21.91 ms p95.

The same-binary 2,400-frame controls reached 50.47 and 50.45 fps.
Two-thread packing reached 50.81 fps, with p95 21.57 ms versus
21.79 and 21.87 ms. Mean simulation task time increased from
18.33/18.34 to 18.52 ms in this comparison. State and viewport
matched all controls, and submissions still drew every frame.

The fast preset now uses the two-thread final packing pass. Set
`--no-dual-pack` to use the shared pool for packing. A one-thread
pool remains single-threaded. The rebuilt default reached 50.53 fps
and 21.27 ms p95 over 2,400 paced frames, with 2,243 missed deadlines
of 2,279. The PGO and regular ARM binaries matched both state
hashes and the viewport in a separate smoke comparison.
See `build/acm/dual-packing-integrated/results.json`.


### CPU time during simulation and rendering

A diagnostic build traced elapsed time and thread CPU time over matching
2,400-tick fight runs. On the 2,241 paired steady-state ticks, simulation
averaged 18.31 ms elapsed and 18.24 ms CPU with full rendering, versus
13.00 ms elapsed and 12.90 ms CPU with drawing disabled. Average time off
the CPU was under 0.1 ms in both cases. With rendering active, the
simulation thread itself consumed more than the 17.384 ms tick budget
on 1,687 of the paired ticks. The geometry worker's mean CPU time also
rose from 3.70 to 3.97 ms.

A second paced run alternated draws while keeping the same full-game
pipeline and simulation worker. Compared with the same tick numbers
in the every-frame run, simulation used 2.24 ms less CPU when the
preceding tick drew and 3.76 ms less CPU when it skipped drawing.
All three runs matched game and geometry hashes. The every-frame
diagnostic also matched the production viewport pixel for pixel.

Thread CPU accounting shows where the extra time accrues. The stock
kernel returned ENOSYS for hardware counters through perf_event_open,
so the traces do not identify cache misses or memory stalls. The
diagnostic and raw traces are in build/acm/concurrency-performance-trace.
The current build still draws every frame.


### Wrapping texture coordinates inside atlas batches

Two GLES2 prototypes submit the original polygons after viewport and mip
clipping, and fold the UVs into each texture tile in the fragment shader.
They keep the existing palette lookup and polygon order. The controls
and trial modes use the same executable in each comparison.

For opaque two-axis repeat, the 2,400-frame paced trial reached
50.69 fps and 21.219 ms p95, against 50.44 and 50.86 fps and 21.590
and 21.248 ms p95 in the controls. Packet preparation increased to
12.249 ms from 12.030 and 12.152 ms. At the final capture, mean RGB
channel error against the independently rendered scanline image was
4.871 versus 4.666 for the control. Game and geometry hashes matched.

The broader trial also folded mirrored and clamped cells. It left
base-plus-microtexture packets on the existing path. In 600 paced
frames, it cut packet preparation to 11.366 ms from 12.214 and
12.072 ms. Sampled batch draws fell from 36.94 to 33.31 on average.
Prior GPU drain increased to 0.925 ms from 0.080 and 0.084 ms.
Throughput fell to 47.99 fps against 49.84 and 49.52 fps, and p99
swap time rose to 32.32 ms from 23.09 and 23.20 ms. The trial and
scanline capture matched both state hashes. The mean channel error
against scanline was 4.661 versus 4.589 in the control. The maximum
local channel difference between the GPU paths was 140.

The selected preset uses the existing atlas and selective hardware
repeat. Both prototypes passed the existing Mesa raster and packing
fixtures. See build/acm/atlas-shader-repeat-trial and
build/acm/atlas-shader-tile-fold-trial for sources, logs, and captures.


### Hardware mirrored repeat

The 600-frame inventory ran with the selected plain-repeat path enabled.
It matched both state hashes and the final viewport. Of 338,964 mip
bands still crossing a cell boundary, 325,062 crossed a clamped axis,
13,377 were plain repeat and 525 crossed a mirrored axis. All 525
belonged to one transparent 256-by-128 material. Its 1,050 cell visits
emitted 3,123 indices.

The trial uses GLES2 mirrored repeat on the eligible axis and the
original clamp behavior on the other axis. It binds a standalone
rectangle with per-axis sampler modes, preserves atlas reservations,
and recenters UVs by an even number of mirrored cells. Mesa passed the
existing raster and direct-pack fixtures. In a bracketed 600-frame
Mini test, it reached 49.87 fps versus 49.64 and 49.48 fps. Mean packet
preparation was 12.043 ms versus 12.025 and 12.085 ms. The sampled
draw count increased from 36.94 to 37.63 per frame. The game and geometry
hashes and final viewport matched both controls. The selected preset
retains its existing sampling paths.

Most unresolved seam work is in clamped materials. Those bands visited
923,340 cells but emitted only 24,951 more indices than an unsplit fan
estimate. The results suggest measuring how much work is spent visiting
cells that emit no triangles. See build/acm/native-mirror-inventory
and build/acm/native-mirror-trial.


### Clamped texture cells and thermal telemetry

The fight inventory counted 923,340 cell visits for 325,062 clamped
texture bands. Of these visits, 22,515 had fewer than three points after
Y clipping and 570,845 reached the fan but emitted no triangles. The
remaining 329,980 emitted indices. The game and geometry hashes and
final viewport matched the selected renderer. The inventory is in
build/acm/clamped-cell-empty-inventory.

Deferring the texture transform until the first valid triangle, and
sharing the two transforms for non-mirrored unit cells, preserved the
expanded packet stream in the x86 and ARM fixtures. Both short trials
matched the hardware viewport. In the 2,400-frame lazy-only retest,
packet prep fell to 12.058 ms from 12.206 and 12.161 ms, while frame p95
rose to 21.738 ms against 21.078 and 21.350 ms. Throughput was 50.64 fps
versus 50.85 and 50.75 fps. Both state hashes and the final viewport
matched. See build/acm/lazy-cell-transform-trial.

The three 2,400-frame runs recorded 81 thermal samples. Temperature
ranged from 53 to 60 C against the first trip point of 75 C. Every
sample reported cooling state zero, four online cores, and CPU clock
1.2 GHz at the configured 1.2 GHz ceiling. The power supply endpoints
returned zero for current and voltage, so they could not quantify draw.


An exact repeated-vertex check before the double-precision area
calculation passed the x86 and ARM packet fixtures and matched both
Mini state hashes and the viewport. In the 600-frame test, packet prep
fell to 12.004 ms from 12.169 and 12.083 ms, but throughput was 49.12 fps
versus 50.02 and 49.82 fps, and p95 was 21.851 ms versus 21.417 and
21.600 ms. The selected renderer keeps the original fan path. See
build/acm/duplicate-cell-vertex-trial.


### Texture-cell clip endpoints and outer planes

The endpoint trial removed a generated intersection when it was
byte-identical to the adjacent endpoint. The outer-plane trial used
bounds with a floating-point error margin to skip edge planes already
outside the polygon. Both matched all 600 per-frame hashes of expanded
texture triangles and the packet fixtures on x86 and ARM. The timing
series ran without triangle hashing and matched the final viewport and
both state hashes.

Removing duplicate endpoints took 12.023 ms in packet preparation
against controls of 11.946 and 12.011 ms. The outer-plane shortcut
took 12.125 ms against 12.085 and 12.048 ms. Its p95 was 21.563 ms
against 21.386 and 21.770 ms, and all three p99 times were within
0.06 ms. Both trials remain outside the selected preset. See
build/acm/duplicate-clip-endpoint-trial and
build/acm/bounded-cell-planes-trial.


### Rendering stages with and without concurrent simulation

The full pipeline ran three 2,400-frame paced fights from the same
checkpoint. Next-frame simulation started before camera projection,
after camera projection, or after packet preparation. Every run drew
every frame and matched both game-state hashes and the final viewport
of the selected build. Thread timestamps confirmed the intended overlap.

Across 2,279 paired ticks, launching simulation immediately before
packets increased packet preparation from 9.011 to 11.281 ms. The
clipping phase, including its parallel palette task, accounted for
1.760 ms of the 2.270 ms increase. Its summed task CPU time rose
from 11.725 to 15.606 ms. Batch planning added 0.388 ms and packing
added 0.094 ms of elapsed time. The geometry worker overlapped
clipping by less than 0.01 ms on average in these runs.

Both delayed launches reduced full-game throughput. The early launch
reached 50.24 fps, against 47.68 after camera and 35.84 after packets.
The current preset keeps early launch. Raw traces and the per-frame
comparison are in build/acm/renderer-overlap-trace.


A separate trial moved the flat-geometry vectors out of the hot direct
batch records. The ARM records shrank to 144 bytes. Direct and reference
packing fixtures matched on Mesa, and the Mini matched the state hashes
and final viewport. Packet preparation fell to 11.872 ms against
12.087 and 12.071 ms. The trial reached 49.72 fps and 21.762 ms p95.
The controls reached 48.75 and 49.82 fps at 21.728 and 21.475 ms p95.
The selected build keeps the original record. See
build/acm/compact-batch-work-trial.


### Sparse storage for indexed mip packets

The indexed renderer keeps reusable packet slots for depth ranges that
can intersect the polygon. The original mip band number still selects
the LOD and material, and the non-indexed renderer retains its existing
path. In 600 fight frames this cut the mean number of live and spare
slot objects from 9,638 to 2,780. All 600 expanded triangle hashes
matched. Native x86 and ARM/QEMU also matched 16,000 packet cases each
with reusable storage and exact LOD boundary inputs. Direct packing and
the original packet golden fixtures passed.

In the 2,400-frame same-executable A/B, packet preparation fell to
10.502 ms from 11.941 and 11.940 ms. The sparse path reached 52.11 fps
and 20.736 ms p95. Controls reached 51.16 and 51.12 fps at 21.002 and
21.128 ms p95. The extra visible simulation wait was 0.83 ms across
the paired steady-state ticks, and the simulation task itself still
averaged 18.15 ms. Both state hashes and final viewport matched.


Sparse mip storage is now the default for indexed packets. The rebuilt
PGO build reached 52.02 fps and 20.665 ms p95 with 10.476 ms packet
preparation. It missed 2,274 of 2,279 sampled pacing deadlines. The
regular ARM build matched the PGO build's game and geometry hashes and
viewport on a 30-frame smoke test. The 2,400-frame cold-start run
matched the scripted attract, select and fight sequence's previous
game and geometry hashes and final viewport. Artifacts are in
build/acm/sparse-mip-packets-trial.


### Simulation samples after sparse mip storage

Two 2,400-tick checkpoint runs used the same production renderer code
with a frame-tagged CPU sampler. One drew every frame and the other
drew one of every eight frames for diagnosis. State hashes and matrix
counters matched. The every-frame capture matched production exactly.
The reduced-draw capture matched 99.9684% of viewport pixels.

On the same 1,386 ticks, selected after three consecutive skipped
draws in the diagnostic, simulation CPU time was 13.300 ms with
reduced drawing and 18.616 ms with full drawing. Mean elapsed time
increased by 5.320 ms. Sampling attributes the CPU difference to
i960 compiled game code (2.05 ms), the TGP core and compiled microcode
(1.83 ms), and board, buses and scheduling (0.93 ms), with the rest
in other code.

Within the full-draw run, the slowest tenth by simulation CPU time
used 20.465 ms versus 18.406 ms in the other ticks. The sampled
increase was about 0.96 ms for i960 and 0.95 ms for TGP, with the
board/scheduler category flat within sampling variation. Breakdown
and raw profiles are in build/acm/sparse-simulation-profile.


Mapped i960 samples attributed about 0.57 ms of the slow-frame excess
to guest-word loads and stores. This includes the host prologue,
bounds, timing, and fallback code emitted around each instruction.
Raw addresses and opcode estimates are in
build/acm/sparse-simulation-profile/guest-ops.json.

The native rotation code was retested with the new renderer and the
core PGO build. ARM/QEMU matched 36,000 rotation cases, including
timed reads, complete DSP state and short execution slices. The
2,400-frame trial executed 292,805 native rotations. Mean simulation
time was 17.97 ms against 18.07 and 18.33 ms. Throughput was 52.52 fps
at 20.598 ms p95, versus 52.36 and 51.42 fps at 20.846 and 20.853 ms
p95. State and viewport matched both controls. The change is archived
for production validation. See build/acm/sparse-dsp-rotation-recheck.


### Projective texture bounds and batch-planning profile

`--projective-packet-bounds` compares the projective texture ratios
by multiplying their binary32 numerators and denominators in binary64.
After finding the extrema it divides only the four endpoints. The
existing per-vertex divisions handle exceptional inputs. Both x86 and
ARM fixtures matched 16,000 packet cases, and the Mini matched 300
consecutive expanded triangle hashes. The game and geometry hashes and
final viewport matched all four 1,200-frame timing runs.

The projective path reached 51.91 and 52.27 fps. The two same-binary
controls reached 52.31 and 51.24 fps. Across matched 60-frame windows,
the median clipping/palette stage was 0.08 ms slower with the option
enabled. The selected renderer keeps the existing texture-bound math.

`--profile-packet-phases` records elapsed time for RAM handling,
clipping and palettes, followed by material and batch planning, then
final buffer packing. `--profile-render-windows` saves main-thread
CPU samples in 60-frame windows. In the 2,400-frame fight, the nine
windows with the highest mean planning time averaged 4.91 ms in planning
against 2.43 ms in the nine lowest. These are different scene windows.
The fastest-growing sampled locations were the serial per-polygon
validation and active packet checks in `prepare_batch_direct`. Texture
key lookups also grew. Game and geometry hashes and the final viewport
matched the selected build. The samples and timing are under
`build/acm/projective-packets-trial`.


### Ordered worker draw lists

`--prepared-draws` lets packet workers emit an ordered list of drawable
indexed packets, including flat geometry. Draw records carry the material
fields and polygon order needed by the submitting thread. Atlas lookups
and insertion, palette row selection, ordering and buffer offsets remain
on that thread. Workers own separate polygon ranges and results are
consumed in source order. Mesh diagnostics, owner rendering and
non-indexed fallback retain their existing draw construction.

The Mesa staged fixtures passed in highp and forced mediump, including
flat-to-textured transitions, exact direct-pack comparison, texture
revisions, a mismatched staged submit and indexed capacity fallback.
The Mini also matched 300 expanded packet hashes and 300 hashes of the
final vertex, index and draw arrays. Both state hashes matched.

Across the four preliminary 1,200-frame runs, the median 60-frame window
saved 1.38 ms in material and batch planning while packet clipping and
palette preparation took 0.24 ms longer. Mean visible simulation wait
increased, so the full swap interval saving was 0.18 ms in the median
paired window. The hardware archives are under
`build/acm/prepared-draws-trial`.

The final 2,400-frame comparison measured 51.81 fps and 20.784 ms p95
with worker draw lists, 51.84 fps and 20.792 ms for the selected build,
and 51.14 fps and 21.228 ms with the feature disabled in the experimental
executable. Preparation took 9.29 ms with worker draw lists versus
10.39 ms in the selected build. Visible simulation wait rose from
2.45 to 3.61 ms. Every run matched core, geometry and final viewport.
The final source also matched all 300 indexed draw-stream hashes.

After subtracting the recorded simulation wait, the number of swap
intervals over 17.384 ms fell from 1,088 to 158 out of 2,278 in this
pair. With the experimental path the simulation task itself exceeded
the budget on 1,719 intervals. These measurements describe the original
overlap schedule of each run. The worker list remains opt-in and provides
a renderer for joint simulation and presentation tests.


### Retained Mini fast paths

The Mini `--fast` preset selects worker-prepared indexed draws and
full-slice native TGP rotations. Use `--no-prepared-draws` and
`--no-native-tgp-rotation` independently for comparisons. Resumable
rotations and native affine translation remain opt-in, using
`--resumable-tgp-rotation` and `--native-tgp-translation`.
The faster sync timer experiment is available on
`acm-combined-fast-paths`. The projective UV experiment and its packet
fixture remain on `acm-projective-packets`, and the packet timing,
draw-stream checks and render-window profiler remain available here.

The retained variant keeps the board profile and trains the changed
TGP core on the Mini. The training record, matching symbols and
hardware comparisons are in `build/acm/retained-fast-paths-trial`.

Over 2,400 paced fight frames the retained fast preset reached 53.08
fps and 20.231 ms p95. The previous selected build reached 52.59 fps
and 20.552 ms p95. Turning both paths off in the retained executable
gave 51.90 fps and 20.864 ms p95. Preparation fell to 9.53 ms from
11.18 ms, and mean simulation task time fell to 17.77 ms from
17.91 ms. Core, geometry and the final viewport matched in all runs.
A separate 2,400-frame cold boot through the scripted fight matched the
prior cold start's game state, geometry and final viewport.


### Native affine translation in the Mini fast preset

`--fast` also enables native TGP affine translation when native matrix
execution is enabled. Use `--no-native-tgp-translation` to disable it
independently. Resumable rotation stays opt-in. With worker draw lists
and full-slice native rotation already active, enabling affine alone
cut the 1,200-frame simulation task mean to 17.54 ms from 18.14 ms in
both controls. Resumable rotation alone measured 18.30 ms.

In the 2,400-frame same-binary replay, affine averaged 53.66 fps and
20.235 ms p95, compared with 52.46 and 52.13 fps and 20.393 and
20.448 ms p95 in the bracketed controls. It saved a median 0.44 ms of
simulation time across 60-frame windows, with a saving in 36 of 37
windows. Core and geometry hashes and the final viewport matched.
A cold start into the scripted fight also matched game state, geometry,
and the final viewport. See `build/acm/affine-retained-trial`.


### Fast FIFO synchronization with the retained renderer

The Mini `--fast` preset enables fast zero-delay FIFO and input-line
synchronization. Use `--no-fast-sync-timers` to disable it independently.
The fast path preserves the scheduled event times, sequence ordering,
and processor abort behavior. Earlier tests matched all 41,908 timed
FIFO records, and the updated build keeps the existing native affine,
rotation and worker draw-list settings.

With fresh profile feedback for the changed board core, the same
executable saved a median 0.424 ms of simulation time in all 17 paired
60-frame windows. It reached 52.21 fps and 20.471 ms p95, compared with
50.93 and 51.22 fps and 21.159 and 20.914 ms p95 in the controls.
Both state hashes and the final viewport matched.

The separate 2,400-frame comparison reached 52.86 fps and 20.289 ms
p95 with the fast sync build, versus 53.04 fps and 20.156 ms p95 with
the prior selection. This run did not establish an end-to-end gain.
The fast sync path is retained for the repeatable simulation saving
within its matched build. See `build/acm/retained-sync-trial`.


### Worker balance and TGP code size in the retained preset

The 600-frame worker sweep used one executable and kept the retained
draw, affine, rotation and FIFO paths active. Both state hashes and the
final viewport matched in all runs. The controls reached 52.62 and
52.88 fps. Pinning simulation to core 3 reached 52.60 fps. Three render
threads reached 52.45 fps, and shared final packing reached 52.34 fps.
A single render thread saved 1.00 ms of simulation time in the median
matched window, while total frame intervals grew by 5.56 ms. It reached
42.38 fps. The retained worker settings stay selected. See
`build/acm/retained-affinity`.

Rebuilding only the compiled TGP unit with `-Os` and fresh profile
feedback shrank its text by 7,910 bytes, about one percent. All other
objects were byte-identical. Over 1,200 frames the experiment reached
51.82 fps and 20.834 ms p95, versus 52.91 and 53.20 fps and 20.393 and
20.240 ms p95 in the controls. Simulation took a median 0.373 ms
longer across matched windows. Both state hashes and the final viewport
matched. The selected build uses the original trained optimization
settings. See `build/acm/tgp-os-trained`.

A separate 2,400-frame simulation-thread profile matched the selected
replay's state and viewport. In the nine heaviest 60-frame windows,
simulation tasks averaged 18.58 ms. Sampling attributed 5.63 ms to
compiled i960 code, 5.50 ms to the TGP core, and 4.95 ms to the board,
buses and scheduler per frame. The scheduler loop accounted for 234 of
1,981 samples, and the TGP dispatch function accounted for 171,
including 113 attributed to its opcode loop. See
`build/acm/retained-simulation-profile` for functions, guest sites and
source lines.


### Keep benchmark logs off the Mini's RAM filesystem

The benchmark tool archives the exact log and exit-status bytes for
finished runs. It also archives those files for the previous interactive
run after confirming its writer has exited. It CRC-checks every copied
file before deleting its temporary device copy. Interactive runs without
profiling report every 600 frames to reduce log growth.

During the mask-scan experiment, kernel logs confirmed that the Mini ran
out of usable memory and killed the game. Archiving and clearing
10,876,960 bytes of old profiling and diagnostic files let the three
1,200-frame runs complete. A separate 30-frame smoke test verified that
the completed and retired logs were archived, their temporary copies
were removed, and the normal interactive build could start. See
`build/acm/benchmark-log-retention` and the device archive under
`build/acm/tgp-mask-scan-trial`.


### Private regions inside DSP commands 0x4a and 0x6b

The AOT generator recognizes seven straight-line regions that access
only internal RAM and the arithmetic registers. Each region validates
its opcodes at compile time and uses a single cycle-budget check. A
partial slice, repeat, stalled instruction, debugger hook, or
unrecognized program uses the individual handlers. Region execution
leaves externally visible FIFO and bus operations on their original
cycle boundaries. Use `--no-tgp-private-regions` to compare the original
instruction path.

The ARM fixture ran 1,680 exact per-slice CPU, RAM and bus comparisons,
including debugger and repeat alternatives. The same comparisons
passed with virtual RAM and no region execution. The original fixture
retained its boot, mutation, revision, and short-slice signatures.

In the 1,200-frame same-executable Mini comparison, 79,335 regions
covered 1,321,334 guest instructions. Mean simulation task time was
18.36 ms versus 18.50 ms in both controls. It reached 51.28 fps and
20.904 ms p95, versus 50.45 and 50.69 fps and 21.137 and 21.034 ms p95.
Both state hashes, guest instruction totals, and the final viewport
matched. Astra reviewed the seven ranges and recommended enabling
them on the trial branch. The TGP object in that first trial had no
profile feedback.

In a second comparison, only the TGP object was rebuilt with fresh
profile feedback from two matched 900-frame runs on the Mini. The
trained path reached 52.59 fps and 20.475 ms p95. The previous selected
build reached 52.19 and 52.16 fps with 20.592 and 20.787 ms p95. The
median simulation saving was 0.103 ms across 17 matched 60-frame windows,
with savings in all 17. Both state hashes and the final viewport
matched. A separate 2,400-frame cold boot through attract, selection
and fight also matched the previous build's state and viewport.
See `build/acm/tgp-private-regions-trial`.
The untrained ARM fallback also matched the trained build's game state,
geometry, and final viewport in a 30-frame Mini smoke test. The default
Mini build uses the trained private regions.


### Direct native i960 branch edges

For supported direct jumps, condition branches, compare-and-branch
instructions and bit tests, the emitter uses native control flow
without storing and reloading guest PCs on an internal edge. Both
outcomes charge the original cycle budget and materialize PIP and IP
on a slice exit or before leaving compiled code. The architectural
condition bits are preserved. Specialized spin and countdown polls
retain their existing paths. The Mini fast preset uses these edges.
Use `--no-i960-direct-branches` to compare the original emission.

The ARM interpreter comparison passed 5,155 extra branch fixtures in
each of the register-cycle, lazy-PC and combined configurations.
The fixtures check both operands, condition bits, slice boundaries,
helper-visible PCs and a target outside compiled code. All 150,764
four-slice fixtures and the 22,680 three-slice timer cases passed in
each configuration.

At 1,200 paced fight frames, the same-executable trial reached 53.86
fps and 20.060 ms p95. The controls reached 52.28 and 52.29 fps and
20.472 and 20.611 ms p95. Mean simulation time was 17.40 ms instead
of 17.93 or 17.91 ms. Both state hashes and the final viewport matched.
The emitted i960 code grew by 17,158 bytes. Astra independently
reviewed the exit behavior and supported the fast-preset default.
See `build/acm/i960-direct-branches-trial`.

The 2,400-frame comparison against the previous selected build reached
54.22 fps and 19.915 ms p95, against 53.30 and 53.31 fps at 20.104 and
20.123 ms p95 in the two controls. The selected variant averaged
17.36 ms per simulation task. State, geometry, and the final viewport
matched in all runs. A cold boot through selection into the scripted
fight and an untrained fallback smoke run also matched.


### Packed S24 tile-map uploads

The Mini fast preset stages the decoded S24 maps as RGBA4444. RGB holds
the 12-bit palette pen and A holds transparency or tile priority. The
shader recovers the two palette coordinates separately, preserving
precision on the Mali. Palette, window mask and vertex formats stay as
before. Tile rectangles are aligned to eight texels. Use
`--no-packed-tile-map` to select RGBA8888.

Both desktop Mesa and the Mini passed all 32 scroll, window, priority
and update cases, including an upload after the original board data
was perturbed. The 2,400-frame same-executable test matched game state,
geometry and the final viewport. With packed maps, stage time averaged
0.208 ms versus 0.319 and 0.244 ms in the controls. The candidate
reached 54.11 fps at 19.703 ms p95, versus 53.66 and 53.82 fps at 20.069
and 19.956 ms p95. The longest measured interval was 29.819 ms, versus
107.607 and 103.671 ms. Intervals over one tick increased from 1,807?1,808 to
1,863 out of 2,278. Astra reviewed the shader precision and unpack
alignment and recommended the half-size format. See
`build/acm/packed-tile-map-trial`.

The separate 2,400-frame comparison with the previously selected
build reached 54.38 fps versus 54.03 and 54.02 fps. P95 was 19.767 ms
versus 19.601 and 19.876 ms. The maximum interval was again below
30 ms, versus 107.779 and 104.897 ms. Mean tile upload fell to
0.207 ms from 0.316 and 0.236 ms. The packed run had 1,807 of 2,278 intervals
over one tick, versus 1,797 and 1,792 in the controls. It missed
1,981 of 2,279 scheduled pacing deadlines. State, geometry,
and viewport matched through fight replay, cold boot and the
fallback build. The Mini fast preset selects packed maps.


### Fixed-layout one-channel opaque atlas trial

`--fixed-atlas-luma` keeps the original logical shelf placement and gives
opaque tiles GL_LUMINANCE backing. Transparent tiles retain
GL_LUMINANCE_ALPHA. Mixed pages have two backing images, both charged
against the 12 MiB atlas budget. Mesh draw keys reserve bit 15 for the
backing selector. The initial encoding used bit 31, which was discarded
by the mesh key and caused incorrect texture bindings. Its timings are
excluded.

Desktop Mesa and the Mini passed the staged and raster fixtures. The
corrected same-binary 2,400-frame replay matched both state hashes, the
logical layout dump and every pixel in the final viewport. Atlas upload
payload fell from 5,346,792 to 3,720,956 bytes (30.4%). The final frame
used 41 atlas batch draws versus 30 in both controls. Mean frame rate was
54.76 fps versus 54.52 and 54.37. The 95th-percentile interval was 19.611 ms
versus 19.559 and 19.841 ms. See
`build/acm/fixed-atlas-luma-trial/packed-id`.

The allocator now claims backing storage before reserving shelf space.
The Mini fixture fills the allocation budget and checks that a failed
claim leaves the shelf available. The final 2,400-frame run measured
54.94 fps and 19.476 ms p95, versus 54.37 and 54.50 fps at 19.816 and
19.686 ms p95. The upload saving and exact final viewport held. Game
state, geometry and the final viewport also matched in the 2,400-frame
cold start and the 30-frame untrained fallback check. Astra reviewed
the layout and allocation behavior and recommended using it in the
fast preset. Use `--no-fixed-atlas-luma` to restore two-channel storage
for opaque tiles. See `build/acm/fixed-atlas-luma-trial/claim-first`.


### Simulation copy profile

A 2,400-frame Mini fight replay collected 8,279 samples from 41.90
simulation CPU seconds with no dropped samples. The display-list
snapshot in `gpu3d_geometry_job::capture` accounted for 138 of 142
sampled memcpy returns. The whole 128 KiB snapshot took 0.294 ms per
list on average. The i960 can write the display buffer through mapped
pages, and lists can jump within all 128 KiB. A partial snapshot needs
to account for both when preserving the geometry worker's ownership.

In the nine slowest 60-frame windows, the simulation took 18.33 ms per
frame. Board, bus and scheduler code used approximately 5.14 ms of
sampled CPU time, versus 4.39 ms in the middle windows. TGP execution
rose from 5.49 to 5.86 ms. The pinned-thread profile, matching replay
hashes and resolved call sites are in
`build/acm/simulation-copy-profile`.


### Native TGP command dispatch

`--tgp-native-dispatch` groups the integer operations after the DSP
reads a command from the input FIFO. The generator verifies the
nine-word sequence and all 112 branch table entries. The DSP checks
the complete program image at a revision change. Each invocation
uses an exact 5-, 9-, or 10-cycle prefix, including the return
address for the exceptional subroutine. The original FIFO read
and its timing stay intact. Command sampling retains the table
entry, and PC sampling and debugger hooks use individual execution.

The trained and untrained ARM fixtures each pass 22,528 exact
slice comparisons per mode. They cover command values in and out
of the table, both branch outcomes, short slices, repeat, FIFO
stall, callback timestamps, debugger, command tags, and forced
opcode guards.

In the same untrained executable, the dispatcher reached 54.92 fps
at 19.583 ms p95. Controls reached 53.00 and 53.16 fps at 20.511
and 20.357 ms p95. Median simulation savings were 0.683 ms across
37 matched 60-frame windows, with savings in all 37. The dispatcher
grouped 22,096,963 additional instructions. Both state hashes, total
guest instructions, atlas layout and the final viewport matched.
See `build/acm/tgp-command-dispatch-trial/paced-2400`.

The profile-guided build reached 55.31 fps and 19.406 ms p95 against
54.65 and 55.06 fps at 19.519 and 19.603 ms p95 for the previously
selected executable. Both state hashes and the final viewport
matched. The 2,400-frame cold boot and the fallback smoke test
also matched state, geometry and the final viewport. The trained
DSP text grew by 1,356 bytes. Simulation
saved a median 0.191 ms across 37 matched 60-frame windows, with
savings in 33. Both Astra's review and the trained measurements
support the fast-preset default. Use `--no-tgp-native-dispatch` to
restore the individual dispatch sequence. See
`build/acm/tgp-command-dispatch-trial/trained-paced-2400`.

The 17.384 ms frame budget still needs headroom. In the steady
fight sample, 722 of 2,278 simulation tasks ran over one tick.
Of 1,688 swap intervals over one tick, 762 still exceeded the
budget after subtracting the wait for simulation measured in
that loop iteration. This residual includes render work and
pacing. See `residual-budget.json` under the trained replay.


### Packed 3D palette row lookup

The Mini fast preset stores the full material key and one-based row
together in a 32 KiB lookup table. Its 4,096 slots support the existing
2,048-row atlas limit. Rows are assigned in source order and reset when
the palette atlas is invalidated. Use `--no-packed-palette-index` for
the node-based lookup.

All 2,048 rows passed the ARM lookup, collision and reset checks.
Mesa and Mali passed the staged and raster fixtures. Both state hashes,
all 300 GPU draw streams, and the final viewport matched. Over the
2,400-frame same-executable replay, clipping and palette preparation
averaged 4.56 ms versus 4.83 and 4.65 ms. The packed build reached 55.18
fps at 19.377 ms p95. Controls reached 54.46 and 55.25 fps at 19.854 and
19.367 ms p95. The fast preset selects the contiguous lookup for the
local preparation saving. Astra independently reviewed the layout,
capacity tests and timing. See `build/acm/packed-palette-index-trial`.


### Reusing worker packing jobs

Packet workers publish the records that the final packing stage consumes.
They borrow each packet's vertex and index buffers and prepare its material
fields. The render thread assigns atlas transforms, the palette row and
disjoint output offsets in source order. Packing workers write directly
to the existing 52-byte GPU vertex layout and the shared index buffer.
The same packer accepts the direct jobs used by the mesh and ownership
paths. Spare-capacity trimming refreshes the borrowed buffers before use.

The untrained build reduced render preparation to 8.58 ms from about
9.6 ms. It reached 54.91 fps versus 55.31 and 55.17 fps for the controls.
Its 300 submitted draw streams matched. The Mali fixtures and the Mesa
forced-trim test also passed.

After training the changed GLES2 unit, the build reached 55.42 fps and
19.282 ms p95, against 55.30 and 54.65 fps at 19.257 and 19.594 ms p95.
Render preparation averaged 8.64 ms. Steady-state atlas and batch
planning averaged 1.62 ms versus 3.02 and 2.90 ms. These measurements
cover the record reuse and the new renderer profile together. The trained
build also matched all 300 draw streams and the full cold-start replay.
The Mini fast preset uses the trained renderer with worker-prepared draws. The
core build applies the renderer profile and `--no-prepared-draws`
restores render-thread job construction. See
`build/acm/worker-packing-jobs-trial`.


### Contiguous TGP internal read window

`--tgp-read-window` backs the two internal RAM banks with one 4 KiB
allocation. Unmapped reads use the zero-filled hole between the banks,
and writes retain the original bank bounds. The state format still
stores the original 768 words in order. Buses without the contiguous
read capability retain the two-bank and virtual bus paths.

The ARM fixture checked every 16-bit address and wrapped aliases with
matching bank contents and a zero-filled hole. It also retained all
22,528 existing dispatch comparisons. The 2,400-frame Mini replay
matched both state hashes, instruction counts, atlas layout and the
final viewport. In the same untrained executable, it achieved 50.98
fps and 21.135 ms p95 against 50.57 and 50.46 fps at 21.394 and 21.448
ms p95. Simulation averaged 18.48 ms versus 18.69 and 18.73 ms.
The median saving was 0.193 ms across 37 aligned windows, with
savings in 36. Both controls used the same backing allocation. Astra reviewed the hole,
native kernel ranges and state format. See
`build/acm/tgp-read-window-trial/paced-2400`.

With fresh profile feedback for both the board and TGP objects, the
read window reached 54.67 fps at 19.712 ms p95. The previous selected
binary reached 54.49 and 54.61 fps at 19.803 and 19.584 ms p95 in the
two controls. The trained replay matched game state, geometry, atlas
layout and the final viewport. The Mini fast preset uses the contiguous
read path for the measured simulation saving in the same-executable
comparison. Use `--no-tgp-read-window` to restore separate bank reads.
See `build/acm/tgp-read-window-trial/trained-paced-2400`.
### Native TGP dispatch across scheduler slices

The Mini fast preset resumes the native command dispatch when a
scheduler slice ends between DSP addresses `0x3b` and `0x3e`. The
resumed sequence uses the remaining cycle budget and preserves the
reference path for short slices, repeats, stalls, and debugger hooks.
Use `--no-tgp-resume-dispatch` to disable it.

The ARM suffix fixture passed 20,480 exact slice comparisons with
normal, tagged, and guarded execution. In the 2,400-frame fight replay,
the trained build reached 54.78 fps at 19.536 ms p95, against 54.32 and
54.62 fps at 19.827 and 19.677 ms p95. The resumed path grouped
1,202,821 additional guest instructions. Game state, geometry, and
the final viewport matched. See
`build/acm/tgp-dispatch-resume-trial/trained-paced-2400`.


### Direct i960 register frames in work RAM

The Mini fast preset copies a complete aligned 64-byte i960 register
frame in work RAM when a call spills it, a return restores it, or
`flushreg` writes it. Unaligned addresses, devices, and other memory
continue to use ordered dword bus accesses. Use
`--no-i960-direct-register-frame` to select those accesses for all
frames. `tools/acm/check-i960-register-frame-arm.sh` checks cycles,
registers, RAM, and the order of fallback accesses.

In the same-executable fight benchmark, the simulation saving was
0.078 ms in the median of 37 matched 60-frame windows. The trained
2,400-frame replay reached 54.67 fps and 19.607 ms p95, against
54.26 and 54.50 fps at 19.739 and 19.709 ms p95 for the prior build.
Mean simulation time was 17.17 ms, against 17.31 and 17.28 ms.
Core state, geometry, and all final viewport pixels matched. Astra
reviewed the address guards and fallback behavior. See
`build/acm/i960-register-frame-trial`.


### Single HALT timer delivery

The Mini fast preset delivers single i960 and TGP HALT events at their
scheduled position and uses a table for active-first CPU ordering when
suspension changes. Batched events and reset use the ordinary callback
path. Use `--no-fast-halt-delivery` to measure the original path.

ARM builds with and without profile feedback matched 41,908 timed FIFO
records and their final core state. The trained build reached 54.92 fps
at 19.461 ms p95 over the 2,400-frame fight replay. Controls reached
54.75 and 54.41 fps at 19.802 and 19.648 ms p95. Mean simulation time
was 17.06 ms, against 17.16 and 17.27 ms. Both state hashes and all
final viewport pixels matched. See `build/acm/fifo-halt-delivery-trial`.


### Fixed TGP program mapping

The Mini fast preset passes the board's fixed 4,096-word program image
and revision directly to the TGP. Program changes still trigger complete
image validation. Debugger hooks, instruction fetch guards, and interpreter
mode keep their existing execution paths. TGP launch options are sampled
together on the first slice. Use `--no-tgp-stable-program` to restore the
virtual program lookup for comparison.

The trained board and TGP build reached 55.41 fps at 19.296 ms p95 in
the 2,400-frame fight replay. Controls reached 54.41 and 54.48 fps at
19.831 and 19.777 ms p95. Mean simulation time fell to 16.84 ms from
17.28 and 17.14 ms. Both state hashes, guest instruction counts, atlas
layout, and the final viewport matched. ARM lifecycle checks matched
through upload mutation, restore, debugger, and short slices, including
forced guarded and interpreted runs. See `build/acm/tgp-stable-program-trial`.


### Arena benchmarks from versus checkpoints

The ARM checkpoint tool accepts an optional arcade versus-stage index
from 1 through 10. It applies the desktop's stage selector before the
game loads the arena. It verifies the requested versus fight before
writing the checkpoint. Omit the index to use the game's selection.

```sh
qemu-arm -cpu cortex-a7 build/acm/bin/vf2checkpoint-tgp create roms/vf2.zip \
    build/acm/versus-arena.checkpoint 2100 tools/scripts/start_versus.txt 10
```

Pass the checkpoint to `benchmark-device.py --state`. Keep the same
fighter input script when comparing arenas. The default arena and
indices 2, 5, and 10 reloaded with matching core and pixel hashes in
fresh ARM processes. See `build/acm/arena-checkpoints`.


### Mip scanning on the Mini

The fast preset computes the mip limit from the smaller integer texture
dimension. It stops scanning once the band's upper depth bound is below
the polygon's minimum depth. Equality still qualifies in both adjacent
bands. Use `--no-limit-mip-scan` for the original scan.

The ARM fixture matched all 65,535 positive 16-bit texture dimensions
and 1,000 packet streams including boundary and reused-slot cases. All
300 submitted waterfront draw streams matched. In the trained 900-frame
standard fight, throughput rose to 54.72 fps from 54.37 and 54.18 fps.
P95 was 20.072 ms versus 20.144 and 20.208 ms. On the waterfront it ran
at 52.87 fps, between 52.75 and 52.99 fps. P95 rose to 19.010 ms from
18.888 and 18.951 ms, while p99 improved to 19.694 ms from 20.119 and
20.130 ms. The state hashes and final viewport matched across both replay
series. See `build/acm/mip-scan-limit-trial`.


### Profiled TGP spans

The trial at `05b938f` groups ten hot straight-line regions of the verified
TGP image. ARM fixtures matched 3,600 comparisons per mode, including short
slices, repeat, debugger hooks and exceptional floating-point operands.
State, geometry and the final viewport matched on the Mini.

The untrained Dural replay eliminated 5.88 million individual dispatches
over 1,200 frames. Its median simulation saving was 0.117 ms across 17
matched windows. With PGO, simulation was slower than both controls in 13
of those windows. The trained 900-frame standard fight reached 54.79 fps
at 20.211 ms p95, against 54.74 and 54.94 fps at 19.881 and 20.030 ms p95.
The trial remains on branch `acm-profiled-tgp-spans`. See
`build/acm/profiled-tgp-spans-trial`.


### Full-rate rendering and simulation on Dural's arena

The selected ARM binary ran the same 1,200-tick checkpoint with full
rendering and with one draw per eight ticks. Game and geometry hashes
and the final viewport matched. Simulation used 17.66–17.67 ms of thread
CPU time per tick with full rendering and 13.36 ms with sparse drawing.
Across 656 matched ticks preceded by three skipped draws in the sparse
run, simulation took 12.98 ms elapsed with sparse drawing and
17.79–17.84 ms with full rendering. In the steady-window samples,
compiled i960 code accounted for the largest increase, about 1.68–1.75
ms of the 4.34–4.36 ms increase in sampled CPU time per tick.

The flat-fragment probe kept full draw cadence, packet counts and
texture uploads while removing 3D sampling and discard from the shader.
It intentionally changes the picture. Median sampled Mali utilization
fell from 71% to 51%. Simulation used 16.87 ms of thread CPU time per
tick and the game reached 51.93 fps at 19.573 ms p95. Full-render
controls reached 51.09–51.32 fps at 19.951–20.137 ms p95. All state and
geometry hashes matched. The probe also changes fragment coverage and
compiler-generated shader code. These are diagnostic runs. The
interactive preset uses full-rate textured rendering. Logs and
sampling results are under `build/acm/dural-overlap`.


### Indexed fans inside the first texture cell

Dural's 600-frame profile averaged 5.665 ms for clipping and palettes,
1.147 ms for atlas and batch planning, and 1.522 ms for packing. See
`build/acm/arena-render-profile/stage-10`.

The Mini fast preset can emit the indexed fan directly when every
vertex is within the viewport, mip band and first texture cell.
It keeps the shared triangle emission order and uses the clipper
for boundary crossings and the base-plus-microtexture band.
Use `--no-origin-packet-fan` for comparison.

ARM fixtures matched 12,000 cases, including seam and depth boundaries,
signed zero and degenerate triangles. The Mini matched all 300 expanded
packet hashes and final draw-stream hashes on Dural's arena. On the
trained 1,200-frame run, the median clipping/palette window saved
0.219 ms. Throughput rose to 51.88 fps at 19.649 ms p95 from 51.35 and
51.37 fps at 19.749 and 19.997 ms p95. In the standard 900-frame fight,
median clipping/palette savings were 0.171 ms and p95 improved to
19.695 ms from 19.891 and 20.084 ms. That replay reached 54.73 fps
versus 54.88 and 54.79 fps. Core, geometry, atlas layout and final
viewport matched. The 2,400-frame cold start and 30-frame fallback
check also matched state, geometry and the viewport. See
`build/acm/inline-origin-packets-trial`.


### Active versus replay on the Mini

Use `tools/scripts/benchmark_versus_fight.txt` with an arena checkpoint
to exercise both fighters with movement, attacks and guards.

```sh
python tools/acm/benchmark-device.py --fast --pgo-core \
    --frames 600 --paced --frame-trace --profile-packet-phases \
    --state build/acm/arena-checkpoints/stage-10.checkpoint \
    --input-script tools/scripts/benchmark_versus_fight.txt
```

With the origin-cell fan build selected, active stages 0, 2, 5 and 10
reached 51.58, 50.85, 50.44 and 47.78 fps. Dural reached 47.86 fps on
repeat, with exact state, geometry and viewport. Dural's no-input run
reached 51.53 fps. The two active runs spent 22.38 to 22.57 ms at p95
versus 19.49 ms idle. The active replay ran 47.52 million compiled DSP
instructions, compared with 45.22 million idle. See
`build/acm/active-arena-sweep`.

In the four-run overlap profile, full rendering added 4.99 ms of
simulation-thread CPU time per tick in the active fight and 4.56 ms in
the idle fight. With one draw per eight ticks, fighter activity added
0.49 ms of simulation CPU time. With every frame drawn, fighter
activity added 0.91 ms. State and geometry matched within each input
mode. See `build/acm/active-dural-overlap` for the profiles.


### Atlas transforms from texture-cell indices

The Mini fast preset reuses the integer cell indices to compute atlas
scale and bias for ordinary indexed texture packets split on both axes.
Microtextures and hardware repeat use the general transform. Use
`--no-cell-index-transforms` for comparison.

ARM fixtures matched 2,808 transform cases and 1,000 packet streams,
including negative mirrored cells, zero bias and the native-repeat
fixture. All 300 expanded packet and final draw-stream hashes matched
on the Mini. The trained active Dural replay saved a median 0.092 ms in
clipping and palette preparation, with p95 at 22.427 ms versus 22.567
and 22.481 ms. The run reached 47.97 fps versus 47.77 and 48.08 fps. In
the standard fight total packet preparation saved 0.13 to 0.17 ms on
average. P95 was 20.007 ms versus 19.982 and 20.022 ms, and throughput
was 54.60 fps versus 55.07 and 54.69 fps. State, geometry, atlas layout
and the viewport matched. A 2,400-frame cold start and the fallback
check matched too. See `build/acm/cell-index-transforms-trial`.


### Camera classification on ARMv7

The NEON trial tests all four camera planes across triangle or quad
vertices before entering the scalar clipper. It uses a bounded range of
finite coefficients and positions to keep the vector arithmetic and
scalar comparisons equivalent. The 132,000 ARM fixture cases and 300
Mini draw streams matched. Camera preparation averaged 2.123 ms versus
2.158 and 2.316 ms. The active Dural replay reached 47.66 fps versus 47.68
and 48.07 fps, with p99 at 24.276 ms versus 23.977 and 23.466 ms. Astra
reviewed the guard and results. The trial remains on
`acm-neon-camera-inside` at `e6b3706`.


### Active Dural packet construction diagnostic

The diagnostic redraws a saved 3D batch against the current 2D layers
while the game advances with both fighters active. Camera preparation,
GPU draw submission and all 600 presentations still run. The
diagnostic changes the 3D picture between refreshes. Game and geometry
state matched in every run.

Reusing the batch on 524 frames reduced mean packet preparation from
9.63 to 9.65 ms in the controls to 1.20 ms and simulation CPU time from
18.86 to 18.90 ms per tick to 16.49 ms. Throughput rose to 50.90 fps
from 46.89 and 47.11 fps. P95 rose to 27.65 ms. The 59 measured refresh
intervals averaged 27.29 ms, and settled replay intervals averaged
18.44 ms.

Holding the batch after warmup removed packet construction from all
478 measured intervals. It reached 51.99 fps and 19.57 ms p95, versus
46.98 and 47.03 fps at 23.06 and 22.96 ms p95 in the controls. Simulation
CPU time averaged 16.28 ms per tick versus 18.83 to 18.85 ms. The held
batch submitted about 7.6% fewer indices on average and sampled Mali
utilization was 71% versus 66%. Flat shading with the same held batch
reached 53.84 fps and 18.79 ms p95, with 52% sampled GPU utilization.
The 57.53 Hz frame interval is 17.384 ms. The diagnostic branch is
`acm-replay-render-batches` at `0e9a894`. Logs and interval breakdowns
are under `build/acm/replay-render-batches-trial`.


### Opaque atlas batch shaders

The Mini fast preset selects an opaque shader only when every polygon
in an existing batch is textured, opaque and free of checkerboard
masking. Batches that use one texture sample can omit the second
sample and its blend calculation. Batch boundaries, texture bindings
and order stay the same. Uniforms persist per linked shader program.
Use `--no-homogeneous-atlas-shaders` for comparison.

In the 450-frame active Dural diagnostic with serialized GPU passes,
the wait after composition fell from 11.023 and 11.024 ms to 9.809 ms.
That measures the queue drain after composition, which also includes
deferred 3D work. The specialized shaders handled 89.9% of submitted
indices in the 600-frame full-rate comparison. The cached uniform
version matched 300 expanded packet and final draw-stream hashes.
Mesa raster and staged tests passed at both shader precisions.

On the trained active Dural run, throughput was 47.86 fps versus
47.84 and 47.79. P95 was 22.738 ms versus 22.645 and 22.730 ms, and
p99 improved to 23.539 ms from 23.930 and 23.947 ms. Clipping and
palette preparation took about 0.21 ms longer. The trained standard
fight reached 55.34 fps at 19.562 ms p95 and 20.928 ms p99. Controls
ran at 55.21 and 54.70 fps, with 20.002 and 19.901 ms p95 and 21.615
and 23.330 ms p99. Both state hashes and the final viewport matched
in each replay. The 2,400-frame cold start and the 30-frame fallback
check matched too. See `build/acm/homogeneous-atlas-shaders-trial`.


### Retained mesh inventory during active versus play

The ARM mesh inventory accepts `--input-script path` after its existing
ROM, checkpoint, frame count and output arguments. It applies the input
on the same tick as the Mini's queued frontend. Use the staged versus
checkpoint, input script and atlas layout from the same hardware run.
Set `VF2_FIXED_ATLAS_LUMA=1` when the layout came from the Mini fast
preset so the model accounts for each opaque page backing separately.
The inventory also accounts for the standalone hardware-repeat pages.

On the 600-tick active replays, the model reproduced both game and geometry
hashes and all 20 sampled Mali draw counts per arena. Unchanged fans from
attributed ROM meshes accounted for 81.2% of textured vertices on Dural
and 67.3% on the waterfront. Of the eligible polygons, 89.3% and 86.7%
used the same mesh, material and mip variant on the preceding frame.

Both replays fit the eligible objects into two frame-local groups of
64 transforms. In the mixed-render draw model, grouping objects by the
atlas page pairs they share reduced projected draw counts from 58.1 to
48.7 on Dural and from 66.5 to 58.5 on the waterfront. The current renderer
averages 42.5 and 53.1 draws respectively, which also match the per-pair
capacity lower bounds. These projections assume
fallback polygons can use an existing compatible draw. They estimate
batching cost and eligibility before live buffer uploads, shader
projection, raster differences and frame-time measurements. See
`build/acm/active-mesh-inventory/summary.json`.


### Render worker claims and task ranges

The Mini fast preset lets workers claim each band with an atomic increment.
The existing mutex still publishes the job and waits for completion before
the caller releases it. When each range represents one task, its bounds
are the task index and the next index. Use `--no-atomic-row-claims` and
`--no-direct-task-ranges` for separate comparisons.

The ARM fixture matched 6,000 job generations and 95,935 hook calls in
each of the four configurations, with concurrent callers and nested
jobs. On the trained active Dural replay, the combined path measured
47.86 fps versus 47.18 and 47.38 fps, with p99 at 24.405 ms versus
24.999 and 25.063 ms. Packet preparation took 0.10 to 0.25 ms longer
per frame. The trained waterfront replay reached 50.16 fps versus
49.47 and 50.18 fps, with p95 at 21.530 ms versus 21.733 and 21.619 ms.
Both state hashes, the atlas layout and the final viewport matched on
both replays. All 300 checked draw streams, the 2,400-frame cold start
and the fallback check matched. See `build/acm/atomic-render-jobs-trial`.


### Direct packing of intact textured fans

The Mini fast preset packs eligible indexed fans from the current frame's
projected vertices and reuses the triangle index pattern. It saves the
intermediate packet and keeps the same atlas, palette, draw order and GPU
submission. Eligibility requires a nondegenerate fan inside the viewport,
one mip band and one texture cell. Other polygons use the regular clipper.
Use `--no-retained-fans` for comparison.

In the trained 600-frame active Dural replay, throughput reached 48.25 fps
versus 47.34 and 47.42 fps. P95 was 22.350 ms versus 22.778 and 22.771 ms.
On active Waterfront, throughput reached 50.41 fps versus 50.18 and 49.89
fps, with p95 at 21.292 ms versus 21.355 and 21.324 ms. Packet preparation
used slightly less render-thread CPU time in both replays. Game state,
geometry, atlas layout and the final viewport matched in each replay.
All 300 checked draw streams and expanded packet hashes matched. The
2,400-frame cold start matched game state, geometry and the viewport.

The `--retained-fan-cell-bounds` option removes repeated ordinary-cell
divisions by comparing exact cell-edge products. In its same-binary trial
it improved Waterfront throughput and reduced Dural throughput. It remains
opt-in. Astra reviewed both paths and their measured results.
See `build/acm/live-retained-fans-trial`.


### Compact vertices for clipped ordinary texture cells

In the staged indexed path, `--compact-cell-vertices` stores the
projected point and two signed cell indices for nonmirrored ordinary
texture cells. It reconstructs the two unit-scale transforms,
including negative zero, in the final packer. The origin fan can
use the same representation. Microtexture splits, mirrored cells,
native repeat, and cell coordinates outside the signed range retain
the full vertex. Final draw vertices and indices use their original
layout. Use `--cell-vertex-stats` to count the intermediate records.

The ARM packet fixture matched the expanded bytes and the existing
golden. At native frame rate settings, all 300 checked packed draw
and expanded texture streams matched on both Dural and Waterfront.
The 600-frame comparisons also matched state, atlas layout, and the
final viewport. All measured intermediate textured vertices in
both replays used the 24-byte representation, down from 52 bytes.
The recorded vertex payload was 51,277,200 versus 111,100,600 bytes
on Dural and 66,159,840 versus 143,346,320 bytes on Waterfront.
These are logical payload counts, without index or allocator overhead.

On Dural, mean packet wall time was 9.590 ms versus 9.951 and
9.938 ms in the controls. The replay reached 48.09 fps versus 47.92
and 48.36 fps, with p95 at 22.257 ms versus 22.149 and 22.128 ms.
Waterfront reached 51.84 fps versus 51.10 and 50.60 fps, with
packet wall time at 10.404 ms versus 10.853 and 10.838 ms. P95
was 20.368 ms versus 20.789 and 20.798 ms. These runs use one
untrained renderer binary. The selected interactive build was
restored.

The renderer was profiled on the Mini with compact vertices enabled and
disabled, covering cold boot and three fight checkpoints. In the trained
600-frame Dural comparison, compact vertices reduced mean packet wall
time by 0.46 to 0.50 ms. Throughput was 49.04 fps versus 48.81 and 47.67
fps, with p95 at 21.855 ms versus 21.857 and 22.381 ms. Waterfront reached
51.45 fps versus 50.86 and 50.76 fps, with p95 at 20.553 ms versus 20.750
and 20.664 ms. The compact Waterfront run had one 61.974 ms interval.

In a 900-frame Waterfront retest with reversed order, both compact runs
reached 52.55 and 52.76 fps versus 52.11 fps. Packet wall time fell by
0.61 ms. P95 was 20.071 and 19.758 ms versus 20.227 ms. P99 was 21.448
and 21.240 ms versus 21.499 ms. The retest did not reproduce the 61.974 ms
interval.

Astra reviewed the format and the tail results. Trained draws matched
all 300 draw-stream and expanded-triangle hashes per arena. Both game
state hashes, the atlas layout and the full gameplay viewport matched
in the active replays. The 2,400-frame cold-start replay and the ARM
fallback smoke matched their references. The Mini fast preset selects
compact vertices with the retained fan path. Use
`--no-compact-cell-vertices` for comparison.
See `build/acm/compact-cell-vertices-trial`.

### Sizing an atlas prewarm

Two 900-frame atlas replays recorded 750 tile recipes for Dural
and 811 for Waterfront, with 874 distinct recipes across both.
The combined bordered payload is 3,466,420 bytes. Reordering those
recipes by height in a model of the existing shelf allocator
required 8,112,000 bytes of logical texture backing, within the
12 MiB atlas cap. The estimate excludes the driver's allocation
overhead and content outside those replays. The game can rewrite
the texels behind a recipe. See
`build/acm/compact-cell-vertices-trial/prewarm-sizing.json`.


### Compact projected point stream

The Mini fast preset sends only the surviving projected points to
the staged GLES2 renderer, with separate polygon material records.
Each worker owns a point lane until draw packing completes. The
renderer retains the reference path for unsupported camera inputs,
batching limits and a failed staged submission plan. Use
`--no-projected-stream` to select the original polygon arrays.

Logical camera-to-packet data fell by 40.35 percent on Dural and
40.36 percent on Waterfront, saving 132,260 and 126,717 bytes per
frame on average. These measurements count live points and polygon
records. The point lanes reserve capacity for up to eight points per
polygon, and the experiment does not establish a peak memory saving.

The trained build was compared against the selected fast build in
two 600-frame series with matching checkpoints and inputs. Dural
reached 49.74 fps against 48.88 and 48.24 fps. The combined camera
and packet stage took 10.770 ms against 11.483 and 11.500 ms. P95
frame intervals were 21.517 ms against 21.823 and 22.491 ms.
Waterfront reached 53.04 fps against 51.70 and 51.44 fps. The
combined stage took 11.508 ms against 12.290 and 12.176 ms. P95
intervals were 19.751 ms against 20.514 and 20.473 ms.

The ARM camera fixture matched 177,125 polygons across 120 frames,
including material and projected point bytes. The Mini replays matched
game and geometry state, atlas placement and final viewport pixels.
Both trained arenas also matched all 300 expanded-triangle and final
draw streams. All 60 frames of the forced CPU planning fallback
matched. Astra reviewed data ownership and fallback. See
`build/acm/projected-stream-trial`.

A 2,400-frame cold replay and the non-PGO smoke matched the game
and geometry states and final viewport. One initial cold run showed
a slowdown during the character-selection transition. The paired
cold retest reached 56.83 fps with the packed stream versus 56.35
fps on the previous build. A frame of about 92 to 94 ms remained
at the same transition in both builds.


### Row-wise atlas borders and in-place transparent edge fill

`--atlas-tile-edges` copies each interior atlas row as a contiguous
span and fills the wrap or clamp endpoints separately. Transparent
texels retain the reference neighbor priority, right then left then
below then above. Only their color changes, and each source neighbor
must be opaque, so the original alpha and opaque colors remain valid
while filling in place. The ARM fixture matched 720 cases across
both atlas formats and all wrap/clamp combinations.

The 1,740-tick cold replays matched game and geometry hashes, atlas
layout and final pixels. At frame 1560, atlas planning took 24.993 ms
against 42.433 and 42.977 ms. The frame interval was 75.353 ms against
92.635 and 93.519 ms. At frame 1604, atlas planning fell to 9.623 ms
from 17.768 and 17.206 ms. These are same-binary ARM controls before
PGO training. The selected interactive build is restored.
See `build/acm/atlas-tile-edges-trial/corrected-results.json`.

The trained renderer preserved the gain against the previously
selected build. Frame 1560 fell from 92.834 to 74.065 ms, and the atlas
portion fell from 41.632 to 24.193 ms. Frame 1604 fell from 33.389 to
27.279 ms. The cold replay matched both state hashes, atlas placement,
and the final viewport. Dural delivered 49.73 fps versus 49.56 fps and
Waterfront delivered 52.82 fps versus 52.65 fps in the active replays.
Both arenas matched their separate 300-frame draw and expanded
triangle streams. The non-PGO ARM build matched the Dural draw stream.
The Mini fast preset enables the path, and
`--no-atlas-tile-edges` restores the original tile treatment.

Cold-start stall counts varied across runs. With the same trained
binary, frame 1560 took 92.712 ms with the original path and 74.707
ms with the optimized path. The selected-build repeat reached
91.567 ms at that frame and recorded 30 intervals above 40 ms from
frames 1440 to 1559. The optimized repeat recorded eight in that
window. All three retests matched state, atlas layout and pixels.

### Lazy clearing of inactive indexed packet slots

`--keep-inactive-packet-slots` leaves unused indexed payloads in
their reusable vectors. Only slots in the live mip range are
published to packing jobs. A reused slot is cleared before its
new packet is written. The existing memory cap counts the full
capacity of inactive slots and discards their payload if trimming
is needed. Flat polygons still own their single live slot.

The ARM fixture matched 1,000 live packet streams. In a 360-frame
Dural comparison, clipping and palette preparation averaged
5.814 ms versus 5.941–6.007 ms, and p95 swap time was 21.217 ms
versus 21.240–21.563 ms. Throughput stayed within the controls
at 51.18 fps. The replay matched game and geometry states, atlas
layout, final pixels and 180 frames of both draw-stream hashes.
Astra reviewed the slot lifetime and identified an omitted capacity
term when a compact packet becomes flat. The corrected Waterfront
build counts that capacity in the trim budget. In its 360-frame
comparison, clipping and palette preparation averaged 6.288 ms
versus 6.390–6.404 ms. It ran at 54.88 fps and 19.990 ms p95,
against 53.98–54.66 fps and 19.740–20.724 ms p95. The two 180-frame
draw comparisons and final state and viewport checks matched.
The path remains opt-in. See `build/acm/inactive-packet-slots-trial`.

### Projected point spans in texture preparation

The packed projected polygon accessor returns its existing points
by reference. Stack clipping copies the live span in one operation
before mutation. Packing an intact fan reads its borrowed points
directly. The expanded polygon fallback still materializes each point.
`--fused-uv-bounds` additionally scans the two texture axes together.

With inactive slots enabled, the span build used 7.651 and 7.700 ms
of packet CPU time on active Dural and Waterfront. The earlier
binary used 7.776–7.846 and 7.804–7.896 ms. With fused bounds added,
p95 frame times were 21.518 and 19.894 ms against 21.709–21.711 and
20.012–20.189 ms. Fused bounds alone showed mixed packet timings.
Both paths matched game and geometry state, atlas placement and
final pixels. The fused path matched 180-frame draw checks in both
arenas and on Dural with projected-stream rendering disabled.
Astra reviewed point lifetime and arithmetic. See
`build/acm/projected-span-reads-trial/results.json`.

### Spare capacity totals from prepared draw chunks

`--packet-chunk-capacity` adds each packet's spare bytes in its own
worker range. The submitting thread sums the ranges instead of
reloading every polygon's capacity record. Unprepared renderer
paths still use their per-polygon accounting.

In same-binary comparisons with inactive storage active, packet CPU
time fell to 7.510 ms from 7.647–7.708 on Dural and to 7.576 ms from
7.785–7.791 on Waterfront. The comparison matched both state hashes,
atlas layout, final viewport, and 180 frames of both draw hashes in
each arena.

`--packet-trim-test` lowers the trim limit from 4 MiB to 16 KiB
and records the count and checksum of the capacity trace. In
120-frame Dural replays, the old and new accounting trimmed 110
times and produced the same trace, draw streams and viewport.
The results also matched with projected-stream rendering disabled.
See `build/acm/packet-chunk-capacity-trial`.

The trained trial combined span reads, fused UV bounds, inactive
slots and per-chunk capacity totals. Compared with the then
installed renderer, packet wall time fell from 8.852–8.854 to
8.630 ms on Dural. P95 fell from 21.694–21.794 to 21.553 ms.
On Waterfront packet preparation fell from 9.597 to 9.390 ms
in the first pair, while p95 was 20.167 ms against 19.919 and
20.044 ms across the two controls. Sustained speed stayed near
50 fps on Dural and 54 fps on Waterfront. The combination
remains opt-in after the mixed whole-frame timing.

The expanded polygon renderer and the ARM executable compiled
without profile feedback both matched the 240-frame Dural draw
and state checks. The 1,740-frame cold replays matched state and
viewport, with arena-entry intervals of 74.685 and 75.205 ms.
See `build/acm/packet-chunk-capacity-trial/trained-results.json`.

### Shared projected packet storage

`--shared-packet-storage` stores clipped geometry in per-worker
buffers for full vertices, compact vertices, relative indices and
retained fans. Each prepared draw records offsets in its worker's
buffers. The atlas pass resolves those offsets after growth and
capacity trimming finish. The expanded polygon path retains its
existing packet storage.

The ARM fixture matched 1,284 appended mip bands after buffer
growth. The Mini matched atlas layout, final viewport pixels and
240 frames of expanded triangles and final draws in each trained
arena replay. The expanded polygon fallback and the non-PGO
ARM build also matched the Dural stream. A separate 180-frame
test forced a small spare-capacity limit and matched every draw
while trimming. See `build/acm/shared-packet-storage-trial`.

The two 600-frame trained comparisons followed identical fight
inputs on stock clocks. Dural reached 51.45 fps with packet
preparation averaging 7.34 ms. The prior build reached 50.30 to
50.49 fps and averaged 8.86 to 8.89 ms for packet preparation.
Waterfront reached 55.67 fps and averaged 8.02 ms, against 54.30
to 54.66 fps and about 9.60 ms. P95 frame intervals measured
21.38 ms on Dural and 19.94 ms on Waterfront, above the 17.38 ms
budget. The 1,740-frame cold replay matched state, atlas layout
and final pixels. Its arena-entry frame measured 72.29 ms
against 74.60 ms.

The Mini fast preset enables shared storage with the projected
stream. Use `--no-shared-packet-storage` to select separate
polygon packets while keeping compact projection.

The packing workers resolve their own geometry offsets after
atlas placement. Serial planning uses only vertex and index
counts. The workers read stable geometry and write their own
draw records and assigned output spans. Draw and clipping order
are unchanged.

On the trained build Dural reached 52.17 fps against 51.74 to
51.83 fps, and Waterfront reached 56.63 fps against 54.91 to
56.03 fps. Packet preparation fell from 7.34?7.36 to 7.13 ms on
Dural and from 8.00?8.08 to 7.87 ms on Waterfront. P95 measured
21.23 and 19.84 ms. The cold arena-entry frame was 72.21 ms
against 72.58 ms. The cold run's p95 rose from 18.59 to 18.91 ms
and its p99 fell from 52.39 to 51.65 ms in one comparison.

Both 240-frame trained draw streams matched, along with the
expanded-polygon fallback, forced trimming, the non-PGO ARM
run and the 1,740-frame cold replay. See
`build/acm/worker-packet-offsets-trial`.

### Fixed FIFO stores from the i960

The board identifies aligned writes to the coprocessor FIFO as fixed-address
stores. The interpreter and recompiler share a multiword-store loop that
reuses the flag lookup after the first word. Each write still goes through
the normal FIFO or microcode-upload operation with the original callback,
cycle charge, stall check and retry index. Generic memory handles unaligned accesses and other address ranges.

Three ARM fixture configurations matched 150,812 four-slice cases each.
The fixed-port fixture dropped flag lookups from 488 to 336. FIFO traces
matched all 184,413 records, including virtual timestamps. The exact draw
checks covered 240 frames each on Dural and Waterfront. Astra reviewed
the fixed-address contract, upload mode and stall handling.

The trained Dural replay reached 52.34 fps against 50.99?51.52 fps in
the previously installed controls. Mean simulation time fell to 17.52 ms
against 17.86?18.08 ms. Waterfront reached 56.53 fps against 55.70?55.99
fps. Simulation took 16.88 ms against 17.03?17.06 ms. P95 frame intervals
were 21.22 and 19.84 ms, respectively.
See `build/acm/i960-fixed-fifo-store-trial`.

The 1,740-frame cold replay matched state and final pixels. Arena entry
took 71.85 ms against 73.13 ms. Cold p95 fell from 18.99 to 18.74 ms
and p99 rose from 52.00 to 52.98 ms in one comparison. The non-PGO ARM
build matched all 240 reference draws and state. The fast preset enables
fixed FIFO stores. Use `--no-i960-fixed-fifo-store` to compare the
generic access path.

### Fixed FIFO loads into the i960

Aligned dword reads of the coprocessor FIFO use the board's FIFO read
operation directly. The interpreter and recompiler reuse the first access
flags for the remaining words of a fixed-address load. Each read still
checks for a stall before writing its destination register. The existing
retry loop re-enters at the recorded word and uses the same FIFO operation.

Three ARM fixture configurations matched 150,872 cases each, including
distinct FIFO words and stalls at different offsets. Fixed-port flag
lookups fell from 628 to 300 in the fixture. Each real transaction check
matched all 184,413 records with fixed stores enabled and with fixed
stores disabled. Astra reviewed the bus contract and retry handling.

Dural reached 52.85 fps against 51.75?52.26 fps on the previously installed
build, with p95 at 21.06 ms. Mean simulation time was 17.40 ms, compared
with 17.66?17.67 ms in the same-binary controls. Waterfront reached
56.41 fps and 19.80 ms p95. Its same-binary controls were 55.95?56.50 fps
and 19.85?19.86 ms p95. The two trained 240-frame draw checks matched
state, expanded packets, final draws, atlas placement and final viewport.
See `build/acm/i960-fixed-fifo-load-trial`.

The 1,740-frame cold replays matched state and pixels. Cold p99 varied
between runs. The first comparison measured 56.54 ms with fixed loads
and 43.90 ms on the installed build. The repeat measured 32.17 ms with
fixed loads, 55.64 ms with the option disabled in the same binary and
52.24 ms on the installed build. First arena entry took 70.61?72.54 ms
across the five runs. The non-PGO build matched all 240 reference draws.
The fast preset enables fixed FIFO loads. Use `--no-i960-fixed-fifo-load`
to compare generic reads independently of fixed stores.

### Guarded FIFO HALT delivery

`--fuse-fifo-halt` combines a FIFO synchronization callback and
the HALT assertion it schedules when that assertion is the next
timer event. It advances the timer sequence and stores the same
disabled one-shot fields. An active line timer, queued transition,
earlier or same-time event, or periodic source uses the ordinary
timer queue.

The ARM trace fixture matched 184,413 FIFO records including virtual
timestamps, plus the final serialized core. A Dural probe counted
11,677 fused assertions in 16 frames. Five focused ARM cases covered
direct delivery and the queue guards. Astra reviewed event ordering.

The untrained Mini replays matched game and geometry hashes and all
viewport pixels. Dural p95 was 20.99 ms against 21.09 and 21.26 ms.
Waterfront was 20.19 ms against 20.02 and 20.49 ms. See `build/acm/fuse-fifo-halt-trial`.

Trained Dural ran at 53.07 fps and 20.79 ms p95. The installed
controls ran at 52.20?52.44 fps and 21.00?21.08 ms p95. Waterfront
ran at 57.08 fps and 19.57 ms p95 against 56.53?56.64 fps and
19.69?19.85 ms p95. Each 240-frame arena check matched both draw
hashes, state, atlas placement and viewport. The 1,740-frame cold
replay and 240-frame non-PGO run also matched. The cold arena-entry
frame took 72.08 ms against 71.52 ms in one comparison.

The fast preset enables guarded fusion with fast HALT delivery
and synchronization timers. Use `--no-fuse-fifo-halt` to compare
the queued delivery path.

### Tracked display RAM snapshots

The Mini fast preset tracks writes from the i960, TGP and geometry ports and
copies the touched pages into the geometry worker's owned snapshot. The i960
routes mirrored display RAM stores through the board so that recompiled code
marks them. Reset and checkpoint load force a full refresh. Use
`--no-track-geometry-pages` to restore the full-copy path.

A cold replay verified all 1,740 ARM snapshots against full copies. The 600-frame
trained comparisons reduced snapshot time to roughly 0.06 ms and reached
52.94 to 53.02 fps on Dural and 57.21 to 57.53 fps on Waterfront. The 1,740-frame
cold Mini replay and the non-PGO draw checks matched the reference state and
viewport. See `build/acm/tracked-geometry-pages`.

### Wider gameplay palette atlas

The Mini fast gameplay preset uses two palette banks in a single 512?1024
texture. Each upload still addresses a 256-pixel-wide bank. This adds about
1 MiB of CPU palette staging and 1 MiB of GPU texture storage. Use
`--no-wide-palette-atlas` for the 2,048-row atlas.

In the instrumented fight replays the wider atlas eliminated 17 capacity
resets on Dural and eight on Waterfront. Trained p95 frame times were 20.26 and
20.64 ms on Dural, and 19.21 and 19.26 ms on Waterfront. Draw comparisons hash
palette colors independently of slot addresses. Both 240-frame draw checks,
the 1,740-frame cold replay and the non-PGO fallback matched reference output.
See `build/acm/wide-palette-atlas`.

### Coalesced tile scanlines

The Mini fast gameplay preset groups GPU tile rows with the same horizontal
scroll. Each band preserves the selected map, layer priority, mask and source
wrap. Use `--no-coalesce-line-scroll` to draw individual scanlines.

The paired ARM row trace matched for 240 frames in Dural and Waterfront.
Dural's two scrolling passes used 213,088 rectangles instead of 288,810.
Trained picture capture took 1.24 and 1.26 ms, compared with 1.48 and 1.44 ms
in the matched controls. The 1,740-frame cold replays and the non-PGO draw
check matched reference output. See `build/acm/coalesced-line-scroll`.

### GPU line scroll for the opaque backdrop

The Mini fast gameplay preset samples the paired System 24 scroll tables
in the tile shader for the opaque backdrop. Foreground layers keep CPU
scanline coalescing and coverage clipping. Use `--no-gpu-line-scroll` to
restore CPU scanline generation for the backdrop too.

In two 600-frame Dural trials, the backdrop used one scroll rectangle per
frame. Trained picture capture fell from 1.22 ms to 0.83?0.86 ms, p95 frame
intervals fell from 19.88?20.00 ms to 19.07 ms, and mean throughput rose
from 53.69?53.78 to 55.05?55.31 fps. Mean GPU fence wait stayed at 0.01 ms.
The Waterfront replay had zero GPU scroll rectangles and similar or
better frame times. The 240-frame draw checks in both arenas and the
additional Dural captures matched the CPU reference. See
`build/acm/gpu-line-scroll` for the timings and full release checks.

### Cached System 24 pens

The Mini fast gameplay preset keeps decoded System 24 pen colors after a
palette rebuild. Ordinary palette writes update their pen immediately.
Translation changes, checkpoint restore, and saved-frame swaps invalidate
the derived colors. Use `--no-cache-2d-pens` to keep the repeated rebuild.

Trained same-binary comparisons in Stage 5, Dural and Waterfront saved
0.11?0.16 ms in picture capture. Waterfront p95 frame intervals fell from
18.53?18.75 ms to 18.38?18.45 ms. Stage 5 and Dural frame-time differences
were smaller and varied between repeats. The three 240-frame draw replays
matched the decoded palettes, draw streams, state and viewport. All 1,740
cold-start pen hashes and the non-PGO fallback also matched. An ARM fixture
covered direct palette and translation writes, checkpoint reload and
saved-frame redraw across four arena checkpoints. See `build/acm/cache-2d-pens`.

### Cached 3D palette shade ramps

The Mini fast preset shares RGB shade ramps across palette rows. The
cache uses about 65 KiB and refreshes when colour translation or gamma
changes. `--no-palette-shade-cache` selects the original path.

Two cold fight-entry measurements saved 2.5 and 2.8 ms in palette
preparation. Full-frame timing varied. The selected build matched
1,600 System 24 pen hashes, 603 colour-aware draw and texture packet
hashes, and the final viewport. The cache-off and non-PGO runs also
matched state and final pixels on Dural. Measurements are under
`build/acm/live-palette-shades` and `build/acm/select-palette-shades`.


### Native System 24 tile expansion

The Mini fast preset expands 4-bit System 24 character rows with ARM
NEON. The pen and priority planes keep their original layout. Use
`--no-s24-neon-decode` for the scalar decoder.

With the arena textures preloaded, two counterbalanced Mini replays
reduced tile expansion at entry from 3.54 and 3.68 ms to 2.73 and
2.46 ms. Both five-frame transition windows improved. The scalar and
NEON plane hashes, game state and final viewport matched. The
worker-pool trial remains separate. See `build/acm/s24-neon-decode`.


### Signed atlas preload on cold boot

`--fast --boot` loads the bundled plan for the attract sequence and
three known first-fight selections. The preloader checks fighter
selection and texture RAM contents, and waits for new writes to settle
before continuing. Unlisted fights load their textures when first used.
Use `--no-atlas-preload` for the on-demand reference or
`--atlas-state-plan FILE` for a custom signed plan. These options also
apply to cold interactive starts.

The selected build retains the profiled simulation object from the
NEON tile release. Cold-start timing, output comparisons, fallback and
interactive verification are archived under
`build/acm/select-atlas-residency`.

### Dural cadence after early tile staging

On the installed fast preset, two 1,200-tick full-draw controls measured
17.275 and 17.280 ms across continuous Dural combat, with p95 at 17.963
and 17.966 ms. The fixed sustained window at ticks 302-541 missed the
17.384 ms budget on 115 and 140 of 240 ticks. Arena entry, warmup and
capture are excluded. Audio remains deferred.

Releasing the tile buffers after the last canvas paint raised early
staging coverage from 62.5% to 84.8% in the sustained window. Deadline
misses across the paired stage-traced controls and trial were 261 and
265 of 480. The experiment remains on its local branch at `88dc1bc`.

Separate traces used the installed board, DSP, geometry and GLES objects.
Geometry consumed about 4.7 ms of worker CPU per sustained frame,
mostly overlapping other work, with 0.36-0.40 ms exposed at the join.
The strip parser and quad assembly accounted for 76% of worker samples.
On release-slow ticks, the matching simulation profiles showed an
additional 0.52-0.57 ms of simulation CPU time. The profiles and exact
viewport comparisons are in `build/acm/post-vblank-profile`,
`build/acm/tile-release-after-paint`, and `build/acm/vblank-geometry-samples`.
The installed interactive binary was restored and verified.

### Aligned i960 bursts within one page

The trial at `bd39fd2` makes a single plain-page lookup for an aligned
multiword i960 transfer wholly within that page. It keeps the original
per-word bus path for all other transfers. ARM differential cases and
145,264 timed FIFO records matched. On the Mini, the untrained helper
reduced sustained Dural simulation time by 0.24 ms per frame in the
counterbalanced comparison. The trained qualification did not reproduce
the gain. Sustained misses were 271 of 480 with the new path, 254 with
it off in the trained binary, and 222 in the installed controls. The
Mini restored the installed release. The flag remains on the local
trial branch. See `build/acm/i960-burst-page-outlined/findings.md` and
`build/acm/i960-burst-page-trained/gate-results.json`.

### Profile-selected private DSP sequences

The opt-in trial at `2bcb2db` groups 20 callback-free instruction
sequences inside the packed DSP dispatcher. It removed about 7,001
dispatch calls per Dural frame. The ARM state fixture, 145,264 timed FIFO
records, and 240 on-device draw and texture comparisons matched.
Sustained timing varied across matched arms. Total Dural deadline misses
rose from 785 to 872 across 2,156 combat intervals with the same untrained
DSP object. The installed build was restored and verified. Astra
recommended keeping the branch and deferring training. See
`build/acm/dsp-hot-private-regions/findings.md` and
`build/acm/dsp-private-region-inventory/analysis.log`.

### Retained i960 operands and a shorter Dural replay

The i960 trial keeps two frequently accessed guest registers in ARM
registers across five validated instruction spans. The trained build
matched draw and state proofs but gave 240 sustained Dural misses versus
237 in the installed controls, with 480 intervals per arm. A further
variant preserves the original JIT prologue and avoided 1,713 register
file accesses per sustained Dural frame. Its untrained same-binary
comparison saved 0.04 ms in simulation, with mixed frame-time tails.
The trials are archived at `874d0a0` and `ee1fe03`. The installed preset
was restored. See `build/acm/i960-retained-spans-direct/findings.md`.

A checkpoint at Dural tick 270 reaches the same core and geometry state
in 269 frames as the original 542-frame verification replay. All 269
expanded texture hashes matched on the Mini. The shorter replay rebuilt
the texture atlas in a different order. It assigned 362 shared keys to
different pages and changed both packed draws and pixels. Use the short
checkpoint for CPU or geometry diagnostics. Full-render release
comparisons continue to use the original replay. See
`build/acm/sustained-dural-checkpoint/findings.md`.

### Restoring Dural draw state for short tests

Ordered atlas capture and replay at `49add5c` restores the 723 tiles
resident before the checkpoint. The short and long replays end with
all 749 keys in identical atlas positions. Per-frame draw order at
`54f08a2` also restores all 269 palette-normalized draw hashes. The
expanded texture hashes, core and geometry state, and final viewport
match on the Mini.

Use the 269-frame replay for paired tests with the same checkpoint,
atlas seed, and renderer. Its 240 measured ticks are the same combat
ticks as the long replay. Cadence still differs, so release comparisons
use the full replay. The opt-in bucket changes stayed experimental.
The flat lookup at `3571c18` shortened packet planning in both traced
repeats, with longer packing and no repeatable frame-time gain. Results
are under `build/acm/checkpoint-atlas-order`,
`build/acm/active-draw-buckets`, and `build/acm/flat-draw-buckets`.

### Single-mip lookup and sampled i960 code placement

The atlas trial at `c1a791e` reuses a resolved tile when both mip
selections refer to it. It skipped 157,901 lookups in the short Dural
replay with exact output. Traced atlas planning fell by 0.03-0.10 ms.
Frame pacing varied. Keep the option on the trial branch. See
`build/acm/single-mip-lookup/findings.md`.

The i960 trial at `fcaa649` groups 32 functions from the combat CPU
profiles. Their bodies occupy 140 contiguous pages, down from 165
occupied pages spread across a 745-page range. Draws, state, and
timed FIFO events matched. The trained full replay missed 253 of 480
sustained Dural intervals, versus 229 with the flag off and 241 on
the installed build. The installed build was restored and verified.
See `build/acm/i960-hot-layout/findings.md`.

### i960 loop traces in sustained combat

The ARM replay traced native i960 instructions and helper calls during
ticks 302-541 of Dural and Stage 5. The instrumented Dural replay ended
with the reference core and geometry hashes. These are execution counts
from QEMU. Use the Mini to measure any timing effect.

Repeated straight-line loops accounted for about 1.1% of Dural's traced
native instruction visits and 1.4% of Stage 5's. The busiest loop becomes
visible when following its internal branch. At `0x2391c` it tests a bit,
skips the load/store body when the bit is clear, increments the bit
index, and branches back. This three-instruction path completed at least
208,774 helper-free backedges in each replay, accounting for about 6%
of native instruction visits. The other frequently repeated paths at
`0x24e00` in Dural and `0x23ac4` in both arenas call a bus helper on
every observed backedge.

The next test is to group consecutive clear-bit iterations at
`0x2391c`. Fall back before a set bit, the terminal iteration or a
guest slice boundary. Preserve guest registers, condition codes, and
cycle consumption. Check timed FIFO events and replay output before a
paired Mini timing run. Trace artifacts are in
`build/acm/i960-loop-census`.

### Grouped i960 clear-bit iterations

Trial `3e0bf1e` skips consecutive clear bits at `0x2391c` when the
whole group fits inside the current i960 slice. The original code handles
set bits, the terminal iteration and slice boundaries. The ARM fixture
matched 278,208 slice states and all 145,264 timed FIFO records matched
across three arenas. The trained hardware proof matched 240 draw and
texture hashes, core and geometry state, and the final viewport.

The short counterbalanced replay missed 241 of 480 sustained deadlines
with the shortcut and 280 with it off. The trained full replay reversed
that result. It missed 283 of 480 sustained deadlines with the shortcut,
238 with it off in the same binary, and 259 in the installed controls.
The shortcut stays on its local trial branch. Timing is archived under
`build/acm/i960-clear-bit-loop` and `build/acm/i960-clear-bit-loop-trained`.

### Board addresses and FIFO store runs in sustained combat

Over 240 sustained combat ticks per arena, the DSP FIFO accounted for
1,022,762 of 1,823,473 i960 bus-helper calls in Dural and 984,524 of
1,772,602 in Stage 5. The callback at `0x23ac8` was a three-word ROM
load, and `0x24e14` was a 16-bit tile RAM store in Dural. See
`build/acm/i960-clear-bit-loop/bus-stage-*.json`.

For adjacent scalar FIFO stores, the recorded queue depth and slice
budget allow up to 154,298 Dural and 155,772 Stage 5 calls to be grouped
away in the measured window. This count assumes grouping every eligible
site. Trial `7b74061` covers four observed guest functions and enqueues
up to five words per call. The original path handles alignment, upload,
overflow, wakeups, tracing callbacks and slice boundaries. In separate
542-tick ARM replays, it removed 164,243 callbacks in Dural, 168,833 in
Stage 5, and 109,768 in Waterfront, with matching final game and geometry
hashes in each arena. See `build/acm/i960-fifo-store-runs/proof.log`.

The on-device proof matched 269 draw and texture signatures, atlas
placement, core and geometry state, and the final viewport. The archived
timing harness had a stale run-name check. Its four timed commands all
left FIFO grouping disabled, so those numbers only describe variation
between controls. The release was restored and verified at CRC `9b1f5565`.

Trial `00fc28c` checks FIFO capacity in generated ARM code before
entering the grouped-store helper. Across three 542-tick ARM replays,
the early check avoided 17,231 helper calls. Each replay retained the
same grouped-store count and final core and geometry hashes as the
original helper.

### FIFO store grouping and ARM capacity check on the Mini

The corrected comparison ran one proof and six sustained timing runs on
the same untrained diagnostic binary, SHA-256 `42f5b9dd...`. Runs were
sequential and followed control, helper, preflight, preflight, helper,
control. The archived commands were audited afterward: the two controls
contain neither experiment flag, the two helper runs contain
`VF2_I960_FIFO_STORE_RUNS=1`, and the two preflight runs contain both
that flag and `VF2_I960_FIFO_STORE_PREFLIGHT=1`. All jobs used the
269-frame tick-270 Dural replay at stock clocks.

The proof matched the archived control's core and geometry state, 269
draw and texture signatures, palette output, atlas layout bytes, TGP
counts and a pixel-identical 480x640 viewport. Every timing run retained
the same state hashes and viewport.

| Arm | Runs | Sustained mean | p95 | p99 | Over 17.384 ms | Simulation mean |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Original path | 2 | 18.323 ms | 19.212 ms | 19.816 ms | 474/480 | 18.164 ms |
| Grouped helper | 2 | 18.353 ms | 19.222 ms | 19.902 ms | 470/480 | 18.162 ms |
| Helper + ARM queue check | 2 | 18.422 ms | 19.243 ms | 20.006 ms | 468/480 | 18.241 ms |

Run means were 18.283-18.364 ms for the original path, 18.324-18.382 ms
for the helper and 18.293-18.551 ms for the checked helper. Per-run p95
spanned 19.140-19.252 ms, 19.216-19.222 ms and 19.185-19.291 ms, and p99
spanned 19.627-19.890 ms, 19.848-19.922 ms and 19.827-20.075 ms. The
0.081 ms spread between the two control runs was larger than any arm
difference.
The first checked-helper run was the slowest and had the highest fence
wait at 0.34 ms per frame; its repeat was the fastest. Grouping removes
guest callbacks in ARM replays, but this comparison shows no repeatable
frame-time gain from either grouping or the capacity check. Keep both
flags opt-in and leave the release defaults unchanged.

The untrained diagnostic is slower than the installed release, so these
absolute times are only comparable within this binary. The interactive
release was restored and verified at PID 3345 and CRC `9b1f5565`.
The run used `python -u build/acm/i960-fifo-store-preflight/run.py`,
then `verify-release.py` and `analyze.py` from the same directory. Raw
jobs, logs and the pooled summary are under
`build/acm/i960-fifo-store-preflight`.

### Render contention, hitches and the steady gap (2026-11-20)

The installed release ran the 269-frame tick-270 Dural short replay with
a per-frame trace at stock clocks, job `20261120-003137-671623`. Core
state `2e7d925031640471` and geometry `724d987cdc7cbca5` matched the
recorded release. Excluding ticks before 30 and intervals above 30 ms:

| Series | n | mean | p50 | p95 | p99 | max | over 17.384 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| swap interval | 237 | 17.518 ms | 17.494 | 18.143 | 18.598 | 19.323 | 155/237 |
| simulation task | 237 | 16.987 ms | 16.943 | 17.953 | 18.805 | 19.476 | 24/237 |
| interval - simulation | 237 | 0.531 ms | 0.594 | 1.171 | 1.571 | 1.767 | - |

Five frames (2, 13, 76, 196 and 269) took 30-170 ms with normal
14.8-17.1 ms simulation tasks, so the excess was render or present, not
emulation. They raise the raw 269-frame mean from about 17.50 to
18.317 ms and p99 from 18.60 to 31.525 ms. The sustained 302-541 window
does not contain them.

A simulation-only arm, job `20261120-003219-d807ab`, was confounded:
`--mode simulation` drops the trained DSP bundle, so it ran fewer TGP
instructions on slower paths and measured 18.945 ms of simulation per
frame against 16.924 ms in full render. It does not test contention and
should not be reused for that purpose.

The 0.531 ms steady gap is close to the 0.175 ms tile descriptor plus
0.345 ms geometry join measured earlier, so the join is the serial
component to attack first. The simulation p95 remains 0.95 ms above its
mean, which is the other half of the p95 gap.

A cost ranking from the saved traces found no hot i960 loop to
specialise: guest labels total 42,939 per frame and the hottest clean
loop is only 0.40% of them. Bus helper calls total 7,598 per frame, of
which 4,262 are DSP FIFO. See `build/acm/cost-ranking.py`.

An independent review completed on 2026-11-20 concluded that incremental
micro-optimisation cannot reach 17.384 ms with a low p95, and ranked
DSP `0x4a` block-aware publication first at 0.05-0.25 ms. Reviews now run
every 4 hours. Details are in `build/acm/render-contention/findings.md`.

### Signed dispatch gap, counterbalanced short-replay screen (2026-11-20)

Trial `91e9c7f` was re-screened with two counterbalanced same-binary pairs
on the 269-frame tick-270 Dural replay. All four jobs used binary
SHA-256 `5e50f8d7...` and matched the release core and geometry state
(`724d987cdc7cbca5`). Steady frames only:

| Arm | Runs | n | interval mean | p95 | p99 | over 17.384 | sim mean | gap mean |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Control | 2 | 478 | 17.878 ms | 18.737 | 19.658 | 432/478 | 17.200 | 0.678 |
| Signed gap | 2 | 478 | 17.993 ms | 18.790 | 19.611 | 458/478 | 17.327 | 0.667 |

The candidate raises the mean by 0.115 ms and p95 by 0.053 ms and adds 26
over-budget intervals, against a 0.067 ms control-to-control mean spread.
It is rejected and stays opt-in. Jobs `20261120-005116-477f68`,
`20261120-005154-c9c291`, `20261120-005230-327744` and
`20261120-005304-abab27`; see `build/acm/dispatch-gap-screen`.

`gpu->settle()` does not block in the GLES backend (it only samples GL
error state), and early tile staging already reaches 84.8% coverage
without reducing misses, so the remaining 0.53-0.68 ms handoff is the
geometry-worker join at the head of the render phase, not the descriptor
or `settle()`.

### Where the join actually is (2026-11-20)

`VF2_RENDER_STAGES=1` on the release, job `20261120-005938-bc0445`
(binary `c5201a5f...`, 269 frames, 53.40 fps, state hashes matched),
reported these main-thread stages in ms per frame:

| Stage | wall | thread CPU |
| --- | ---: | ---: |
| simulation wait | 1.9616 | 0.1143 |
| picture capture | 0.3686 | 0.3670 |
| camera preparation | 1.7651 | 1.7001 |
| packet/material preparation | 6.9191 | 6.4221 |
| prior GPU drain | 2.3714 | 0.2018 |
| tile upload | 0.5466 | 0.5386 |
| 3D submit | 3.4055 | 3.3579 |
| composition submit | 0.2951 | 0.2937 |
| fence/flush | 0.2867 | 0.2861 |
| EGL swap | 0.7651 | 0.7236 |

`Geometry jobs: 0.108 ms/list snapshot, 4.363 execution, 0.317
materialization wait`; `Pipeline simulation tasks: 268, 17.12 ms/task`.

The join is `geometry_wait()` in the `T_VBLANK_END` timer
(`src/machine/model2a.cpp:1585`), on the simulation thread inside the
17.12 ms task, and is only 0.317 ms. Palette/atlas preparation is part of
the 6.92 ms packet stage on the main thread, so the two cannot overlap.
The render-side waits are larger: 2.371 ms of prior GPU drain with 0.202
ms of CPU, with the prior fence still pending in 192 of 269 frames, and
0.765 ms of EGL swap. See `build/acm/join-attribution/findings.md`.

### Why an earlier geometry snapshot cannot help (2026-11-20)

`geo_parse()` is called from `T_VBLANK_START` (`model2a.cpp:1567-1575`)
and immediately captures the display list and starts the worker
(`model2_video.cpp:3381-3400`). That is the earliest point the list is
complete, so there is no earlier snapshot to move to. The wait is paid at
`T_VBLANK_END` and again inside `frame_end()` (`model2_video.cpp:576-584`)
because `render_polygons()` consumes the parsed geometry, so deferring
it is incorrect.

With 4.363 ms of worker execution and 0.317 ms of wait, the
`T_VBLANK_START`-to-`T_VBLANK_END` window is about 4.05 ms. The worker
must become about 7.3% faster to fit. See
`build/acm/join-attribution/early-snapshot.md`.

### Geometry worker material gather is not a lever (2026-11-20)

The worker profile's hottest mapped lines are the material words
(`model2_video.cpp:826/831`), so a trial chose each texture stream's
RAM/ROM base and mask once and prefetched the next polygon's spans. On one
binary with two counterbalanced same-binary pairs (jobs
`20261120-013445-9dae10`, `20261120-013523-4b3035`,
`20261120-013559-05e06e`, `20261120-013626-37ba1b`, SHA `7acb4dd36799...`)
worker execution was 4.008 ms plain against 4.009 ms gathered/prefetched
and the pooled steady interval mean was 19.187 against 19.225 ms, inside a
0.087 ms control spread. Rejected; the source is reverted.

The trial binary is a full non-PGO rebuild: the release object set
predates later header changes (an incremental rebuild crashed from mixed
object ages) and `build/acm/pgo/data` is stale against the current sources,
failing `-Werror=coverage-mismatch`. Its absolute numbers are not
comparable with the release.

The wait is `worker execution - vblank window`. The release runs 4.363 ms
against a ~4.05 ms window for 0.317 ms of wait; this slower build runs
4.01 ms against ~4.2 ms for 0.17 ms. So a 0.32 ms gain needs about 7.3% off
the whole worker, and comparing waits across builds of different speed is
misleading. See `build/acm/geometry-material-gather/findings.md`.

### The prior-GPU drain is not on the critical path (2026-11-20)

The recorded next step was a one-frame-deeper GPU queue to remove the
2.371 ms `prior GPU drain`. That was already run twice and failed for
structural reasons: `gpu_frame_queue::end_frame()` drains before creating
the next fence, so the queue is depth one, and deferring the drain moves
the wait into the tile/texture upload, which then blocks on the GPU reading
the same textures. `gpu-fence-depth-trial` job `20261117-124750-8bd486`
measured 44.012 ms of tile upload and 14.58 fps (baseline
`20261117-124728-c3c490` 0.712 ms and 39.44 fps), and `double-gpu-trial`
job `20261117-105006-6f7b45` removed the waits with duplicated resources
but gained no throughput and changed interpolation.

So this turn tested whether the drain is on the critical path at all, on
the current release. Same binary, 269-frame Dural replay, stages on:
`20261120-005938-bc0445` (3 render threads) against
`20261120-015211-90f18c` (`--render-threads 2`). Removing one worker raised
packet preparation 7.15 to 7.67 ms, 3D submit 3.30 to 3.68 ms and the drain
1.58 to 2.25 ms, but the steady interval moved only 17.548 to 17.605 ms and
the simulation task 17.211 to 17.212 ms, because the simulation wait fell
from 2.83 to 0.80 ms. Main-thread CPU 13.690 ms plus 1.380 ms of exposed
drain against a 17.280 ms simulation task leaves 2.210 ms of slack.

The interval is `sim_task + gap` with sim 17.2 and gap 0.34-0.53 ms, so the
mean and the p95 must both come from the simulation task, whose p95 is
18.0 against a 17.2 mean. See `build/acm/gpu-drain-inventory/findings.md`.

### The simulation tail is not DSP dispatch (2026-11-20)

The 2026-11-18 attribution named DSP commands 0xff, 0x32, 0x0b, 0x01 and
0x37 as about 75% of the extra work on slow frames, so the tail was
attributed again on the current tree with a new per-frame counter trace
(`VF2_FRAME_COUNTS`, six counter reads and one `fprintf` per frame; the
patch is quoted in the findings). Two 269-frame Dural runs on one non-PGO
full rebuild, jobs `20261120-021005-04f995` and `20261120-021231-bca165`,
239 steady frames each, split into the slowest decile by simulation time:

| Metric | Run 1 | Run 2 |
| --- | ---: | ---: |
| simulation excess on slow ticks | +1.092 ms | +1.118 ms |
| extra DSP instructions/frame | +3,987 (+4.92%) | +4,027 (+4.97%) |
| extra DSP block runs/frame | +112 (+6.53%) | +111 (+6.48%) |
| DSP share of the excess (regression) | +0.480 ms (44%) | +0.475 ms (42%) |
| native DSP helpers / entries | 0 / 0 | 0 / 0 |

So at most 44% of the tail is DSP instruction work, and it is genuine
extra game work, not overhead. Dispatch overhead is separately ruled out:
`build/acm/tgp-command32` removed 1,266,261 dispatches (~1,691 per late
tick) and still regressed Dural, and `tgp-command37` was flat. The
remaining ~0.6 ms is the i960 guest, the board scheduler/FIFOs and thread
scheduling. See `build/acm/sim-tail-attribution/findings.md`.

### Failed i960 FIFO reads are a mean cost, not a tail cost (2026-11-20)

The counter harness was extended with FIFO counts (attempts, empty and full
stalls per direction) and run twice on one non-PGO full rebuild, jobs
`20261120-022319-a29990` and `20261120-022352-14c695`, 239 and 238 steady
frames. Slow decile versus the rest:

| Metric | Run 1 | Run 2 |
| --- | ---: | ---: |
| simulation excess | +5.91% | +6.29% |
| DSP instructions | +5.02% | +4.57% |
| TGP->i960 pops | +9.75% | +8.61% |
| failed i960 reads | +8.49% | +7.38% |
| failed i960 reads as a share of i960 reads | 41.46% -> **40.98%** | 41.45% -> **40.98%** |

The failure share is flat (slightly lower on slow ticks) while traffic
rises, so retries do not concentrate on the tail and no FIFO handoff change
should be screened for the tail. The absolute numbers are the useful
result: **41.5% of i960 FIFO reads find the FIFO empty** (853 per frame)
and the input FIFO is full 384 times per frame, each paying a
`timer_sync` and a stall, against a previously measured 1.29 ms/tick total
retry cost. That is the largest remaining mean lever. The tail itself is
proportional work in every subsystem (DSP +5%, block runs +6-7%, FIFO
traffic +5-10%), so no single hotspot removal reaches it; a ~5% global
simulation throughput gain is what would. See
`build/acm/sim-tail-fifo/findings.md`.

### The per-event scheduler cost is the real lever (2026-11-20)

Two follow-ups priced that 1.29 ms/tick figure and then attributed it.

First, `build/acm/fifo-empty-price` bracketed every failed i960 output-FIFO
read and the successful retry that consumes the DSP result, in host time
on one non-PGO full rebuild (jobs `20261120-024307-1e81df` and
`20261120-024411-d5f7b8`, 239 steady frames each):

| Metric | Run 1 | Run 2 |
| --- | ---: | ---: |
| failed reads / completed spans per frame | 860.6 / 860.6 | 860.6 / 860.6 |
| empty-FIFO timer already the scheduler head | 100% | 100% |
| handoff wall per frame | 8.037 ms (39.5% of sim) | 8.232 ms (39.9%) |
| handoff wall per episode | 9,339 ns | 9,565 ns |
| slow decile, ns/episode | 9,310 -> 9,594 | 9,526 -> 9,906 |

The earlier trace gives 6.6 us of virtual time to retry on Dural; at this
emulator's ~0.85x real time that should cost ~7.8 us of wall, so the
measured 9.3-9.6 us leaves **1.3-1.5 ms/frame of host overhead** and
confirms the 1.29 ms figure independently. The span is proportional work,
not a constant.

Second, a clean simulation-thread profile (`build/acm/sched-glue-profile`,
job `20261120-030518-6eedf0`, 1,013 samples, 18.96 ms/frame) named where
it lives:

| Symbol | Share | ms/frame |
| --- | ---: | ---: |
| `model2a::timeslice_impl<true,true,true,true,true>` | 11.65% | **2.21** |
| `[native JIT code]` (i960 rec + DSP AOT kernels) | 25.67% | 4.87 |
| `model2a::write_dword` | 3.26% | 0.62 |
| `mb86233_device::execute_run_impl` (both entries) | 5.23% | 0.99 |
| `model2a::timer_sync` | 2.17% | 0.41 |
| libc | 2.47% | 0.47 |
| `model2a::fifo_push` | 1.58% | 0.30 |
| `i960rec::runtime::execute` | 1.48% | 0.28 |
| `model2a::tgp_bus::io_r` | 1.28% | 0.24 |
| `model2a::dev_write` | 1.18% | 0.22 |
| `model2a::apply_suspend_changes` | 0.99% | 0.19 |
| `model2a::fifo_pop` / `run_frame` / `flags` | 0.89% each | 0.17 each |

Scheduler plus FIFO/timer glue totals **~4.07 ms/frame, 21.5% of the
task**. The same replay with `-DVF2_SCHED_DIAGNOSTICS` (job
`20261120-025102-b9b94c`) counted **6,136 CPU dispatches per frame**
(i960 2,079, TGP 2,165, Z80 1,892, 68K 1), 3,796 passes, 2,213 slices,
~3,040 timer firings (`L_I960_HALT` 1,322, `T_FOUT_EMPTY` 862,
`T_FIN_FULL` 381, `T_TGP_HALT` 303, `T_FIN_EMPTY` 151), 1,396 i960 and
812 TGP timeslice aborts, and 441,576 single-line against **0 batched**
line deliveries. `timeslice_impl` is 129 ns per visit or dispatch, none of
it executing a guest instruction; the i960 returns early on 99.6% of its
dispatches, averaging 209 executed cycles against a 97,608-cycle budget.

So the lever is per-event host overhead, not the DSP or the geometry path.
Two tempting targets are already spent, verified in the code and the
release command: with `VF2_MODULAR_CLOCK=1` the loop's `clock_type` is
already `uint64_t` (`clock_type=std::conditional_t<Modular,uint64_t,atime>`
in `timeslice_impl`, and the instantiation is
`<true,true,true,true,true>`), so a 128-bit fast path is a no-op; and
`m_line_queue` is `std::vector<int> m_line_queue[N_LINES]`
(`model2a.h:610`), not a `std::deque`. The 2.21 ms is event count, with
the remainder inside inlined event delivery that a 5 ms sampler cannot
split.

Ranked next steps from the independent review (ms saved x P(state)):

1. **Fuse the halt clear in `fifo_push`**, 0.1-0.2 ms, ~0.9. The fusion
   is already on the assert side (`assert_fifo_halt`, used by both
   `fifo_sync_empty` and `fifo_sync_full`), so the ~862 empty-episode
   asserts and ~381 full-stall asserts do not fire a timer; the unfused
   side is the unblock, where `fifo_push` calls `set_input_line(...,
   CLEAR_LINE)` unconditionally (`model2a.cpp:1801-1804`). That inserts a
   zero-delay timer which becomes the head and aborts the DSP timeslice
   about 862 times per frame; ~862 clears plus ~460 fallback asserts
   account for the 1,322 `L_I960_HALT` firings. Apply the queued line
   state and `resume()` inline when the one-shot would be the immediate
   head, keeping the `abort_timeslice()` so the emulated instant and the
   ordering are unchanged. Gate: core and geometry hashes with the
   reference timers both enabled and disabled, `fired[L_I960_HALT]`
   falling toward the assert count, then a paired short replay above
   0.08 ms.

   **Outcome (implemented and rejected, 2026-11-20):** the mechanism
   works - `L_I960_HALT` firings fall 1,321.7 -> 828.0 and `L_TGP_HALT`
   302.9 -> 143.7 per frame - but it is neither equivalent nor faster.
   Core state hash `2e7d925031640471` -> `1f72e5857eac3b97`, DSP
   instructions -2,127, DSP block runs -2,033, pixels identical, and
   +0.238 ms mean / +0.238 p95 / +0.280 ms simulation / +2 misses
   (jobs `20261120-034641-522bd7` control vs `20261120-034717-5323a9`
   fused, same binary). The timestamp is preserved but the *host* order
   is not: `timer_sync` records the change during the DSP write and only
   `execute_timers` applies it at `m_base >= expire`, so applying it
   inline sets `m_suspend_changes` one pass early and lets the i960 run
   at emulated times the reference forbids. Opt-in flag and source
   reverted. See `build/acm/fifo-clear-fusion/findings.md`.
2. **Timer queue as an indexed heap**, 0.15-0.25 ms, ~0.90. Gate:
   `VF2_SCHED_LOG` firing-sequence diff against the reference timers.
3. **Budget divide and visit micro-opts**, 0.10-0.15 ms, ~0.97. Gate:
   advanced/budget counters bit-equal.

Glue reduction alone clears the mean but only marginally reaches p95. The
guest is saturated: 2,079 i960 dispatches x 209 executed cycles = 434,552
cycles per frame against the 25 MHz x 17.384 ms = 434,600-cycle budget, so
p95 needs a global throughput gain in the i960/DSP kernels, which is also
what audio will need.

The clear-fusion outcome above also closes the "delete scheduler events"
direction in its cheap form: every one of those events marks a real
emulated instant at which a CPU becomes runnable, so removing one changes
when the i960 may execute. Any further scheduler proposal has to keep the
application point, which means the saving is confined to the bookkeeping
around it - and the bookkeeping is already only ~129 ns per visit.

See `build/acm/fifo-empty-price/findings.md`,
`build/acm/sched-glue-profile/findings.md` and
`build/acm/fifo-clear-fusion/findings.md`.

## The render thread is the critical path (2026-11-20)

The scheduler campaign measured the wrong thread. A free clock check
started the pass: all four cores run governor `performance` at 1.20 GHz
(cap 1.344), 46 C, so the slow-tick +5-6% inflation is neither DVFS nor
thermal.

Two cheap levers were then closed. The PGO retrain
(`build/acm/pgo-retrain`) rebuilt from profiles collected on the current
sources under the current flag set; it is exactly state-identical and
changes the interval by +0.041 ms mean / +0.045 p95, below the floor. The
copro function-port fast path (`build/acm/copro-port-fast`) is also
state-identical but slower by +0.13 and +0.36 ms in two counterbalanced
pairs, and `fifo_push` self time of 0.30 ms/frame bounds that whole path
anyway.

The useful experiment was `-fomit-frame-pointer
-fno-asynchronous-unwind-tables -fno-unwind-tables` on the retrained PGO
profile (`build/acm/omit-frame-pointer`). State is identical and the
simulation task gets **0.106 ms cheaper** (16.987 -> 16.881 mean, pairs
-0.168 and -0.044), but the swap interval moves **+0.020 ms** - flat.

That flat interval exposes the real bottleneck. Per frame the phase log
reads `CPU 0.27 render 16.42 sync 0.00 composite 0.57 swap 0.17` = 17.44
against a 17.484 interval, where `CPU` is the wait for the previous
simulation job. The simulation task is 16.918 ms and the mean wait is
0.454 ms. The main thread's render phase alone is 94% of the frame. The
pacing deadline is 26.6 ms behind at frame 2 and 90.1 ms behind at frame
267, so `sleep_until` never sleeps and the interval is pure work time,
not vsync or sleep jitter.

`--profile-render` on the main thread (job
`20261120-044653-e462ee`, 744 samples, 14.10 ms/frame) shows where it
goes: libMali 20.70% (2.92 ms), libc 20.43% (2.88), `prepare_batch_direct`
13.71% (1.93), `clip_packet_into` 9.54% (1.35), `tile_atlas::get` 5.91%
(0.83), `prepare_batch` 5.11% (0.72), `prepare_projected` 4.97% (0.70),
`retain_fan` 4.30% (0.61), `prepare_shared_projected` 4.30% (0.61).

So the next levers are the render thread's libc/memcpy and GL upload
paths, then the packet-preparation hot functions - not the scheduler. A
10% cut in that 14 ms is ~1.4 ms of interval, the size of the whole p95
gap; the simulation-side work can only recover the 0.45 ms wait. Keep
`-fomit-frame-pointer` on the shelf: it is free and state-identical, but
it cannot move the interval while the render thread is the limiter.

See `build/acm/render-profile/findings.md`,
`build/acm/omit-frame-pointer/findings.md` and
`build/acm/copro-port-fast/findings.md`.

### Addendum: the frame is GPU-queue-gated

`gpu_queue.drain()` is inside the render phase (`acm_main.cpp:567`), and
the production-cadence jobs report **3.5-4.0 ms/frame of fence wait**
(the queue holds one fence; the non-PGO builds at 18.8-19.3 ms report
0.05 ms). `VF2_SYNC_GPU` is dead under `VF2_GPU_QUEUE=1`, so the only
real GPU wait is the drain.

A depth-2 fence ring (`build/acm/gpu-queue-depth`, `build/acm/pgo-depth`,
with a profile recollected for the new header) removes the wait
(3.50 -> 0.06 ms) and improves the mean **-0.124 ms** while halving the
frames over budget (316 -> 143 of 478), but p95 rises **18.27 -> 20.01**
because queued frames stall 26-28 ms. Rejected on the p95 criterion.

The implication is sharp: removing 3.4 ms of fence wait leaves the mean
at 17.43 ms, so the frame is pinned by whatever takes ~17.4 ms in the
present path, and a deeper queue cannot exceed the sustained present
rate.

That pin is not established as GPU saturation. The Mali utilization
counters read 0% at every sample (`gpu-runtime.json`, GPU clock 384 MHz),
and a VBO instead of client arrays - `VF2_CLIENT_ARRAYS` is a runtime
flag - is much worse (+0.686 and +1.739 ms mean in two counterbalanced
pairs, state and pixels identical, `build/acm/vbo-submit`). So the
release's client-array choice is already the cheaper vertex path.

`--profile-copy-callers` (`build/acm/copy-callers`, job
`20261120-053353-a494ef`) resolves 30 of 45 memcpy callers inside
libMali, dominated by `_mali_convert_texture_common` (21) and
`_mali_surface_alloc_surface_instance` (6), with the app-side remainder
in `clip_packet_into` (5) and `prepare_batch` (5). The atlas-"1.6 MB per
frame" lead was a misreading: `uploaded` is cumulative and the real rate
is bytes per frame.

`--profile-render-stages` (job `20261120-053946-83b2e5`) gives the
target: packet/material preparation 6.71 ms wall / 6.28 CPU, prior GPU
drain 3.95 / 0.25, 3D submit 3.61 / 3.55, camera preparation 1.70, EGL
swap 0.75, picture capture 0.53, tile upload 0.59. Main-thread CPU is
14.24 ms against 16.9 ms of simulation, and the previous fence is pending
at the packet stage in 265 of 269 frames. The remaining ~1 ms has to come
from the packet/material stage or the 14.2 ms as a whole.

### The short replay is GPU-fenced; the full replay is pacing-jittered (2026-11-20)

The 269-frame tick-270 replay and the 900-frame full Dural replay from
`build/acm/fight.checkpoint` are different regimes:

| | Short (tick 270) | Full (fight start) |
| --- | ---: | ---: |
| Interval mean | 17.53 | **17.377** |
| Fence wait | 3.5-3.9 ms | **0.12 ms** |
| Sim task mean | 17.0 | 14.6 |
| `input/pace` sleep | ~0.01 ms | **5.86 ms** |

The full replay's mean is ~tick and the main thread has ~5.9 ms of slack
(`--profile-render-stages`, job `20261120-063153-56ee9d`: packet/material
5.81 / 5.39 CPU, 3D submit 2.53, camera 1.69, pace sleep 5.86). An earlier
reading of its tail ("424 late frames slept and still overshot") was an
**identity, not a cause** - independent review, 2026-11-20: deadlines
advance by exactly one tick, so `interval(n) - tick == slack(n-1) -
slack(n)` for every frame (residual 0.000000 ms). Pacing is
`sleep_until(next_tick)` after the swap (`src/host/acm_main.cpp:630-640`),
so a frame presents at `tick_n + work(n)` and the interval is
`tick_period + work(n) - work(n-1)`.

**Fix, qualified and adopted:** move the swap after the pacing sleep
(`VF2_PRESENT_AT_TICK`; harness `--present-at-tick`, default on for paced
full runs). The frame is already complete when the sleep starts, so
presents land on the tick grid and replace frame-work jitter with
swap-duration jitter. Present-offset jitter (frame-to-frame p95) falls
from 1.438 to 0.288 ms on Dural and from 0.803 to 0.158 ms on Stage 5.
Two counterbalanced pairs per scene:

| Scene | p95 base -> tick | p99 base -> tick |
| --- | ---: | ---: |
| Full Dural | 18.392 -> **17.539** | 19.308 -> 17.706 |
| Stage 5 | 17.881 -> **17.503** | 18.835 -> 17.548 |
| Waterfront | 17.930 -> **17.477** | 18.300 -> 17.600 |

Mean is unchanged (it is the tick by construction), state is identical in
every scene and pair, and Dural pixels are 100.0000% identical. The short
tick-270 replay, where the deadline is already in the past, is a no-op
(17.523 -> 17.520). Intervals over 17.384 rise slightly because presents
now sit on the grid with symmetric ~0.1-0.3 ms jitter; p95/p99 are the
metrics that matter for the target. Evidence:
`build/acm/tick-present/findings.md`.

**Follow-up measurement (same day):** the claim that the remaining
0.15-0.17 ms "is the swap call" was asserted, not measured. The trace now
records `swap_start_ns` and `sleep_end_ns` (`build/acm/present-times`,
plain `-O3`, 900-frame Dural, 4 arms):

| Arm | Metric | p95 | p99 |
| --- | --- | ---: | ---: |
| base | swap_end | 18.580 | 19.774 |
| base | sleep_end | 17.781 | 18.666 |
| grid | swap_end | 17.809 | 18.461 |
| grid | sleep_start->swap_start | 17.760 | 18.441 |
| grid + 1 ms spin | swap_start | 17.688 / 17.572 | 18.49 / 18.27 |

So in the grid arm the **sleep-end spacing itself** is 0.38-0.44 ms late at
p95 and removing the swap from the metric changes p95 by at most 0.07 ms:
the residual is `sleep_until` wake-up latency, not the swap call. Spinning
the last millisecond cuts p95 by ~0.15 ms but leaves p99 18.2-18.5 and max
22-28 ms, set by the ~2 of 870 frames whose work overruns the period.
`VF2_PACE_SPIN_US` and the extra trace columns are opt-in.

Then the PGO build carrying both was qualified on three scenes: with a
1 ms spin the **present-grid** spacing is p95 **17.385 ms** - the 17.384
tick plus one clock tick - on Dural, Stage 5 and Waterfront, mean 17.374,
6 of 6 counterbalanced pairs negative. The `swap_end` column still reports
17.45-17.51 because it is taken when `eglSwapBuffers` returns; the grid
column is the present timestamp. 1 ms spin is now the paced default.
Evidence: `build/acm/pgo-spin/findings.md`.

Audio was then measured on the same binary (`build/acm/pgo-audio`). With
the sound board running the game state is bit-identical (core
`59f2ee171fb93ed2`, geometry `a36957b440da7a79`), but the simulation has
a bimodal tail: 54 of 870 frames take 24-26 ms, starting at frame 446.
Muting the ALSA write, zeroing the SCSP mixer, holding the 68000 and
forcing the lazy aux path all leave that tail unchanged. It is the
scheduler fast path: `modular_frame_ready()` returns false whenever
`m_sound_on`, and disabling the modular path on the **sound-off** arm
reproduces the tail exactly (sim p95 24.29 vs 16.90, same 54 spikes).
`no-modular` sound-off and sound-on agree to ~0.05 ms, so the board's own
cost is ~0.05 ms, not the +0.85 first reported. Evidence:
`build/acm/pgo-audio/findings.md`.

That lever was then implemented: a modular-clock pass that runs the 68000
in the same loop (`timeslice_impl<false,true,true,...>`, opt-in
`VF2_MODULAR_SOUND`). The 54-spike tail disappears - Dural sound-on goes
back to 2 spikes of 870, Stage 5 and Waterfront to 0, with sim p95
17.16-17.90 and identical state and pixels. Next, the synchronous ALSA
write was exposed (+2.9 ms grid p95), so `audio::write()` now hands
buffers to a writer thread; grid p95 returns to 17.385-17.40. The
remaining issue is host output: the PCM drains only ~126 of 900 blocks
(`Audio: dropped 774`). Audio is therefore not yet incorporated; the next
step is the card-1 period/drain rate. Evidence:
`build/acm/pgo-audio2/findings.md`.

Shader work on the short replay: the homogeneous atlas modes are worth
**+1.002 / +1.072 ms** mean when disabled (counterbalanced, identical
pixels), so fragment complexity is a real lever. A fourth mode for
textured-but-maybe-transparent buckets
(`VF2_TEXTURED_TRANSPARENT_SHADER`, mode 0's textured path without the
`vInfo0.x` and checker branches) is pixel-exact and gives 4/4 pairs
mean -0.078 / -0.102 / -0.080 / -0.073 ms with misses 602 -> 467 of 954,
but the gain is on the 0.08 ms floor and the full replay does not
reproduce it (-0.004 / -0.014 ms mean, p95 +0.171 / +0.082). It stays
opt-in with the default off. Evidence:
`build/acm/tex-mode/findings.md`.
