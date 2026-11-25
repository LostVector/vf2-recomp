#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
g++ -std=c++20 -O3 tools/acm/check-frame-clock.cpp -o build/acm/check-frame-clock
build/acm/check-frame-clock
g++ -std=c++20 -O3 -DVF2_FORCE_PORTABLE_U128 tools/acm/check-frame-clock.cpp -o build/acm/check-frame-clock-portable
build/acm/check-frame-clock-portable
arm-linux-gnueabihf-g++ -std=c++20 -O3 -static -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard tools/acm/check-frame-clock.cpp -o build/acm/bin/check-frame-clock
qemu-arm -cpu cortex-a7 build/acm/bin/check-frame-clock
