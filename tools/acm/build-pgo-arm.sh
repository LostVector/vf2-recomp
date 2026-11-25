#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mode=${1:-generate}
case "$mode" in generate|use|core) ;; *) echo 'Expected generate, use or core' >&2;exit 2;; esac
obj=build/acm/pgo/obj-$mode
bin=build/acm/pgo/$mode/bin
flags="-std=c++20 -O3 -g1 -DNDEBUG -ffp-contract=off -Isrc/core/shim_include -Ibuild/acm/deps/include -ffunction-sections -fdata-sections -fprofile-prefix-path=/work/vf2-recomp/$obj"
if [ "$mode" = generate ]; then
    flags="$flags -fprofile-generate=/run/vf2/pgo -fprofile-update=atomic"
else
    for unit in src#host#acm_main src#core#mb86233_aot src#machine#model2a src#gpu#gpu3d_gles2 src#rec#i960_native third_party#mame#i960#i960; do
        test -s "build/acm/pgo/data/$unit.gcda" || { echo "Missing training profile: $unit" >&2;exit 2; }
    done
    if [ "$mode" = use ]; then flags="$flags -fprofile-use=/work/vf2-recomp/build/acm/pgo/data -Werror=coverage-mismatch";fi
fi
rules=build/acm/pgo/$mode-rules.mk
mkdir -p "$(dirname "$rules")"
: > "$rules"
if [ "$mode" = core ]; then
    for unit in src/machine/model2a src/core/mb86233_aot src/core/i960_device src/rec/i960_rec src/core/mame_shim src/gpu/gpu3d_gles2 third_party/mame/i960/i960; do
        printf '%s: override CXXFLAGS += -fprofile-use=/work/vf2-recomp/build/acm/pgo/data -Werror=coverage-mismatch\n' "$obj/$unit.o" >> "$rules"
    done
fi
make -B -j6 -f Makefile -f "$rules" OBJ="$obj" BIN="$bin" \
    CXX='arm-linux-gnueabihf-g++ -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard' \
    CXXFLAGS="$flags" \
    LDLIBS='build/acm/deps/lib/libz.a -static-libstdc++ -static-libgcc -pthread -latomic -Wl,--gc-sections -Wl,-rpath-link,/usr/arm-linux-gnueabihf/lib' \
    ACM_LIBS='build/acm/stock/libMali.so build/acm/stock/libasound.so.2' \
    GPU3D=none "$bin/vf2-acm-tgp"
cp "$bin/vf2-acm-tgp" "build/acm/pgo/$mode/vf2-acm-tgp.symbols"
arm-linux-gnueabihf-nm -nC "build/acm/pgo/$mode/vf2-acm-tgp.symbols" > "build/acm/pgo/$mode/vf2-acm-tgp.symbols.txt"
arm-linux-gnueabihf-strip "$bin/vf2-acm-tgp"

# Let the benchmark prefer this build only while its compiled inputs still match.
if [ "$mode" = core ]; then
    find src third_party/mame third_party/sljit -type f -print0 | sort -z | xargs -0 sha256sum > build/acm/pgo/core/sources.sha256
    sha256sum Makefile tools/acm/build-pgo-arm.sh tools/acm/build-frontend-arm.sh build/gen/vf2_tgp.inc build/gen/vf2_tgp_blocks.inc build/gen/vf2_tgp_matrix_timing.inc >> build/acm/pgo/core/sources.sha256
    sha256sum "$bin/vf2-acm-tgp" > build/acm/pgo/core/binary.sha256
fi
