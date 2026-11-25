#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
g++ -std=c++20 -O3 tools/acm/check-cycle-duration.cpp -o build/acm/check-cycle-duration
build/acm/check-cycle-duration
g++ -std=c++20 -O3 -DVF2_FORCE_PORTABLE_U128 tools/acm/check-cycle-duration.cpp -o build/acm/check-cycle-duration-portable
build/acm/check-cycle-duration-portable
arm-linux-gnueabihf-g++ -std=c++20 -O3 -static -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    tools/acm/check-cycle-duration.cpp -o build/acm/bin/check-cycle-duration
qemu-arm -cpu cortex-a7 build/acm/bin/check-cycle-duration
