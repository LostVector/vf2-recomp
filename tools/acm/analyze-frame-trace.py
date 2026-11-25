"""Report submission cadence and pacing deadlines from a Mini frame trace."""
import argparse
import csv
import json
import math
from pathlib import Path
from statistics import mean

TICK_NS = 17_384_000


def distribution(values):
    ordered = sorted(values)
    if not ordered:
        return {}
    return {
        key: round(ordered[math.ceil(p * len(ordered)) - 1] / 1e6, 3)
        for key, p in (("p50_ms", .5), ("p95_ms", .95), ("p99_ms", .99), ("max_ms", 1))
    }


def summarize(rows):
    swaps = [row["swap_end_ns"] for row in rows if row["swap_end_ns"]]
    intervals = [end - start for start, end in zip(swaps, swaps[1:])]
    lateness = [
        row["swap_end_ns"] - row["pace_deadline_ns"]
        for row in rows if row["swap_end_ns"] and row["pace_deadline_ns"]
    ]
    return {
        "first_frame": rows[0]["frame"],
        "last_frame": rows[-1]["frame"],
        "frames": len(rows),
        "swap_intervals": {
            "count": len(intervals),
            "over_tick": sum(interval > TICK_NS for interval in intervals),
            **distribution(intervals),
        },
        "sim_task": distribution([row["sim_task_ns"] for row in rows]),
        "sim_wait": distribution([row["sim_wait_ns"] for row in rows]),
        "paced_deadlines": {
            "count": len(lateness),
            "missed": sum(late > 0 for late in lateness),
            "over_one_tick_late": sum(late > TICK_NS for late in lateness),
            "clock_rebases": sum(row["pace_resynced"] for row in rows),
            **distribution(lateness),
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--warmup", type=int, default=120, help="Exclude startup ticks from the steady-state view")
    args = parser.parse_args()
    with args.trace.open(newline="") as stream:
        rows = [{key: int(value) for key, value in row.items()} for row in csv.DictReader(stream)]
    if not rows or [row["frame"] for row in rows] != list(range(1, len(rows) + 1)):
        parser.error("Expected sequential frame numbers from the beginning of the run")
    if args.warmup < 0:
        parser.error("Warmup must be nonnegative")
    # The benchmark reads the framebuffer before swapping its final frame.
    steady = rows[args.warmup:-1]
    windows = [rows[start:start + 60] for start in range(0, len(rows) - 60, 60)]
    slowest = sorted(windows, key=lambda window: mean(row["sim_wait_ns"] for row in window), reverse=True)
    print(json.dumps({
        "tick_ms": TICK_NS / 1e6,
        "full_run": summarize(rows),
        "excluding_warmup_and_final_capture": summarize(steady) if steady else None,
        "slowest_sim_wait_windows": [
            {"first": window[0]["frame"], "last": window[-1]["frame"],
             "mean_wait_ms": round(mean(row["sim_wait_ns"] for row in window) / 1e6, 3)}
            for window in slowest[:5]
        ],
    }, indent=2))


if __name__ == "__main__":
    main()
