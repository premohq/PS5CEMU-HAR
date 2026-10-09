#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Configures and builds melonDS's core for the PS5 (build/melonds): the Nintendo DS emulator the 3DS
# side starts DS games with, from .deps/melonDS with patches/melonds, with the toolchain Cemu's build
# uses (tools/ps5.cmake), and PS5CEMU-HAR's frontend for it (port/melonds: the DS's platform, the
# DualSense, AudioOut). melonDS's own frontend (Qt and SDL) is not built, nor its OpenGL renderers:
# the DS's 3D is its software renderer, on a thread of its own. build/melonds/melonds_ps5_libraries.txt
# then lists the archives the app links (tools/build-cemu.sh).
#
#   tools/build-melonds.sh               apply patches/melonds if needed, configure, build
#   tools/build-melonds.sh --reconfigure start the build directory over
#   tools/build-melonds.sh TARGET...     build only those targets
#
# The JIT's code comes from the platform layer RADV is linked with (ps5platform/exec.h, in
# libps5platform.a), whose headers it is given; there is no fast memory (patch 0002). No BIOS or
# firmware is built in but melonDS's own replacements (FreeBIOS): a DS's own dumps are read from
# /data/ps5cemu/melonds/bios when they are there.

set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
melonds=$PS5CEMU_ROOT/.deps/melonDS
build=$PS5CEMU_BUILD/melonds

if [[ ${1:-} == --reconfigure ]]; then
    rm -rf "$build"
    shift
fi
# CMake reads the toolchain's flags (tools/ps5.cmake) only into a new cache: when that file
# changes, the build folder starts over
toolchain=$(sha1sum <"$PS5CEMU_TOOLCHAIN" | cut -c1-40)
if [[ -d $build && $(cat "$build/ps5-toolchain" 2>/dev/null) != "$toolchain" ]]; then
    echo "tools/ps5.cmake changed: $build starts over"
    rm -rf "$build"
fi

bash "$PS5CEMU_ROOT/tools/melonds-patches.sh" apply

options=(
    -DCMAKE_TOOLCHAIN_FILE="$PS5CEMU_TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release
    -DBUILD_QT_SDL=OFF -DENABLE_OGLRENDERER=OFF -DENABLE_GDBSTUB=OFF -DENABLE_JIT=ON -DENABLE_JIT_PROFILING=OFF
    -DENABLE_LTO_RELEASE=OFF -DENABLE_LTO=OFF -DUSE_CCACHE=OFF -DUSE_VCPKG=OFF
    -DPS5_PLATFORM_INCLUDE="$PS5CEMU_ROOT/.deps/PS5_PayloadSDK/platform/include"
    -DPS5_VULKAN_INCLUDE="$PS5CEMU_ROOT/.deps/Cemu/dependencies/Vulkan-Headers/include"
    -DMELONDS_EXTERNAL_FRONTEND="$PS5CEMU_ROOT/port/melonds"
)
# configured again when the options change (this script's), as CMake would not know
if [[ ! -f $build/build.ninja || $(cat "$build/ps5-options" 2>/dev/null) != "${options[*]}" ]]; then
    mkdir -p "$build"
    cmake -S "$melonds" -B "$build" -G Ninja -Wno-dev "${options[@]}" \
        >"$build.configure.log" 2>&1 || { tail -40 "$build.configure.log"; exit 1; }
    echo "${options[*]}" >"$build/ps5-options"
    echo "$toolchain" >"$build/ps5-toolchain"
fi

if (($# == 0)); then
    set -- melonds_ps5_libraries
fi
ninja -C "$build" -j "$JOBS" "$@"
