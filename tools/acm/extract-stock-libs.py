import pathlib,sys
BASE=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(BASE.parent/'python-libs'))
from dissect.extfs import ExtFS
backup=pathlib.Path((BASE.parent/'diagnostics/current-backup.txt').read_text().strip())
out=BASE/'build/acm/stock';out.mkdir(parents=True,exist_ok=True)
with (backup/'nandd.img').open('rb') as f:
    fs=ExtFS(f)
    for source,name in [('/usr/lib/libMali.so','libMali.so'),('/usr/lib/libasound.so.2.0.0','libasound.so.2')]:
        (out/name).write_bytes(fs.get(source).open().read())
        print(source,'->',out/name)
    module=fs.get('/lib/modules/3.4.113/kernel/drivers/sunxi-mali400-km/mali.ko').open().read()
    import re
    print('Stock Mali module:',re.findall(rb'vermagic=[^\0]+',module))
