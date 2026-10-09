#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Assembles the app folder from build/cemu/ps5cemu.elf, as PS5_Vulkan packages a RADV title
# (tools/build-radv-title.sh):
#
#   build/app/PPSA99360/              copied to /data/homebrew/PPSA99360 on the console
#     eboot.bin                       the ELF, converted and fake-signed by ps5-native-tool
#     sce_sys/param.json, icon0.png   the title's parameters and home screen tile
#     sce_sys/pic0.dds, pic1.dds      its home screen background (selected, starting)
#     sce_module/libc.prx             the boilerplate's clean-room runtime
#     sandbox-elevator.elf            the boilerplate's /data helper, built for PPSA99360
#     assets/ui/                      the launcher's font (fonts/lexend.sdf), the in-game menus'
#                                     (fonts/Lexend-*.ttf), sounds and the two sides' tiles
#     assets/compatibility.md         the compatibility list, for the game pages' status
#     assets/cemu/                    Cemu's game profiles and the Wii U system fonts
#     assets/graphicPacks/            the community graphic packs (installed on first start)
#     assets/gametdb/                 GameTDB's game information (descriptions, publishers, dates)
#
#   tools/package.sh          the app; the ELF must be linked with RADV (tools/link.sh)
#   tools/package.sh --check  the same steps for a link check's ELF, into build/app-check: not an app

set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
title=PPSA99360
check=0
[[ ${1:-} == --check ]] && check=1
elf=$PS5CEMU_BUILD/cemu/ps5cemu.elf
deps=$PS5CEMU_ROOT/.deps
vulkan=$deps/PS5_Vulkan
boilerplate=$PS5CEMU_BOILERPLATE
work=$PS5CEMU_BUILD/package
app=$PS5CEMU_BUILD/$( ((check)) && echo app-check || echo app)/$title

[[ -f $elf ]] || { echo "$elf is missing: build it first (make build)" >&2; exit 2; }
if [[ -f $elf.linkcheck ]] && ((!check)); then
    echo "$elf is a link check without RADV, not an app: build RADV (make radv), then make build" >&2
    exit 2
fi
mkdir -p "$work"

# The host tool that converts and signs the ELF: PS5_Vulkan's native tooling, which its RADV
# titles go through, with the boilerplate's zlib.
tool=$PS5CEMU_BUILD/host/ps5-native-tool
native=$vulkan/tooling/native
zlib=$boilerplate/.deps/native/zlib/root
if [[ ! -x $tool ]] || [[ -n $(find "$native" -newer "$tool" -name '*.[ch]*' -print -quit) ]]; then
    mkdir -p "$(dirname "$tool")"
    clang++-18 -std=c++20 -O2 -Wall -Wextra -I "$zlib/usr/include" "$native/native_app_builder.cpp" \
        "$native/self_container.cpp" "$native/elf_object.cpp" "$native/sce_module_writer.cpp" \
        "$zlib/usr/lib/libz.a" -o "$tool"
fi

# The runtime libc.prx: the boilerplate's clean-room build, checked against its recorded digest.
[[ -f $boilerplate/runtime/libc.prx ]] || USE_CCACHE=${USE_CCACHE:-0} bash "$boilerplate/tools/rebuild-libc.sh"
(cd "$boilerplate/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)

# eboot.bin: the AGC link stubs RADV was linked with name its imports from those modules
sdk=$PS5_PAYLOAD_SDK
stubs=()
if ((!check)); then
    sdk=${RADV_SDK:-$vulkan/.deps/native/ps5-payload-sdk}
    stubs=(--stub "$PS5CEMU_BUILD/link/libSceAgc.so" --stub "$PS5CEMU_BUILD/link/libSceAgcDriver.so")
fi
"$tool" link --in "$elf" --out "$work/eboot.elf" --stub-dir "$sdk/target/lib" "${stubs[@]}" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
rm -rf "$app"
mkdir -p "$app/sce_sys" "$app/sce_module" "$app/assets"
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$boilerplate/runtime/libc.prx" "$app/sce_module/libc.prx"
python3 -B "$PS5CEMU_ROOT/tools/render-icons.py" "$work/icons"
cp "$PS5CEMU_ROOT/sce_sys/icon0.png" "$app/sce_sys/" # tools/render-presentation.py's, committed
# param.json with the Makefile's VERSION, the one place the release's version is set: 2.1.3d is
# contentVersion 02.001.304 (the patch number in hundreds, then the letter: none 0, a 1, b 2...) and
# masterVersion 02.01
version=$(sed -n 's/^VERSION := *\([^ ]*\).*/\1/p' "$PS5CEMU_ROOT/Makefile")
python3 - "$PS5CEMU_ROOT/sce_sys/param.json" "$app/sce_sys/param.json" "$version" <<'PY'
import json, re, sys
source, target, version = sys.argv[1:4]
match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)([a-z]?)", version)
if not match:
    sys.exit(f"the Makefile's VERSION ({version!r}) is not like 2.0.0 or 2.0.0d")
major, minor, patch = (int(part) for part in match.groups()[:3])
letter = ord(match[4]) - ord("a") + 1 if match[4] else 0
if major > 99 or minor > 99 or patch > 9:
    sys.exit(f"{version} does not fit param.json's version numbers")
param = json.load(open(source))
param["contentVersion"] = f"{major:02d}.{minor:03d}.{patch * 100 + letter:03d}"
param["masterVersion"] = f"{major:02d}.{minor:02d}"
with open(target, "w", newline="\n") as out:
    json.dump(param, out, indent=2)
    out.write("\n")
print(f"==> [package] version {version}: contentVersion {param['contentVersion']}, masterVersion {param['masterVersion']}")
PY
# the home screen's background while PS5Cemu is selected and while it starts: one picture
# (tools/render-presentation.py renders sce_sys/pic0.dds), in the form the boilerplate checks
cp "$PS5CEMU_ROOT/sce_sys/pic0.dds" "$app/sce_sys/pic0.dds"
cp "$PS5CEMU_ROOT/sce_sys/pic0.dds" "$app/sce_sys/pic1.dds"
bash "$boilerplate/tools/validate-assets.sh" "$app/sce_sys" >/dev/null

# The /data helper elfldr runs when the HEN does not jailbreak PS5Cemu (port/ps5/privilege.h).
# It serves one title only: the boilerplate's, made PS5Cemu's.
helper=$work/elevation
rm -rf "$helper"
mkdir -p "$helper/payload"
cp "$boilerplate/examples/sandbox-elevation/protocol.hpp" "$helper/"
cp "$boilerplate/examples/sandbox-elevation/payload/Makefile" "$helper/payload/"
sed "s/PPSA99790/$title/" "$boilerplate/examples/sandbox-elevation/payload/main.cpp" >"$helper/payload/main.cpp"
grep -q "\"$title\"" "$helper/payload/main.cpp" || { echo "the elevation helper's title ID moved" >&2; exit 1; }
make -s -C "$helper/payload" PS5_PAYLOAD_SDK="$PS5_PAYLOAD_SDK" OUTPUT="$app/sandbox-elevator.elf"
python3 -B "$boilerplate/tools/validate-elevation-helper.py" "$app/sandbox-elevator.elf" >/dev/null

# The launcher (port/ui, docs/UI-REDESIGN.md): Lexend's four weights as one signed-distance atlas
# (tools/render-sdf-font.sh, committed), three as TrueType for the in-game menus' ImGui
# (tools/render-menu-fonts.py, committed) and their licence, the menu's sounds and the music
# (tools/render-sounds.py, committed), and the two sides' tiles the bar shows (tools/render-icons.py)
ui=$app/assets/ui
mkdir -p "$ui/fonts" "$ui/sounds" "$ui/icons"
cp "$PS5CEMU_ROOT/port/ui/fonts/lexend.sdf" "$PS5CEMU_ROOT"/port/ui/fonts/Lexend-*.ttf "$PS5CEMU_ROOT/tools/fonts/OFL.txt" "$ui/fonts/"
cp "$PS5CEMU_ROOT"/port/frontend/ui/sounds/*.wav "$ui/sounds/"
cp "$work/icons/ui/icons/ps5cemu-72.tga" "$work/icons/ui/icons/azahar-72.tga" "$work/icons/ui/icons/melonds-72.tga" "$ui/icons/"

# Cemu's read-only data: game profiles, and the Wii U's system fonts games draw text with.
mkdir -p "$app/assets/cemu/resources"
cp -a "$deps/Cemu/bin/gameProfiles" "$app/assets/cemu/"
# the certificate authorities curl checks GitHub against for the update notice (app/updates.cpp):
# the console has no store of its own, so the build system's goes along
cp /etc/ssl/certs/ca-certificates.crt "$app/assets/cacert.pem"
# GameTDB's game information for the details page and the in-game menus (tools/render-gametdb.py,
# committed)
mkdir -p "$app/assets/gametdb"
cp "$PS5CEMU_ROOT"/port/app/gametdb/*.tsv.gz "$app/assets/gametdb/"
# the compatibility list the game pages show a status from (app/compatibility.h)
cp "$PS5CEMU_ROOT/docs/COMPATIBILITY.md" "$app/assets/compatibility.md"
# the 3DS side's border themes (tools/render-borders.py, committed)
mkdir -p "$app/assets/borders"
cp "$PS5CEMU_ROOT"/port/azahar/borders/*.tga "$app/assets/borders/"
cp -a "$deps/Cemu/bin/resources/sharedFonts" "$app/assets/cemu/resources/"

# The community graphic packs, with the version file Cemu's downloader writes.
python3 - "$deps/graphic-packs/graphicPacks987.zip" "$app/assets/graphicPacks" <<'PY'
import os, sys, zipfile
archive, target = sys.argv[1:3]
with zipfile.ZipFile(archive) as packs:
    packs.extractall(target)
with open(os.path.join(target, "version.txt"), "w") as version:
    version.write("Github987")
PY

if ((check)); then
    echo "LINK CHECK ONLY: built without RADV, this folder cannot run on a console." >"$app/NOT-AN-APP.txt"
fi
# Every file and folder readable, writable and runnable by all (0777): the console starts an app only
# then (with any other mode it answers "Can't start the game or app", CE-107750-0), and that is how
# files copied over to it arrive. The release ZIP keeps these modes, so a file manager that honours
# them, rather than writing 0777 itself, now installs an app that starts (#22: every ZIP so far had
# eboot.bin at 0644).
chmod -R 0777 "$app"
"$tool" self --inspect --file "$app/eboot.bin" >"$work/eboot-inspection.txt"
echo "==> [package] $app ($(du -sh "$app" | cut -f1), eboot.bin $(stat -c %s "$app/eboot.bin") bytes)"
