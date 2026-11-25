"""Select the internal LCD in a RAM-only copy of the upstream memboot image."""
import pathlib,struct,hashlib,json
BASE=pathlib.Path(__file__).resolve().parents[2]
source=BASE.parent/'package/acm-install_20221203/data/u-boot-sun8iw5p1_memboot.bin'
original=source.read_bytes();data=bytearray(original)
base=0xf0000
count,size=struct.unpack_from('<II',data,base)
assert 0<count<200 and 16<size<=len(data)-base
changes=[]
for section in range(count):
    name,n,offset=struct.unpack_from('<32sII',data,base+16+40*section)
    if name.split(b'\0')[0]!=b'disp_init':continue
    for index in range(n):
        key,voffset,pattern=struct.unpack_from('<32sII',data,base+4*offset+40*index)
        if key.split(b'\0')[0]==b'screen0_output_type':
            assert pattern==0x10001
            address=base+4*voffset;old=struct.unpack_from('<I',data,address)[0]
            assert old==3
            struct.pack_into('<I',data,address,1)
            changes.append({'section':'disp_init','key':'screen0_output_type','old':old,'new':1,'offset':address})
assert len(changes)==1
# Verify and update the Allwinner additive boot-image checksum as well, even
# though FEL enters this image directly. No NAND or saved environment is written.
assert sum(a!=b for a,b in zip(original,data))==1
assert original[4:12]==b'uboot\0\0\0'
assert struct.unpack_from('<I',original,20)[0]==len(original)
def checksum(blob):
    checked=bytearray(blob);struct.pack_into('<I',checked,12,0x5f0a6c39)
    return sum(struct.unpack('<%dI'%(len(checked)//4),checked))&0xffffffff
assert checksum(original)==struct.unpack_from('<I',original,12)[0]
struct.pack_into('<I',data,12,checksum(data))
assert checksum(data)==struct.unpack_from('<I',data,12)[0]
out=BASE/'build/acm/u-boot-acm-lcd.bin';out.write_bytes(data)
(BASE/'build/acm/memboot-patch.json').write_text(json.dumps({'source_sha256':hashlib.sha256(original).hexdigest(),'output_sha256':hashlib.sha256(data).hexdigest(),'changes':changes},indent=2))
print(out,changes)
