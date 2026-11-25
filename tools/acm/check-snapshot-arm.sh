#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
arm-linux-gnueabihf-g++ -std=c++20 -O3 -static -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    tools/acm/check-snapshot.cpp -o build/acm/bin/check-snapshot
qemu-arm -cpu cortex-a7 build/acm/bin/check-snapshot
VF2_FULL_TEXTURE_SNAPSHOT=1 qemu-arm -cpu cortex-a7 build/acm/bin/check-snapshot
