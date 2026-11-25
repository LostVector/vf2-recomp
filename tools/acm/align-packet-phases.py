"""Align VF2_PACKET_PHASES records with a frame trace and attribute packet spikes.

The packet-phase stream has one record per frame that prepared packets, but the
stream can drift by a couple of records against the frame trace (a record is
pushed for frames the trace did not keep as 3D, and vice versa). The records are
`(capture->clipped, clipped->completed minus pack, pack)` in ms, so the third
column is the pack phase and the second is the serial planning/atlas remainder.

The alignment walks both streams in order, letting the record index run ahead by
a bounded amount, and picks the pair with the smallest gap while preferring to
stay in step. Usage:

  python tools/acm/align-packet-phases.py JOB [top]
"""
import csv, pathlib, re, sys

ITER = pathlib.Path('build/acm/hardware/iterations')


def pct(v, p):
    s = sorted(v)
    return s[min(len(s) - 1, int(round(p * (len(s) - 1))))] if s else float('nan')


def main():
    job = sys.argv[1] if len(sys.argv) > 1 else '20261121-143531-b5b9a2'
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 14
    d = ITER / job
    rows = list(csv.DictReader(open(d / 'frames.csv', newline='')))
    recs = []
    for line in (d / 'game.log').read_text(errors='replace').splitlines():
        m = re.match(r'Packet phases (\d+) ([\d.]+) ([\d.]+) ([\d.]+)', line)
        if m:
            recs.append((float(m.group(2)), float(m.group(3)), float(m.group(4))))
    frames = [r for r in rows if float(r['packets_ms']) > 0]

    # Monotone alignment: allow the record cursor to run ahead by up to 3 but
    # never backwards, and score each pairing by the gap against packets_ms.
    pairs = []
    r = 0
    for k, row in enumerate(frames):
        pkt = float(row['packets_ms'])
        best, best_r = None, None
        for ahead in range(0, 4):
            idx = r + ahead
            if idx >= len(recs):
                break
            gap = abs(sum(recs[idx]) - pkt)
            score = gap + 0.35 * ahead
            if best is None or score < best:
                best, best_r = score, idx
        if best_r is None:
            continue
        r = best_r + 1
        clip, rest, pack = recs[best_r]
        pairs.append({'frame': int(row['frame']), 'pkt': pkt, 'clip': clip,
                      'rest': rest, 'pack': pack, 'gap': abs(clip + rest + pack - pkt)})

    print('job %s: %d packet frames, %d phase records, %d aligned' %
          (job, len(frames), len(recs), len(pairs)))
    clip = [p['clip'] for p in pairs]
    rest = [p['rest'] for p in pairs]
    pack = [p['pack'] for p in pairs]
    print('%-6s %7s %7s %7s' % ('phase', 'p50', 'p99', 'max'))
    for name, v in (('clip', clip), ('rest', rest), ('pack', pack)):
        print('%-6s %7.2f %7.2f %7.2f' % (name, pct(v, .5), pct(v, .99), max(v)))
    print()
    print('worst planning (`rest`) frames:')
    print('  %6s %8s %7s %7s %7s %7s' % ('frame', 'packets', 'clip', 'rest', 'pack', 'gap'))
    for p in sorted(pairs, key=lambda p: -p['rest'])[:top]:
        print('  %6d %8.2f %7.2f %7.2f %7.2f %7.2f' % (
            p['frame'], p['pkt'], p['clip'], p['rest'], p['pack'], p['gap']))
    print()
    print('worst pack frames:')
    for p in sorted(pairs, key=lambda p: -p['pack'])[:6]:
        print('  %6d %8.2f %7.2f %7.2f %7.2f %7.2f' % (
            p['frame'], p['pkt'], p['clip'], p['rest'], p['pack'], p['gap']))


if __name__ == '__main__':
    main()
