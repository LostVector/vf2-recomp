"""Catch startup, confirm FEL, and load the development image into RAM only."""
import pathlib,sys,time,subprocess
BASE=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(BASE.parent/'python-libs'))
import usb.core,usb.util,libusb_package
DATA=BASE.parent/'package/acm-install_20221203/data'
fel=DATA/'sunxi-fel.exe'
def run(*args):
    print('FEL',' '.join(map(str,args)),flush=True)
    r=subprocess.run([str(fel),*map(str,args)],capture_output=True,text=True,timeout=180)
    print(r.stdout+r.stderr,flush=True)
    if r.returncode: raise RuntimeError(f'FEL command failed: {r.returncode}')
if '--wait' in sys.argv:
    backend=libusb_package.get_libusb1_backend()
    packet=bytes.fromhex('55 53 42 43 4a 53 48 4b 20 00 00 00 00 00 02 f8 02 00 00 00 00 00 00 00 00 00 00 00 00 00 00')
    print('Listening for Mini startup every 50 ms for 15 minutes.',flush=True)
    deadline=time.monotonic()+900
    while time.monotonic()<deadline:
        dev=usb.core.find(idVendor=0x1f3a,idProduct=0xefe8,backend=backend)
        if dev is None: time.sleep(.05);continue
        try:
            intf=dev.get_active_configuration()[(0,0)]
            endpoint=next(e.bEndpointAddress for e in intf if usb.util.endpoint_direction(e.bEndpointAddress)==usb.util.ENDPOINT_OUT)
            usb.util.claim_interface(dev,0)
            sent=dev.write(endpoint,packet,timeout=1000)
            print(f'Startup handshake: {sent} bytes on endpoint {endpoint:#x}',flush=True)
        except (usb.core.USBError,ValueError) as e: print('Startup state:',e,flush=True)
        finally: usb.util.dispose_resources(dev)
        time.sleep(3)
        ready=False
        for _ in range(6):
            try:
                r=subprocess.run([str(fel),'ver'],capture_output=True,text=True,timeout=4)
                if r.returncode==0 and 'AWUSBFEX' in r.stdout:
                    print(r.stdout,flush=True);ready=True;break
            except subprocess.TimeoutExpired: pass
            time.sleep(1)
        if ready:break
        print('Recovery not confirmed; waiting for another startup.',flush=True)
        while usb.core.find(idVendor=0x1f3a,idProduct=0xefe8,backend=backend) is not None and time.monotonic()<deadline: time.sleep(.1)
    else: raise RuntimeError('Startup listener expired')
run('ver')
run('write','0x2000',DATA/'fes1.bin')
run('exe','0x2000')
run('write','0x43800000',DATA/'uImage')
run('write','0x48000000',BASE/'build/acm/dev_uRamdisk')
run('write','0x47000000',BASE/'build/acm/u-boot-acm-lcd.bin')
run('exe','0x47000000')
print('RAM development image started. No NAND flash commands were used.',flush=True)
