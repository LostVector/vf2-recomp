#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
g++ -std=c++20 -O2 tools/acm/check-gles2.cpp src/gpu/gpu3d_gles2.cpp -lEGL -lGLESv2 -o build/acm/check-gles2
EGL_PLATFORM=surfaceless MESA_GLES_VERSION_OVERRIDE=2.0 build/acm/check-gles2
VF2_GLES_MEDIUMP=1 EGL_PLATFORM=surfaceless MESA_GLES_VERSION_OVERRIDE=2.0 build/acm/check-gles2
