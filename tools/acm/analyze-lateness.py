"""Arcade-cadence lateness analysis for a frame trace.

Per steady-state frame the trace gives one absolute timeline (ns from `began`):

  pace_deadline_ns   the scheduled arcade tick
  sleep_end_ns       when the pacing wait returned (wake)
  swap_start_ns      just before the present
  swap_end_ns        when the present returned (submission complete)
  sim_task_ns        duration of the pipelined simulation task
  sim_wait_ns        when that task finished, from frame start
  *_ms               wall time of each render-chain stage (--profile-render-stages)

Derived quantities:

  enter_us   when the render chain finished and entered the pacing wait, from
             swap_end(n-1) + pace(n-1) + sum(stages before pacing). Positive
             means the work was still running when the tick arrived.
  wake_us    wake - deadline: scheduler/spin overshoot past the tick.
  start_us   swap_start - deadline: when the present began, relative to the tick.
  submit_us  swap_end - deadline: when the present completed.
  present_us submit_us - start_us: the cost of the present itself.

The pipeline is: work -> [enter] -> sleep/spin -> [wake] -> present -> [submit].
So if enter_us is already positive, the delay is work before the wait; if enter
is negative but wake is positive, it is the scheduler; if submit - start is
large, it is the present.

A frame keeps its arcade slot if it begins presenting before the next tick, so
`slip` counts frames whose start_us exceeds one tick period.

Usage: python analyze-lateness.py [--skip N] [--tail N] job [job...]
"""
import argparse, csv, pathlib

ITER = pathlib.Path('build/acm/hardware/iterations')
TICK_NS = 17384000
BEFORE_WAIT = ('sim_ms', 'capture_ms', 'camera_ms', 'packets_ms', 'prior_gpu_ms',
               'tile_upload_ms', 'submit_ms', 'gpu_3d_ms', 'composite_ms',
               'gpu_composite_ms', 'flush_ms')


def pct(v, p):
    s = sorted(v)
    return s[min(len(s) - 1, int(round(p * (len(s) - 1))))] if s else float('nan')


def analyze(job, skip=30, tail=2):
    d = ITER / job
    log = (d / 'game.log').read_text(errors='replace')
    rows = list(csv.DictReader(open(d / 'frames.csv', newline='')))
    staged = 'packets_ms' in rows[0]
    out = {'job': job, 'staged': staged}
    for line in log.splitlines():
        if line.startswith('Detached sound:'):
            parts = [p.strip() for p in line.split(':', 1)[1].split(',')]
            out['underruns'] = next((p.split()[0] for p in parts if 'underrun' in p), '?')
            out['dropped'] = next((p.split()[0] for p in parts if 'dropped' in p), '?')
        if line.startswith('State signature:'):
            out['state'] = line.split(':', 1)[1].strip()

    body = rows[skip:len(rows) - tail if tail else len(rows)]
    if staged:
        body = [r for r in body if float(r['capture_ms']) < 0.5]

    prev = None
    frames = []
    for r in body:
        w = sum(float(r[c]) for c in BEFORE_WAIT) if staged else None
        deadline = int(r['pace_deadline_ns'])
        wake = int(r['sleep_end_ns'])
        start = int(r['swap_start_ns'])
        submit = int(r['swap_end_ns'])
        pace_ns = float(r['pace_ms']) * 1e6 if staged else 0.0
        row = {'frame': int(r['frame']), 'work_ms': w,
               'sim_task_ms': int(r['sim_task_ns']) / 1e6,
               'sim_join_ms': int(r['sim_wait_ns']) / 1e6,
               'wake_us': (wake - deadline) / 1000.0,
               'start_us': (start - deadline) / 1000.0,
               'submit_us': (submit - deadline) / 1000.0,
               'present_us': (submit - start) / 1000.0}
        if prev is not None and w is not None:
            enter = prev['swap_end'] + pace_ns + w * 1e6
            row['enter_us'] = (enter - deadline) / 1000.0
        frames.append(row)
        prev = {'swap_end': submit}

    out['n'] = len(frames)
    out['work_p50'] = pct([f['work_ms'] for f in frames if f['work_ms']], .50)
    out['work_p99'] = pct([f['work_ms'] for f in frames if f['work_ms']], .99)
    out['sim_p99'] = pct([f['sim_task_ms'] for f in frames], .99)
    out['join_p99'] = pct([f['sim_join_ms'] for f in frames], .99)
    out['enter_p99'] = pct([f['enter_us'] for f in frames if 'enter_us' in f], .99)
    out['wake_p99'] = pct([f['wake_us'] for f in frames], .99)
    out['start_p99'] = pct([f['start_us'] for f in frames], .99)
    out['submit_p99'] = pct([f['submit_us'] for f in frames], .99)
    out['present_p99'] = pct([f['present_us'] for f in frames], .99)
    out['slip'] = sum(1 for f in frames if f['start_us'] > TICK_NS / 1000.0)
    tick_us = TICK_NS / 1000.0
    out['on_tick_0'] = sum(1 for f in frames if f['start_us'] <= 0.0)
    out['on_tick_1ms'] = sum(1 for f in frames if f['start_us'] <= 1000.0)
    out['on_tick_half'] = sum(1 for f in frames if f['start_us'] <= tick_us / 2)
    out['on_tick_slot'] = sum(1 for f in frames if f['start_us'] <= tick_us)
    out['frames'] = frames
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('jobs', nargs='+')
    ap.add_argument('--skip', type=int, default=30)
    ap.add_argument('--tail', type=int, default=2)
    ap.add_argument('--worst', type=int, default=5)
    a = ap.parse_args()
    print('p99 values; times in us vs the scheduled tick, work/sim/join in ms')
    print('on-tick = share of frames whose present began within 0us / 1ms / half a tick / the tick of its deadline')
    print('%-8s %5s %7s %7s %7s %7s %8s %8s %8s %8s %8s %5s %5s %4s' % (
        'job', 'n', 'work50', 'work99', 'sim99', 'join99', 'enter99', 'wake99',
        'start99', 'submit99', 'pres99', 'slip', 'und', 'drop'))
    results = [analyze(j, a.skip, a.tail) for j in a.jobs]
    for r in results:
        print('%-8s %5d %7.3f %7.3f %7.3f %7.3f %8.1f %8.1f %8.1f %8.1f %8.1f %5d %5s %4s' % (
            r['job'][-6:], r['n'], r['work_p50'], r['work_p99'], r['sim_p99'],
            r['join_p99'], r['enter_p99'], r['wake_p99'], r['start_p99'],
            r['submit_p99'], r['present_p99'], r['slip'], r['underruns'], r['dropped']))
    print('%-8s %5s %7s %7s %7s %7s' % ('job','n','tick0%','1ms%','half%','slot%'))
    for r in results:
        n=float(r['n'])
        print('%-8s %5d %6.1f%% %6.1f%% %6.1f%% %6.1f%%' % (
            r['job'][-6:], r['n'], 100*r['on_tick_0']/n, 100*r['on_tick_1ms']/n,
            100*r['on_tick_half']/n, 100*r['on_tick_slot']/n))
    for r in results:
        if not r['staged']:
            print('\n%s: no stage columns, rerun with --profile-render-stages' % r['job'])
            continue
        worst = sorted(r['frames'], key=lambda f: -f['start_us'])[:a.worst]
        print('\nworst frames of %s: start late by us' % r['job'][-6:])
        print('  %5s %8s %10s %9s %9s %9s %9s %9s' % (
            'frame', 'work_ms', 'enter_us', 'wake_us', 'start_us', 'submit_us', 'present', 'sim_ms'))
        for f in worst:
            print('  %5d %8.3f %10.1f %9.1f %9.1f %9.1f %9.1f %9.3f' % (
                f['frame'], f['work_ms'], f.get('enter_us', float('nan')), f['wake_us'],
                f['start_us'], f['submit_us'], f['present_us'], f['sim_task_ms']))


if __name__ == '__main__':
    main()
