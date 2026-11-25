"""Segment the full attract loop into 3D scenes and report cadence per scene.

A frame counts as 3D when the packet/geometry path is active for it
(`packets_ms > 1.0` or `gpu_3d_ms > 0.05`). Contiguous runs separated by the
2D screens and title cards become scenes. For every scene the report gives the
frame count, work percentiles, how many submissions landed after their
scheduled arcade tick, how many whole ticks were skipped, and the worst frames.

Usage: python analyze-attract.py [job]
"""
import csv, pathlib, sys

ITER = pathlib.Path('build/acm/hardware/iterations')
TICK_NS = 17384000
STAGES = ['sim_ms', 'capture_ms', 'camera_ms', 'packets_ms', 'prior_gpu_ms',
          'tile_upload_ms', 'submit_ms', 'gpu_3d_ms', 'composite_ms',
          'gpu_composite_ms', 'flush_ms', 'swap_ms']


def pct(v, p):
    s = sorted(v)
    return s[min(len(s) - 1, int(round(p * (len(s) - 1))))] if s else float('nan')


def load(job):
    d = ITER / job
    rows = list(csv.DictReader(open(d / 'frames.csv', newline='')))
    out = {}
    for line in (d / 'game.log').read_text(errors='replace').splitlines():
        if line.startswith('Detached sound:'):
            parts = [p.strip() for p in line.split(':', 1)[1].split(',')]
            out['underruns'] = next((p.split()[0] for p in parts if 'underrun' in p), '?')
            out['dropped'] = next((p.split()[0] for p in parts if 'dropped' in p), '?')
    return rows, out


def work(r):
    return sum(float(r[c]) for c in STAGES if c in r)


def segments(rows):
    act = [float(r['packets_ms']) > 1.0 or float(r['gpu_3d_ms']) > 0.05 for r in rows]
    segs, i = [], 0
    while i < len(rows):
        if act[i]:
            j = i
            gap = 0
            while j < len(rows):
                if act[j]:
                    gap = 0
                else:
                    gap += 1
                    if gap > 12: break
                j += 1
            segs.append((i, j - gap))
            i = j
        else:
            i += 1
    return [(a, b) for a, b in segs if b - a >= 20]


def main():
    job = sys.argv[1] if len(sys.argv) > 1 else '20261121-134251-a61878'
    rows, audio = load(job)
    segs = segments(rows)
    print('job %s  frames %d  scenes %d  underruns %s  dropped %s' %
          (job, len(rows), len(segs), audio.get('underruns'), audio.get('dropped')))
    print('%-13s %6s %7s %7s %7s %6s %6s %6s %5s %5s' % (
        'scene frames', 'n', 'work50', 'work99', 'worst', 'early%', 'late',
        '>tick', 'skip', 'resync'))
    total = {'n': 0, 'late': 0, 'over': 0, 'skip': 0}
    for n, (a, b) in enumerate(segs, 1):
        body = rows[a:b]
        w = [work(r) for r in body]
        late = [(int(r['swap_end_ns']) - int(r['pace_deadline_ns'])) / 1000.0 for r in body]
        early = sum(1 for v in late if v <= 0)
        over = sum(1 for v in late if v > TICK_NS / 1000.0)
        # whole ticks skipped: a deadline jump larger than one tick
        skip = 0
        for x, y in zip(body, body[1:]):
            d = int(y['pace_deadline_ns']) - int(x['pace_deadline_ns'])
            if d > TICK_NS:
                skip += int(round(d / TICK_NS)) - 1
        res = sum(1 for r in body if r['pace_resynced'] == '1')
        total['n'] += len(body); total['late'] += len(body) - early
        total['over'] += over; total['skip'] += skip
        print('%5d-%5d %6d %7.2f %7.2f %7.2f %5.1f%% %6d %6d %5d %5d' % (
            a + 1, b, len(body), pct(w, .5), pct(w, .99), max(w),
            100.0 * early / len(body), len(body) - early, over, skip, res))
    print('\ntotal 3D frames %d  late %d (%.2f%%)  over one tick %d  skipped ticks %d' % (
        total['n'], total['late'], 100.0 * total['late'] / total['n'], total['over'], total['skip']))
    # Skipped ticks over the whole trace: a deadline jump larger than one tick is
    # a whole number of ticks that never got a frame. Count across segment
    # boundaries too, which the per-scene figure cannot see.
    whole_skip = 0
    for x, y in zip(rows, rows[1:]):
        d = int(y['pace_deadline_ns']) - int(x['pace_deadline_ns'])
        if d > TICK_NS:
            whole_skip += int(round(d / TICK_NS)) - 1
    resyncs = sum(1 for r in rows if r['pace_resynced'] == '1')
    print('whole trace: skipped ticks %d, resyncs %d, 2D-only frames %d' % (
        whole_skip, resyncs, len(rows) - total['n']))

    worst = sorted(((a + 1 + i, r) for a, b in segs for i, r in enumerate(rows[a:b])),
                   key=lambda p: -((int(p[1]['swap_end_ns']) - int(p[1]['pace_deadline_ns'])) / 1e6))[:12]
    print('\nworst submissions (us past the tick, plus the stage breakdown in ms):')
    print('  %6s %10s %8s %7s %7s %7s %7s %7s %7s %7s' % (
        'frame', 'late_us', 'sim', 'packets', 'submit', 'composite', 'capture', 'camera', 'flush', 'work'))
    for f, r in worst:
        print('  %6d %10.1f %8.2f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f' % (
            f, (int(r['swap_end_ns']) - int(r['pace_deadline_ns'])) / 1000.0,
            float(r['sim_ms']), float(r['packets_ms']), float(r['submit_ms']),
            float(r['composite_ms']), float(r['capture_ms']), float(r['camera_ms']),
            float(r['flush_ms']), work(r)))


if __name__ == '__main__':
    main()
