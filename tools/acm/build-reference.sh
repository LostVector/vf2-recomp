#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
make -j6 OBJ=build/host/obj BIN=build/host/bin CXX=g++ \
    CXXFLAGS='-std=c++20 -O2 -ffp-contract=off -Isrc/core/shim_include' \
    GPU3D=none build/host/bin/vf2run
