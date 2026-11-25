#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/acm/validation/host build/acm/validation/arm
case "${1:-arm}" in
    host) build/host/bin/vf2run roms/vf2.zip 1200 --sound --frames-log --png 300,600,1199 --out build/acm/validation/host > build/acm/validation/host.log 2>&1 ;;
    arm) qemu-arm -cpu cortex-a7 build/acm/bin/vf2run roms/vf2.zip 1200 --sound --frames-log --png 300,600,1199 --out build/acm/validation/arm > build/acm/validation/arm.log 2>&1 ;;
esac
