#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The port's changes to melonDS's own files, kept as patches/melonds/*.patch against the pinned
# commit.
#
#   tools/melonds-patches.sh apply    put them on a branch named ps5 in .deps/melonDS (once)
#   tools/melonds-patches.sh export   write the ps5 branch's commits back to patches/melonds
#
# To change melonDS: edit .deps/melonDS on the ps5 branch, commit, then export.

set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
melonds=$PS5CEMU_ROOT/.deps/melonDS
patches=$PS5CEMU_ROOT/patches/melonds
pin=$(python3 -c 'import json,sys; print(next(i["commit"] for i in json.load(open(sys.argv[1]))["items"] if i["name"] == "melonds"))' \
    "$PS5CEMU_ROOT/tools/deps.json")
identity=(-c user.name=ps5cemu -c user.email=ps5cemu@localhost)

case ${1:-} in
apply)
    if git -C "$melonds" rev-parse -q --verify refs/heads/ps5 >/dev/null; then
        exit 0 # already there (and possibly being worked on)
    fi
    git -C "$melonds" checkout -q -b ps5 "$pin"
    shopt -s nullglob
    files=("$patches"/*.patch)
    if ((${#files[@]})); then
        git "${identity[@]}" -C "$melonds" am -q --keep-cr "${files[@]}"
    fi
    echo "==> [melonds] applied ${#files[@]} patches on branch ps5"
    ;;
export)
    [[ -z $(git -C "$melonds" status --porcelain --untracked-files=no) ]] ||
        { echo "commit the changes in .deps/melonDS first" >&2; exit 1; }
    rm -f "$patches"/*.patch
    mkdir -p "$patches"
    git -C "$melonds" format-patch -q --no-signature --zero-commit --no-numbered -o "$patches" "$pin..ps5"
    ls "$patches"
    ;;
*)
    echo "usage: tools/melonds-patches.sh apply|export" >&2
    exit 2
    ;;
esac
