"""Generates HyoExam's icon set + hyot.dev banner, on-concept and on-brand.

Concept: an exam-day clock + timetable board for classroom TV/projector output.
Brand (HyoT-brand-kit.md): rounded-square diagonal gradient tile
(#4A9FE0 -> #2B7CC7), single white focal glyph, generous padding, transparent
background. Banner follows the family hero style (see HyoSnap): transparent
1200x340 canvas (matches the site's `aspectRatio: 1200/340` + object-cover, and
its bottom ~23% fades to the page bg), icon tile on the left, name + tagline on
the right in brand blue.

Run once; commit the output. Requires Pillow + Malgun Gothic (Windows built-in).
"""
from PIL import Image, ImageDraw, ImageFont, ImageFilter
import math, os

# ---- Brand palette ----
C1 = (0x4A, 0x9F, 0xE0)   # HyoT blue (light)
C2 = (0x2B, 0x7C, 0xC7)   # HyoT blue (dark)
WHITE = (255, 255, 255, 255)

SS = 4  # supersampling factor for crisp anti-aliased edges

FONT_BOLD = r"C:\Windows\Fonts\malgunbd.ttf"


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def diagonal_gradient(size):
    """135deg diagonal gradient tile, C1 (top-left) -> C2 (bottom-right)."""
    img = Image.new("RGB", (size, size))
    px = img.load()
    for y in range(size):
        for x in range(size):
            t = (x + y) / (2 * size)
            px[x, y] = lerp(C1, C2, t)
    return img


def make_tile(size, radius_frac=0.225, highlight=True):
    """Rounded-square brand-gradient tile with a soft top highlight, RGBA."""
    s = size * SS
    grad = diagonal_gradient(s)

    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, s - 1, s - 1], radius=int(s * radius_frac), fill=255)

    tile = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    tile.paste(grad, (0, 0), mask)

    if highlight:
        # Soft elliptical sheen across the upper area for subtle depth. Blurred
        # so it reads as a gentle top-light, not a hard arc across the middle.
        glow = Image.new("L", (s, s), 0)
        ImageDraw.Draw(glow).ellipse(
            [-int(s * 0.15), -int(s * 0.62), int(s * 1.15), int(s * 0.42)], fill=42)
        glow = glow.filter(ImageFilter.GaussianBlur(s * 0.06))
        # Clip the sheen to the rounded tile.
        glow = Image.composite(glow, Image.new("L", (s, s), 0), mask)
        white = Image.new("RGBA", (s, s), (255, 255, 255, 0))
        white.putalpha(glow)
        tile = Image.alpha_composite(tile, white)

    return tile.resize((size, size), Image.LANCZOS)


def draw_glyph(size, pad_frac=0.20):
    """White clock + timetable-rows glyph, drawn at SS scale, returned at `size`."""
    s = size * SS
    g = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)

    pad = s * pad_frac
    inner = s - 2 * pad            # safe-area square
    # Clock occupies the left ~52% of the safe area; timetable rows the right.
    clock_r = inner * 0.29
    cx = pad + clock_r + inner * 0.02
    cy = s / 2
    ring_w = max(2, int(s * 0.042))

    # Clock ring.
    d.ellipse([cx - clock_r, cy - clock_r, cx + clock_r, cy + clock_r],
              outline=WHITE, width=ring_w)
    # 12 o'clock tick.
    tick = clock_r * 0.20
    d.line([cx, cy - clock_r + ring_w * 0.5, cx, cy - clock_r + ring_w * 0.5 + tick],
           fill=WHITE, width=ring_w)

    # Hands: hour ~10 o'clock, minute ~2 o'clock (a legible, balanced pose).
    def hand(angle_deg, length, width):
        a = math.radians(angle_deg - 90)
        d.line([cx, cy, cx + length * math.cos(a), cy + length * math.sin(a)],
               fill=WHITE, width=width)
    hand(-58, clock_r * 0.52, int(s * 0.046))   # hour
    hand(66, clock_r * 0.74, int(s * 0.033))    # minute
    hub = int(s * 0.016)
    d.ellipse([cx - hub, cy - hub, cx + hub, cy + hub], fill=WHITE)

    # Timetable rows to the right of the clock: 3 rounded bars, staggered widths,
    # reading as a schedule list.
    rows_left = cx + clock_r + inner * 0.10
    rows_right = pad + inner
    bar_h = max(2, int(inner * 0.085))
    gap = inner * 0.135
    widths = [1.0, 0.82, 0.62]      # descending, like list items
    total_h = bar_h * 3 + gap * 2
    y0 = cy - total_h / 2
    for i, wf in enumerate(widths):
        y = y0 + i * (bar_h + gap)
        x1 = rows_left + (rows_right - rows_left) * wf
        d.rounded_rectangle([rows_left, y, x1, y + bar_h],
                            radius=bar_h / 2, fill=WHITE)

    return g.resize((size, size), Image.LANCZOS)


def make_icon(size):
    tile = make_tile(size)
    glyph = draw_glyph(size)
    out = tile.copy()
    out.alpha_composite(glyph)
    return out


def gradient_text(draw_size, text, font, c_from=C1, c_to=C2):
    """Render `text` filled with a horizontal C1->C2 gradient. Returns RGBA image
    sized to the text bbox."""
    tmp = Image.new("L", (10, 10))
    bbox = ImageDraw.Draw(tmp).textbbox((0, 0), text, font=font)
    w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
    pad = int(h * 0.35)
    W, H = w + pad * 2, h + pad * 2

    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).text((pad - bbox[0], pad - bbox[1]), text, font=font, fill=255)

    grad = Image.new("RGB", (W, H))
    gp = grad.load()
    for x in range(W):
        t = x / max(1, W - 1)
        col = lerp(c_from, c_to, t)
        for y in range(H):
            gp[x, y] = col
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    out.paste(grad, (0, 0), mask)
    return out


def make_banner():
    """1200x340 transparent hero banner: icon tile left, name + tagline right."""
    W, H = 1200, 340
    banner = Image.new("RGBA", (W, H), (0, 0, 0, 0))

    # Icon tile, vertically centered, biased slightly above the bottom fade zone.
    tile_sz = 236
    tile = make_icon(tile_sz)
    tx = 84
    ty = int(H * 0.46) - tile_sz // 2   # nudge up out of the bottom ~23% fade
    banner.alpha_composite(tile, (tx, ty))

    text_x = tx + tile_sz + 56
    # App name: gradient-filled, large bold.
    name_font = ImageFont.truetype(FONT_BOLD, 92)
    name_img = gradient_text(banner, "HyoExam", name_font)
    name_y = int(H * 0.46) - 92
    banner.alpha_composite(name_img, (text_x - int(92 * 0.35), name_y))

    # Tagline: solid HyoT blue, medium.
    tag_font = ImageFont.truetype(FONT_BOLD, 40)
    tag = "시험 시간표 · 실시간 시계 송출"
    td = ImageDraw.Draw(banner)
    td.text((text_x, name_y + 118), tag, font=tag_font, fill=(0x4A, 0x9F, 0xE0, 235))

    return banner


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    os.chdir(here)

    icon_1024 = make_icon(1024)
    icon_1024.save("icon_1024.png")
    for s in (512, 256, 128, 48, 32, 24, 16):
        make_icon(s).save(f"icon_{s}.png")

    # Multi-resolution .ico for the executable.
    ico_sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (128, 128), (256, 256)]
    imgs = [make_icon(s[0]) for s in ico_sizes]
    imgs[0].save("hyoexam.ico", format="ICO", sizes=ico_sizes, append_images=imgs[1:])

    # hyot.dev registration icon (512) + banner (1200x340).
    make_icon(512).save(os.path.join("..", "data", "icon.webp"), "WEBP", quality=95)
    make_banner().save(os.path.join("..", "data", "banner.webp"), "WEBP", quality=95)

    print("done")


if __name__ == "__main__":
    main()
