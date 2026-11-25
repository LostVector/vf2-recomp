#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
export VF2_CHECK_DIRECT_PACK=1 VF2_GLES_BATCHED=1 VF2_GLES_STACK_PACKETS=1
unset VF2_GLES_INDEXED
sh tools/acm/check-gles2.sh
export VF2_GLES_INDEXED=1
sh tools/acm/check-gles2.sh
