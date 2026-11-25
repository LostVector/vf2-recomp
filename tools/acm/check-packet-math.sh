#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
g++ -std=c++20 -O3 -ffp-contract=off tools/acm/check-packet-math.cpp -o build/acm/check-packet-math
build/acm/check-packet-math > build/acm/check-packet-math-host.log
VF2_REFERENCE_PACKET_MATH=1 build/acm/check-packet-math > build/acm/check-packet-math-host-reference.log
cmp build/acm/check-packet-math-host.log build/acm/check-packet-math-host-reference.log
arm-linux-gnueabihf-g++ -std=c++20 -O3 -ffp-contract=off -static -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    tools/acm/check-packet-math.cpp -o build/acm/bin/check-packet-math
qemu-arm -cpu cortex-a7 build/acm/bin/check-packet-math > build/acm/check-packet-math-arm.log
VF2_REFERENCE_PACKET_MATH=1 qemu-arm -cpu cortex-a7 build/acm/bin/check-packet-math > build/acm/check-packet-math-arm-reference.log
cmp build/acm/check-packet-math-arm.log build/acm/check-packet-math-arm-reference.log
cat build/acm/check-packet-math-host.log build/acm/check-packet-math-arm.log
