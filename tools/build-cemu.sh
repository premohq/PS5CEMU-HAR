#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Configures and builds Cemu for the PS5 (build/cemu), with the port's host code from port/.
#
#   tools/build-cemu.sh              apply patches/cemu if needed, configure once, build and link
#                                    build/cemu/ps5cemu.elf (tools/link.sh; PS5CEMU_LINK_CHECK=1 links
#                                    a check without RADV)
#   tools/build-cemu.sh --reconfigure  start the CMake build directory over
#   tools/build-cemu.sh TARGET...    build only those targets
#
# Cemu itself stays at its pinned commit in .deps/Cemu; patches/cemu/*.patch are applied on a
# branch named ps5 (tools/cemu-patches.sh).

set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
cemu=$PS5CEMU_ROOT/.deps/Cemu
build=$PS5CEMU_BUILD/cemu

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

bash "$PS5CEMU_ROOT/tools/cemu-patches.sh" apply
bash "$PS5CEMU_ROOT/tools/reshade-patches.sh" apply # its effect compiler (port/cemu/ReShadeEffects.cpp)

options=(
    -DCMAKE_TOOLCHAIN_FILE="$PS5CEMU_TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release
    -DCEMU_PS5=ON -DCEMU_PS5_PORT_DIR="$PS5CEMU_ROOT/port"
    -DENABLE_VCPKG=OFF -DENABLE_WXWIDGETS=OFF -DENABLE_OPENGL=OFF -DENABLE_METAL=OFF -DENABLE_VULKAN=ON
    -DENABLE_DISCORD_RPC=OFF -DENABLE_HIDAPI=OFF -DENABLE_SDL=OFF -DENABLE_LIBUSB=OFF -DENABLE_CUBEB=OFF
    -DALLOW_PORTABLE=OFF -DBoost_USE_STATIC_LIBS=ON -DBoost_ROOT="$PS5CEMU_SYSROOT"
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=OFF
    # Azahar's core, when tools/build-azahar.sh has built it (port/CMakeLists.txt)
    -DPS5CEMU_AZAHAR_LIBRARIES="$PS5CEMU_BUILD/azahar/azahar_ps5_libraries.txt"
    # melonDS's core, when tools/build-melonds.sh has built it (port/CMakeLists.txt)
    -DPS5CEMU_MELONDS_LIBRARIES="$PS5CEMU_BUILD/melonds/melonds_ps5_libraries.txt"
)
# configured again when the options change (this script's), as CMake would not know
if [[ ! -f $build/build.ninja || $(cat "$build/ps5-options" 2>/dev/null) != "${options[*]}" ]]; then
    mkdir -p "$build"
    cmake -S "$cemu" -B "$build" -G Ninja -Wno-dev "${options[@]}" \
        >"$build.configure.log" 2>&1 || { tail -40 "$build.configure.log"; exit 1; }
    echo "${options[*]}" >"$build/ps5-options"
    echo "$toolchain" >"$build/ps5-toolchain"
fi

# ps5cemu.elf is relinked when a link check gives way to RADV (tools/link.sh), or the other way round,
# and when RADV is another archive than the one it was linked with (other patches/mesa, or
# RADV_ARCHIVE): ninja does not see the difference
elf=$build/ps5cemu.elf
if [[ -f $elf.linkcheck && ${PS5CEMU_LINK_CHECK:-0} != 1 ]] || [[ -f $elf && ! -f $elf.linkcheck && ${PS5CEMU_LINK_CHECK:-0} == 1 ]]; then
    rm -f "$elf" "$elf.linkcheck"
fi
if [[ -f $elf && ${PS5CEMU_LINK_CHECK:-0} != 1 && $(cat "$elf.radv" 2>/dev/null) != "$(ps5cemu_radv_stamp)" ]]; then
    echo "==> [cemu] RADV is not the archive ps5cemu.elf was linked with: linking again"
    rm -f "$elf"
fi

ninja -C "$build" -j "$JOBS" "$@"
