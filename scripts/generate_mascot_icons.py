#!/usr/bin/env python3
"""Generate the reading-companion mascot ("书灵") icon headers.

The AI screens (hub, chat, image, Ask AI, dictionary, word card) share one
cartoon character drawn in a handful of poses. E-ink is 1-bit, so each pose is
rendered as a packed 1-bpp bitmap: bit 1 = paper (transparent in
drawImageTransparent), bit 0 = black ink.

Artwork is defined here once and regenerated, instead of hand-editing 2 KB
byte arrays:

  python3 scripts/generate_mascot_icons.py            # write headers
  python3 scripts/generate_mascot_icons.py --preview  # also ./mascot-preview

Headers go to src/components/icons/mascot_<pose>128.h and are intentionally
checked in (the firmware build never runs Pillow). Requires Pillow.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw

SIZE = 128
SS = 4  # supersampling factor; draw at SIZE*SS then downsample
W = SIZE * SS
# The artwork below is authored directly in this 512-unit canvas, so drawing
# coordinates are used as-is and only the final resize reduces them to SIZE.

OUT_DIR = Path(__file__).resolve().parent.parent / "src" / "components" / "icons"

INK = 0
PAPER = 255


def s(v: float) -> int:
    """Canvas-unit to pixel coordinate (identity: the canvas is 4x the icon)."""
    return int(round(v))


def new_canvas() -> Image.Image:
    return Image.new("L", (W, W), PAPER)


def ellipse(d, box, fill=PAPER, outline=INK, width=14):
    d.ellipse([s(v) for v in box], fill=fill, outline=outline, width=s(width))


def rrect(d, box, radius, fill=PAPER, outline=INK, width=14):
    d.rounded_rectangle([s(v) for v in box], radius=s(radius), fill=fill, outline=outline, width=s(width))


def polygon(d, points, fill=INK, outline=None):
    pts = [(s(x), s(y)) for x, y in points]
    d.polygon(pts, fill=fill, outline=outline)
    if outline is not None:
        d.line(pts + [pts[0]], fill=outline, width=s(10), joint="curve")


def line(d, points, width=14, fill=INK):
    d.line([(s(x), s(y)) for x, y in points], fill=fill, width=s(width), joint="curve")


def arc(d, box, start, end, width=14, fill=INK):
    d.arc([s(v) for v in box], start, end, fill=fill, width=s(width))


def dot(d, cx, cy, r, fill=INK):
    d.ellipse([s(cx - r), s(cy - r), s(cx + r), s(cy + r)], fill=fill)


def draw_body(d, eyes="open", look=(0, 0), mouth="smile"):
    # Ear tufts first, so the body outline overlaps their bases.
    polygon(d, [(132, 176), (168, 58), (236, 158)], fill=PAPER, outline=INK)
    polygon(d, [(380, 176), (344, 58), (276, 158)], fill=PAPER, outline=INK)

    # Body: a paper egg with a heavy ink contour. Outlined art keeps the panel
    # light (solid black at this size ghosts badly on e-ink) and lets the props
    # show against the body instead of disappearing into it.
    ellipse(d, (92, 96, 420, 478), fill=PAPER, outline=INK, width=16)

    # Wings: outlined ovals pinned to the sides.
    ellipse(d, (62, 272, 146, 408), fill=PAPER, outline=INK, width=14)
    ellipse(d, (366, 272, 450, 408), fill=PAPER, outline=INK, width=14)

    # Feet.
    ellipse(d, (154, 438, 236, 482), fill=PAPER, outline=INK, width=10)
    ellipse(d, (276, 438, 358, 482), fill=PAPER, outline=INK, width=10)

    # Eyes.
    if eyes == "closed":
        arc(d, (168, 226, 248, 306), 200, 340, width=16)
        arc(d, (264, 226, 344, 306), 200, 340, width=16)
    else:
        ellipse(d, (166, 226, 250, 310), fill=INK, outline=INK, width=0)
        ellipse(d, (262, 226, 346, 310), fill=INK, outline=INK, width=0)
        dot(d, 199 + look[0], 259 + look[1], 12, PAPER)
        dot(d, 295 + look[0], 259 + look[1], 12, PAPER)

    # Beak and mouth.
    polygon(d, [(238, 322), (274, 322), (256, 350)])
    if mouth == "smile":
        arc(d, (228, 340, 284, 378), 20, 160, width=8)
    elif mouth == "frown":
        arc(d, (228, 356, 284, 398), 200, 340, width=8)
    elif mouth == "open":
        ellipse(d, (238, 344, 274, 378), fill=INK, outline=INK, width=0)


def pose_read() -> Image.Image:
    img = new_canvas()
    d = ImageDraw.Draw(img)
    draw_body(d, eyes="closed", mouth="smile")
    # An open book held across the belly, thumbs on the page edges.
    polygon(d, [(104, 330), (256, 306), (256, 460), (96, 442)], fill=PAPER, outline=INK)
    polygon(d, [(408, 330), (256, 306), (256, 460), (416, 442)], fill=PAPER, outline=INK)
    line(d, [(256, 306), (256, 460)], width=10)
    for i in range(3):
        y = 352 + i * 26
        line(d, [(132, y), (222, y - 8)], width=6)
        line(d, [(290, y - 8), (380, y)], width=6)
    dot(d, 118, 414, 18)
    dot(d, 394, 414, 18)
    return img


def pose_chat() -> Image.Image:
    img = new_canvas()
    d = ImageDraw.Draw(img)
    draw_body(d, eyes="open", look=(6, -2), mouth="open")
    # Speech bubble up and to the right, tail pointing at the head.
    rrect(d, (300, 52, 488, 176), 34, fill=PAPER, outline=INK, width=14)
    polygon(d, [(330, 166), (338, 216), (378, 168)], fill=PAPER, outline=INK)
    for i in range(3):
        dot(d, 348 + i * 44, 114, 12)
    return img


def pose_listen() -> Image.Image:
    img = new_canvas()
    d = ImageDraw.Draw(img)
    draw_body(d, eyes="open", look=(0, -2), mouth="open")
    # Headband + ear cups; the band sits just inside the head contour.
    arc(d, (124, 92, 388, 320), 190, 350, width=18)
    rrect(d, (66, 226, 152, 336), 30, fill=INK, outline=INK, width=0)
    rrect(d, (360, 226, 446, 336), 30, fill=INK, outline=INK, width=0)
    ellipse(d, (92, 254, 128, 310), fill=PAPER, outline=PAPER, width=0)
    ellipse(d, (384, 254, 420, 310), fill=PAPER, outline=PAPER, width=0)
    return img


def pose_lookup() -> Image.Image:
    img = new_canvas()
    d = ImageDraw.Draw(img)
    draw_body(d, eyes="open", look=(8, 0), mouth="smile")
    # Magnifier to the side of the face.
    line(d, [(356, 356), (442, 442)], width=34, fill=PAPER)
    line(d, [(356, 356), (438, 438)], width=18)
    ellipse(d, (292, 208, 432, 348), fill=PAPER, outline=INK, width=16)
    ellipse(d, (308, 224, 416, 332), fill=PAPER, outline=PAPER, width=0)
    # Sparkle inside the lens.
    line(d, [(366, 262), (366, 306)], width=8)
    line(d, [(344, 284), (388, 284)], width=8)
    return img


def pose_paint() -> Image.Image:
    img = new_canvas()
    d = ImageDraw.Draw(img)
    draw_body(d, eyes="open", look=(-6, 0), mouth="smile")
    # Beret.
    d.chord([s(136), s(84), s(376), s(200)], 180, 360, fill=INK, outline=INK, width=0)
    dot(d, 256, 84, 14)
    line(d, [(256, 84), (272, 52)], width=10)
    # Brush in the raised wing: paper halo first so the stick crosses the body.
    line(d, [(380, 396), (466, 300)], width=32, fill=PAPER)
    line(d, [(380, 396), (462, 304)], width=16)
    polygon(d, [(446, 320), (482, 272), (500, 300), (464, 348)], fill=INK)
    polygon(d, [(500, 300), (512, 258), (478, 276)], fill=INK)
    return img


def pose_think() -> Image.Image:
    img = new_canvas()
    d = ImageDraw.Draw(img)
    draw_body(d, eyes="open", look=(-2, -8), mouth="frown")
    # Thought trail: two small puffs and one cloud, rising to the upper right.
    ellipse(d, (318, 206, 352, 240), fill=PAPER, outline=INK, width=12)
    ellipse(d, (348, 168, 396, 216), fill=PAPER, outline=INK, width=12)
    ellipse(d, (386, 50, 474, 138), fill=PAPER, outline=INK, width=14)
    ellipse(d, (438, 66, 510, 138), fill=PAPER, outline=INK, width=14)
    d.rectangle([s(430), s(80), s(470), s(112)], fill=PAPER)
    line(d, [(434, 138), (474, 138)], width=14)
    dot(d, 430, 96, 7)
    dot(d, 458, 110, 7)
    return img


def pose_sad() -> Image.Image:
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # Looking down with a frown and one tear: upset without looking angry.
    draw_body(d, eyes="open", look=(0, 5), mouth="frown")
    # A single tear on the cheek below the left eye.
    polygon(d, [(188, 326), (204, 366), (172, 366)], fill=INK, outline=INK)
    dot(d, 189, 354, 7, PAPER)
    return img


POSES = {
    "read": pose_read,
    "chat": pose_chat,
    "listen": pose_listen,
    "lookup": pose_lookup,
    "paint": pose_paint,
    "think": pose_think,
    "sad": pose_sad,
}


def packed_bytes(img: Image.Image) -> bytes:
    desired = img.resize((SIZE, SIZE), Image.LANCZOS)
    # matches scripts/convert_icon.py: headers hold the artwork pre-rotated 90°
    # counter-clockwise, which drawIcon() gives back upright on the panel.
    small = desired.transpose(Image.ROTATE_90)
    out = bytearray()
    for y in range(SIZE):
        for xb in range(SIZE // 8):
            byte = 0
            for bit in range(8):
                pixel = small.getpixel((xb * 8 + bit, y))
                if pixel < 128:  # ink
                    byte |= 0x80 >> bit
                # 1 = paper (drawImageTransparent keeps the framebuffer)
            out.append(0xFF ^ byte)
    return bytes(out)


def header_text(name: str, data: bytes) -> str:
    rows = []
    for y in range(SIZE):
        chunk = data[y * (SIZE // 8) : (y + 1) * (SIZE // 8)]
        rows.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    return (
        "#pragma once\n"
        "#include <cstdint>\n\n"
        f"// size: {SIZE}x{SIZE} (generated by scripts/generate_mascot_icons.py)\n"
        f"static const uint8_t Mascot{name}Icon[] = {{\n" + "\n".join(rows) + "\n};\n"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", action="store_true", help="also write PNG previews")
    args = parser.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    preview_dir = Path(__file__).resolve().parent.parent / "build" / "mascot-preview"
    if args.preview:
        preview_dir.mkdir(parents=True, exist_ok=True)

    for name, draw in POSES.items():
        img = draw()
        data = packed_bytes(img)
        path = OUT_DIR / f"mascot_{name}{SIZE}.h"
        path.write_text(header_text(name.capitalize(), data), encoding="utf-8")
        print(f"wrote {path} ({len(data)} bytes)")
        if args.preview:
            small = img.resize((SIZE, SIZE), Image.LANCZOS).point(lambda p: 255 if p >= 128 else 0)
            small.resize((SIZE * 3, SIZE * 3), Image.NEAREST).save(preview_dir / f"{name}.png")


if __name__ == "__main__":
    main()
