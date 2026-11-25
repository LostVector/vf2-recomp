"""USB client for the RAM-only FunctionFS development transport."""
import pathlib, sys, struct, argparse, zlib
BASE=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(BASE.parent/'python-libs'))
import libusb_package, usb.core, usb.util

class Client:
    def __init__(self,timeout=120000):
        self.timeout=timeout
        self.dev=usb.core.find(idVendor=0x1f3a,idProduct=0xefe8,backend=libusb_package.get_libusb1_backend())
        if self.dev is None: raise RuntimeError('ACM USB development device is absent')
        cfg=self.dev.get_active_configuration()
        intf=cfg[(0,0)]
        # The RPC interface is vendor-specific. FunctionFS assigns endpoint addresses.
        self.out=next(e.bEndpointAddress for e in intf if usb.util.endpoint_direction(e.bEndpointAddress)==usb.util.ENDPOINT_OUT)
        self.inp=next(e.bEndpointAddress for e in intf if usb.util.endpoint_direction(e.bEndpointAddress)==usb.util.ENDPOINT_IN)
        usb.util.claim_interface(self.dev,0)
    def write(self,data):
        for at in range(0,len(data),16384):
            chunk=data[at:at+16384]
            written=self.dev.write(self.out,chunk,timeout=10000)
            if written!=len(chunk): raise RuntimeError(f'Short USB write {written}/{len(chunk)}')
    def read(self,n,timeout=None):
        if n>64*1024*1024: raise RuntimeError(f'Invalid RPC response length {n}')
        if timeout is None: timeout=self.timeout
        data=bytearray()
        while len(data)<n:
            data+=self.dev.read(self.inp,min(n-len(data),16384),timeout=timeout)
        return bytes(data)
    def request(self,op,name,data=b''):
        name=name.encode()
        self.write(struct.pack('<IIII',0x314d4341,op,len(name),len(data)))
        self.write(name)
        if data: self.write(data)
        status,n=struct.unpack('<iI',self.read(8))
        payload=self.read(n)
        return status,payload
    def execute(self,command): return self.request(1,command)
    def upload(self,local,remote):
        data=pathlib.Path(local).read_bytes()
        status,payload=self.request(2,remote,data)
        if status: raise RuntimeError(f'Upload failed {status}: {payload!r}')
        return len(data),zlib.crc32(data)
    def chmod(self,remote,mode=0o755):
        name=remote.encode()
        self.write(struct.pack('<IIII',0x314d4341,4,len(name),mode))
        self.write(name)
        status,n=struct.unpack('<iI',self.read(8))
        payload=self.read(n)
        if status: raise RuntimeError(f'chmod failed {status}: {payload!r}')
    def download(self,remote,local):
        status,payload=self.request(3,remote)
        if status: raise RuntimeError(f'Download failed {status}: {payload!r}')
        pathlib.Path(local).write_bytes(payload)
    def close(self): usb.util.dispose_resources(self.dev)

if __name__=='__main__':
    p=argparse.ArgumentParser()
    sub=p.add_subparsers(dest='op',required=True)
    ex=sub.add_parser('exec');ex.add_argument('command')
    up=sub.add_parser('put');up.add_argument('local');up.add_argument('remote')
    down=sub.add_parser('get');down.add_argument('remote');down.add_argument('local')
    mode=sub.add_parser('chmod');mode.add_argument('remote');mode.add_argument('mode',type=lambda s:int(s,8),nargs='?',default=0o755)
    a=p.parse_args(); c=Client()
    try:
        if a.op=='exec':
            status,data=c.execute(a.command);sys.stdout.buffer.write(data);sys.exit(status if 0<=status<=255 else 1)
        if a.op=='put': c.upload(a.local,a.remote)
        if a.op=='get': c.download(a.remote,a.local)
        if a.op=='chmod': c.chmod(a.remote,a.mode)
    finally: c.close()
