from pathlib import Path
import sys,zlib,json,hashlib,shlex
BASE=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(Path(__file__).resolve().parent))
from usb_client import Client
b=BASE/'build/acm/pgo';c=Client()
def execute(command):
    status,data=c.execute(command)
    if status:raise RuntimeError(data.decode(errors='replace'))
    return data.decode(errors='replace')
try:
    data=b/'data';data.mkdir(exist_ok=True);files=[]
    for name in execute('ls /run/vf2/pgo').splitlines():
        assert name.endswith('.gcda') and '/' not in name and chr(92) not in name
        remote='/run/vf2/pgo/'+name;target=data/name;c.download(remote,target)
        crc=f'{zlib.crc32(target.read_bytes()):08x}'
        assert crc in execute('/bin/crc32 '+shlex.quote(remote)).lower()
        files.append(target)
    assert files,'No GCC profiles'
    print('Downloaded and CRC verified',len(files),'GCC profiles;',sum(p.stat().st_size for p in files),'bytes')
    (b/'training.json').write_text(json.dumps({'job':json.loads((BASE/'build/acm/hardware/iteration.json').read_text()),'files':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in files}},indent=2))
finally:c.close()
