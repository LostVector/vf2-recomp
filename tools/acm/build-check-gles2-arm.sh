#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
arm-linux-gnueabihf-g++ -std=c++20 -O3 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    tools/acm/check-gles2.cpp src/gpu/gpu3d_gles2.cpp -o build/acm/bin/check-gles2 \
    -static-libstdc++ -static-libgcc -pthread -Wl,-rpath-link,/usr/arm-linux-gnueabihf/lib build/acm/stock/libMali.so
