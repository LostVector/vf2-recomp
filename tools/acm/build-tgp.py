"""Build and verify the microcode-specialized ARM frontend from the user's ROM.

Requires the configured acm-vf2-armv7 Docker container and ARM dependencies.
Generated microcode and binaries stay in ignored build/ paths.
"""
import pathlib,subprocess,sys

base=pathlib.Path(__file__).resolve().parents[2]
container='acm-vf2-armv7'

def run(*args):
    subprocess.run(args,cwd=base,check=True,stderr=subprocess.STDOUT)

mode='verify-existing' if (base/'build/acm/fight.checkpoint').exists() else 'create'
run('docker','exec',container,'sh','tools/acm/build-checkpoint.sh',mode)
run('docker','exec',container,'qemu-arm','-cpu','cortex-a7',
    'build/acm/bin/vf2checkpoint','dump-tgp','roms/vf2.zip',
    'build/acm/fight.checkpoint','build/acm/vf2-tgp.bin')
run(sys.executable,'tools/acm/generate-tgp.py','build/acm/vf2-tgp.bin','build/gen/vf2_tgp.inc')
run('docker','exec',container,'sh','tools/acm/build-frontend-arm.sh','vf2-acm-tgp')
run('docker','exec',container,'sh','tools/acm/build-checkpoint.sh','verify-existing','vf2checkpoint-tgp')
