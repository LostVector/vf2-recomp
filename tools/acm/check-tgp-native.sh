#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
arm-linux-gnueabihf-g++ -std=c++20 -O3 -g1 -ffp-contract=off -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -DTGP_REFERENCE_COMPILED=1 -Isrc/core -static tools/acm/check-tgp-native.cpp build/acm/obj-cortexa7/src/core/mb86233_aot.o build/acm/obj-cortexa7/src/core/mb86233_native.o build/acm/obj-cortexa7/third_party/sljit/sljitLir.o -o build/acm/bin/check-tgp-native -pthread -latomic
VF2_TGP_NATIVE_JIT=1 qemu-arm -cpu cortex-a7 build/acm/bin/check-tgp-native build/acm/vf2-tgp.bin
