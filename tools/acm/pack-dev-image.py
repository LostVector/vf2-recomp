"""Repack only a RAM initramfs; never include the installer or NAND images."""
import pathlib,gzip,struct,zlib,hashlib,json
BASE=pathlib.Path(__file__).resolve().parents[2]
source=BASE.parent/'package/acm-install_20221203/data/install_uRamdisk'
blob=source.read_bytes();archive=gzip.decompress(blob[64:]);entries=[];pos=0
while pos+110<=len(archive):
    h=archive[pos:pos+110]; assert h[:6]==b'070701'
    fields=[int(h[6+i*8:14+i*8],16) for i in range(13)]
    size,ns=fields[6],fields[11]
    name=archive[pos+110:pos+110+ns-1].decode()
    start=(pos+110+ns+3)&~3;data=archive[start:start+size];pos=(start+size+3)&~3
    if name=='TRAILER!!!':break
    if name in ('init','restore_init','data') or name.startswith('data/'):continue
    entries.append((name,fields[1],data))
entries.extend([
    ('init',0o100755,(BASE/'tools/acm/dev-init.sh').read_bytes().replace(b'\r\n',b'\n')),
    ('bin/usb-rpc',0o100755,(BASE/'build/acm/bin/usb-rpc').read_bytes()),
])
result=bytearray()
for ino,(name,mode,data) in enumerate(entries+[('TRAILER!!!',0,b'')],1):
    namebytes=name.encode()+b'\0'
    fields=[ino,mode,0,0,1,0,len(data),0,0,0,0,len(namebytes),0]
    result+=b'070701'+''.join(f'{x:08x}' for x in fields).encode()+namebytes
    result+=b'\0'*(-len(result)%4);result+=data;result+=b'\0'*(-len(result)%4)
payload=gzip.compress(result,compresslevel=9,mtime=0)
header=bytearray(blob[:64]);header[4:8]=b'\0'*4
header[12:16]=struct.pack('>I',len(payload));header[24:28]=struct.pack('>I',zlib.crc32(payload))
header[32:64]=b'ACM VF2 RAM development'.ljust(32,b'\0')
header[4:8]=struct.pack('>I',zlib.crc32(header))
out=BASE/'build/acm/dev_uRamdisk';out.write_bytes(header+payload)
(BASE/'build/acm/dev-image-manifest.json').write_text(json.dumps({n:{'mode':oct(m),'size':len(d),'sha256':hashlib.sha256(d).hexdigest()} for n,m,d in entries},indent=2))
print(out,len(header+payload))
