#!/usr/bin/env node
/**
 * 按 iconset.json 生成 resources/icons 下的 PNG 与 resources.qrc。
 *
 *   node tools/icons/build-iconset.mjs
 *
 * 流程：
 *   1. 缺字形 SVG 时用 IconMCP 取（已存在则离线可用，不依赖网络）
 *   2. 调 svg2png 渲染 16/18/20/24/32/40/48/64 px（appicon 另有 128/256）
 *   3. 组合 appicon.svg（品牌渐变圆角底 + 白色字形）并生成 appicon.ico
 *   4. 重新生成 resources.qrc 的 /icons 段
 *
 * svg2png 用 QtSvg 渲染；构建方法见 tools/icons/README.md。
 */
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { IconMcpClient } from './iconmcp.mjs';

const HERE = import.meta.dirname;
const ROOT = path.resolve(HERE, '..', '..');
const RES = path.join(ROOT, 'resources');
const ICON_DIR = path.join(RES, 'icons');
const SVG_DIR = path.join(ICON_DIR, 'svg');
// svg2png 在部分受限环境里只能写工作目录：统一先渲到 tools/icons/work/render，
// 再由 Node 复制进 resources/icons（Node 的写权限是完整的）。
const STAGE = path.join(HERE, 'work', 'render');
const SPEC = JSON.parse(fs.readFileSync(path.join(HERE, 'iconset.json'), 'utf8'));
const SVG2PNG = process.env.SVG2PNG || path.join(HERE, 'svg2png', '.build', 'svg2png.exe');

function fail(msg) {
  console.error(`error: ${msg}`);
  process.exit(1);
}

function readSvg(name) {
  return fs.readFileSync(path.join(SVG_DIR, name), 'utf8');
}

async function fetchGlyph(name, style, target, { stroke = 1.5 } = {}) {
  if (fs.existsSync(target)) return false;
  let lastError;
  for (let attempt = 1; attempt <= 3; attempt++) {
    try {
      const client = new IconMcpClient();
      await client.connect();
      const svg = await client.getIcon(name, { style, size: 24, stroke });
      fs.writeFileSync(target, svg, 'utf8');
      console.log(`fetch ${path.basename(target)}  <- ${name} (${style})`);
      return true;
    } catch (e) {
      lastError = e;
      console.warn(`retry ${name} ${style} (${attempt}/3): ${e.message}`);
    }
  }
  fail(`IconMCP 取不到 ${name} (${style}): ${lastError?.message}`);
}

function svgBody(svgText) {
  const m = svgText.match(/<svg[^>]*>([\s\S]*)<\/svg>/);
  if (!m) fail('字形 SVG 缺少 <svg> 主体');
  return m[1].trim();
}

function makeAppIconSvg(glyphXml, spec) {
  const glyph = svgBody(glyphXml).replaceAll('currentColor', spec.glyphColor);
  const side = 16 * spec.scale;
  const off = (256 - side) / 2;
  const solid = spec.style === 'solid';
  const paint = solid
    ? `fill="${spec.glyphColor}" stroke="${spec.glyphColor}"`
    : `stroke="${spec.glyphColor}" fill="none"`;
  const [top, bottom] = spec.background;
  return `<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="${top}"/>
      <stop offset="1" stop-color="${bottom}"/>
    </linearGradient>
  </defs>
  <rect width="256" height="256" rx="${spec.radius}" ry="${spec.radius}" fill="url(#bg)"/>
  <g transform="translate(${off},${off}) scale(${spec.scale})" ${paint}
     stroke-width="1.25" stroke-linecap="round" stroke-linejoin="round">
${glyph.split('\n').map((line) => '    ' + line).join('\n')}
  </g>
</svg>
`;
}

function render(svgPath, prefix, sizes, color) {
  fs.mkdirSync(STAGE, { recursive: true });
  const args = [
    '--in', svgPath,
    '--outdir', STAGE,
    '--prefix', prefix,
    '--sizes', sizes.join(','),
    '--color', color,
  ];
  const r = spawnSync(SVG2PNG, args, { stdio: 'inherit' });
  if (r.error) fail(`无法运行 ${SVG2PNG}: ${r.error.message}`);
  if (r.status !== 0) fail(`svg2png 渲染 ${prefix} 失败（exit ${r.status}）`);
  for (const size of sizes) {
    const name = `${prefix}_${size}.png`;
    fs.copyFileSync(path.join(STAGE, name), path.join(ICON_DIR, name));
  }
}

/** 多尺寸 PNG 打包成 ICO（Vista+ 支持 PNG 编码的图标项）。 */
function writeIco(target, entries) {
  const header = Buffer.alloc(6);
  header.writeUInt16LE(0, 0);
  header.writeUInt16LE(1, 2);
  header.writeUInt16LE(entries.length, 4);

  const directory = Buffer.alloc(16 * entries.length);
  let offset = 6 + directory.length;
  entries.forEach((entry, i) => {
    const b = entry.size >= 256 ? 0 : entry.size;
    directory.writeUInt8(b, i * 16 + 0);
    directory.writeUInt8(b, i * 16 + 1);
    directory.writeUInt8(0, i * 16 + 2);
    directory.writeUInt8(0, i * 16 + 3);
    directory.writeUInt16LE(1, i * 16 + 4);
    directory.writeUInt16LE(32, i * 16 + 6);
    directory.writeUInt32LE(entry.data.length, i * 16 + 8);
    directory.writeUInt32LE(offset, i * 16 + 12);
    offset += entry.data.length;
  });

  fs.writeFileSync(target, Buffer.concat([header, directory, ...entries.map((e) => e.data)]));
}

function qrcXml() {
  const lines = [];
  lines.push('<!DOCTYPE RCC><RCC version="1.0">');
  lines.push('<qresource prefix="/icons">');
  for (const slot of Object.keys(SPEC.icons).sort()) {
    for (const size of SPEC.sizes) {
      lines.push(`  <file alias="${slot}_${size}.png">icons/${slot}_${size}.png</file>`);
    }
  }
  const app = SPEC.appicon;
  lines.push('  <file alias="appicon.png">icons/appicon.png</file>');
  for (const size of app.sizes) {
    lines.push(`  <file alias="appicon_${size}.png">icons/appicon_${size}.png</file>`);
  }
  lines.push('</qresource>');
  lines.push('<qresource prefix="/theme">');
  lines.push('  <file alias="light.qss">theme/light.qss</file>');
  lines.push('  <file alias="icons/close_light.xpm">theme/icons/close_light.xpm</file>');
  lines.push('  <file alias="icons/close_light_hover.xpm">theme/icons/close_light_hover.xpm</file>');
  lines.push('</qresource>');
  lines.push('</RCC>');
  return lines.join('\n') + '\n';
}

async function main() {
  if (!fs.existsSync(SVG2PNG)) {
    fail(`找不到 ${SVG2PNG}；先按 tools/icons/README.md 构建 svg2png，或设置 SVG2PNG 环境变量`);
  }
  fs.mkdirSync(SVG_DIR, { recursive: true });

  // 1. 字形
  for (const [slot, glyph] of Object.entries(SPEC.icons)) {
    const target = path.join(SVG_DIR, `${slot}.svg`);
    if (!fs.existsSync(target)) await fetchGlyph(glyph, SPEC.meta.style, target, { stroke: SPEC.meta.stroke });
  }
  const appGlyph = path.join(SVG_DIR, 'appicon-glyph.svg');
  await fetchGlyph(SPEC.appicon.glyph, SPEC.appicon.style, appGlyph);

  // 2. 普通图标
  for (const [slot, glyph] of Object.entries(SPEC.icons)) {
    const svg = path.join(SVG_DIR, `${slot}.svg`);
    const color = SPEC.meta.ink;
    render(svg, slot, SPEC.sizes, color);
    console.log(`render ${slot}  <- ${glyph}`);
  }

  // 3. 应用图标
  const appSvgPath = path.join(SVG_DIR, 'appicon.svg');
  fs.writeFileSync(appSvgPath, makeAppIconSvg(fs.readFileSync(appGlyph, 'utf8'), SPEC.appicon), 'utf8');
  render(appSvgPath, 'appicon', SPEC.appicon.sizes, '#FFFFFF');
  fs.copyFileSync(path.join(ICON_DIR, 'appicon_256.png'), path.join(ICON_DIR, 'appicon.png'));
  writeIco(
    path.join(ICON_DIR, 'appicon.ico'),
    SPEC.appicon.sizes.map((size) => ({
      size,
      data: fs.readFileSync(path.join(ICON_DIR, `appicon_${size}.png`)),
    })),
  );
  // qmake 的 windres 规则只依赖 appicon.rc、不依赖 .ico：不碰一下 rc，
  // 改了图标后 exe 里嵌的还是旧 ICO。更新时间戳即可让下一次 make 重新 windres。
  const rc = path.join(RES, 'appicon.rc');
  if (fs.existsSync(rc)) {
    const now = new Date();
    fs.utimesSync(rc, now, now);
  }
  console.log('render appicon (+ appicon.ico, touched appicon.rc)');

  // 4. qrc
  fs.writeFileSync(path.join(RES, 'resources.qrc'), qrcXml(), 'utf8');
  console.log(`qrc  -> resources/resources.qrc (${Object.keys(SPEC.icons).length} icons x ${SPEC.sizes.length} sizes)`);
}

main().catch((e) => fail(e?.stack || String(e)));
