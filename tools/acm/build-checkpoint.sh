#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
variant=${2:-vf2checkpoint}
case "$variant" in vf2checkpoint|vf2checkpoint-tgp|vf2checkpoint-tgp-lto) ;; *) exit 2;; esac
obj=build/acm/obj-cortexa7
flags='-std=c++20 -O3 -DNDEBUG -ffp-contract=off -Isrc/core/shim_include -Ibuild/acm/deps/include -ffunction-sections -fdata-sections'
if [ "$variant" = vf2checkpoint-tgp-lto ]; then obj=$obj-lto;flags="$flags -flto=6";fi
make -j6 OBJ="$obj" BIN=build/acm/bin \
    CXX='arm-linux-gnueabihf-g++ -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard' \
    CXXFLAGS="$flags" \
    LDLIBS='build/acm/deps/lib/libz.a -static -pthread -latomic -Wl,--gc-sections' \
    GPU3D=none "build/acm/bin/$variant"
if [ "${1:-create}" != verify-existing ]; then
    qemu-arm -cpu cortex-a7 "build/acm/bin/$variant" create roms/vf2.zip build/acm/fight.checkpoint 2400 tools/scripts/start_game.txt
fi
qemu-arm -cpu cortex-a7 "build/acm/bin/$variant" verify roms/vf2.zip build/acm/fight.checkpoint
