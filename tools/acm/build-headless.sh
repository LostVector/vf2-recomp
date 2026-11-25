#!/bin/sh
# Run in a Debian bookworm build container with the ARM hard-float cross compiler.
set -eu
cd "$(dirname "$0")/../.."
printf '%s\n' '9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23  build/deps/zlib.tar.gz' | sha256sum -c -
mkdir -p build/acm/deps
if [ ! -f build/acm/deps/lib/libz.a ]; then
    tar xzf build/deps/zlib.tar.gz -C build/acm
    (
        cd build/acm/zlib-1.3.1
        CC=arm-linux-gnueabihf-gcc AR=arm-linux-gnueabihf-ar RANLIB=arm-linux-gnueabihf-ranlib \
            CFLAGS='-O3 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard' \
            ./configure --static --prefix="$(pwd)/../deps"
        make -j6
        make install
    )
fi
make -j6 OBJ=build/acm/obj-cortexa7 BIN=build/acm/bin \
    CXX='arm-linux-gnueabihf-g++ -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard' \
    CXXFLAGS='-std=c++20 -O3 -DNDEBUG -ffp-contract=off -Isrc/core/shim_include -Ibuild/acm/deps/include -ffunction-sections -fdata-sections' \
    LDLIBS='build/acm/deps/lib/libz.a -static -pthread -latomic -Wl,--gc-sections' \
    GPU3D=none build/acm/bin/vf2run build/acm/bin/vf2romset
arm-linux-gnueabihf-strip build/acm/bin/vf2run build/acm/bin/vf2romset
file build/acm/bin/vf2run
