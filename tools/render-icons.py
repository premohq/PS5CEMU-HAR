#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draw PS5CEMU-HAR's icons, with nothing but the standard library.

    render-icons.py OUTPUT_DIR

The three emulators each have a device and a background: Cemu a Wii U GamePad on the Wii U Homebrew
Launcher's blue with its bubbles (tools/render-background.py), Azahar a 3DS on the 3DS Homebrew
Launcher's waves made yellow (as the launcher draws them), under the README banner's dark
overlay (a lighter one on the yellow), and melonDS a DS Lite on melon green with rising pixels.
Writes:

  sce_sys/icon0.png          512x512, opaque: the two side by side (the console's tile is now
                             tools/render-presentation.py's, with the app's name)
  ui/icons/ps5cemu.tga       336x336, and ps5cemu-72.tga: the GamePad on the bubbles (Cemu's side)
  ui/icons/azahar.tga        336x336, and azahar-72.tga: the 3DS on the waves (Azahar's side)
  ui/icons/melonds.tga       336x336, and melonds-72.tga: the DS on the pixels (melonDS's side)
  ui/icons/har-72.tga        72x72: the two side by side (the start screen)
  ui/icons/start-wiiu.tga    the GamePad alone, on nothing, for the start screen's left half
  ui/icons/start-3ds.tga     the 3DS alone, for its right half

Shapes are signed distance functions, so every size is drawn sharp, and edges are anti-aliased over
a pixel.
"""

import importlib.util
import math
import os
import struct
import sys
import zlib

BODY = (0xf4, 0xf8, 0xfc)
DETAIL = (0x6a, 0x7e, 0x93)
# each device's screen gradient and accent: blue for the Wii U, gold for the 3DS
BLUE = {"top": (0x1e, 0x4f, 0x7a), "bottom": (0x0b, 0x1e, 0x33), "accent": (0x9f, 0xd6, 0xff), "detail": DETAIL}
GOLD = {"top": (0x7c, 0x56, 0x10), "bottom": (0x33, 0x22, 0x06), "accent": (0xff, 0xd2, 0x5a), "detail": (0x8f, 0x82, 0x6c)}
# and green for the DS, melonDS's
GREEN = {"top": (0x24, 0x6b, 0x1c), "bottom": (0x0b, 0x2a, 0x08), "accent": (0xb4, 0xf0, 0x8c), "detail": (0x74, 0x8c, 0x6c)}
OVERLAY = 0.35  # as the banner's
WAVE_OVERLAY = 0.22  # lighter on the yellow, which darkens to brown

# Azahar's background, as the launcher draws it on its 1920x1080 screen
WAVE_TOP, WAVE_BOTTOM = (255, 204, 64), (236, 158, 22)
WAVES = [  # top, wavelength, amplitude, colour, alpha
    (580, 1280, 36, (255, 228, 140), 0.26),
    (670, 960, 28, (255, 236, 166), 0.32),
    (770, 720, 22, (255, 244, 196), 0.40),
]


def load_background():
    spec = importlib.util.spec_from_file_location(
        "render_background", os.path.join(os.path.dirname(os.path.abspath(__file__)), "render-background.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def bubbles(width, height, region):
    """Rows of RGB: region (x, y, w, h) of the Homebrew Launcher's 1280x720 screen, scaled to the size."""
    hbl = load_background()
    rx, ry, rw, rh = region
    rows = []
    for y in range(height):
        t = (ry + (y + 0.5) / height * rh) / 720.0
        rows.append([[hbl.TOP[i] + (hbl.BOTTOM[i] - hbl.TOP[i]) * t for i in range(3)] for _ in range(width)])
    sx, sy = width / rw, height / rh
    for x, y, radius, alpha in hbl.particles():
        cx, cy, r = (x * 1280 - rx) * sx, (y * 720 - ry) * sy, radius * 1280 * sx
        if r < 0.5 or cx + r < 0 or cx - r > width or cy + r < 0 or cy - r > height:
            continue
        for py in range(max(0, int(cy - r - 1)), min(height, int(cy + r + 2))):
            for px in range(max(0, int(cx - r - 1)), min(width, int(cx + r + 2))):
                a = alpha * min(max(r - math.hypot(px + 0.5 - cx, py + 0.5 - cy) + 0.5, 0.0), 1.0)
                if a > 0.0:
                    pixel = rows[py][px]
                    for c in range(3):
                        pixel[c] += (255 - pixel[c]) * a
    return darken(rows)


def waves(width, height, region):
    """Rows of RGB: region (x, y, w, h) of Azahar's 1920x1080 screen, its waves where they start."""
    rx, ry, rw, rh = region
    sx, sy = rw / width, rh / height
    rows = []
    for y in range(height):
        fy = ry + (y + 0.5) * sy
        t = fy / 1080.0
        base = [WAVE_TOP[i] + (WAVE_BOTTOM[i] - WAVE_TOP[i]) * t for i in range(3)]
        row = []
        for x in range(width):
            fx = rx + (x + 0.5) * sx
            pixel = list(base)
            for top, length, amplitude, colour, alpha in WAVES:
                surface = top + amplitude * (1.0 - math.cos(2.0 * math.pi * fx / length))
                depth = (fy - surface) / sy  # in this picture's pixels, for its edge
                body = min(max(depth + 0.5, 0.0), 1.0)
                crest = min(max(1.0 - abs(depth - 1.5 / sy) / (2.5 / sy), 0.0), 1.0)
                a = min(1.0, body * alpha + crest * 0.35)
                for c in range(3):
                    pixel[c] += (colour[c] - pixel[c]) * a
            row.append(pixel)
        rows.append(row)
    return darken(rows, WAVE_OVERLAY)


# melonDS's background: a melon's green, lighter at the top, with the DS's pixels rising as squares
MELON_TOP, MELON_BOTTOM = (126, 217, 87), (58, 150, 40)
PIXELS = [  # x, y (0 to 1 of a 1080 square), size, alpha
    (0.12, 0.18, 0.045, 0.30), (0.30, 0.08, 0.030, 0.22), (0.72, 0.14, 0.055, 0.26), (0.88, 0.30, 0.035, 0.30),
    (0.08, 0.52, 0.040, 0.20), (0.92, 0.62, 0.050, 0.24), (0.20, 0.86, 0.060, 0.22), (0.62, 0.90, 0.040, 0.28),
    (0.46, 0.04, 0.025, 0.20), (0.80, 0.84, 0.030, 0.20),
]


def pixels(width, height, region):
    """Rows of RGB: region (x, y, w, h) of the DS side's 1080x1080 square, its rising squares."""
    rx, ry, rw, rh = region
    sx, sy = rw / width, rh / height
    rows = []
    for y in range(height):
        fy = ry + (y + 0.5) * sy
        t = fy / 1080.0
        base = [MELON_TOP[i] + (MELON_BOTTOM[i] - MELON_TOP[i]) * t for i in range(3)]
        row = []
        for x in range(width):
            fx = rx + (x + 0.5) * sx
            pixel = list(base)
            for px, py, size, alpha in PIXELS:
                half = size * 1080 / 2
                d = rounded_box(fx, fy, px * 1080, py * 1080, half, half, half * 0.3)
                a = alpha * min(max(0.5 - d / sx, 0.0), 1.0)
                for c in range(3):
                    pixel[c] += (255 - pixel[c]) * a
            row.append(pixel)
        rows.append(row)
    return darken(rows, WAVE_OVERLAY)


def darken(rows, overlay=OVERLAY):
    for row in rows:
        for pixel in row:
            for c in range(3):
                pixel[c] *= 1.0 - overlay
    return rows


def rounded_box(x, y, cx, cy, half_w, half_h, radius):
    """Signed distance to a rounded rectangle (negative inside)."""
    qx = abs(x - cx) - half_w + radius
    qy = abs(y - cy) - half_h + radius
    outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
    return outside + min(max(qx, qy), 0.0) - radius


def circle(x, y, cx, cy, r):
    return math.hypot(x - cx, y - cy) - r


def coverage(distance, pixel):
    """Area of the pixel inside a shape at that signed distance (in pixels of the unit square)."""
    return min(max(0.5 - distance / pixel, 0.0), 1.0)


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def play(u, v, cx, cy, size):
    """A play triangle, pointing right, centred on (cx, cy)."""
    return max(cx - 0.45 * size - u, abs(v - cy) * 1.15 - (cx + 0.55 * size - u) * 0.62)


def gamepad(u, v, pixel, colour, palette=BLUE):
    """The Wii U GamePad over colour at (u, v) in its unit square; returns (colour, silhouette)."""
    accent, detail = palette["accent"], palette["detail"]
    body = rounded_box(u, v, 0.5, 0.53, 0.40, 0.205, 0.085)
    colour = mix(colour, (0, 0, 0), 0.35 * coverage(body - 0.02, pixel * 6) * (1 - coverage(body, pixel)))  # shadow
    silhouette = coverage(body, pixel)
    colour = mix(colour, BODY, silhouette)
    # its screen, with an accent edge, and a play triangle
    colour = mix(colour, accent, coverage(rounded_box(u, v, 0.5, 0.52, 0.205, 0.145, 0.02), pixel))
    screen = rounded_box(u, v, 0.5, 0.52, 0.19, 0.13, 0.014)
    colour = mix(colour, mix(palette["top"], palette["bottom"], (v - 0.39) / 0.26), coverage(screen, pixel))
    colour = mix(colour, accent, coverage(play(u, v, 0.515, 0.52, 0.09), pixel))
    # sticks
    for cx in (0.175, 0.825):
        colour = mix(colour, detail, coverage(circle(u, v, cx, 0.43, 0.04), pixel))
        colour = mix(colour, BODY, coverage(circle(u, v, cx, 0.43, 0.024), pixel))
    # d-pad
    dpad = min(rounded_box(u, v, 0.175, 0.585, 0.045, 0.014, 0.005), rounded_box(u, v, 0.175, 0.585, 0.014, 0.045, 0.005))
    colour = mix(colour, detail, coverage(dpad, pixel))
    # face buttons, the top one in the accent colour
    for (cx, cy, c) in ((0.825, 0.545, accent), (0.86, 0.585, detail), (0.79, 0.585, detail), (0.825, 0.625, detail)):
        colour = mix(colour, c, coverage(circle(u, v, cx, cy, 0.017), pixel))
    return colour, silhouette


def n3ds(u, v, pixel, colour, palette=GOLD):
    """A Nintendo 3DS, open, over colour at (u, v) in its unit square; returns (colour, silhouette)."""
    accent, detail = palette["accent"], palette["detail"]
    top = rounded_box(u, v, 0.5, 0.30, 0.31, 0.175, 0.05)
    bottom = rounded_box(u, v, 0.5, 0.685, 0.31, 0.175, 0.05)
    hinge = rounded_box(u, v, 0.5, 0.4925, 0.285, 0.03, 0.02)
    shell = min(top, bottom)
    colour = mix(colour, (0, 0, 0), 0.35 * coverage(min(shell, hinge) - 0.02, pixel * 6) * (1 - coverage(min(shell, hinge), pixel)))
    silhouette = coverage(min(shell, hinge), pixel)
    colour = mix(colour, detail, coverage(hinge, pixel))
    colour = mix(colour, BODY, coverage(shell, pixel))
    # the wide top screen (5:3), with an accent edge and a play triangle
    colour = mix(colour, accent, coverage(rounded_box(u, v, 0.5, 0.30, 0.215, 0.135, 0.016), pixel))
    screen = rounded_box(u, v, 0.5, 0.30, 0.2, 0.12, 0.01)
    colour = mix(colour, mix(palette["top"], palette["bottom"], (v - 0.18) / 0.24), coverage(screen, pixel))
    colour = mix(colour, accent, coverage(play(u, v, 0.51, 0.30, 0.085), pixel))
    # the touch screen (4:3)
    colour = mix(colour, detail, coverage(rounded_box(u, v, 0.5, 0.665, 0.13, 0.1, 0.012), pixel))
    touch = rounded_box(u, v, 0.5, 0.665, 0.12, 0.09, 0.008)
    colour = mix(colour, mix(palette["top"], palette["bottom"], (v - 0.575) / 0.18 * 0.6 + 0.4), coverage(touch, pixel))
    # the circle pad and the d-pad on the left
    colour = mix(colour, detail, coverage(circle(u, v, 0.27, 0.6, 0.042), pixel))
    colour = mix(colour, BODY, coverage(circle(u, v, 0.27, 0.6, 0.028), pixel))
    dpad = min(rounded_box(u, v, 0.27, 0.735, 0.038, 0.012, 0.004), rounded_box(u, v, 0.27, 0.735, 0.012, 0.038, 0.004))
    colour = mix(colour, detail, coverage(dpad, pixel))
    # A, B, X and Y on the right, X in the accent colour
    for (cx, cy, c) in ((0.73, 0.6, accent), (0.765, 0.635, detail), (0.695, 0.635, detail), (0.73, 0.67, detail)):
        colour = mix(colour, c, coverage(circle(u, v, cx, cy, 0.0155), pixel))
    # Select, Home and Start under the touch screen
    for cx in (0.43, 0.5, 0.57):
        colour = mix(colour, detail, coverage(rounded_box(u, v, cx, 0.8, 0.022, 0.007, 0.007), pixel))
    return colour, silhouette


def nds(u, v, pixel, colour, palette=GREEN):
    """A Nintendo DS Lite, open, over colour at (u, v) in its unit square; returns (colour, silhouette).
    Its two screens are the same size (4:3), the top one framed by the speakers."""
    accent, detail = palette["accent"], palette["detail"]
    top = rounded_box(u, v, 0.5, 0.30, 0.30, 0.17, 0.045)
    bottom = rounded_box(u, v, 0.5, 0.69, 0.30, 0.17, 0.045)
    hinge = rounded_box(u, v, 0.5, 0.495, 0.27, 0.03, 0.02)
    shell = min(top, bottom)
    colour = mix(colour, (0, 0, 0), 0.35 * coverage(min(shell, hinge) - 0.02, pixel * 6) * (1 - coverage(min(shell, hinge), pixel)))
    silhouette = coverage(min(shell, hinge), pixel)
    colour = mix(colour, detail, coverage(hinge, pixel))
    colour = mix(colour, BODY, coverage(shell, pixel))
    # the top screen (4:3), with an accent edge and a play triangle, and the speakers beside it
    colour = mix(colour, accent, coverage(rounded_box(u, v, 0.5, 0.30, 0.155, 0.12, 0.014), pixel))
    screen = rounded_box(u, v, 0.5, 0.30, 0.14, 0.105, 0.008)
    colour = mix(colour, mix(palette["top"], palette["bottom"], (v - 0.195) / 0.21), coverage(screen, pixel))
    colour = mix(colour, accent, coverage(play(u, v, 0.51, 0.30, 0.08), pixel))
    for cx in (0.285, 0.715):
        for cy in (0.27, 0.30, 0.33):
            colour = mix(colour, detail, coverage(circle(u, v, cx, cy, 0.008), pixel))
    # the touch screen, the same size
    colour = mix(colour, detail, coverage(rounded_box(u, v, 0.5, 0.685, 0.15, 0.115, 0.012), pixel))
    touch = rounded_box(u, v, 0.5, 0.685, 0.14, 0.105, 0.008)
    colour = mix(colour, mix(palette["top"], palette["bottom"], (v - 0.58) / 0.21 * 0.6 + 0.4), coverage(touch, pixel))
    # the d-pad on the left, A, B, X and Y on the right (A in the accent colour)
    dpad = min(rounded_box(u, v, 0.27, 0.66, 0.04, 0.013, 0.004), rounded_box(u, v, 0.27, 0.66, 0.013, 0.04, 0.004))
    colour = mix(colour, detail, coverage(dpad, pixel))
    for (cx, cy, c) in ((0.765, 0.66, accent), (0.73, 0.695, detail), (0.73, 0.625, detail), (0.695, 0.66, detail)):
        colour = mix(colour, c, coverage(circle(u, v, cx, cy, 0.0155), pixel))
    # Start and Select under the buttons
    for cy in (0.75, 0.78):
        colour = mix(colour, detail, coverage(circle(u, v, 0.735, cy, 0.008), pixel))
    return colour, silhouette


DEVICES = {"wiiu": gamepad, "3ds": n3ds, "ds": nds}


def compose(width, height, parts, rounded=False):
    """Rows of RGBA. parts: (x0, x1, background rows, device, scale, centre): from column x0 to x1,
    the background, and the device's unit square drawn scale pixels wide, its centre at centre.
    Without a background the device is drawn on nothing (its silhouette is the alpha)."""
    rows = [[None] * width for _ in range(height)]
    for x0, x1, background, device, scale, (ox, oy) in parts:
        pixel = 1.0 / scale
        for y in range(height):
            v = (y + 0.5 - oy) / scale + 0.5
            for x in range(x0, x1):
                u = (x + 0.5 - ox) / scale + 0.5
                under = background[y][x - x0] if background else BODY
                colour, silhouette = DEVICES[device](u, v, pixel, under)
                alpha = 1.0 if background else silhouette
                if rounded:
                    # the tile's corners are rounded in the launcher (the console rounds icon0.png's itself)
                    alpha *= coverage(rounded_box((x + 0.5) / width, (y + 0.5) / height, 0.5, 0.5, 0.5, 0.5, 0.11), 1.0 / width)
                rows[y][x] = tuple(int(round(max(0, min(255, c)))) for c in colour) + (int(round(alpha * 255)),)
    return rows


def divide(rows, x, colour=(255, 255, 255), alpha=0.35):
    """A line between the two halves of a split picture, at column x."""
    for row in rows:
        r, g, b, a = row[x]
        row[x] = tuple(int(round(c + (k - c) * alpha)) for c, k in zip((r, g, b), colour)) + (a,)
    return rows


def single(size, device, rounded=True):
    if device == "wiiu":
        background = bubbles(size, size, (280, 0, 720, 720))
    elif device == "ds":
        background = pixels(size, size, (0, 0, 1080, 1080))
    else:
        background = waves(size, size, (420, 0, 1080, 1080))
    return compose(size, size, [(0, size, background, device, size * 0.98, (size / 2, size / 2))], rounded)


def split(size, rounded):
    """The two side by side: the GamePad on the bubbles on the left, the 3DS on the waves on the right."""
    half = size // 2
    left = bubbles(half, size, (460, 0, 360, 720))
    right = waves(half, size, (690, 0, 540, 1080))
    parts = [(0, half, left, "wiiu", size * 0.56, (half / 2, size * 0.52)),
             (half, size, right, "3ds", size * 0.62, (half + half / 2, size * 0.5))]
    return divide(compose(size, size, parts, rounded), half)


def alone(width, height, device):
    return compose(width, height, [(0, width, None, device, width * 1.0, (width / 2, height / 2))])


def write_png(path, rows):
    height, width = len(rows), len(rows[0])
    raw = b"".join(b"\x00" + bytes(channel for p in row for channel in p) for row in rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as out:
        out.write(png)


def write_tga(path, rows):
    height, width = len(rows), len(rows[0])
    # uncompressed true colour, 32 bits, 8 of alpha, top-down: what the launcher reads (ui/images.cpp)
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, width, height, 32, 0x28)
    with open(path, "wb") as out:
        out.write(header + b"".join(bytes((p[2], p[1], p[0], p[3])) for row in rows for p in row))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    icons = os.path.join(out, "ui", "icons")
    os.makedirs(os.path.join(out, "sce_sys"), exist_ok=True)
    os.makedirs(icons, exist_ok=True)
    write_png(os.path.join(out, "sce_sys", "icon0.png"), split(512, rounded=False))
    write_tga(os.path.join(icons, "har-72.tga"), split(72, rounded=True))
    write_tga(os.path.join(icons, "ps5cemu.tga"), single(336, "wiiu"))
    write_tga(os.path.join(icons, "ps5cemu-72.tga"), single(72, "wiiu"))
    write_tga(os.path.join(icons, "azahar.tga"), single(336, "3ds"))
    write_tga(os.path.join(icons, "azahar-72.tga"), single(72, "3ds"))
    write_tga(os.path.join(icons, "start-wiiu.tga"), alone(440, 440, "wiiu"))
    write_tga(os.path.join(icons, "start-3ds.tga"), alone(440, 440, "3ds"))
    write_tga(os.path.join(icons, "melonds.tga"), single(336, "ds"))
    write_tga(os.path.join(icons, "melonds-72.tga"), single(72, "ds"))


if __name__ == "__main__":
    main()
