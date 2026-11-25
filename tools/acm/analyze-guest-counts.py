"""Summarize bounded guest traces; counts describe execution, not CPU time."""
import argparse,collections,pathlib
p=argparse.ArgumentParser();p.add_argument('directory',type=pathlib.Path);a=p.parse_args()
totals=collections.Counter();functions=collections.Counter();helpers=collections.Counter();pcs=[]
for line in (a.directory/'guest.i960').read_text().splitlines():
    if line.startswith('#'):continue
    pc,n,interp,inline,op,owner=line.split();n=int(n);interp=int(interp)
    totals['inline-capable' if int(inline) else 'helper-only']+=n;totals['interpreter']+=interp
    functions[owner]+=n+interp
    if not int(inline):helpers[op[:2]]+=n
    pcs.append((n+interp,pc,op,inline,owner))
print('i960 instruction attempts:',dict(totals))
for title,counts in [('Functions',functions),('Helper opcode majors',helpers)]:
    print(title)
    for name,n in counts.most_common(20):print(f'  {name}: {n:>10,}')
print('i960 PCs: attempts, PC, opcode, inline, function')
for row in sorted(pcs,reverse=True)[:30]:print(' ',*row)
tgp=[]
for line in (a.directory/'guest.tgp').read_text().splitlines():
    if line.startswith('#'):continue
    fields=line.split();pc,n,op=fields[:3];repeat=int(fields[3]) if len(fields)>3 else 0
    tgp.append((int(n),pc,op,repeat))
print('TGP instruction attempts:',sum(row[0] for row in tgp))
print('TGP consecutive same-PC attempts:',sum(row[3] for row in tgp))
print('TGP PCs: attempts, PC, opcode, consecutive same PC')
for row in sorted(tgp,reverse=True)[:40]:print(' ',*row)
commands=a.directory/'guest.commands'
if commands.exists():
    rows=[line.split() for line in commands.read_text().splitlines() if not line.startswith('#')]
    total=sum(int(row[3]) for row in rows)
    print('TGP commands: attempt share, command, entry, calls, attempts, consecutive same PC')
    for command,entry,calls,attempts,repeats in sorted(rows,key=lambda row:int(row[3]),reverse=True):
        print(f' {int(attempts)*100/max(total,1):6.2f}% {command} {entry} {int(calls):9,d} {int(attempts):11,d} {int(repeats):11,d}')
