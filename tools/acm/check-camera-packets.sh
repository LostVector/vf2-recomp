#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
arm-linux-gnueabihf-g++ -std=c++20 -O3 -ffp-contract=off -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    -static -pthread tools/acm/check-camera-clip.cpp -o build/acm/bin/check-camera-clip
qemu-arm -cpu cortex-a7 build/acm/bin/check-camera-clip
make -j6 OBJ=build/acm/obj-cortexa7 BIN=build/acm/bin \
    CXX='arm-linux-gnueabihf-g++ -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard' \
    CXXFLAGS='-std=c++20 -O3 -g1 -DNDEBUG -ffp-contract=off -Isrc/core/shim_include -Ibuild/acm/deps/include -ffunction-sections -fdata-sections' \
    LDLIBS='build/acm/deps/lib/libz.a -static -pthread -latomic -Wl,--gc-sections' \
    GPU3D=none build/acm/bin/vf2camera-check-tgp
VF2_THREADS=3 qemu-arm -cpu cortex-a7 build/acm/bin/vf2camera-check-tgp roms/vf2.zip build/acm/fight.checkpoint
