#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
arm-linux-gnueabihf-g++ -std=c++20 -O3 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    tools/acm/check-gpu2d.cpp src/gpu/gpu2d_gles2.cpp -o build/acm/bin/check-gpu2d \
    -static-libstdc++ -static-libgcc -pthread -Wl,-rpath-link,/usr/arm-linux-gnueabihf/lib build/acm/stock/libMali.so
