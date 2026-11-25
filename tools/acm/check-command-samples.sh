#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
arm-linux-gnueabihf-g++ -std=c++20 -O3 -ffp-contract=off -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -static tools/acm/check-command-samples.cpp build/acm/obj-cortexa7/src/core/mb86233_aot.o -o build/acm/bin/check-command-samples -pthread -latomic
VF2_TGP_COMMAND_SAMPLES=1 VF2_TGP_NATIVE_MATRIX=1 VF2_TGP_EXTENDED_MATRIX=1 VF2_TGP_RESUMABLE_MATRIX=1 qemu-arm -cpu cortex-a7 build/acm/bin/check-command-samples build/acm/vf2-tgp.bin
