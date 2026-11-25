#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
arm-linux-gnueabihf-g++ -std=c++20 -O3 -ffp-contract=off -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
 -static tools/acm/check-mesh-geometry.cpp -o build/acm/bin/check-mesh-geometry
qemu-arm -cpu cortex-a7 build/acm/bin/check-mesh-geometry
