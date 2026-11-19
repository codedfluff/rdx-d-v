"""Rasterise a monospace TTF into the kernel's 8x16 1-bit console font.

Output: src/font.bin, 96 glyphs (ASCII 32..127), 16 bytes each, MSB = leftmost
pixel. Also writes build/font_preview.png so the result can be eyeballed.

Usage: python tools/mkfont.py [path-to-ttf] [pixel-size]
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ttf = sys.argv[1] if len(sys.argv) > 1 else r"C:\Windows\Fonts\consola.ttf"
size = int(sys.argv[2]) if len(sys.argv) > 2 else 14
W, H = 8, 16

font = ImageFont.truetype(ttf, size)
ascent, descent = font.getmetrics()
y0 = max(0, (H - (ascent + descent)) // 2)

out = bytearray()
glyphs = []
for code in range(32, 128):
    img = Image.new("1", (W, H), 0)
    if code < 127:
        d = ImageDraw.Draw(img)
        d.fontmode = "1"  # no anti-aliasing
        d.text((0, y0), chr(code), fill=1, font=font)
    glyphs.append(img)
    for y in range(H):
        b = 0
        for x in range(W):
            if img.getpixel((x, y)):
                b |= 0x80 >> x
        out.append(b)

with open(os.path.join(ROOT, "src", "font.bin"), "wb") as f:
    f.write(out)

# Preview sheet: 16 glyphs per row, 4x zoom.
os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
sheet = Image.new("1", (16 * W, 6 * H), 0)
for i, g in enumerate(glyphs):
    sheet.paste(g, ((i % 16) * W, (i // 16) * H))
sheet = sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST)
sheet.save(os.path.join(ROOT, "build", "font_preview.png"))
print(f"wrote {len(out)} bytes ({ttf} @ {size}px, ascent={ascent}, descent={descent}, y0={y0})")
