#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The app's own shaders as SPIR-V headers, which are committed: the build needs no shader compiler.
# Run it again after changing a shader. Needs glslangValidator (glslang-tools).
#
#   port/ui/shaders/*.vert, *.frag       the UI kit's: port/ui/shaders.h (ui::shaders)
#   port/melonds/shaders/*.vert, *.frag  the DS's screens on the TV: port/melonds/shaders.h (ps5melonds::shaders)

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# render FOLDER HEADER NAMESPACE WHAT
render() {
    local folder=$1 out=$2 namespace=$3 what=$4
    {
        echo "// SPDX-License-Identifier: GPL-3.0-or-later"
        echo "// Written by tools/render-shaders.sh from ${folder#"$root"/}: $what as SPIR-V."
        echo
        echo "#pragma once"
        echo
        echo "#include <cstdint>"
        echo
        echo "namespace $namespace"
        echo "{"
    } >"$out"
    for shader in "$folder"/*.vert "$folder"/*.frag; do
        name=$(basename "$shader" | tr '.' '_')
        glslangValidator -V --target-env vulkan1.0 --vn "k_$name" -o "$work/$name.h" "$shader" >/dev/null
        # glslang's header, without its comment and with the array in the namespace
        grep -v '^\s*//' "$work/$name.h" | sed -e 's/^const uint32_t/\tinline constexpr uint32_t/' -e 's/^\([0-9x]\)/\t\t\1/' -e 's/^};/\t};/' -e '/^\s*$/d' -e '/#pragma once/d' >>"$out"
        echo >>"$out"
    done
    echo "}" >>"$out"
    echo "${out#"$root"/}: $(grep -c 'inline constexpr' "$out") shaders"
}

render "$root/port/ui/shaders" "$root/port/ui/shaders.h" "ui::shaders" "the UI kit's shaders"
render "$root/port/melonds/shaders" "$root/port/melonds/shaders.h" "ps5melonds::shaders" "the DS's screens' shaders"
