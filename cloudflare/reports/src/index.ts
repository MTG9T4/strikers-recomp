// SPDX-License-Identifier: GPL-3.0-or-later
// Strikers Recomp's report service: players send a report zip only when they choose to (F1 > Report > Send, or Yes after a
// crash). The game's side is runtime/core/report_send.c; the format is in README.md. Workers + KV only, and it stays inside
// the free allowances on any plan: KV writes (2 a report, DAILY_CAP a day) and KV storage (STORE_CAP_MB in all), so a
// flood of reports fills the service instead of running up a bill.
// IP addresses are used for the rate limit and never stored or logged.

interface RateLimit { limit(options: { key: string }): Promise<{ success: boolean }> }
interface Env {
  REPORTS: KVNamespace;
  ADMIN_TOKEN: string;
  REPORT_LIMIT?: RateLimit;
  DAILY_CAP?: string;
  STORE_CAP_MB?: string;
}
interface Usage { bytes: number; day: string; n: number }

const MAX_ZIP = 10 * 1024 * 1024;
const KEEP_SECONDS = 90 * 86400;
const ID_LETTERS = '23456789ABCDEFGHJKMNPQRSTUVWXYZ';
const KINDS = new Set(['menu', 'f9', 'crash']);
const FIELDS: [string, string, number][] = [
  ['version', 'X-Strikers-Version', 32], ['kind', 'X-Strikers-Kind', 8], ['gpu', 'X-Strikers-GPU', 128],
  ['cpu', 'X-Strikers-CPU', 128], ['os', 'X-Strikers-OS', 64], ['memory', 'X-Strikers-Memory', 16],
  ['description', 'X-Strikers-Description', 1000]];

const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' } });
const fail = (status: number, error: string) => json({ error }, status);

function newId(): string {
  const bytes = crypto.getRandomValues(new Uint8Array(6));
  return 'R-' + Array.from(bytes, b => ID_LETTERS[b % ID_LETTERS.length]).join('');
}

// Constant time for equal lengths; the token's length is not a secret worth hiding.
function sameToken(given: string, expected: string): boolean {
  const a = new TextEncoder().encode(given), b = new TextEncoder().encode(expected);
  if (!expected || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}

// The summary as KV metadata (at most 1024 bytes): the description is cut until it fits.
function metadata(meta: Record<string, string | number>): Record<string, string | number> {
  const m = { ...meta };
  let d = String(m.description ?? '');
  while (JSON.stringify(m).length > 1000 && d.length) { d = d.slice(0, Math.max(0, d.length - 50)); m.description = d + '...'; }
  return m;
}

// One usage record a calendar month: bytes stored that month, and the count for its latest day. A report lives 90 days, so
// the reports still stored all fall in this month and the 3 before it; summing those 4 can only overcount.
const monthKey = (iso: string) => 'use/' + iso.slice(0, 7);
async function usage(env: Env, key: string): Promise<Usage> {
  return { bytes: 0, day: '', n: 0, ...((await env.REPORTS.get(key, 'json')) as Usage | null ?? {}) };
}
function lastMonths(now: Date, count: number): string[] {
  return Array.from({ length: count }, (_, i) => monthKey(new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth() - i, 1)).toISOString()));
}

async function receive(req: Request, env: Env): Promise<Response> {
  const length = Number(req.headers.get('Content-Length') ?? '0');
  if (length > MAX_ZIP) return fail(413, 'The report is too big (the limit is 10 MB).');
  if ((req.headers.get('Content-Type') ?? '').split(';')[0].trim() !== 'application/zip') return fail(400, 'A report is sent as one zip file.');
  const meta: Record<string, string | number> = {};
  for (const [name, header, max] of FIELDS) {
    const raw = req.headers.get(header) ?? '';
    let value: string;
    try { value = decodeURIComponent(raw); } catch { return fail(400, `The ${name} of the report is not readable.`); }
    value = value.replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g, ' ').trim();
    if (name === 'description') value = value.slice(0, max);
    else if (value.length > max) return fail(400, `The ${name} of the report is too long.`);
    meta[name] = value;
  }
  if (!meta.version) return fail(400, 'The report does not say which version of the game sent it.');
  if (!KINDS.has(String(meta.kind))) return fail(400, 'The report does not say how it was made.');
  if (env.REPORT_LIMIT) {
    const { success } = await env.REPORT_LIMIT.limit({ key: req.headers.get('CF-Connecting-IP') ?? 'unknown' });
    if (!success) return fail(429, 'Too many reports. Try again in a minute.');
  }
  const now = new Date(), today = now.toISOString().slice(0, 10);
  const months = lastMonths(now, 4), used = await Promise.all(months.map(k => usage(env, k)));
  const current = used[0], sent = current.day === today ? current.n : 0;
  if (sent >= Number(env.DAILY_CAP ?? '300')) return fail(503, 'The report service is full today. Your report was saved on your PC.');
  const stored = used.reduce((sum, u) => sum + u.bytes, 0), storeCap = Number(env.STORE_CAP_MB ?? '900') * 1024 * 1024;
  if (stored + Math.max(length, 22) > storeCap) return fail(503, 'The report service is full. Your report was saved on your PC.');
  const body = await req.arrayBuffer();
  if (body.byteLength > MAX_ZIP) return fail(413, 'The report is too big (the limit is 10 MB).');
  const head = new Uint8Array(body, 0, Math.min(4, body.byteLength));
  if (body.byteLength < 22 || head[0] !== 0x50 || head[1] !== 0x4b || head[2] !== 3 || head[3] !== 4) return fail(400, 'A report is sent as one zip file.');
  if (stored + body.byteLength > storeCap) return fail(503, 'The report service is full. Your report was saved on your PC.');
  const id = newId(), at = now.toISOString();
  meta.id = id; meta.at = at; meta.size = body.byteLength;
  await env.REPORTS.put(`r/${at}_${id}`, body, { expirationTtl: KEEP_SECONDS, metadata: metadata(meta) });
  await env.REPORTS.put(months[0], JSON.stringify({ bytes: current.bytes + body.byteLength, day: today, n: sent + 1 }),
    { expirationTtl: 130 * 86400 });
  return json({ id });
}

async function admin(req: Request, env: Env, url: URL): Promise<Response> {
  const auth = req.headers.get('Authorization') ?? '';
  if (!auth.startsWith('Bearer ') || !sameToken(auth.slice(7), env.ADMIN_TOKEN ?? '')) return fail(401, 'Not allowed.');
  if (req.method === 'GET' && url.pathname === '/v1/admin/reports') {
    const list = await env.REPORTS.list({ prefix: 'r/', cursor: url.searchParams.get('cursor') || undefined, limit: 1000 });
    return json({ reports: list.keys.map(k => ({ key: k.name, ...(k.metadata as object ?? {}) })), cursor: list.list_complete ? null : list.cursor });
  }
  const key = url.searchParams.get('key') ?? '';
  if (url.pathname === '/v1/admin/report' && /^r\/[0-9TZ:.\-]+_R-[2-9A-Z]{6}$/.test(key)) {
    if (req.method === 'GET') {
      const zip = await env.REPORTS.get(key, 'arrayBuffer');
      return zip ? new Response(zip, { headers: { 'Content-Type': 'application/zip', 'Cache-Control': 'no-store' } }) : fail(404, 'No such report.');
    }
    if (req.method === 'DELETE') {
      const found = await env.REPORTS.getWithMetadata(key, 'stream');
      if (!found.value) return fail(404, 'No such report.');
      await found.value.cancel();
      await env.REPORTS.delete(key);
      const size = Number((found.metadata as { size?: number } | null)?.size ?? 0), month = monthKey(key.slice(2)), u = await usage(env, month);
      if (size && u.bytes) await env.REPORTS.put(month, JSON.stringify({ ...u, bytes: Math.max(0, u.bytes - size) }), { expirationTtl: 130 * 86400 });
      return json({ deleted: key });
    }
  }
  return fail(404, 'Unknown route.');
}

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    const url = new URL(req.url);
    try {
      if (req.method === 'POST' && url.pathname === '/v1/report') return await receive(req, env);
      if (url.pathname.startsWith('/v1/admin/')) return await admin(req, env, url);
      return fail(404, 'Unknown route.');
    } catch {
      console.error(JSON.stringify({ event: 'report_error', path: url.pathname }));
      return fail(503, 'The report service is not available right now. Your report was saved on your PC.');
    }
  },
};
