"""把改版前（git HEAD）与改版后的 64px 图标拼成对照图。"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

old_dir = Path(sys.argv[1])
new_dir = Path(sys.argv[2])
out = Path(sys.argv[3])

slots = ["appicon", "about", "load", "unload", "merge", "split", "add", "add_dir",
         "browse", "delete", "export", "save", "diagnose", "manage", "formats",
         "reload", "show_path", "show_name", "view_tree", "view_flat"]

try:
    font = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 13)
    small = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 11)
    head = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 18)
except OSError:
    font = small = head = ImageFont.load_default()

cols = 7
cell = 92
pad = 10
rows = (len(slots) + cols - 1) // cols
section_h = 46 + rows * cell + 14
sheet_w = cols * cell + pad * 2
sheet = Image.new("RGB", (sheet_w, section_h * 2 + 12), (246, 247, 249))
d = ImageDraw.Draw(sheet)


def draw_section(y, title, subtitle, directory):
    d.text((pad + 2, y + 2), title, fill=(24, 30, 38), font=head)
    d.text((pad + 2, y + 26), subtitle, fill=(110, 118, 130), font=small)
    y += 52
    for i, name in enumerate(slots):
        cx = pad + (i % cols) * cell
        cy = y + (i // cols) * cell
        d.rounded_rectangle([cx, cy, cx + cell - 6, cy + cell - 8], radius=8,
                            fill=(255, 255, 255), outline=(226, 230, 236))
        p = directory / f"{name}_64.png"
        if p.exists():
            im = Image.open(p).convert("RGBA")
            sheet.paste(im, (cx + 11, cy + 6), im)
        else:
            d.text((cx + 8, cy + 24), "—", fill=(180, 186, 196), font=font)
        d.text((cx + 6, cy + 70), name, fill=(80, 88, 100), font=small)
    return y


draw_section(10, "改版前", "22 个图标 / 风格不统一 / 手工维护 7 档尺寸", old_dir)
draw_section(section_h + 10, "改版后", "IconMCP 统一线性图标 / 单套 SVG 生成 8 档尺寸 / 应用图标重绘", new_dir)

sheet.save(out)
print(f"{sheet.width}x{sheet.height} -> {out}")
