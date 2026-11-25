"""Resolve sampled native ARM instructions to source lines using a matching ELF.

Use --addr2line "docker exec -i acm-vf2-armv7 arm-linux-gnueabihf-addr2line"
on the host. The ELF path must also be accessible to that command.
Only the executable mapping is resolved; library samples remain in the total.
"""
import argparse
import collections
import json
import pathlib
import re
import shlex
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('samples', type=pathlib.Path)
p.add_argument('--elf', type=pathlib.Path, required=True)
p.add_argument('--maps', type=pathlib.Path)
p.add_argument('--addr2line', default='arm-linux-gnueabihf-addr2line',
               help='Tool command/prefix; split into arguments without a shell')
p.add_argument('--function', help='Substring to select in the resolved inline call stack')
p.add_argument('--project-root', help='Attribute inline library helpers to their nearest source frame under this prefix')
p.add_argument('--top', type=int, default=40)
p.add_argument('--json', type=pathlib.Path)
a = p.parse_args()

counts = collections.Counter()
for pc, n in (line.split() for line in a.samples.read_text().splitlines()):
    counts[int(pc, 16) & ~1] += int(n)
maps = []
for line in (a.maps or pathlib.Path(str(a.samples) + '.maps')).read_text().splitlines():
    fields = line.split(maxsplit=5)
    if len(fields) < 6 or not fields[5].endswith('/vf2-acm'):
        continue
    lo, hi = (int(x, 16) for x in fields[0].split('-'))
    maps.append((lo, hi, int(fields[2], 16)))
addresses = collections.Counter()
for pc, n in counts.items():
    for lo, hi, offset in maps:
        if lo <= pc < hi:
            addresses[pc - lo + offset] += n
            break
if not addresses:
    raise SystemExit('No executable samples found in these mappings')

result = subprocess.run([*shlex.split(a.addr2line), '-a', '-f', '-C', '-i', '-e', a.elf.as_posix()],
                        input=''.join(f'{pc:x}\n' for pc in addresses),
                        text=True, capture_output=True, check=True)
resolved = {}
address = None
for line in result.stdout.splitlines():
    if re.fullmatch(r'0x[0-9a-fA-F]+', line):
        address = int(line, 16)
        resolved[address] = []
    elif address is not None:
        resolved[address].append(line)
if set(resolved) != set(addresses):
    raise SystemExit('addr2line returned incomplete address records')

lines = collections.Counter()
unknown = selected = 0
for pc, n in addresses.items():
    stack = resolved[pc]
    if len(stack) < 2 or len(stack) % 2:
        raise SystemExit(f'Malformed source stack for {pc:x}')
    located = [i for i in range(0, len(stack), 2)
               if re.search(r':[1-9][0-9]*(?:\s|$)', stack[i + 1])]
    if not located:
        unknown += n
        continue
    if a.function and not any(a.function in fn for fn in stack[::2]):
        continue
    selected += n
    # By default show the actual instruction, including inlined helpers.
    # Project attribution instead exposes which container operation called it.
    frame = located[0]
    if a.project_root:
        frame = next((i for i in located
                      if stack[i + 1].startswith(a.project_root)), frame)
    lines[(stack[frame + 1], stack[frame])] += n

total = sum(counts.values())
report = {'total_cpu_samples': total, 'executable_samples': sum(addresses.values()),
          'unresolved_executable_samples': unknown, 'selected_samples': selected,
          'elf': str(a.elf), 'function_filter': a.function, 'project_root': a.project_root,
          'lines': [{'source': source, 'function': fn, 'samples': n,
                     'percent_total_cpu': 100 * n / max(1, total)}
                    for (source, fn), n in lines.most_common()]}
if not selected:
    raise SystemExit('No source lines matched; check ELF debug info and filter')
print(f'{total} total CPU samples; {sum(addresses.values())} executable; '
      f'{unknown} executable samples without source lines; {selected} selected')
for row in report['lines'][:a.top]:
    print(f'{row["percent_total_cpu"]:6.2f}% {row["samples"]:6d} '
          f'{row["source"]} | {row["function"]}')
if a.json:
    a.json.write_text(json.dumps(report, indent=2) + '\n')
