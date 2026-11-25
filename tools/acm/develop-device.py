"""Boot, deploy to RAM, probe Mali and benchmark. Does not install firmware."""
import pathlib,sys,subprocess,time,json,re,shlex,uuid
from usb_client import Client
BASE=pathlib.Path(__file__).resolve().parents[2]
OUT=BASE/'build/acm/hardware';OUT.mkdir(parents=True,exist_ok=True)
RUN_TAG=uuid.uuid4().hex[:12]
def say(message): print(time.strftime('%H:%M:%S'),message,flush=True)
def execute(c,command):
    status,data=c.execute(command)
    text=data.decode(errors='replace')
    if status: raise RuntimeError(f'Device command failed ({status}): {command}\n{text}')
    return text
def upload(c,local,remote,executable=False):
    local=pathlib.Path(local);size,crc=c.upload(local,remote)
    expected=f'{crc:08x}'
    actual=execute(c,'/bin/crc32 '+shlex.quote(remote)).lower()
    if expected not in actual: raise RuntimeError(f'CRC mismatch uploading {local}: {actual!r}')
    if executable:c.chmod(remote)
    say(f'Uploaded and CRC-verified {local.name} ({size} bytes)')
def process(c,pid):
    raw=execute(c,f'if test -r /proc/{pid}/stat; then cat /proc/{pid}/stat; else echo MISSING; fi').strip()
    if raw=='MISSING':return None
    fields=raw.rsplit(') ',1)[1].split()
    return {'state':fields[0],'start_ticks':int(fields[19])}
def launch(c,name,command):
    log=f'/tmp/{name}-{RUN_TAG}.log';end=f'/tmp/{name}-{RUN_TAG}.exit'
    line=f'( {command} > {log} 2>&1; echo $? > {end} ) < /dev/null > /dev/null 2>&1 & echo $!'
    pid=int(execute(c,line).strip())
    say(f'Started {name}, device PID {pid}')
    live=process(c,pid)
    record={'pid':pid,'command':command,'log':log,'exit_file':end,'session':RUN_TAG,'process':live}
    (OUT/(name+'.job.json')).write_text(json.dumps(record,indent=2))
    return record
def job(c,name,command,timeout=600):
    record=launch(c,name,command);pid=record['pid'];log=record['log'];end=record['exit_file']
    began=time.monotonic();last_report=began;last_log=''
    while True:
        state=execute(c,f'if test -f {end}; then cat {end}; else echo PENDING; fi').strip()
        live=process(c,pid)
        if live and record['process'] and live['start_ticks']!=record['process']['start_ticks']:
            raise RuntimeError(f'{name} PID was reused; refusing to report the new process as this job')
        terminal=live is None or live['state'] in ('Z','X')
        if state!='PENDING' and terminal:
            c.download(log,OUT/(name+'.log'))
            say(f'{name} exited {state}')
            print((OUT/(name+'.log')).read_text(errors='replace')[-6000:],flush=True)
            if state!='0':raise RuntimeError(f'{name} failed with exit status {state}; see saved log')
            return
        if terminal and state=='PENDING':raise RuntimeError(f'{name} PID disappeared without exit record')
        if time.monotonic()-began>timeout:raise RuntimeError(f'{name} is still running as PID {pid}; observation deadline reached, not restarted')
        if time.monotonic()-last_report>30:
            say(f'{name} PID {pid} remains live ({int(time.monotonic()-began)} seconds)')
            current=execute(c,'cat '+shlex.quote(log))
            (OUT/(name+'.live.log')).write_text(current)
            if current!=last_log:print(current[len(last_log):][-1800:],flush=True);last_log=current
            last_report=time.monotonic()
        time.sleep(3)

if '--boot' in sys.argv:
    subprocess.run([sys.executable,'-u',str(BASE/'tools/acm/boot-device.py'),'--wait'],check=True)
    time.sleep(7)
say('Waiting for RAM USB RPC interface')
deadline=time.monotonic()+90;c=None
while time.monotonic()<deadline:
    try:
        c=Client(timeout=3000)
        if 'ACM_RPC_READY' in execute(c,'echo ACM_RPC_READY'):break
        c.close();c=None
    except Exception as e:
        if c:
            try:c.close()
            except Exception:pass
        c=None;time.sleep(2)
else:raise RuntimeError('RAM USB RPC interface did not become ready')
try:
    say('RAM USB RPC connected')
    execute(c,'mkdir -p /run/vf2/lib /tmp/vf2')
    info=execute(c,'cat /proc/cpuinfo; cat /proc/meminfo; cat /proc/asound/cards; cat /proc/mounts')
    (OUT/'device-before.txt').write_text(info)
    for name in ('vf2run','vf2-acm','load-module'):
        if name=='vf2run' and '--skip-cpu' in sys.argv:continue
        upload(c,BASE/'build/acm/bin'/name,'/run/vf2/'+name,True)
    upload(c,BASE/'roms/vf2.zip','/run/vf2/vf2.zip')
    for lib in sorted((BASE/'build/acm/runtime').iterdir()):upload(c,lib,'/run/vf2/lib/'+lib.name,True)
    upload(c,BASE/'tools/acm/run-on-device.sh','/run/vf2/run-game',True)
    upload(c,BASE/'tools/acm/asound.conf','/run/vf2/asound.conf')
    prefix='/run/vf2/lib/ld-linux-armhf.so.3 --library-path /run/vf2/lib:/stock/usr/lib:/stock/lib '
    module='/stock/lib/modules/3.4.113/kernel/drivers/sunxi-mali400-km/mali.ko'
    if '--skip-cpu' not in sys.argv:
        job(c,'vf2-cpu', '/run/vf2/vf2run /run/vf2/vf2.zip 1200 --sound --render-all --out /tmp/vf2',600)
    execute(c,'if test -f /run/vf2/vf2run; then rm /run/vf2/vf2run; fi')
    job(c,'mali-probe',f'(if test -d /sys/module/mali; then true; else /run/vf2/load-module {module}; fi) && {prefix}/run/vf2/vf2-acm --probe',120)
    job(c,'vf2-gles2','/run/vf2/run-game --release-rom --frames 1200 --uncapped --mute --capture /tmp/vf2-gles2.png',600)
    c.download('/tmp/vf2-gles2.png',OUT/'vf2-gles2.png')
    info=execute(c,'cat /proc/meminfo; cat /proc/modules; dmesg')
    (OUT/'device-after.txt').write_text(info)
    if '--no-launch' not in sys.argv:
        upload(c,BASE/'roms/vf2.zip','/run/vf2/vf2.zip')
        record=launch(c,'vf2-interactive','/run/vf2/run-game --release-rom')
        time.sleep(2)
        live=process(c,record['pid'])
        if live is None or live['state'] in ('Z','X'):
            c.download(record['log'],OUT/'vf2-interactive.log')
            raise RuntimeError('Interactive launch terminated; see vf2-interactive.log')
        if record['process'] and live['start_ticks']!=record['process']['start_ticks']:
            raise RuntimeError('Interactive PID was reused before launch verification')
        say(f'Interactive VF2 launched as PID {record["pid"]}; sound and built-in controls enabled. Hardware interaction still needs verification.')
    else:
        say('Device rendering/benchmark logs and screenshot saved. Physical input and audio still need verification.')
finally:
    if c:c.close()
