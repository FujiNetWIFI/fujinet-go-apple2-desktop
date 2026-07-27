#!/usr/bin/env python3
"""Render the desktop icon set from the shared FujiNet Go Apple II launcher art.

The artwork is the Android app's adaptive icon: a transparent foreground
(data/icons/src/fujinet-go-apple2-foreground.png, copied verbatim from
fujinet-go-apple2/app/src/main/res/mipmap-xxxhdpi/ic_launcher_adaptive_fore.png)
over the flat brand background colour, so the desktop app and the Android app
are visibly the same product. Android applies the rounded mask at
runtime; on the desktop the mask has to be baked in, so this composites the
two into a rounded square and writes every hicolor size the desktop
environments look for, plus the macOS .icns the AppKit bundle uses.

The results are committed (data/icons/hicolor/..., data/icons/mac/...) so
building the project needs no image tooling; re-run this only when the
artwork changes:

    python3 tools/icons/make-icons.py

The .icns is written with Pillow directly (no iconutil, so this runs on any
host, not just macOS) by supplying each LANCZOS-downsampled size explicitly
via append_images -- left to its own resize, Pillow's ICNS writer uses a
lower-quality filter for the sizes it derives from the base image.
"""

import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
FOREGROUND = ROOT / "data/icons/src/fujinet-go-apple2-foreground.png"
OUTDIR = ROOT / "data/icons/hicolor"
ICNS_OUT = ROOT / "data/icons/mac/fujinet-go-apple2.icns"

BACKGROUND = (0xF4, 0x43, 0x36, 0xFF)   # values/colors.xml: ic_launcher_background
MASTER = 1024                            # render big, downsample with LANCZOS
CORNER_RADIUS = 0.22                     # fraction of the edge
FOREGROUND_ZOOM = 1.18                   # Android's mask crops; compensate a little
SIZES = (16, 24, 32, 48, 64, 128, 192, 256, 512)
ICNS_SIZES = (32, 64, 128, 256, 512)     # plus MASTER (1024) itself, unresized


def render_master() -> Image.Image:
    art = Image.open(FOREGROUND).convert("RGBA")

    # Rounded-square background on a transparent canvas, drawn at 4x and
    # downsampled so the corners are antialiased.
    scale = 4
    big = Image.new("RGBA", (MASTER * scale, MASTER * scale), (0, 0, 0, 0))
    ImageDraw.Draw(big).rounded_rectangle(
        (0, 0, MASTER * scale - 1, MASTER * scale - 1),
        radius=int(MASTER * scale * CORNER_RADIUS),
        fill=BACKGROUND,
    )
    icon = big.resize((MASTER, MASTER), Image.LANCZOS)

    art_size = int(MASTER * FOREGROUND_ZOOM)
    art = art.resize((art_size, art_size), Image.LANCZOS)
    offset = (MASTER - art_size) // 2
    overlay = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    overlay.paste(art, (offset, offset), art)

    # Keep the foreground inside the rounded silhouette.
    composed = Image.alpha_composite(icon, overlay)
    composed.putalpha(Image.composite(composed.getchannel("A"),
                                      Image.new("L", (MASTER, MASTER), 0),
                                      icon.getchannel("A")))
    return composed


def main() -> int:
    if not FOREGROUND.exists():
        print(f"missing artwork: {FOREGROUND}", file=sys.stderr)
        return 1

    master = render_master()
    for size in SIZES:
        out = OUTDIR / f"{size}x{size}" / "apps" / "fujinet-go-apple2.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        master.resize((size, size), Image.LANCZOS).save(out, optimize=True)
        print(f"wrote {out.relative_to(ROOT)}")

    ICNS_OUT.parent.mkdir(parents=True, exist_ok=True)
    appended = [master.resize((size, size), Image.LANCZOS) for size in ICNS_SIZES]
    master.save(ICNS_OUT, format="ICNS", append_images=appended)
    print(f"wrote {ICNS_OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
