"""用 IconMCP 的线性/实心字形组合出应用图标候选，方便挑一个再落地。

产物：work/appicon/<variant>/appicon_<size>.png
预览：work/appicon-sheet.png（浅色/深色两条，按真实尺寸）
"""
import re
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

BASE = Path(__file__).parent
CAND = BASE / "work" / "candidates"
OUT = BASE / "work" / "appicon"
EXE = BASE / "svg2png" / ".build" / "svg2png.exe"
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]

# 变体：名字 -> (字形文件, 字形类型, 缩放, 颜色)
VARIANTS = {
    "files-solid-10": ("files-solid.svg", "solid", 10.0, "#ffffff"),
    "files-solid-11": ("files-solid.svg", "solid", 11.0, "#ffffff"),
    "files-outline-10": ("files.svg", "outline", 10.0, "#ffffff"),
    "file-search-solid-10": ("file-search-solid.svg", "solid", 10.0, "#ffffff"),
    "archive-outline-10": ("archive.svg", "outline", 10.0, "#ffffff"),
    "archive-solid-10": ("archive-solid.svg", "solid", 10.0, "#ffffff"),
    "archive-solid-11": ("archive-solid.svg", "solid", 11.0, "#ffffff"),
    "box-solid-10": ("box-solid.svg", "solid", 10.0, "#ffffff"),
    "box-outline-10": ("box.svg", "outline", 10.0, "#ffffff"),
}


def inner_svg(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    m = re.search(r"<svg[^>]*>(.*)</svg>", text, re.S)
    if not m:
        raise ValueError(f"no svg body in {path}")
    return m.group(1).strip()


def compose(glyph: str, solid: bool, scale: float, color: str) -> str:
    # 字形原本在 16×16 网格里；平移到 256 画布中央并按 scale 放大
    glyph = glyph.replace("currentColor", color)
    side = 16 * scale
    off = (256 - side) / 2
    paint = f'fill="{color}" stroke="{color}"' if solid else f'stroke="{color}" fill="none"'
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#3D9BF6"/>
      <stop offset="1" stop-color="#1F6FE0"/>
    </linearGradient>
  </defs>
  <rect width="256" height="256" rx="56" ry="56" fill="url(#bg)"/>
  <g transform="translate({off:g},{off:g}) scale({scale:g})" {paint}
     stroke-width="1.25" stroke-linecap="round" stroke-linejoin="round">
{chr(10).join('    ' + line for line in glyph.splitlines())}
  </g>
</svg>
"""


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    made = {}
    for name, (glyph_file, kind, scale, color) in VARIANTS.items():
        glyph_path = CAND / glyph_file
        if not glyph_path.exists():
            print(f"skip {name}: {glyph_file} missing")
            continue
        svg = compose(inner_svg(glyph_path), kind == "solid", scale, color)
        vdir = OUT / name
        vdir.mkdir(parents=True, exist_ok=True)
        svg_path = vdir / "appicon.svg"
        svg_path.write_text(svg, encoding="utf-8")
        subprocess.run(
            [str(EXE), "--in", str(svg_path), "--outdir", str(vdir), "--prefix", "appicon",
             "--sizes", ",".join(map(str, SIZES))],
            check=True,
        )
        made[name] = vdir
        print(f"made {name}")

    # 预览：每个变体一行，上行浅色底、下行深色底，按真实尺寸并排
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 12)
    except OSError:
        font = ImageFont.load_default()

    show_sizes = [16, 20, 24, 32, 48, 64]
    label_w = 170
    row_h = 150
    band_h = 74
    sheet_w = label_w + 12 + sum(s + 26 for s in show_sizes)
    sheet = Image.new("RGB", (sheet_w, row_h * len(made) + 16), (246, 247, 249))
    d = ImageDraw.Draw(sheet)
    y = 8
    for name, vdir in made.items():
        d.text((12, y + 12), name, fill=(35, 40, 48), font=font)
        x = label_w
        for s in show_sizes:
            p = vdir / f"appicon_{s}.png"
            if not p.exists():
                continue
            im = Image.open(p).convert("RGBA")
            d.rectangle([x, y, x + s + 12, y + band_h - 4], fill=(255, 255, 255), outline=(228, 231, 236))
            sheet.paste(im, (x + 6, y + (band_h - 4 - s) // 2), im)
            d.rectangle([x, y + band_h, x + s + 12, y + 2 * band_h - 4], fill=(32, 33, 36))
            sheet.paste(im, (x + 6, y + band_h + (band_h - 4 - s) // 2), im)
            d.text((x, y + 2 * band_h - 2), str(s), fill=(120, 128, 140), font=font)
            x += s + 26
        y += row_h
    sheet.save(BASE / "work" / "appicon-sheet.png")
    print(f"sheet -> {BASE / 'work' / 'appicon-sheet.png'}")


if __name__ == "__main__":
    main()
