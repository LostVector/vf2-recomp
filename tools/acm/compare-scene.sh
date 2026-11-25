#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
make -j6 OBJ=build/host/obj BIN=build/host/bin CXX=g++ \
    CXXFLAGS='-std=c++20 -O2 -ffp-contract=off -Isrc/core/shim_include' \
    GPU3D=none build/host/bin/gpu3dcmp-gles2
EGL_PLATFORM=surfaceless MESA_GLES_VERSION_OVERRIDE=2.0 \
    build/host/bin/gpu3dcmp-gles2 roms/vf2.zip 2401 - 1 1.3333333 400 build/acm/gles2-scenes 1200,2400
