#!/usr/bin/env node
// 按候选清单批量下载 SVG 到 work/candidates/，供人工挑选。
//
// IconMCP 偶尔会对某个名字挂起（服务端不返回），所以每条请求：
//   - 独立 MCP 会话（互不牵连）
//   - 15s 超时 + 2 次重试
//   - 3 路并发，避免串行等挂起
import fs from 'node:fs/promises';
import path from 'node:path';
import { IconMcpClient } from './iconmcp.mjs';

const names = process.argv.slice(2);
if (!names.length) {
  console.error('usage: node fetch-candidates.mjs <icon-name> [...]');
  process.exit(2);
}

const outDir = path.join(import.meta.dirname, 'work', 'candidates');
await fs.mkdir(outDir, { recursive: true });

async function fetchOne(name) {
  const file = path.join(outDir, `${name}.svg`);
  try {
    await fs.access(file);
    return `skip ${name}`;
  } catch {
    /* not downloaded yet */
  }
  for (let attempt = 1; attempt <= 2; attempt++) {
    try {
      const c = new IconMcpClient();
      await c.connect();
      const svg = await c.getIcon(name, { style: 'outline', size: 24, stroke: 1.5 });
      await fs.writeFile(file, svg, 'utf8');
      return `ok   ${name}`;
    } catch (e) {
      if (attempt === 2) return `FAIL ${name}: ${e.message}`;
    }
  }
  return `FAIL ${name}`;
}

const queue = [...names];
const workers = Array.from({ length: 3 }, async () => {
  while (queue.length) {
    const name = queue.shift();
    console.log(await fetchOne(name));
  }
});
await Promise.all(workers);
