#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/acm/bin
arm-linux-gnueabihf-g++ -std=c++20 -O3 -DNDEBUG -ffp-contract=off \
    -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    -Isrc/core/shim_include -Ithird_party/mame/i960 \
    -ffunction-sections -fdata-sections -static -Wl,--gc-sections \
    tools/acm/check-i960-register-frame.cpp third_party/mame/i960/i960.cpp \
    third_party/mame/i960/i960dis.cpp src/core/mame_shim.cpp \
    -pthread -latomic -o build/acm/bin/check-i960-register-frame
VF2_I960_DIRECT_REGISTER_FRAME=1 qemu-arm -cpu cortex-a7 build/acm/bin/check-i960-register-frame
