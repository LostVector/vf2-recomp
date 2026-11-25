#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
arm-linux-gnueabihf-g++ -std=c++20 -O3 -ffp-contract=off -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    -static tools/acm/check-tgp.cpp build/acm/obj-cortexa7/src/core/mb86233_aot.o \
    -o build/acm/bin/check-tgp -pthread -latomic
for mode in blocks single slow reference counter; do
    case "$mode" in
        blocks) flags='VF2_TGP_BLOCKS=1' ;;
        single) flags='VF2_TGP_SINGLE=1' ;;
        slow) flags='VF2_TGP_BLOCKS=1 VF2_TGP_SLOW_RAM=1' ;;
        reference) flags='VF2_TGP_SINGLE=1 VF2_TGP_SLOW_RAM=1 VF2_TGP_FETCH_GUARDS=1' ;;
        counter) flags='VF2_TGP_COUNTER_REFERENCE=1' ;;
    esac
    env $flags qemu-arm -cpu cortex-a7 build/acm/bin/check-tgp build/acm/vf2-tgp.bin > "build/acm/check-tgp-$mode.log"
    sed -n '/^STATE /p;/^COUNTS /p' "build/acm/check-tgp-$mode.log" > "build/acm/check-tgp-$mode.states"
    cmp build/acm/check-tgp-blocks.states "build/acm/check-tgp-$mode.states"
done
echo 'All five ARM TGP paths match in state and counts across boot, mutation, restore, debugger, resume and short slices'
