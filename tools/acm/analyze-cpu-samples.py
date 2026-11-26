"""Resolve ARM sampling addresses against the matching unstripped build's nm output."""
import argparse,bisect,collections,pathlib
p=argparse.ArgumentParser()
p.add_argument('samples',type=pathlib.Path)
p.add_argument('--maps',type=pathlib.Path)
p.add_argument('--symbols',type=pathlib.Path,default=pathlib.Path('build/acm/vf2-symbols.txt'))
p.add_argument('--subtract',type=pathlib.Path)
p.add_argument('--module-symbols',action='append',default=[],metavar='MODULE=NM_FILE',help='Resolve a shared library using nm -D -S -nC output (exact symbol extents)')
p.add_argument('--jit',type=pathlib.Path,help='Per-process native-to-guest map (default: samples.jit)')
p.add_argument('--window-end',type=int,action='append',help='Limit samples to the specified report ending frame; repeat to combine windows')
a=p.parse_args()
def read(path):return {int(pc,16):int(n) for pc,n in (line.split() for line in path.read_text().splitlines())}
if a.window_end:
    if a.subtract:p.error('--subtract requires a full profile')
    counts=collections.Counter()
    matching=set()
    window_file=pathlib.Path(str(a.samples)+'.windows')
    for line in window_file.read_text().splitlines():
        frame,pc,n=line.split()
        if int(frame) in a.window_end:
            counts[int(pc,16)]+=int(n)
            matching.add(int(frame))
    if missing:=set(a.window_end)-matching:p.error(f'No samples for windows ending at {sorted(missing)}')
else:
    counts=read(a.samples)
if a.subtract:
    for pc,n in read(a.subtract).items():counts[pc]=max(0,counts.get(pc,0)-n)
maps=[]
for line in (a.maps or pathlib.Path(str(a.samples)+'.maps')).read_text().splitlines():
    fields=line.split(maxsplit=5);lo,hi=(int(x,16) for x in fields[0].split('-'))
    maps.append((lo,hi,int(fields[2],16),fields[1],fields[5] if len(fields)>5 else '[anonymous]'))
symbols=[]
for line in a.symbols.read_text().splitlines():
    fields=line.split(maxsplit=2)
    if len(fields)==3 and fields[1] in ('t','T','w','W'):
        try:symbols.append((int(fields[0],16),fields[2]))
        except ValueError:pass
symbols.sort();starts=[s[0] for s in symbols]
module_symbols={}
for item in a.module_symbols:
    module,filename=item.split('=',1);entries=[]
    for line in pathlib.Path(filename).read_text().splitlines():
        fields=line.split(maxsplit=3)
        if len(fields)==4 and fields[2] in ('t','T','w','W'):
            try:entries.append((int(fields[0],16)&~1,int(fields[1],16),fields[3]))
            except ValueError:pass
    entries.sort();module_symbols[module]=([s[0] for s in entries],entries)
jit_path=a.jit or pathlib.Path(str(a.samples)+'.jit')
jit=[]
if jit_path.exists():
    jit=sorted(tuple(int(x,16) for x in line.split()) for line in jit_path.read_text().splitlines())
    assert all(len(entry)==4 and entry[0]<entry[1] for entry in jit),'Invalid JIT ranges'
    assert all(left[1]<=right[0] for left,right in zip(jit,jit[1:])),'Overlapping JIT ranges'
jit_starts=[entry[0] for entry in jit];guest_pcs=collections.Counter()
totals=collections.Counter();modules=collections.Counter()
for pc,n in counts.items():
    if not n:continue
    name=f'[unmapped {pc:08x}]';module='unmapped'
    for lo,hi,offset,perms,path in maps:
        if lo<=pc<hi:
            module=path;name=path
            if pathlib.PurePosixPath(path).name in ('vf2-acm','daytona-acm'):
                address=pc-lo+offset;at=bisect.bisect_right(starts,address)-1
                if at>=0:name=symbols[at][1]
            elif path in module_symbols:
                address=pc-lo+offset;positions,entries=module_symbols[path]
                at=bisect.bisect_right(positions,address)-1
                if at>=0 and address<entries[at][0]+entries[at][1]:name=pathlib.PurePosixPath(path).name+'!'+entries[at][2]
                else:name=path+'+0x'+format(address,'x')
            elif 'x' in perms and (path=='[anonymous]' or '/dev/zero' in path):
                name=module='[native JIT code]'
                at=bisect.bisect_right(jit_starts,pc)-1
                if at>=0 and pc<jit[at][1]:
                    _,_,guest,entry=jit[at];name=f'[i960 function {entry:08x}]';guest_pcs[(entry,guest)]+=n
            break
    totals[name]+=n;modules[module]+=n
total=sum(totals.values())
thread=pathlib.Path(str(a.samples)+'.thread')
if a.window_end:print(f'{total} samples in windows ending at {a.window_end}, approximately {total/200:.2f} CPU seconds')
elif thread.exists():
    metadata=dict(line.split() for line in thread.read_text().splitlines())
    print(f'{total} samples from thread {metadata["tid"]}; measured {float(metadata["cpu_seconds"]):.3f} thread CPU seconds')
else:print(f'{total} samples, approximately {total/200:.2f} CPU seconds')
print('Modules:')
for name,n in modules.most_common():print(f'{100*n/max(total,1):6.2f}% {n:7d} {name}')
print('Functions:')
for name,n in totals.most_common(35):print(f'{100*n/max(total,1):6.2f}% {n:7d} {name}')
if jit:
    print('JIT guest instructions (excludes time inside C++ helpers):')
    for (entry,pc),n in guest_pcs.most_common(25):print(f'{100*n/max(total,1):6.2f}% {n:7d} function {entry:08x} PC {pc:08x}')
groups=collections.Counter()
for name,n in totals.items():
    if 'mb86233_device::' in name:group='TGP core and compiled microcode'
    elif name=='[native JIT code]' or name.startswith('[i960 function '):group='i960 compiled game code'
    elif 'model2_video::' in name or 'clip_polygon(' in name or 'geometry' in name:group='Geometry processing'
    elif 'model2a::' in name:group='Board, buses and scheduler'
    else:group='Other'
    groups[group]+=n
print('Subsystems:')
for name,n in groups.most_common():print(f'{100*n/max(total,1):6.2f}% {n:7d} {name}')
for suffix,title in [('tgp-commands','DSP command CPU samples (ff: dispatcher/initial partial)'),('tgp','DSP instruction CPU samples')]:
    path=pathlib.Path(str(a.samples)+'.'+suffix)
    if not a.window_end and path.exists():
        dsp=read(path);subtotal=sum(dsp.values());print(title+f': {subtotal} samples')
        for pc,n in sorted(dsp.items(),key=lambda row:row[1],reverse=True)[:25]:print(f'{100*n/max(total,1):6.2f}% total {100*n/max(subtotal,1):6.2f}% DSP {n:6d} {pc:04x}')
