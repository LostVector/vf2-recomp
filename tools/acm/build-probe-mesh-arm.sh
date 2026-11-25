#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
arm-linux-gnueabihf-g++ -std=c++20 -O3 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
 tools/acm/probe-gles2-mesh.cpp -o build/acm/bin/probe-gles2-mesh \
 -static-libstdc++ -static-libgcc -pthread -Wl,-rpath-link,/usr/arm-linux-gnueabihf/lib build/acm/stock/libMali.so
