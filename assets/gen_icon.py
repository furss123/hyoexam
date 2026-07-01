"""Generates the HyoExam app icon per HyoT-brand-kit.md's master icon prompt:
rounded-square diagonal gradient tile (#4A9FE0 -> #2B7CC7, ~135deg), white
clock glyph, ~18% safe padding, transparent background. Run once; commit the
output, don't regenerate on every build.
"""
from PIL import Image, ImageDraw
import math

SIZE = 1024
PAD = int(SIZE * 0.18)
RADIUS = int(SIZE * 0.22)
C1 = (0x4A, 0x9F, 0xE0)
C2 = (0x2B, 0x7C, 0xC7)

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

# Diagonal gradient tile.
tile = Image.new("RGB", (SIZE, SIZE))
px = tile.load()
for y in range(SIZE):
    for x in range(SIZE):
        t = (x + y) / (2 * SIZE)
        px[x, y] = lerp(C1, C2, t)

mask = Image.new("L", (SIZE, SIZE), 0)
d = ImageDraw.Draw(mask)
d.rounded_rectangle([0, 0, SIZE - 1, SIZE - 1], radius=RADIUS, fill=255)

canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
canvas.paste(tile, (0, 0), mask)

# White clock glyph: outer ring + hour/minute hands, centered within the safe area.
glyph = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
gd = ImageDraw.Draw(glyph)
cx, cy = SIZE // 2, SIZE // 2
r = (SIZE - 2 * PAD) // 2
ring_w = int(SIZE * 0.045)
gd.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(255, 255, 255, 255), width=ring_w)

# Tick at 12 o'clock.
tick_len = int(r * 0.14)
gd.line([cx, cy - r + ring_w // 2, cx, cy - r + ring_w // 2 + tick_len], fill=(255, 255, 255, 255), width=ring_w)

# Hour hand (pointing to ~10) and minute hand (pointing to ~2), like a clock reading a fixed time.
def hand(angle_deg, length, width):
    a = math.radians(angle_deg - 90)
    x2 = cx + length * math.cos(a)
    y2 = cy + length * math.sin(a)
    gd.line([cx, cy, x2, y2], fill=(255, 255, 255, 255), width=width)

hand(-60, r * 0.5, int(SIZE * 0.05))   # hour hand
hand(60, r * 0.72, int(SIZE * 0.035))  # minute hand
gd.ellipse([cx - 14, cy - 14, cx + 14, cy + 14], fill=(255, 255, 255, 255))

canvas.alpha_composite(glyph)
canvas.save("icon_1024.png")

for s in (512, 256, 128, 48, 32, 24, 16):
    canvas.resize((s, s), Image.LANCZOS).save(f"icon_{s}.png")

canvas.resize((512, 512), Image.LANCZOS).save("../data/icon.webp", "WEBP")

sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (128, 128), (256, 256)]
imgs = [canvas.resize(s, Image.LANCZOS) for s in sizes]
imgs[0].save("hyoexam.ico", format="ICO", sizes=sizes, append_images=imgs[1:])

print("done")
