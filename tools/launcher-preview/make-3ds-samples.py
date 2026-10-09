#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write sample 3DS and DS files for the launcher's preview, with nothing but the standard library.

    make-3ds-samples.py 3DS_FOLDER DS_FOLDER

Each is only what Azahar's side's library reads (port/azahar/library.cpp): an SMDH (names,
publisher, a 48x48 icon) where each kind of file keeps it, in an NCSD's NCCH ExeFS (.3ds, .cci),
an NCCH's (.cxi), a CIA's meta section, a 3DSX's extended header, and one encrypted dump, which
the library lists by its file name; and for the DS side's games (port/melonds/library.cpp), a DS
header and its banner (titles, a 32x32 icon in 16 colours). They hold no game.
"""

import os
import struct
import sys


def smdh(title, publisher, colour):
    data = bytearray(0x36C0)
    data[0:4] = b"SMDH"
    for language in range(16):
        entry = 0x8 + language * 0x200
        for offset, text, size in ((0, title, 0x80), (0x80, title, 0x100), (0x180, publisher, 0x80)):
            encoded = text.encode("utf-16-le")[:size - 2]
            data[entry + offset:entry + offset + len(encoded)] = encoded
    # the large icon: the colour, lighter towards the top, with a white disc; RGB565 in 8x8 tiles
    for y in range(48):
        for x in range(48):
            shade = 1.0 - y / 96.0
            r, g, b = (int(c * shade) for c in colour)
            if (x - 24) ** 2 + (y - 24) ** 2 < 11 ** 2:
                r, g, b = 250, 250, 250
            value = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)
            tile = (y // 8) * 6 + x // 8
            tx, ty = x % 8, y % 8
            morton = (tx & 1) | (ty & 1) << 1 | (tx & 2) << 1 | (ty & 2) << 2 | (tx & 4) << 2 | (ty & 4) << 3
            struct.pack_into("<H", data, 0x24C0 + (tile * 64 + morton) * 2, value)
    return bytes(data)


def ncch(title_id, icon, encrypted=False, product=""):
    """An NCCH with an ExeFS holding the icon, 0x400 + the ExeFS; its product code (CTR-P-AREE)
    gives the box art's ID."""
    header = bytearray(0x400)
    header[0x100:0x104] = b"NCCH"
    header[0x150:0x160] = product.encode().ljust(16, b"\0")
    struct.pack_into("<Q", header, 0x108, title_id)
    struct.pack_into("<Q", header, 0x118, title_id)
    header[0x18F] = 0 if encrypted else 0x4  # NoCrypto
    struct.pack_into("<II", header, 0x1A0, 2, (0x200 + len(icon) + 0x1FF) // 0x200)
    exefs = bytearray(0x200)
    exefs[0:4] = b"icon"
    struct.pack_into("<II", exefs, 8, 0, len(icon))
    return bytes(header) + bytes(exefs) + icon


def ncsd(title_id, icon, encrypted=False, product=""):
    header = bytearray(0x4000)
    header[0x100:0x104] = b"NCSD"
    partition = ncch(title_id, icon, encrypted, product)
    struct.pack_into("<II", header, 0x120, 0x4000 // 0x200, (len(partition) + 0x1FF) // 0x200)
    return bytes(header) + partition


def cia(title_id, version, icon):
    align = lambda value: (value + 63) & ~63
    certs, ticket, tmd, content, meta = 0xA00, 0x350, 0xB34, 0x100, 0x3AC0
    header = bytearray(0x2020)
    struct.pack_into("<IHHIIIIQ", header, 0, 0x2020, 0, 0, certs, ticket, tmd, meta, content)
    data = bytearray(align(0x2020) + align(certs) + align(ticket) + align(tmd) + align(content) + meta)
    data[0:len(header)] = header
    tmd_at = align(0x2020) + align(certs) + align(ticket)
    data[tmd_at:tmd_at + 4] = b"\x00\x01\x00\x04"  # RSA-2048, SHA-256
    struct.pack_into(">Q", data, tmd_at + 0x140 + 0x4C, title_id)
    struct.pack_into(">H", data, tmd_at + 0x140 + 0x9C, version)
    meta_at = tmd_at + align(tmd) + align(content)
    data[meta_at + 0x400:meta_at + 0x400 + len(icon)] = icon
    return bytes(data)


def threedsx(icon):
    header = struct.pack("<4sHHIIIIIIIII", b"3DSX", 0x2C, 8, 0, 0, 0, 0, 0, 0, 0x2C, len(icon), 0)
    return header + icon


def nds(code, title, colour, unit=0):
    """A DS ROM's header and banner: the game code (its box's ID), the banner's titles (name, then
    publisher, on lines of their own) and its icon, 4 bits a pixel in 8x8 tiles with 16 BGR555
    colours, the first transparent: the colour, a white disc on it."""
    banner = bytearray(0xA40)
    struct.pack_into("<H", banner, 0, 1)
    shades = [(int(c * (1.0 - i / 20.0)) for c in colour) for i in range(14)]
    palette = [0, 0x7FFF] + [(b >> 3) << 10 | (g >> 3) << 5 | (r >> 3) for r, g, b in shades]
    for i, value in enumerate(palette):
        struct.pack_into("<H", banner, 0x220 + i * 2, value)
    for y in range(32):
        for x in range(32):
            inside = 2 <= x < 30 and 2 <= y < 30
            index = 0 if not inside else 1 if (x - 16) ** 2 + (y - 16) ** 2 < 7 ** 2 else 2 + y * 14 // 32
            tile = (y // 8) * 4 + x // 8
            at = 0x20 + tile * 32 + (y % 8) * 4 + (x % 8) // 2
            banner[at] |= index << 4 if x & 1 else index
    for language in range(6):
        encoded = title.encode("utf-16-le")[:0xFE]
        banner[0x240 + language * 0x100:0x240 + language * 0x100 + len(encoded)] = encoded
    header = bytearray(0x4000)
    header[0:12] = title.split("\n")[0].upper().encode("ascii", "replace")[:12].ljust(12, b"\0")
    header[0x0C:0x10] = code.encode()
    header[0x10:0x12] = b"01"
    header[0x12] = unit
    struct.pack_into("<IIII", header, 0x20, 0x4000, 0x02000000, 0x02000000, 0x100)
    struct.pack_into("<I", header, 0x68, len(header))
    struct.pack_into("<H", header, 0x15E, sum(header[:0x15E]) & 0xFFFF)
    return bytes(header) + bytes(banner) + bytes(0x100)


SAMPLES = [
    ("Super Mario 3D Land.3ds", lambda i: ncsd(0x0004000000053F00, i, product="CTR-P-AREE"), "Super Mario 3D Land", "Nintendo", (220, 40, 40)),
    ("Mario Kart 7.cci", lambda i: ncsd(0x0004000000030600, i, product="CTR-P-AMKE"), "Mario Kart 7", "Nintendo", (240, 160, 20)),
    ("Pokémon Ultra Sun.3ds", lambda i: ncsd(0x00040000001B5000, i), "Pokémon Ultra Sun", "Nintendo", (240, 120, 30)),
    ("Fire Emblem Awakening.cxi", lambda i: ncch(0x00040000000A0500, i), "Fire Emblem Awakening", "Nintendo", (40, 80, 180)),
    ("Kirby Planet Robobot.cia", lambda i: cia(0x000400000017C100, 1040, i), "Kirby: Planet Robobot", "Nintendo", (240, 120, 180)),
    ("Kirby Planet Robobot Update.cia", lambda i: cia(0x0004000E0017C100, 1040, i), "Kirby: Planet Robobot", "Nintendo", (240, 120, 180)),
    ("Luigi's Mansion Dark Moon.3ds", lambda i: ncsd(0x0004000000055F00, i, encrypted=True), "", "", (40, 160, 60)),
    ("Checkpoint.3dsx", lambda i: threedsx(i), "Checkpoint", "Bernardo Giordano, FlagBrew", (60, 60, 60)),
    ("FBI.3dsx", lambda i: threedsx(i), "FBI", "Steveice10", (30, 120, 200)),
    ("More/Kid Icarus Uprising.3ds", lambda i: ncsd(0x0004000000030200, i), "Kid Icarus: Uprising", "Nintendo", (230, 210, 90)),
]

# the DS side's games: file, game code, banner title (name, subtitle, publisher), colour
DS_SAMPLES = [
    ("New Super Mario Bros.nds", "A2DE", "New SUPER MARIO BROS.\nNintendo", (200, 40, 30)),
    ("Zelda Phantom Hourglass.nds", "AZEE", "The Legend of Zelda\nPhantom Hourglass\nNintendo", (40, 150, 70)),
    ("DS/Mario Kart DS.nds", "AMCE", "MARIO KART DS\nNintendo", (220, 60, 40)),
    ("DS/Pokemon HeartGold.nds", "IPKE", "POKEMON HG\nNintendo", (210, 170, 40), 2),
]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    for name, make, title, publisher, colour in SAMPLES:
        path = os.path.join(sys.argv[1], name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as file:
            file.write(make(smdh(title, publisher, colour)))
    for name, code, title, colour, *unit in DS_SAMPLES:
        path = os.path.join(sys.argv[2], name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as file:
            file.write(nds(code, title, colour, *unit))


if __name__ == "__main__":
    main()
