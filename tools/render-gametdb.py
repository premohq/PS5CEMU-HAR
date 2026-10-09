#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""GameTDB's game information for the launcher and the in-game menus: each game's description,
developer, publisher, release date, genre, players and rating, by the ID on its box (ALZE01 for a
Wii U game, AREE for a 3DS one, AMCE for a DS one, as for the box art).

    render-gametdb.py [OUTPUT_FOLDER] [--from FOLDER] [--only wiiu|3ds|ds]

Downloads GameTDB's English Wii U, 3DS and DS databases (www.gametdb.com/wiiutdb.zip, 3dstdb.zip and
dstdb.zip), or reads them from FOLDER, and writes wiiu.tsv.gz, 3ds.tsv.gz and ds.tsv.gz (default:
port/app/gametdb), or only the one --only names: one game a line, sorted by ID, its fields separated
by tabs, with tabs, line breaks and backslashes in them written as \\t, \\n and \\\\:

    id  title  region  synopsis  developer  publisher  released  genre  players  rating

released is YYYY, YYYY-MM or YYYY-MM-DD; genre GameTDB's own list ("action,adventure"); rating its
board and age ("ESRB E10+"). The files are committed, so a build needs no network; run this again
for GameTDB's newest. The information is GameTDB's and its contributors' (https://www.gametdb.com),
which the app credits. Needs only Python's standard library.
"""

import gzip
import io
import os
import re
import sys
import urllib.request
import xml.etree.ElementTree as ElementTree
import zipfile

SOURCES = {"wiiu": "wiiutdb", "3ds": "3dstdb", "ds": "dstdb"}
URL = "https://www.gametdb.com/{}.zip?LANG=EN"


def text(element):
    """An element's text with its runs of spaces closed up and its paragraphs kept."""
    if element is None or element.text is None:
        return ""
    value = element.text.replace("\r", "")
    lines = [re.sub(r"[ \t\u00a0]+", " ", line).strip() for line in value.split("\n")]
    # paragraphs: one blank line between them at most
    out = re.sub(r"\n{3,}", "\n\n", "\n".join(lines)).strip()
    return out


def released(game):
    date = game.find("date")
    if date is None:
        return ""
    year, month, day = (date.get(part, "") for part in ("year", "month", "day"))
    if not year.isdigit() or int(year) == 0:
        return ""
    if not month.isdigit() or not 1 <= int(month) <= 12:
        return year
    if not day.isdigit() or not 1 <= int(day) <= 31:
        return f"{year}-{int(month):02d}"
    return f"{year}-{int(month):02d}-{int(day):02d}"


def field(value):
    return value.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")


def convert(xml, out_path):
    root = ElementTree.fromstring(xml)
    rows = {}
    for game in root.iter("game"):
        game_id = (game.findtext("id") or "").strip().upper()
        if not re.fullmatch(r"[A-Z0-9]{4}|[A-Z0-9]{6}", game_id) or game_id in rows:
            continue
        locale = next((l for l in game.findall("locale") if l.get("lang") == "EN"), None)
        title = text(locale.find("title")) if locale is not None else ""
        synopsis = text(locale.find("synopsis")) if locale is not None else ""
        rating = game.find("rating")
        rating_text = ""
        if rating is not None and rating.get("value"):
            rating_text = f"{rating.get('type', '')} {rating.get('value')}".strip()
        players = game.find("input")
        player_count = players.get("players", "") if players is not None else ""
        row = [game_id, title, text(game.find("region")), synopsis, text(game.find("developer")),
               text(game.find("publisher")), released(game), text(game.find("genre")),
               player_count if player_count.isdigit() and player_count != "0" else "", rating_text]
        if not any(row[1:]):
            continue
        rows[game_id] = "\t".join(field(value) for value in row)
    with gzip.GzipFile(out_path, "wb", compresslevel=9, mtime=0) as out:
        out.write(("\n".join(rows[key] for key in sorted(rows)) + "\n").encode("utf-8"))
    return len(rows)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    source = None
    if "--from" in sys.argv:
        source = sys.argv[sys.argv.index("--from") + 1]
        args = [a for a in args if a != source]
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1]
        args = [a for a in args if a != only]
        if only not in SOURCES:
            sys.exit(f"--only takes one of {', '.join(SOURCES)}")
    out = args[0] if args else os.path.join(os.path.dirname(__file__), "..", "port", "app", "gametdb")
    os.makedirs(out, exist_ok=True)
    for system, name in SOURCES.items():
        if only and system != only:
            continue
        if source:
            with open(os.path.join(source, f"{name}.zip"), "rb") as file:
                data = file.read()
        else:
            request = urllib.request.Request(URL.format(name), headers={"User-Agent": "PS5CEMU-HAR"})
            with urllib.request.urlopen(request, timeout=120) as response:
                data = response.read()
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            xml = archive.read(f"{name}.xml")
        version = re.search(rb'version="(\d+)"', xml)
        path = os.path.join(out, f"{system}.tsv.gz")
        count = convert(xml, path)
        print(f"{path}: {count} games from GameTDB's {name} {version[1].decode() if version else ''} "
              f"({os.path.getsize(path) // 1024} KiB)")


if __name__ == "__main__":
    main()
