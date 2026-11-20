#!/usr/bin/env node
/**
 * IconMCP (https://iconmcp.com/mcp) 的最小 Streamable-HTTP MCP 客户端。
 *
 * 站点只提供 MCP 端点（JSON-RPC 2.0），没有普通 REST 下载接口。
 * Windows 上 PowerShell 的 schannel 取不到 TLS 凭证（SEC_E_NO_CREDENTIALS），
 * 所以统一用 Node 的 OpenSSL 栈发请求。
 *
 * CLI：
 *   node iconmcp.mjs tools
 *   node iconmcp.mjs sets
 *   node iconmcp.mjs search "merge files" [--limit 12]
 *   node iconmcp.mjs get <icon-name> [--style outline|solid|duotone] [--size 24] [--out x.svg]
 *   node iconmcp.mjs batch <queries.json> [--out results.json]
 */

import { pathToFileURL } from 'node:url';

const URL_MCP = process.env.ICONMCP_URL || 'http://iconmcp.com/mcp';

function parseSse(text) {
  const out = [];
  for (const block of text.split(/\r?\n\r?\n/)) {
    const data = block
      .split(/\r?\n/)
      .filter((l) => l.startsWith('data:'))
      .map((l) => l.slice(5).trimStart())
      .join('\n');
    if (data) out.push(data);
  }
  return out;
}

export class IconMcpClient {
  constructor(url = URL_MCP) {
    this.url = url;
    this.sessionId = null;
    this.nextId = 1;
  }

  async rpc(method, params) {
    const id = this.nextId++;
    const headers = {
      'content-type': 'application/json',
      accept: 'application/json, text/event-stream',
      'mcp-protocol-version': '2025-06-18',
    };
    if (this.sessionId) headers['mcp-session-id'] = this.sessionId;

    const res = await fetch(this.url, {
      method: 'POST',
      headers,
      body: JSON.stringify({ jsonrpc: '2.0', id, method, params }),
      // 站点偶发挂起：没有超时的话整批下载会卡死在同一只图标上
      signal: AbortSignal.timeout(Number(process.env.ICONMCP_TIMEOUT_MS || 25000)),
    });
    const sid = res.headers.get('mcp-session-id');
    if (sid) this.sessionId = sid;

    const text = await res.text();
    if (!res.ok) throw new Error(`HTTP ${res.status} for ${method}: ${text.slice(0, 500)}`);

    const ctype = res.headers.get('content-type') || '';
    const payloads = ctype.includes('text/event-stream') ? parseSse(text) : [text];
    for (const p of payloads) {
      if (!p.trim()) continue;
      let msg;
      try {
        msg = JSON.parse(p);
      } catch {
        continue;
      }
      if (msg.error) throw new Error(`${method}: ${JSON.stringify(msg.error)}`);
      if (msg.id === id) return msg.result;
    }
    throw new Error(`no JSON-RPC response for ${method}`);
  }

  async connect() {
    const init = await this.rpc('initialize', {
      protocolVersion: '2025-06-18',
      capabilities: {},
      clientInfo: { name: 'filewrapper-iconset', version: '1.0.0' },
    });
    const headers = {
      'content-type': 'application/json',
      accept: 'application/json, text/event-stream',
      'mcp-protocol-version': '2025-06-18',
      'mcp-session-id': this.sessionId,
    };
    try {
      await fetch(this.url, {
        method: 'POST',
        headers,
        body: JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized', params: {} }),
        signal: AbortSignal.timeout(Number(process.env.ICONMCP_TIMEOUT_MS || 25000)),
      });
    } catch {
      // 通知是 fire-and-forget，站点没应答也不影响后续 tools/call
    }
    return init;
  }

  async callTool(name, args) {
    const r = await this.rpc('tools/call', { name, arguments: args });
    const text = (r?.content || [])
      .filter((c) => c.type === 'text')
      .map((c) => c.text)
      .join('\n');
    return { text, structured: r?.structuredContent, raw: r };
  }

  async search(query, { limit = 10, set } = {}) {
    const args = { query, limit };
    if (set) args.set = set;
    const { text, structured } = await this.callTool('search_icons', args);
    if (structured && Array.isArray(structured.results)) return structured;
    try {
      return JSON.parse(text);
    } catch {
      throw new Error(`search_icons returned unexpected payload: ${text.slice(0, 300)}`);
    }
  }

  async getIcon(name, opts = {}) {
    const { text } = await this.callTool('get_icon', { name, ...opts });
    const m = text.match(/<svg[\s\S]*<\/svg>/);
    if (!m) throw new Error(`get_icon(${name}) returned no <svg>: ${text.slice(0, 300)}`);
    return m[0];
  }
}

async function cli() {
  const argv = process.argv.slice(2);
  const cmd = argv[0];
  const opt = (name, dflt) => {
    const i = argv.indexOf(`--${name}`);
    return i >= 0 ? argv[i + 1] : dflt;
  };
  const fs = await import('node:fs/promises');

  if (!cmd || cmd === '-h' || cmd === '--help') {
    console.log(
      'usage: iconmcp.mjs tools|sets|search <query>|get <name>|batch <queries.json> [options]',
    );
    return;
  }

  const c = new IconMcpClient();
  await c.connect();

  if (cmd === 'tools') {
    const r = await c.rpc('tools/list', {});
    for (const t of r.tools) console.log(`${t.name}: ${t.description || ''}\n`);
    return;
  }

  if (cmd === 'sets') {
    const { text } = await c.callTool('list_sets', {});
    console.log(text);
    return;
  }

  if (cmd === 'search') {
    const r = await c.search(argv[1], { limit: Number(opt('limit', '12')) });
    console.log(JSON.stringify(r, null, 2));
    return;
  }

  if (cmd === 'get') {
    const svg = await c.getIcon(argv[1], {
      style: opt('style', 'outline'),
      size: Number(opt('size', '24')),
      stroke: Number(opt('stroke', '1.25')),
      color: opt('color'),
    });
    const out = opt('out');
    if (out) {
      await fs.writeFile(out, svg, 'utf8');
      console.log(`wrote ${out} (${svg.length} bytes)`);
    } else {
      console.log(svg);
    }
    return;
  }

  if (cmd === 'batch') {
    const queries = JSON.parse(await fs.readFile(argv[1], 'utf8'));
    const results = {};
    for (const q of queries) {
      try {
        results[q.key || q.query] = await c.search(q.query, { limit: q.limit || 10, set: q.set });
        const names = results[q.key || q.query].results.map((x) => `${x.name} (${x.label})`).join(', ');
        console.log(`${q.key || q.query}: ${names}`);
      } catch (e) {
        results[q.key || q.query] = { error: String(e.message || e) };
        console.error(`${q.key || q.query}: ERROR ${e.message}`);
      }
    }
    const out = opt('out');
    if (out) {
      await fs.writeFile(out, JSON.stringify(results, null, 2), 'utf8');
      console.log(`wrote ${out}`);
    }
    return;
  }

  throw new Error(`unknown command: ${cmd}`);
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  cli().catch((e) => {
    console.error(String(e.stack || e));
    process.exit(1);
  });
}
