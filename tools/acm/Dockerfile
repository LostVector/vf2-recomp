FROM debian:bookworm-slim@sha256:734a72b3d6142a71993b4bc580ca01d8ca687f77a2c422d96a961d3376b179bc
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    g++ g++-arm-linux-gnueabihf make pkg-config qemu-user file curl ca-certificates \
    zlib1g-dev libboost1.74-dev libegl-dev libgles-dev libegl1-mesa-dev libgl1-mesa-dri libasound2-dev
WORKDIR /work/vf2-recomp
