# -*- coding: utf-8 -*-
"""生成 plugin_tests 用的测试素材（覆盖真实 docx 排版与各类异常输入）。

默认写到本文件所在目录（tests/fixtures），文件名与 plugin_tests.cpp 里的用法一致：
  basic.docx     python-docx 生成的常规文档：标题/字符格式/对齐/列表/表格合并/图片/超链接
  chinese.docx   手工 OOXML：中文样式名「标题 1」+ chineseCounting 编号 + 网格合并单元格
  legacy.doc     旧版 OLE2 魔数（验证「另存为 .docx」提示）
  sample.rtf     纯文本 RTF（验证不支持提示）
  broken.docx    ZIP 头 + 垃圾字节（验证解析失败提示）
  notword.docx   只有 hello.txt 的 ZIP（模拟 xlsx/odt，验证缺少正文部件提示）
  empty.docx     0 字节（验证空文件提示）
  pic.png        basic.docx 里内嵌的图片素材
  sample.txt     PluginText 冒烟素材（UTF-8 中文 + 原样保留的符号）
  sample.epub    最小合法 EPUB（PluginBook 冒烟素材，同时覆盖共享 ZipReader 的 inflate）
  sample.html    带标题/粗体/列表的 HTML（PluginText 走富文本渲染的冒烟素材）
  sample.odt     最小合法 ODT（PluginWord 的 OpenDocument 分支冒烟素材）
  sample.svg     简单矢量图（PluginPicture 的 SVG 解码冒烟素材）
  sample.webp    WebP 位图（PluginPicture 的 WebP 解码冒烟素材）

依赖：python-docx、Pillow、openpyxl、python-pptx。素材已经入库，只有想改排版时才需要重新生成：
  python tests/fixtures/make_fixtures.py [输出目录]
"""
import io
import os
import struct
import sys
import re
import zipfile
import zlib

try:
    from docx import Document
    from docx.enum.text import WD_ALIGN_PARAGRAPH
    from docx.oxml.ns import qn
    from docx.opc.constants import RELATIONSHIP_TYPE as RT
    from docx.shared import Inches, Pt, RGBColor
    from PIL import Image, ImageDraw
except ImportError:
    # 缺依赖时让 _preflight_deps() 在 __main__ 里给出可读提示，而不是先抛 traceback。
    pass

WORK = (os.path.abspath(sys.argv[1]) if len(sys.argv) > 1
        else os.path.dirname(os.path.abspath(__file__)))
os.makedirs(WORK, exist_ok=True)


def make_png(path):
    img = Image.new("RGB", (160, 100), (40, 120, 200))
    d = ImageDraw.Draw(img)
    d.rectangle([10, 10, 149, 89], outline=(255, 255, 255), width=4)
    d.text((52, 42), "IMG", fill=(255, 255, 255))
    img.save(path)


def add_hyperlink(paragraph, url, text):
    part = paragraph.part
    r_id = part.relate_to(url, RT.HYPERLINK, is_external=True)
    hyperlink = paragraph._p.makeelement(qn("w:hyperlink"), {qn("r:id"): r_id})
    run = paragraph._p.makeelement(qn("w:r"), {})
    rpr = run.makeelement(qn("w:rPr"), {})
    color = rpr.makeelement(qn("w:color"), {qn("w:val"): "0563C1"})
    under = rpr.makeelement(qn("w:u"), {qn("w:val"): "single"})
    rpr.append(color)
    rpr.append(under)
    t = run.makeelement(qn("w:t"), {})
    t.text = text
    run.append(rpr)
    run.append(t)
    hyperlink.append(run)
    paragraph._p.append(hyperlink)


def make_basic():
    doc = Document()
    doc.core_properties.title = "基础排版测试文档"
    doc.core_properties.author = "测试作者"

    doc.add_heading("一级标题：测试文档标题", level=1)

    p = doc.add_paragraph("第一段普通正文，用于验证段落文本与顺序。")
    p.add_run(" 里面有")

    # 混合字符格式
    p2 = doc.add_paragraph()
    r = p2.add_run("粗体文字")
    r.bold = True
    r = p2.add_run("，斜体文字")
    r.italic = True
    r = p2.add_run("，下划线文字")
    r.underline = True
    r = p2.add_run("，删除线文字")
    r.font.strike = True
    r = p2.add_run("，红色大字")
    r.font.color.rgb = RGBColor(0xC0, 0x00, 0x00)
    r.font.size = Pt(18)
    r = p2.add_run("，上标x")
    r.font.superscript = True
    r = p2.add_run("，下标y")
    r.font.subscript = True
    r = p2.add_run("，结束。")

    # 对齐
    pc = doc.add_paragraph("居中对齐段落")
    pc.alignment = WD_ALIGN_PARAGRAPH.CENTER
    pr = doc.add_paragraph("右对齐段落")
    pr.alignment = WD_ALIGN_PARAGRAPH.RIGHT

    # 列表
    doc.add_paragraph("编号第一项", style="List Number")
    doc.add_paragraph("编号第二项", style="List Number")
    doc.add_paragraph("编号第三项", style="List Number")
    doc.add_paragraph("项目第一项", style="List Bullet")
    doc.add_paragraph("项目第二项", style="List Bullet")

    # 表格：表头横向合并 + 首列纵向合并 + 表头底纹
    t = doc.add_table(rows=3, cols=4)
    t.style = "Table Grid"
    hdr = t.cell(0, 0).merge(t.cell(0, 1))
    hdr.text = "合并表头"
    t.cell(0, 2).text = "列C"
    t.cell(0, 3).text = "列D"
    vm = t.cell(1, 0).merge(t.cell(2, 0))
    vm.text = "纵向合并"
    t.cell(1, 1).text = "R1C1"
    t.cell(1, 2).text = "R1C2"
    t.cell(1, 3).text = "R1C3"
    t.cell(2, 1).text = "R2C1"
    t.cell(2, 2).text = "R2C2"
    t.cell(2, 3).text = "R2C3"
    for cell in (hdr, t.cell(0, 2), t.cell(0, 3)):
        tcpr = cell._tc.get_or_add_tcPr()
        shd = tcpr.makeelement(qn("w:shd"), {})
        shd.set(qn("w:val"), "clear")
        shd.set(qn("w:fill"), "FFF2CC")
        tcpr.append(shd)

    # 内嵌图片
    pimg = doc.add_paragraph()
    pimg.add_run().add_picture(os.path.join(WORK, "pic.png"), width=Inches(1.5))

    # 超链接
    ph = doc.add_paragraph()
    ph.add_run("参考链接：")
    add_hyperlink(ph, "https://example.com/doc", "示例站点")

    out = os.path.join(WORK, "basic.docx")
    doc.save(out)
    return out


CT = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>
<Override PartName="/word/numbering.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml"/>
</Types>"""

ROOT_RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>"""

DOC_RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"/>"""

NS = ('xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" '
      'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"')

# 中文版 Word 的「标题 1」：styleId 是 "1"，样式名 localize 为「标题 1」
STYLES = f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles {NS}>
<w:docDefaults><w:rPrDefault><w:rPr>
<w:rFonts w:ascii="Times New Roman" w:eastAsia="宋体"/><w:sz w:val="21"/><w:szCs w:val="21"/>
</w:rPr></w:rPrDefault>
<w:pPrDefault><w:pPr><w:spacing w:line="360" w:lineRule="auto"/></w:pPr></w:pPrDefault></w:docDefaults>
<w:style w:type="paragraph" w:styleId="1"><w:name w:val="标题 1"/><w:basedOn w:val="Normal"/>
<w:pPr><w:outlineLvl w:val="0"/><w:spacing w:before="240" w:after="120"/></w:pPr>
<w:rPr><w:b/><w:sz w:val="44"/><w:color w:val="1F4E79"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Normal"><w:name w:val="Normal"/></w:style>
<w:style w:type="table" w:styleId="TableGrid"><w:name w:val="网格型"/>
<w:tblPr><w:tblBorders>
<w:top w:val="single" w:sz="4"/><w:left w:val="single" w:sz="4"/>
<w:bottom w:val="single" w:sz="4"/><w:right w:val="single" w:sz="4"/>
<w:insideH w:val="single" w:sz="4"/><w:insideV w:val="single" w:sz="4"/>
</w:tblBorders></w:tblPr></w:style>
</w:styles>"""

NUMBERING = f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:numbering {NS}>
<w:abstractNum w:abstractNumId="0">
<w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="chineseCounting"/><w:lvlText w:val="%1、"/></w:lvl>
<w:lvl w:ilvl="1"><w:start w:val="1"/><w:numFmt w:val="decimal"/><w:lvlText w:val="（%2）"/></w:lvl>
</w:abstractNum>
<w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num>
</w:numbering>"""


def p_text(text, extra_ppr="", rpr=""):
    return (f'<w:p><w:pPr>{extra_ppr}</w:pPr><w:r><w:rPr>{rpr}</w:rPr>'
            f'<w:t xml:space="preserve">{text}</w:t></w:r></w:p>')


def make_chinese():
    body = []
    body.append('<w:p><w:pPr><w:pStyle w:val="1"/></w:pPr>'
                '<w:r><w:t>中文样式标题</w:t></w:r></w:p>')

    npr = '<w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr>'
    body.append(p_text("中文编号第一项", npr))
    body.append(p_text("中文编号第二项", npr))
    body.append(p_text("中文编号第三项", npr))

    npr2 = '<w:numPr><w:ilvl w:val="1"/><w:numId w:val="1"/></w:numPr>'
    body.append(p_text("二级编号甲", npr2))
    body.append(p_text("二级编号乙", npr2))

    body.append('<w:p><w:pPr><w:jc w:val="center"/><w:ind w:firstLine="420"/></w:pPr>'
                '<w:r><w:t>居中且首行缩进的段落</w:t></w:r></w:p>')

    body.append('<w:p><w:r><w:rPr><w:b/><w:i/><w:u w:val="single"/>'
                '<w:color w:val="00B050"/><w:highlight w:val="yellow"/></w:rPr>'
                '<w:t>加粗斜体下划线绿字</w:t></w:r></w:p>')

    tbl = ('<w:tbl><w:tblPr><w:tblStyle w:val="TableGrid"/>'
           '<w:tblW w:w="0" w:type="auto"/></w:tblPr>'
           '<w:tblGrid><w:gridCol w:w="2000"/><w:gridCol w:w="2000"/></w:tblGrid>'
           '<w:tr>'
           '<w:tc><w:tcPr><w:gridSpan w:val="2"/><w:shd w:val="clear" w:fill="DDEBF7"/></w:tcPr>'
           '<w:p><w:r><w:t>横跨两列</w:t></w:r></w:p></w:tc>'
           '</w:tr>'
           '<w:tr>'
           '<w:tc><w:tcPr><w:vMerge w:val="restart"/></w:tcPr><w:p><w:r><w:t>纵向跨行</w:t></w:r></w:p></w:tc>'
           '<w:tc><w:p><w:r><w:t>RB1</w:t></w:r></w:p></w:tc>'
           '</w:tr>'
           '<w:tr>'
           '<w:tc><w:tcPr><w:vMerge/></w:tcPr><w:p/></w:tc>'
           '<w:tc><w:p><w:r><w:t>RB2</w:t></w:r></w:p></w:tc>'
           '</w:tr>'
           '</w:tbl>')
    body.append(tbl)
    body.append(p_text("表格之后的收尾段落。"))

    document = f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document {NS}><w:body>
{''.join(body)}
<w:sectPr><w:pgSz w:w="11906" w:h="16838"/></w:sectPr>
</w:body></w:document>"""

    out = os.path.join(WORK, "chinese.docx")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", CT)
        z.writestr("_rels/.rels", ROOT_RELS)
        z.writestr("word/document.xml", document)
        z.writestr("word/_rels/document.xml.rels", DOC_RELS)
        z.writestr("word/styles.xml", STYLES)
        z.writestr("word/numbering.xml", NUMBERING)
    return out


CONTAINER_XML = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

EPUB_OPF = """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Plugins Smoke EPUB</dc:title>
    <dc:identifier id="bookid">urn:uuid:00000000-0000-0000-0000-000000000001</dc:identifier>
    <dc:language>zh-CN</dc:language>
  </metadata>
  <manifest>
    <item id="chap1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine>
    <itemref idref="chap1"/>
  </spine>
</package>
"""

EPUB_CHAPTER = """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>第一章 冒烟</title></head>
<body>
  <h1>第一章 冒烟</h1>
  <p>EPUB 正文内容：这段文字来自测试素材。</p>
</body>
</html>
"""


def make_text():
    """PluginText 的冒烟素材：UTF-8 中文 + 不该被转义的符号。"""
    path = os.path.join(WORK, "sample.txt")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("文本插件冒烟测试\n")
        f.write("Second line: plain text stays as-is.\n")
        f.write("第三行：数字 12345，符号 <> & 应原样显示。\n")
    return path


def make_epub():
    """最小合法 EPUB：mimetype 必须是第一个条目且不压缩（OCF 规范）。"""
    path = os.path.join(WORK, "sample.epub")
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), b"application/epub+zip",
                   zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER_XML, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/content.opf", EPUB_OPF, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/chapter1.xhtml", EPUB_CHAPTER, zipfile.ZIP_DEFLATED)
    return path


HTML_SAMPLE = """<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>HTML 冒烟素材</title>
  <style>body { font-family: sans-serif; }</style>
</head>
<body>
  <h1>HTML 标题</h1>
  <p>普通段落，含 <b>粗体</b> 与 <i>斜体</i>。</p>
  <ul><li>列表项一</li><li>列表项二</li></ul>
  <table border="1"><tr><td>表格 A1</td><td>表格 B1</td></tr></table>
</body>
</html>
"""

ODT_CONTENT = """<?xml version="1.0" encoding="UTF-8"?>
<office:document-content
 xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0"
 xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0"
 xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0"
 xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0"
 xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0"
 xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0"
 xmlns:xlink="http://www.w3.org/1999/xlink"
 xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0"
 office:version="1.3">
 <office:automatic-styles>
  <style:style style:name="P1" style:family="paragraph">
   <style:paragraph-properties fo:text-align="center"/>
  </style:style>
  <style:style style:name="T1" style:family="text">
   <style:text-properties fo:font-weight="bold" fo:color="#cc0000"/>
  </style:style>
  <style:style style:name="T2" style:family="text">
   <style:text-properties fo:font-style="italic"/>
  </style:style>
 </office:automatic-styles>
 <office:body>
  <office:text>
   <text:h text:outline-level="1">第一章 ODT 冒烟</text:h>
   <text:p>普通段落，含<text:span text:style-name="T1">红色粗体</text:span>与<text:span text:style-name="T2">斜体</text:span>。</text:p>
   <text:p text:style-name="P1">居中段落</text:p>
   <text:list text:style-name="L1">
    <text:list-item><text:p>有序项一</text:p></text:list-item>
    <text:list-item><text:p>有序项二</text:p></text:list-item>
   </text:list>
   <table:table>
    <table:table-row>
     <table:table-cell><text:p>单元格 A1</text:p></table:table-cell>
     <table:table-cell><text:p>单元格 B1</text:p></table:table-cell>
    </table:table-row>
    <table:table-row>
     <table:table-cell><text:p>单元格 A2</text:p></table:table-cell>
     <table:table-cell><text:p>单元格 B2</text:p></table:table-cell>
    </table:table-row>
   </table:table>
  </office:text>
 </office:body>
</office:document-content>
"""

ODT_STYLES = """<?xml version="1.0" encoding="UTF-8"?>
<office:document-styles
 xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0"
 xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0"
 xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0"
 xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0"
 office:version="1.3">
 <office:styles>
  <text:list-style style:name="L1">
   <text:list-level-style-number text:level="1" style:num-format="1"/>
  </text:list-style>
 </office:styles>
</office:document-styles>
"""

ODT_MANIFEST = """<?xml version="1.0" encoding="UTF-8"?>
<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0" manifest:version="1.3">
 <manifest:file-entry manifest:full-path="/" manifest:media-type="application/vnd.oasis.opendocument.text"/>
 <manifest:file-entry manifest:full-path="content.xml" manifest:media-type="text/xml"/>
 <manifest:file-entry manifest:full-path="styles.xml" manifest:media-type="text/xml"/>
</manifest:manifest>
"""

SVG_SAMPLE = """<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <rect x="4" y="4" width="112" height="72" rx="8" fill="#2b6cb0"/>
  <circle cx="40" cy="40" r="20" fill="#f6ad55"/>
  <text x="70" y="46" font-size="14" fill="#ffffff">SVG</text>
</svg>
"""


def make_html():
    path = os.path.join(WORK, "sample.html")
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(HTML_SAMPLE)
    return path


def make_odt():
    """最小合法 ODT：mimetype 第一个条目且不压缩；含标题/字符格式/列表/表格。"""
    path = os.path.join(WORK, "sample.odt")
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), b"application/vnd.oasis.opendocument.text",
                   zipfile.ZIP_STORED)
        z.writestr("META-INF/manifest.xml", ODT_MANIFEST, zipfile.ZIP_DEFLATED)
        z.writestr("content.xml", ODT_CONTENT, zipfile.ZIP_DEFLATED)
        z.writestr("styles.xml", ODT_STYLES, zipfile.ZIP_DEFLATED)
    return path


def make_svg():
    path = os.path.join(WORK, "sample.svg")
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(SVG_SAMPLE)
    return path


def make_webp():
    path = os.path.join(WORK, "sample.webp")
    img = Image.new("RGB", (64, 48), (43, 108, 176))
    img.save(path, "WEBP")
    return path


def make_rtf():
    """RTF 子集素材：对齐 + 粗体 + 斜体 + 颜色 + 中英文（\\uN 转义）。"""
    # 中文字符用 \uN? 形式（N 是十进制码点，? 是回退字符，解析时跳过）
    title = r"\u31532?\u19968?\u31456?\u32?RTF\u32?\u20882?\u28895?"
    body  = (r"\u26222?\u36890?\u27573?\u33853? {\b \u31895?\u20307?}"
             r" {\i \u26012?\u20307?} {\cf1 \u32418?\u33394?}\par")
    rtf = ("{\\rtf1\\ansi\\deff0\\deflang2052\n"
           "{\\fonttbl{\\f0\\fnil Calibri;}{\\f1\\fnil SimSun;}}\n"
           "{\\colortbl;\\red204\\green0\\blue0;}\n"
           "\\pard\\qc\\f0\\fs32 " + title + "\\par\n"
           "\\pard\\ql\\f1\\fs22 " + body + "\n"
           "}\n")
    path = os.path.join(WORK, "sample.rtf")
    with io.open(path, "w", encoding="ascii", newline="\n") as f:
        f.write(rtf)
    return path


ODS_MIMETYPE = "application/vnd.oasis.opendocument.spreadsheet"
ODP_MIMETYPE = "application/vnd.oasis.opendocument.presentation"

ODS_CONTENT = """<?xml version="1.0" encoding="UTF-8"?>
<office:document-content
 xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0"
 xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0"
 xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0"
 xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0"
 office:version="1.3">
 <office:body>
  <office:spreadsheet>
   <table:table table:name="Sheet1">
    <table:table-row>
     <table:table-cell office:value-type="string"><text:p>姓名</text:p></table:table-cell>
     <table:table-cell office:value-type="string"><text:p>分数</text:p></table:table-cell>
    </table:table-row>
    <table:table-row>
     <table:table-cell office:value-type="string"><text:p>王五</text:p></table:table-cell>
     <table:table-cell office:value-type="float" office:value="77"><text:p>77</text:p></table:table-cell>
    </table:table-row>
   </table:table>
  </office:spreadsheet>
 </office:body>
</office:document-content>
"""

ODP_CONTENT = """<?xml version="1.0" encoding="UTF-8"?>
<office:document-content
 xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0"
 xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0"
 xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0"
 xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0"
 xmlns:xlink="http://www.w3.org/1999/xlink"
 office:version="1.3">
 <office:body>
  <office:presentation>
   <draw:page draw:name="第1页">
    <draw:frame svg:width="20cm" svg:height="2cm">
     <draw:text-box><text:h>ODP 冒烟标题</text:h></draw:text-box>
    </draw:frame>
    <draw:frame svg:width="20cm" svg:height="6cm">
     <draw:text-box><text:p>第一页正文内容</text:p></draw:text-box>
    </draw:frame>
   </draw:page>
   <draw:page draw:name="第2页">
    <draw:frame svg:width="20cm" svg:height="2cm">
     <draw:text-box><text:h>第二页标题</text:h></draw:text-box>
    </draw:frame>
   </draw:page>
  </office:presentation>
 </office:body>
</office:document-content>
"""


def _write_odf(path, mimetype, content_xml, extra_files=None):
    """写一个最小 ODF 包（mimetype 必须是第一个条目且不压缩）。"""
    manifest = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0"'
        ' manifest:version="1.3">\n'
        ' <manifest:file-entry manifest:full-path="/" manifest:media-type="%s"/>\n'
        ' <manifest:file-entry manifest:full-path="content.xml" manifest:media-type="text/xml"/>\n'
        '</manifest:manifest>\n' % mimetype
    )
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), mimetype.encode("ascii"), zipfile.ZIP_STORED)
        z.writestr("META-INF/manifest.xml", manifest, zipfile.ZIP_DEFLATED)
        z.writestr("content.xml", content_xml, zipfile.ZIP_DEFLATED)
        for name, data in (extra_files or {}).items():
            z.writestr(name, data, zipfile.ZIP_DEFLATED)
    return path


def make_ods():
    path = os.path.join(WORK, "sample.ods")
    return _write_odf(path, ODS_MIMETYPE, ODS_CONTENT)


def make_odp():
    path = os.path.join(WORK, "sample.odp")
    return _write_odf(path, ODP_MIMETYPE, ODP_CONTENT)


def make_xlsx():
    """用 openpyxl 生成：两个工作表 + 合并单元格 + 数字/中文。"""
    from openpyxl import Workbook

    wb = Workbook()
    ws = wb.active
    ws.title = "数据"
    ws["A1"] = "姓名"
    ws["B1"] = "分数"
    ws["C1"] = "备注"
    ws["A2"] = "张三"
    ws["B2"] = 95
    ws["C2"] = "第一行"
    ws["A3"] = "李四"
    ws["B3"] = 88
    ws["A5"] = "合并单元格"
    ws.merge_cells("A5:B5")

    ws2 = wb.create_sheet("第二页")
    ws2["A1"] = "第二张表"
    ws2["A2"] = 123

    path = os.path.join(WORK, "sample.xlsx")
    wb.save(path)
    return path


def make_pptx():
    """用 python-pptx 生成：两页，各自标题与正文。"""
    from pptx import Presentation
    from pptx.util import Inches

    prs = Presentation()
    slide1 = prs.slides.add_slide(prs.slide_layouts[1])   # Title and Content
    slide1.shapes.title.text = "PPTX 冒烟标题"
    slide1.placeholders[1].text = "第一页正文内容"

    slide2 = prs.slides.add_slide(prs.slide_layouts[5])   # Title Only
    slide2.shapes.title.text = "第二页标题"
    box = slide2.shapes.add_textbox(Inches(1), Inches(2), Inches(5), Inches(1.5))
    box.text_frame.text = "第二页文本框内容"

    path = os.path.join(WORK, "sample.pptx")
    prs.save(path)
    return path


FWDA_MAGIC = b"FWDA"


def crc32_bytes(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def build_fwda(entries, root=""):
    """按 FWDA v2 布局生成归档字节（entries: [(相对路径, 内容 bytes)]）。

    布局与 FileWrapper/archiveformat.h、filemanager.cpp 的 parseFromDevice 一致：
      全局头 24B: magic | version=2 | headerSize | fileCount | headerChecksum | reserved
      头部区    : rootLen + root(UTF-8)；每条 pathLen|size|contentPos|contentCrc32|path
      headerChecksum = CRC32(全局头[0..15] + 全局头[20..23] + 头部区)
    """
    root_b = root.encode("utf-8")
    entry_fixed = 4 + 8 + 8 + 4
    hsize = (24 + 4 + len(root_b)
             + sum(entry_fixed + len(p.encode("utf-8")) for p, _ in entries))

    out = bytearray(FWDA_MAGIC)
    out += struct.pack("<III", 2, hsize, len(entries))
    out += b"\x00\x00\x00\x00"      # headerChecksum 占位
    out += b"\x00\x00\x00\x00"      # reserved
    out += struct.pack("<I", len(root_b))
    out += root_b

    pos = hsize
    for path, data in entries:
        path_b = path.encode("utf-8")
        out += struct.pack("<I", len(path_b))
        out += struct.pack("<Q", len(data))
        out += struct.pack("<Q", pos)
        out += struct.pack("<I", crc32_bytes(data))
        out += path_b
        pos += len(data)

    header = bytes(out)
    out[16:20] = struct.pack("<I",
                             crc32_bytes(header[0:16] + header[20:24] + header[24:hsize]))
    for _, data in entries:
        out += data
    return bytes(out)


def make_rooted_dat():
    """一个 root 不是当前盘符的归档：用来验证合并展开后路径归属不会被改挂。

    root 故意写成 D:/testfiles_copy（与测试素材所在的 E: 不同盘），
    条目 a/file1.txt 合并后应仍保留 D/testfiles_copy/... 的来源信息。
    """
    path = os.path.join(WORK, "rooted.dat")
    with open(path, "wb") as f:
        f.write(build_fwda([("a/file1.txt", b"rooted archive entry")],
                           root="D:/testfiles_copy"))
    return path


def make_dup_paths_dat():
    """两条相对路径完全相同的归档：用于验证保存时的重复路径拦截。

    真实场景来自旧版"添加外部文件后保存"：root 为空时条目被拍平成文件名，
    不同目录的同名文件会撞成同一路径，保存后无法区分。
    """
    path = os.path.join(WORK, "dup_paths.dat")
    with open(path, "wb") as f:
        f.write(build_fwda([("same.bin", b"first copy"), ("same.bin", b"second copy")]))
    return path


def make_broken_files():
    with open(os.path.join(WORK, "legacy.doc"), "wb") as f:
        f.write(bytes([0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1]) + b"\x00" * 5000)

    with open(os.path.join(WORK, "broken.docx"), "wb") as f:
        f.write(b"PK\x03\x04" + bytes(range(256)) * 20)

    with zipfile.ZipFile(os.path.join(WORK, "notword.docx"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("hello.txt", "this zip has no word/document.xml")

    open(os.path.join(WORK, "empty.docx"), "wb").close()


def _normalize_zip_time(path):
    """Rewrite every zip entry with a fixed timestamp and fixed core.xml dates.

    Generated fixtures are checked in; without this, every regeneration changes
    bytes even when content is identical, making review diffs meaningless.
    """
    tmp = path + ".tmp"
    with zipfile.ZipFile(path, "r") as src, zipfile.ZipFile(tmp, "w") as dst:
        for info in src.infolist():
            data = src.read(info.filename)
            if info.filename.endswith("docProps/core.xml"):
                txt = data.decode("utf-8")
                txt = re.sub(r"(<dcterms:created[^>]*>)[^<]*(</dcterms:created>)",
                             r"\g<1>2020-01-01T00:00:00Z\g<2>", txt)
                txt = re.sub(r"(<dcterms:modified[^>]*>)[^<]*(</dcterms:modified>)",
                             r"\g<1>2020-01-01T00:00:00Z\g<2>", txt)
                data = txt.encode("utf-8")
            zi = zipfile.ZipInfo(info.filename, date_time=(2020, 1, 1, 0, 0, 0))
            zi.compress_type = info.compress_type
            zi.external_attr = info.external_attr
            dst.writestr(zi, data)
    os.replace(tmp, path)


def _normalize_all_fixtures():
    for name in os.listdir(WORK):
        if name.lower().endswith((".docx", ".xlsx", ".pptx", ".odt", ".ods",
                                  ".odp", ".epub")):
            _normalize_zip_time(os.path.join(WORK, name))


def _preflight_deps():
    missing = []
    for mod, name in (("docx", "python-docx"), ("PIL", "Pillow"),
                      ("openpyxl", "openpyxl"), ("pptx", "python-pptx")):
        try:
            __import__(mod)
        except ImportError:
            missing.append(name)
    if missing:
        raise SystemExit("missing dependencies: " + ", ".join(missing) + "; pip install ...")


if __name__ == "__main__":
    _preflight_deps()
    make_png(os.path.join(WORK, "pic.png"))
    for fn in (make_basic, make_chinese):
        path = fn()
        print("wrote", path, os.path.getsize(path), "bytes")
    for fn in (make_text, make_epub):
        path = fn()
        print("wrote", path, os.path.getsize(path), "bytes")
    for fn in (make_html, make_odt, make_svg, make_webp, make_rtf,
               make_ods, make_odp, make_xlsx, make_pptx, make_rooted_dat,
               make_dup_paths_dat):
        path = fn()
        print("wrote", path, os.path.getsize(path), "bytes")
    make_broken_files()
    _normalize_all_fixtures()
    print("wrote broken fixtures into", WORK)
