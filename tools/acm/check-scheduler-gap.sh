#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
g++ -std=c++20 -O3 tools/acm/check-scheduler-gap.cpp -o build/acm/check-scheduler-gap
build/acm/check-scheduler-gap
g++ -std=c++20 -O3 -DVF2_FORCE_PORTABLE_U128 tools/acm/check-scheduler-gap.cpp -o build/acm/check-scheduler-gap-portable
build/acm/check-scheduler-gap-portable
arm-linux-gnueabihf-g++ -std=c++20 -O3 -static -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard tools/acm/check-scheduler-gap.cpp -o build/acm/bin/check-scheduler-gap
qemu-arm -cpu cortex-a7 build/acm/bin/check-scheduler-gap
