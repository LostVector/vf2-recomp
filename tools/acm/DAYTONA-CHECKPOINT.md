# Daytona USA Mini checkpoint

Daytona USA Revision A runs natively on the Astro City Mini with
full rendering and quiet speaker audio. The cadence target is
delivered p99 at or below 17.384 ms and at least 99% on-time fresh
frames in verified in-race replays on all three courses.

| Course | Recent delivered p50 | Delivered p99 | On-time fresh |
| --- | ---: | ---: | ---: |
| Beginner | 18.930 ms | 26.089 ms | 14 / 419 |
| Intermediate | 17.380 ms | 18.909 ms | 705 / 749 |
| Expert | 17.383 ms | 17.541 ms | 656 / 659 |

These are full-render replay measurements at 1.344 GHz from
separate comparisons. Expert passes the freshness gate. All
three still miss the p99 target. Gameplay timing excludes
startup, loading and capture. Verify RGB and audio in separate
proof runs.

Beginner's heaviest 80 ticks show the banked left-hand turn
on the opening lap in chase view. Simulation alone takes
roughly 19–20 ms of CPU time per tick and overlaps camera,
clipping, packet preparation and GPU work. Preserve the
existing course assets and focus on reusable reductions in
the packet pipeline, GPU work, and board execution.

The Expert static compositor quad is an opt-in change with
matched moving captures and exact PCM. More aggressive
presentation shader and state-reuse trials had mixed or
worse delivered tails. Keep the experimental options out
of the interactive baseline.

Intermediate's isolated six-pixel variation follows the
initial values of four atlas rows adjacent to one fully
uniform 2x2 tile. Re-uploading the whole backing retains the
effect. A diagnostic mapping to the tile's center produces
the expected color in both seed cases. The diagnostic is
specific to the witness tile. A production candidate needs
content-based selection, revision-safe invalidation and
moving-capture qualification on the affected courses.

The interactive release was restored and verified at stock
1.2 GHz caps, speaker lineout 20 and clean staging. The
ROM, device libraries, checkpoints, trial binaries, captures
and detailed experiment logs stay on the PC. The ARM helper
builds with locally supplied dependencies and Mini libraries.
The `daytona-acm` and `daytona-tgp-dump` targets were verified
from a clean source archive with those dependencies supplied.

See [Daytona build and experiment notes](DAYTONA.md) for
the existing build workflow and additional findings.
