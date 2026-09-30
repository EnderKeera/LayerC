"""Regenerates the Windows program icons in resources/windows/.

The icons are drawn from simple shapes so they can be rebuilt without an
image editor: a copper cooldown ring on the editor surface colour, with a
small badge per program (none for LayerD, a pencil for the editor, a
crosshair for calibration). Requires Pillow.

    python tools/make_icons.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SCALE = 4  # supersampling factor
SURFACE = (30, 35, 42, 255)
TRACK = (62, 68, 78, 255)
COPPER = (216, 142, 75, 255)
BADGE = (80, 186, 200, 255)


def draw_base(size: int) -> Image.Image:
    s = size * SCALE
    image = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    radius = s * 0.19
    draw.rounded_rectangle((0, 0, s - 1, s - 1), radius=radius, fill=SURFACE)
    margin = s * 0.17
    width = max(SCALE * 2, round(s * 0.13))
    box = (margin, margin, s - margin, s - margin)
    draw.arc(box, 0, 360, fill=TRACK, width=width)
    # A cooldown three-quarters elapsed, starting at 12 o'clock.
    draw.arc(box, -90, 180, fill=COPPER, width=width)
    return image


def draw_badge(image: Image.Image, kind: str) -> None:
    s = image.width
    draw = ImageDraw.Draw(image)
    r = s * 0.2
    cx, cy = s - r - s * 0.02, s - r - s * 0.02
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=SURFACE)
    inner = r * 0.78
    line = max(SCALE, round(s * 0.05))
    if kind == "editor":
        draw.line((cx - inner * 0.6, cy + inner * 0.6, cx + inner * 0.6,
                   cy - inner * 0.6), fill=BADGE, width=line * 2)
        draw.polygon([(cx - inner * 0.75, cy + inner * 0.75),
                      (cx - inner * 0.75, cy + inner * 0.3),
                      (cx - inner * 0.3, cy + inner * 0.75)], fill=BADGE)
    elif kind == "calibration":
        ring = inner * 0.62
        draw.ellipse((cx - ring, cy - ring, cx + ring, cy + ring),
                     outline=BADGE, width=line)
        draw.line((cx - inner, cy, cx + inner, cy), fill=BADGE, width=line)
        draw.line((cx, cy - inner, cx, cy + inner), fill=BADGE, width=line)


def render(kind: str, size: int) -> Image.Image:
    image = draw_base(size)
    # Badges are unreadable below 24 px; the ring alone identifies LayerC.
    if kind != "daemon" and size >= 24:
        draw_badge(image, kind)
    return image.resize((size, size), Image.Resampling.LANCZOS)


def main() -> None:
    out = Path(__file__).resolve().parent.parent / "resources" / "windows"
    for kind, name in (("daemon", "layerc.ico"), ("editor", "layerc_editor.ico"),
                       ("calibration", "layerc_calibration.ico")):
        frames = [render(kind, size) for size in SIZES]
        frames[-1].save(out / name, format="ICO",
                        sizes=[(size, size) for size in SIZES],
                        append_images=frames[:-1])
        print(out / name)


if __name__ == "__main__":
    main()
