#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The new launcher's screens on the PC, as PNGs in build/shell-preview, to work on them without a
# console (tools/launcher-preview/shell.cpp says how):
#
#   tools/preview-shell.sh [SCRIPT]    default: tools/launcher-preview/shell-screens.txt
#
# PREVIEW_FIRST=1 starts it as on a first start, PREVIEW_ASK=1 with Start on: Ask each time,
# PREVIEW_UPDATE=1 with a newer release found, PREVIEW_NO_DATA=1 with /data out of reach,
# PREVIEW_LAUNCH_ERROR=1 after a game that did not start; PREVIEW_LANGUAGE=de (a code of app/lang.cpp's, or
# qps, tools/lang.py pseudo's longer text) in that language, PREVIEW_SYSTEM_LANGUAGE=4 with the PS5 set to
# its language number.
#
# Needs what `make deps` fetches (pacbrew's fmt, the sysroot's RapidJSON, ReShade's stb), clang-18,
# python3, and the PC's Vulkan loader with a driver (Mesa's lavapipe will do: libvulkan-dev and
# mesa-vulkan-drivers), FreeType's and zlib's headers (libfreetype-dev, zlib1g-dev); for the languages
# Lexend lacks, the Noto fonts (fonts-noto-core, fonts-noto-cjk) stand in for the console's. What the
# launcher writes (its settings, the catalogue, the 3DS games' icons) goes in build/shell-preview/data,
# in place of /data/ps5cemu. Box art can be tried by putting TGAs in
# build/shell-preview/boxart/<wiiu|3ds>/<ID>.tga.

set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
cd "$PS5CEMU_ROOT"
out=$PS5CEMU_BUILD/shell-preview
script=${1:-tools/launcher-preview/shell-screens.txt}
mkdir -p "$out/include" "$out/obj"
ln -sfn "$PS5CEMU_PACBREW/include/fmt" "$out/include/fmt"

# folders for the folder browsers to show, and 3DS games for Azahar's side to read, as the console
# preview has them
games=$out/games
rm -rf "$games" "$out/data"
mkdir -p "$games/Mario Kart 8" "$games/Splatoon" "$games/Super Mario 3D World [00050000101C9400]" "$out/data"
for title in "BotW Update v208" "BotW DLC"; do
    mkdir -p "$games/Installs/$title/code" "$games/Installs/$title/content" "$games/Installs/$title/meta"
    echo '<menu/>' >"$games/Installs/$title/meta/meta.xml"
done
python3 -B tools/launcher-preview/make-3ds-samples.py "$games/3ds"

# in place of the console's own fonts (the fallback for what Lexend lacks: CJK, Cyrillic, Greek, Thai,
# Arabic), the few the PC has that do the same, where it has them (fonts-noto-core, fonts-noto-cjk)
rm -rf "$out/fonts"
mkdir -p "$out/fonts"
for font in /usr/share/fonts/truetype/noto/NotoSans-Regular.ttf /usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf \
    /usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc; do
    [[ -f $font ]] && ln -s "$font" "$out/fonts/"
done

version=$(sed -n 's/^VERSION := *\([^ ]*\).*/\1/p' Makefile)
flags=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -DFMT_HEADER_ONLY
    -DPS5CEMU_LAUNCHER_PREVIEW "-DPS5CEMU_VERSION=\"$version\"" "-DPS5CEMU_DATA=\"$out/data\""
    -I "$out/include" -I "$PS5CEMU_SYSROOT/include" -I port -I port/app -I tools/launcher-preview
    -I "$PS5CEMU_ROOT/.deps/reshade/deps/stb" $(pkg-config --cflags freetype2))
sources=(tools/launcher-preview/shell.cpp tools/launcher-preview/console.cpp
    port/frontend/shell/home.cpp port/frontend/shell/hub.cpp port/frontend/shell/library.cpp port/frontend/shell/pages.cpp
    port/frontend/shell/settings.cpp port/frontend/shell/setup.cpp port/frontend/shell/shell.cpp port/frontend/shell/widgets.cpp
    port/ui/canvas.cpp port/ui/feedback.cpp port/ui/gfx.cpp port/ui/images.cpp port/ui/input.cpp port/ui/qr.cpp port/ui/text.cpp
    port/ui/vkfn.cpp port/app/catalog.cpp port/app/gameinfo.cpp port/app/lang.cpp port/app/compatibility.cpp port/frontend/actions.cpp
    port/frontend/settings.cpp port/azahar/library.cpp port/azahar/controls.cpp port/azahar/unavailable.cpp)

# each source compiled again when it, or a header of the port's, is newer than its object
newest_header=$(find port tools/launcher-preview -name '*.h' -printf '%T@\n' | sort -n | tail -1)
objects=()
stale=()
for source in "${sources[@]}"; do
    object=$out/obj/$(echo "${source%.cpp}" | tr / _).o
    objects+=("$object")
    if [[ ! -f $object || $source -nt $object ]] || awk -v h="$newest_header" -v o="$(stat -c %Y "$object")" 'BEGIN { exit !(h > o) }'; then
        stale+=("$source:$object")
    fi
done
if ((${#stale[@]})); then
    printf '%s\n' "${stale[@]}" | xargs -P "$JOBS" -I{} bash -c 'source=${1%%:*}; object=${1#*:}; shift; clang++-18 "$@" -c "$source" -o "$object"' _ {} "${flags[@]}"
fi
clang++-18 "${objects[@]}" -lvulkan -lfreetype -lz -lpthread -o "$out/shell-preview"
"$out/shell-preview" "$out" "$script" "$games" "$games/3ds" 2>"$out/preview.log"
