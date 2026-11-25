#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
make -j6 OBJ=build/acm/obj-cortexa7 BIN=build/acm/bin \
    CXX='arm-linux-gnueabihf-g++ -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard' \
    CXXFLAGS='-std=c++20 -O3 -g1 -DNDEBUG -ffp-contract=off -Isrc/core/shim_include -Ibuild/acm/deps/include -ffunction-sections -fdata-sections' \
    LDLIBS='build/acm/deps/lib/libz.a -static -pthread -latomic -Wl,--gc-sections' GPU3D=none build/acm/bin/check-modular-clock
VF2_MODULAR_CLOCK=1 VF2_LAZY_AUX_CLOCKS=1 VF2_TGP_NATIVE_MATRIX=1 VF2_TGP_EXTENDED_MATRIX=1 VF2_TGP_RESUMABLE_MATRIX=1 qemu-arm -cpu cortex-a7 build/acm/bin/check-modular-clock roms/vf2.zip build/acm/fight.checkpoint
rollover_modular=$(VF2_MODULAR_CLOCK=1 VF2_LAZY_AUX_CLOCKS=1 VF2_SYNC_TIMER_FAST=1 VF2_FAST_HALT_DELIVERY=1 \
    qemu-arm -cpu cortex-a7 build/acm/bin/check-modular-clock roms/vf2.zip build/acm/fight.checkpoint rollover)
rollover_reference=$(env -u VF2_MODULAR_CLOCK VF2_LAZY_AUX_CLOCKS=1 VF2_SYNC_TIMER_FAST=1 VF2_FAST_HALT_DELIVERY=1 \
    qemu-arm -cpu cortex-a7 build/acm/bin/check-modular-clock roms/vf2.zip build/acm/fight.checkpoint rollover-reference)
test "$rollover_modular" = "$rollover_reference"
printf '%s\n' "$rollover_modular"
