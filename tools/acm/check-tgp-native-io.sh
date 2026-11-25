#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
VF2_TGP_NATIVE_IO=1 VF2_TGP_VFP_CACHE=1 sh tools/acm/check-tgp-native.sh
VF2_TGP_NATIVE_IO=1 VF2_TGP_NATIVE_JIT=1 env -u VF2_TGP_VFP_CACHE qemu-arm -cpu cortex-a7 build/acm/bin/check-tgp-native build/acm/vf2-tgp.bin
