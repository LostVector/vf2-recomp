#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
arm-linux-gnueabihf-g++ -std=c++20 -O3 -static -mcpu=cortex-a7 \
    tools/acm/check-word-fifo.cpp -o build/acm/bin/check-word-fifo
qemu-arm -cpu cortex-a7 build/acm/bin/check-word-fifo
