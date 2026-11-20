"""把 tools/icons/work/preview 下渲染好的候选图标拼成对照图。

用法：
  python make_sheet.py <preview_dir> <out.png> [name_filter ...]

每个单元格：48px 大图 + 24px / 18px 小图 + 图标名。浅色底、浅灰边框，
方便按真实工具栏尺寸判断可读性。
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

preview = Path(sys.argv[1])
out = Path(sys.argv[2])
filters = [f.lower() for f in sys.argv[3:]]

names = sorted({p.stem.rsplit("_", 1)[0] for p in preview.glob("*_48.png")})
if filters:
    names = [n for n in names if any(f in n.lower() for f in filters)]

try:
    font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 11)
except OSError:
    font = ImageFont.load_default()

cell_w, cell_h = 160, 104
cols = 6
rows = (len(names) + cols - 1) // cols
pad = 12
sheet = Image.new("RGB", (cols * cell_w + pad * 2, rows * cell_h + pad * 2), (246, 247, 249))
draw = ImageDraw.Draw(sheet)

for i, name in enumerate(names):
    cx = pad + (i % cols) * cell_w
    cy = pad + (i // cols) * cell_h
    draw.rectangle([cx, cy, cx + cell_w - 8, cy + cell_h - 8], fill=(255, 255, 255), outline=(221, 225, 231))
    draw.text((cx + 8, cy + 6), name, fill=(35, 40, 48), font=font)

    x = cx + 8
    y = cy + 24
    big = preview / f"{name}_48.png"
    small24 = preview / f"{name}_24.png"
    small18 = preview / f"{name}_18.png"
    if big.exists():
        im = Image.open(big).convert("RGBA")
        sheet.paste(im, (x, y), im)
    if small24.exists():
        im = Image.open(small24).convert("RGBA")
        sheet.paste(im, (x + 54, y + 4), im)
        draw.text((x + 54, y + 30), "24", fill=(130, 138, 150), font=font)
    if small18.exists():
        im = Image.open(small18).convert("RGBA")
        sheet.paste(im, (x + 84, y + 7), im)
        draw.text((x + 84, y + 30), "18", fill=(130, 138, 150), font=font)

sheet.save(out)
print(f"{len(names)} icons -> {out} ({sheet.width}x{sheet.height})")
