#!/bin/ash
set -eu
home_dir=${VF2_HOME:-/run/vf2}
export ALSA_CONFIG_PATH="$home_dir/asound.conf"
exec "$home_dir/lib/ld-linux-armhf.so.3" \
    --library-path "$home_dir/lib:/stock/usr/lib:/stock/lib" \
    "$home_dir/vf2-acm" "$home_dir/vf2.zip" --rotate 270 "$@"
