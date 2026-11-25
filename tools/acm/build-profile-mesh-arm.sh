#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
make -j6 -f Makefile -f tools/acm/mesh-profile.mk OBJ=build/acm/obj-cortexa7 BIN=build/acm/bin \
 CXX='arm-linux-gnueabihf-g++ -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard' \
 CXXFLAGS='-std=c++20 -O3 -g1 -DNDEBUG -ffp-contract=off -Isrc/core/shim_include -Ibuild/acm/deps/include -ffunction-sections -fdata-sections' \
 LDLIBS='build/acm/deps/lib/libz.a -static -pthread -latomic -Wl,--gc-sections' \
 GPU3D=none build/acm/bin/vf2mesh-profile-tgp
