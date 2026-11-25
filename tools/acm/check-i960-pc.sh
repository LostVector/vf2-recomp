#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
make -j6 OBJ=build/acm/obj-cortexa7 BIN=build/acm/bin CXX='arm-linux-gnueabihf-g++ -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard' CXXFLAGS='-std=c++20 -O3 -g1 -DNDEBUG -ffp-contract=off -Isrc/core/shim_include -Ibuild/acm/deps/include -ffunction-sections -fdata-sections' LDLIBS='-static -pthread -latomic -Wl,--gc-sections' GPU3D=none build/acm/bin/check-i960-pc
env -u VF2_REC_REG_CYCLES VF2_REC_LAZY_PC=1 qemu-arm -cpu cortex-a7 build/acm/bin/check-i960-pc
env -u VF2_REC_LAZY_PC VF2_REC_REG_CYCLES=1 qemu-arm -cpu cortex-a7 build/acm/bin/check-i960-pc
VF2_REC_LAZY_PC=1 VF2_REC_REG_CYCLES=1 qemu-arm -cpu cortex-a7 build/acm/bin/check-i960-pc
