#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
export VF2_STAGE_MATERIALS=1 VF2_GLES_DIRECT_PACK=1 VF2_ATLAS_PAGE_CACHE=1
sh tools/acm/check-direct-pack.sh
