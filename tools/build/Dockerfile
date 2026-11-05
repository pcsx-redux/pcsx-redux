# Dockerfile for ghcr.io/pcsx-redux/pcsx-redux-build
#
# This one needs the repository root as its build context, as the first stage
# builds the mips toolchain using tools/linux-mips/spawn-compiler.sh:
#
#   docker build -f tools/build/Dockerfile -t ghcr.io/pcsx-redux/pcsx-redux-build .
#
# BASE picks the Ubuntu release. The general image tracks the newest one, but
# an AppImage needs the glibc it was built against, so the packaging job uses a
# second tag built from the oldest LTS we want the AppImage to run on:
#
#   docker build -f tools/build/Dockerfile --build-arg BASE=ubuntu:24.04 \
#     --build-arg GXX_VERSION=15 -t ghcr.io/pcsx-redux/pcsx-redux-build:noble .

ARG BASE=ubuntu:26.04

FROM ${BASE} AS toolchain

ENV DEBIAN_FRONTEND noninteractive
ENV TZ Etc/GMT

RUN apt update
RUN apt dist-upgrade -y
RUN apt install -y make g++ wget bzip2 xz-utils bison flex texinfo
RUN apt install -y libgmp-dev libmpfr-dev libmpc-dev

ARG JOBS=

WORKDIR /tmp/toolchain
COPY tools/linux-mips/spawn-compiler.sh .
RUN JOBS=$JOBS sh spawn-compiler.sh

# SDL3 isn't packaged before 25.10, so it's built here on every base. The tag
# follows the third_party/SDL submodule, so Linux ships the same SDL3 as the
# MSVC build. The dependency list is the one in SDL's docs/README-linux.md,
# plus what the Wayland backend needs.
FROM ${BASE} AS sdl

ENV DEBIAN_FRONTEND noninteractive
ENV TZ Etc/GMT

ARG SDL_TAG=release-3.4.8
ARG JOBS=

RUN apt update
RUN apt dist-upgrade -y
RUN apt install -y git cmake ninja-build g++ pkg-config
RUN apt install -y libasound2-dev libpulse-dev libaudio-dev libfribidi-dev \
    libjack-dev libsndio-dev libx11-dev libxext-dev libxrandr-dev \
    libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
    libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev \
    libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev libthai-dev
RUN apt install -y libdecor-0-dev libpipewire-0.3-dev libwayland-dev \
    wayland-protocols

WORKDIR /tmp/sdl
RUN git clone --depth 1 --branch $SDL_TAG https://github.com/libsdl-org/SDL.git .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_INSTALL_LIBDIR=lib
RUN cmake --build build ${JOBS:+-j $JOBS}
RUN cmake --install build

FROM ${BASE}

# The tzdata package isn't docker-friendly, and something pulls it.
ENV DEBIAN_FRONTEND noninteractive
ENV TZ Etc/GMT

RUN apt update
RUN apt dist-upgrade -y

# Utility packages
RUN apt install -y git
RUN apt install -y make
RUN apt install -y pkg-config

# Compilers & base libraries
RUN apt install -y g++

# 24.04's g++ is 13, which runs the interpreter about 3% slower and the dynarec
# about 5% slower than 15. The noble tag installs a newer g++ from the toolchain
# PPA and makes it the default. Its libstdc++ is newer than 24.04's, so whatever
# ships from this image has to link it statically.
ARG GXX_VERSION=
RUN if [ -n "$GXX_VERSION" ]; then \
      apt install -y software-properties-common && \
      add-apt-repository -y ppa:ubuntu-toolchain-r/test && \
      apt update && \
      apt install -y gcc-$GXX_VERSION g++-$GXX_VERSION && \
      update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-$GXX_VERSION 100 && \
      update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-$GXX_VERSION 100 && \
      update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-$GXX_VERSION 100 && \
      update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-$GXX_VERSION 100; \
    fi

# Development packages
RUN apt install -y libavcodec-dev
RUN apt install -y libavformat-dev
RUN apt install -y libavutil-dev
RUN apt install -y libcapstone-dev
RUN apt install -y libcurl4-openssl-dev
RUN apt install -y libfreetype-dev
# These used to come in with libsdl3-dev, and the Makefile links them directly.
RUN apt install -y libgl-dev libx11-dev libxcb1-dev
# Also came in with libsdl3-dev. The AppImage expects it from the host, and
# nugget's CI runs the AppImage in this image.
RUN apt install -y libasound2t64
RUN apt install -y libswresample-dev
RUN apt install -y libuv1-dev
RUN apt install -y zlib1g-dev

# CI utilities
RUN apt install -y curl wget
RUN apt install -y xvfb
RUN apt install -y python3-pip python3-setuptools patchelf desktop-file-utils libgdk-pixbuf-2.0-dev fakeroot strace
RUN apt install -y glibc-tools lcov
RUN apt install -y file

# AppImage dependencies
RUN apt install -y libfuse2t64
RUN apt install -y fuse

# AppImage tools
RUN apt install -y imagemagick gtk-update-icon-cache appstream
RUN apt install -y squashfs-tools zip zsync

# The mips toolchain, built from source in the first stage. Ubuntu dropped the
# mipsel cross compilers, so there's no distribution package to install here.
COPY --from=toolchain /usr/local /usr/local

# SDL3, from the stage above.
COPY --from=sdl /usr/local /usr/local
RUN ldconfig

# Both projects name their releases after uname's spelling of the architecture,
# which is also the one the AppImage ends up carrying.
WORKDIR /tmp
RUN wget https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-`uname -m`.AppImage && \
    chmod +x appimagetool-`uname -m`.AppImage && \
    mv appimagetool-`uname -m`.AppImage /usr/bin/appimagetool
RUN wget https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-`uname -m`.AppImage && \
    chmod +x linuxdeploy-`uname -m`.AppImage && \
    mv linuxdeploy-`uname -m`.AppImage /usr/bin/linuxdeploy

# xmake resolves fmt from its own repository, which needs cmake and ninja to
# build it. Without these it fetches and builds cmake from source on every clean
# run, which is by far the slowest thing in the job.
RUN apt install -y cmake ninja-build

# xmake, built from source rather than dropped in as a release binary. There is
# no prebuilt linux/arm64 build published at all, and the x86_64 one wants
# libncurses, which isn't in here. The source build only needs make and g++,
# works the same on both architectures, and this image is rebuilt rarely enough
# that the couple of minutes are free.
ARG XMAKE_VERSION=3.1.1
RUN wget https://github.com/xmake-io/xmake/releases/download/v$XMAKE_VERSION/xmake-v$XMAKE_VERSION.tar.gz && \
    tar xzf xmake-v$XMAKE_VERSION.tar.gz && \
    cd xmake-$XMAKE_VERSION && \
    ./configure && \
    make -j `nproc` && \
    make install PREFIX=/usr/local && \
    cd /tmp && \
    rm -rf xmake-$XMAKE_VERSION xmake-v$XMAKE_VERSION.tar.gz

# Links the package to the repository, so its permissions follow from there
# rather than from a settings page somebody has to remember.
LABEL org.opencontainers.image.source=https://github.com/grumpycoders/pcsx-redux

RUN mkdir /project
ENV HOME=/home/coder
RUN mkdir -p /home/coder/dconf
RUN chmod a+rwx /home/coder/dconf

WORKDIR /project
