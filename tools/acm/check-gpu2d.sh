#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
g++ -std=c++20 -O2 tools/acm/check-gpu2d.cpp src/gpu/gpu2d_gles2.cpp -lEGL -lGLESv2 -o build/acm/check-gpu2d
EGL_PLATFORM=surfaceless MESA_GLES_VERSION_OVERRIDE=2.0 build/acm/check-gpu2d
