#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
# Compile once and verify both register allocation strategies.
env -u VF2_TGP_VFP_CACHE sh tools/acm/check-tgp-native.sh
VF2_TGP_VFP_CACHE=1 VF2_TGP_NATIVE_JIT=1 qemu-arm -cpu cortex-a7 build/acm/bin/check-tgp-native build/acm/vf2-tgp.bin
