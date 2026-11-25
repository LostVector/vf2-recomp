#!/bin/ash
set -eu
home_dir=${DAYTONA_HOME:-/run/daytona}
runtime=${ACM_RUNTIME:-/run/vf2/lib}
export ALSA_CONFIG_PATH=${ALSA_CONFIG_PATH:-/run/vf2/asound.conf}

export VF2_GPU_CANVAS=1
export VF2_PIPELINE_RENDER=1
export VF2_CAMERA_PACKETS=1
export VF2_GEOMETRY_WORKER=1
export VF2_TRACK_GEOMETRY_PAGES=1
export VF2_MODULAR_CLOCK=1
export VF2_LAZY_AUX_CLOCKS=1
export VF2_SWAP_INTERVAL=0
export VF2_GLES_BATCHED=1
export VF2_GLES_INDEXED=1
export VF2_GLES_STACK_PACKETS=1
export VF2_GLES_DIRECT_PACK=1
export VF2_PREPARED_DRAWS=1
export VF2_GPU_QUEUE=1
export VF2_SYNC_GPU=1
export VF2_CLIENT_ARRAYS=1
export VF2_DIRECT_PROJECTION=1
export VF2_PROJECTED_STREAM=1
export VF2_COMPACT_CAMERA=1
export VF2_SHARED_CAMERA=1
export VF2_SHARED_CAMERA_COLORS=1
export VF2_COMPACT_PACKETS=1
export VF2_COMPACT_CELL_VERTICES=1
export VF2_SHARED_PACKET_STORAGE=1
export VF2_DIRECT_GEOMETRY=1
export VF2_EARLY_MATERIAL=1
export VF2_ATOMIC_ROW_CLAIMS=1
export VF2_DIRECT_TASK_RANGES=1
export VF2_STAGE_MATERIALS=1
export VF2_OVERLAP_PALETTES=1
export VF2_DUAL_PACK=1
export VF2_FAST_VIEWPORT_BOUNDS=1
export VF2_LIMIT_MIP_SCAN=1
export VF2_ORIGIN_PACKET_FAN=1
export VF2_RETAINED_FANS=1
export VF2_RETAINED_FAN_CELL_BOUNDS=1
export VF2_CELL_INDEX_TRANSFORMS=1
export VF2_NATIVE_REPEAT_MATERIAL=1
# Daytona-accepted byte-exact wins (see tools/acm/DAYTONA.md). The narrow road
# native-repeat route (-0.92 ms p99, byte-identical at busy frames) and the two
# clip micro-opts (-0.4 ms p99, byte-exact). The floor was measured with these;
# the release must ship them. Reversible: delete these three lines to revert.
export VF2_NATIVE_REPEAT_TRACK=1
export VF2_CELL_COL_CLIP=1
export VF2_CELL_CLIP_HOIST=1
export VF2_ATLAS_TILE_EDGES=1
export VF2_HOMOGENEOUS_ATLAS_SHADERS=1
export VF2_ATLAS_PAGE_CACHE=1
export VF2_ATLAS_SNAPSHOT_DECODE=1
export VF2_ATLAS_LIMIT_MB=16
export VF2_FIXED_ATLAS_LUMA=1
export VF2_ROTATE_3D_PALETTE=1
export VF2_WIDE_PALETTE_ATLAS=1
export VF2_PACKED_PALETTE_INDEX=1
export VF2_PALETTE_SHADE_CACHE=1
export VF2_CACHE_2D_PENS=1
export VF2_GPU_LINE_SCROLL=1
export VF2_GPU_LINE_SCROLL_OPAQUE_ONLY=1
export VF2_COALESCE_LINE_SCROLL=1
export VF2_CLIP_EMPTY_TILES=1
export VF2_ROTATE_TILE_TEXTURES=1
export VF2_QUADRANT_TILE_TEXTURES=1
export VF2_PACKED_TILE_MAP=1
export VF2_S24_NEON_DECODE=1
export VF2_SOUND_CPU=3
export VF2_SOUND_PRIO=10
export VF2_LINEOUT=20

if [ "${DAYTONA_FULL_DRAW:-0}" != 1 ]; then
    export DAYTONA_ADAPTIVE_DRAW=1
else
    unset DAYTONA_ADAPTIVE_DRAW
fi

exec "$runtime/ld-linux-armhf.so.3" \
    --library-path "$runtime:/stock/usr/lib:/stock/lib" \
    "$home_dir/daytona-acm" "$home_dir/daytona.zip" --rotate 270 \
    --no-sound-board --detached-audio speaker --nvram "$home_dir/cabinet.nv" "$@"
