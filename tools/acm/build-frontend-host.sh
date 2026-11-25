#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
make -j6 OBJ=build/host/obj BIN=build/host/bin CXX=g++ \
    CXXFLAGS='-std=c++20 -O2 -ffp-contract=off -Isrc/core/shim_include' \
    GPU3D=none build/host/bin/vf2-acm
EGL_PLATFORM=surfaceless MESA_GLES_VERSION_OVERRIDE=2.0 \
    build/host/bin/vf2-acm roms/vf2.zip --headless --frames 1200 --capture build/acm/gles2-frontend.png
