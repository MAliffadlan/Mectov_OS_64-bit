#!/usr/bin/env python3
"""scripts/mkassets.py — build desktop assets (D4).

Procedurally renders wallpaper + icons with PIL (no copyrighted pixels,
fully deterministic for gate asserts) and encodes them as QOI (tiny
built-in encoder following the vendored third_party/qoi.h spec).

Usage: python3 scripts/mkassets.py <outdir>
Outputs: wallpaper.qoi (1024x768), icon_term.qoi, icon_demo.qoi (48x48).
"""
import os
import struct
import sys

W, H = 1024, 768
TOP = (10, 22, 40)
BOT = (26, 43, 60)
PANEL = (136, 34, 34)
PANEL_BOX = (760, 560, 1000, 680)  # visible: clear of windows/taskbar


def qoi_encode(pixels, w, h):
    """pixels: bytes RGBA row-major. Returns QOI bytes (channels=4)."""
    out = bytearray()
    out += b"qoif"
    out += struct.pack(">II", w, h)
    out += bytes((4, 0))
    index = [(0, 0, 0, 0)] * 64
    prev = (0, 0, 0, 255)
    run = 0
    n = w * h

    def flush_run():
        nonlocal run
        if run:
            out.append(0xC0 | (run - 1))
            run = 0

    for i in range(n):
        o = i * 4
        px = (pixels[o], pixels[o + 1], pixels[o + 2], pixels[o + 3])
        if px == prev:
            run += 1
            if run == 62 or i == n - 1:
                flush_run()
            continue
        flush_run()
        hsh = (px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64
        if index[hsh] == px:
            out.append(hsh)
        else:
            index[hsh] = px
            if px[3] == prev[3]:
                vr = px[0] - prev[0]
                vg = px[1] - prev[1]
                vb = px[2] - prev[2]
                if -2 <= vr <= 1 and -2 <= vg <= 1 and -2 <= vb <= 1:
                    out.append(0x40 | (vr + 2) << 4 | (vg + 2) << 2 |
                               (vb + 2))
                elif (-32 <= vg <= 31 and -8 <= vr - vg <= 7 and
                        -8 <= vb - vg <= 7):
                    out.append(0x80 | (vg + 32))
                    out.append(((vr - vg + 8) << 4) | (vb - vg + 8))
                else:
                    out += bytes((0xFE, px[0], px[1], px[2]))
            else:
                out += bytes((0xFF, px[0], px[1], px[2], px[3]))
        prev = px
    out += b"\x00" * 7 + b"\x01"
    return bytes(out)


def wallpaper():
    from PIL import Image, ImageDraw
    im = Image.new("RGB", (W, H))
    px = im.load()
    for y in range(H):
        r = TOP[0] + (BOT[0] - TOP[0]) * y // (H - 1)
        g = TOP[1] + (BOT[1] - TOP[1]) * y // (H - 1)
        b = TOP[2] + (BOT[2] - TOP[2]) * y // (H - 1)
        for x in range(W):
            px[x, y] = (r, g, b)
    d = ImageDraw.Draw(im)
    d.rectangle(PANEL_BOX, fill=PANEL)
    d.text((PANEL_BOX[0] + 40, PANEL_BOX[1] + 50), "MECTOV OS",
           fill=(240, 240, 240))
    d.text((16, 66), "Terminal", fill=(200, 200, 200))
    d.text((16, 130), "GfxDemo", fill=(200, 200, 200))
    return im.convert("RGBA")


def icon_term():
    from PIL import Image, ImageDraw
    im = Image.new("RGB", (48, 48), (16, 16, 16))
    d = ImageDraw.Draw(im)
    d.rectangle((8, 14, 28, 20), fill=(0, 200, 0))
    d.rectangle((8, 26, 20, 32), fill=(0, 200, 0))
    d.rectangle((30, 26, 38, 32), fill=(0, 200, 0))
    return im.convert("RGBA")


def icon_demo():
    from PIL import Image, ImageDraw
    im = Image.new("RGB", (48, 48), (240, 240, 240))
    d = ImageDraw.Draw(im)
    d.ellipse((8, 8, 40, 40), fill=(0, 120, 215))
    d.ellipse((18, 18, 30, 30), fill=(240, 240, 240))
    return im.convert("RGBA")


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "assets"
    os.makedirs(outdir, exist_ok=True)
    for name, im in (("wallpaper", wallpaper()), ("icon_term", icon_term()),
                     ("icon_demo", icon_demo())):
        raw = im.tobytes()
        qoi = qoi_encode(raw, im.width, im.height)
        path = os.path.join(outdir, name + ".qoi")
        with open(path, "wb") as f:
            f.write(qoi)
        print(f"{path}: {len(raw)} -> {len(qoi)} bytes")


if __name__ == "__main__":
    main()
