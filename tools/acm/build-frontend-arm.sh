#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
variant=${1:-vf2-acm}
case "$variant" in vf2-acm|vf2-acm-tgp|vf2-acm-tgp-lto|vf2-acm-tgp-stats) ;; *) echo 'Unknown frontend target' >&2;exit 2;; esac
obj=build/acm/obj-cortexa7
flags='-std=c++20 -O3 -g1 -DNDEBUG -ffp-contract=off -Isrc/core/shim_include -Ibuild/acm/deps/include -ffunction-sections -fdata-sections'
if [ "$variant" = vf2-acm-tgp-lto ]; then obj=$obj-lto;flags="$flags -flto=6";fi
if [ "$variant" = vf2-acm-tgp-stats ]; then obj=$obj-stats;flags="$flags -DVF2_SCHED_DIAGNOSTICS=1";fi
arm-linux-gnueabihf-gcc -O2 -static -mcpu=cortex-a7 tools/acm/load-module.c -o build/acm/bin/load-module
arm-linux-gnueabihf-gcc -O2 -static -pthread -mcpu=cortex-a7 tools/acm/usb-rpc.c -o build/acm/bin/usb-rpc
make -j6 OBJ="$obj" BIN=build/acm/bin \
    CXX='arm-linux-gnueabihf-g++ -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard' \
    CXXFLAGS="$flags" \
    LDLIBS='build/acm/deps/lib/libz.a -static-libstdc++ -static-libgcc -pthread -latomic -Wl,--gc-sections -Wl,-rpath-link,/usr/arm-linux-gnueabihf/lib' \
    ACM_LIBS='build/acm/stock/libMali.so build/acm/stock/libasound.so.2' \
    GPU3D=none "build/acm/bin/$variant"
if arm-linux-gnueabihf-readelf -S "build/acm/bin/$variant" | grep -q '\.symtab'; then
    cp "build/acm/bin/$variant" "build/acm/$variant.symbols"
fi
arm-linux-gnueabihf-nm -nC "build/acm/$variant.symbols" > "build/acm/$variant.symbols.txt"
if [ "$variant" = vf2-acm ]; then cp "build/acm/$variant.symbols.txt" build/acm/vf2-symbols.txt;fi
arm-linux-gnueabihf-strip "build/acm/bin/$variant" build/acm/bin/load-module build/acm/bin/usb-rpc
mkdir -p build/acm/runtime
for name in ld-linux-armhf.so.3 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 libgcc_s.so.1 libatomic.so.1; do
    cp -L "/usr/arm-linux-gnueabihf/lib/$name" "build/acm/runtime/$name"
done
file "build/acm/bin/$variant"
arm-linux-gnueabihf-readelf -d "build/acm/bin/$variant"
