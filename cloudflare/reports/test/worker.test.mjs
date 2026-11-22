// The production entry point in workerd (miniflare) with a real local KV and rate limit; all outbound fetches denied.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import ts from 'typescript';
import { Miniflare, convertV4MiniflareOptions } from 'miniflare';

const zip = (n = 200) => { const b = new Uint8Array(n); b.set([0x50, 0x4b, 3, 4]); return b; };
const summary = { 'X-Strikers-Version': '0.2.0', 'X-Strikers-Kind': 'menu', 'X-Strikers-GPU': 'AMD%20Radeon%20Graphics',
  'X-Strikers-CPU': 'AMD Ryzen Z1 Extreme', 'X-Strikers-OS': 'Windows 11 build 26200', 'X-Strikers-Memory': '13 GB',
  'X-Strikers-Description': encodeURIComponent('Black screen after the intro — "é" 100%') };

async function worker(extra = {}) {
  const script = ts.transpileModule(await readFile('src/index.ts', 'utf8'),
    { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext } }).outputText;
  return new Miniflare(convertV4MiniflareOptions({ modules: true, script, compatibilityDate: '2026-11-05', stripCfConnectingIp: false,
    kvNamespaces: ["REPORTS"], bindings: { ADMIN_TOKEN: "test-admin", DAILY_CAP: "300", STORE_CAP_MB: "900", ...extra },
    ratelimits: { REPORT_LIMIT: { namespace_id: '24890', simple: { limit: 2, period: 60 } } },
    outboundService: () => { throw Error('no outbound fetch'); } }));
}

test('reports: accept, refuse, limit, admin', async t => {
  const mf = await worker();
  let ip = 1;
  const send = (body, headers = {}, fixedIp) => mf.dispatchFetch('https://reports.test/v1/report', { method: 'POST', body,
    headers: { 'Content-Type': 'application/zip', 'CF-Connecting-IP': fixedIp ?? `198.51.100.${ip++}`, ...summary, ...headers } });
  const adminFetch = (path, method = 'GET', token = 'test-admin') =>
    mf.dispatchFetch('https://reports.test' + path, { method, headers: token ? { Authorization: 'Bearer ' + token } : {} });
  try {
    let id;
    await t.test('a zip is stored with its summary', async () => {
      const r = await send(zip(5000)); const b = await r.json();
      assert.equal(r.status, 200, JSON.stringify(b)); assert.match(b.id, /^R-[2-9A-HJ-NP-Z]{6}$/); id = b.id;
      const list = await (await adminFetch('/v1/admin/reports')).json();
      assert.equal(list.reports.length, 1);
      const m = list.reports[0];
      assert.equal(m.id, id); assert.equal(m.version, '0.2.0'); assert.equal(m.gpu, 'AMD Radeon Graphics');
      assert.equal(m.description, 'Black screen after the intro — "é" 100%'); assert.equal(m.size, 5000);
      assert.ok(!JSON.stringify(m).includes('198.51.100'), 'no IP address is stored');
      const got = await adminFetch('/v1/admin/report?key=' + encodeURIComponent(m.key));
      assert.equal(got.status, 200); assert.deepEqual(new Uint8Array(await got.arrayBuffer()), zip(5000));
    });
    await t.test('refusals', async () => {
      const bad = async (r, status, words) => { const b = await r.json(); assert.equal(r.status, status, JSON.stringify(b)); assert.match(b.error, words); };
      await bad(await send(new Uint8Array(300)), 400, /zip/);
      await bad(await send(zip(), { 'Content-Type': 'text/plain' }), 400, /zip/);
      await bad(await send(zip(10)), 400, /zip/);
      await bad(await send(zip(10 * 1024 * 1024 + 10)), 413, /too big/);
      await bad(await send(zip(), { 'X-Strikers-Version': '' }), 400, /version/);
      await bad(await send(zip(), { 'X-Strikers-Kind': 'other' }), 400, /how it was made/);
      await bad(await send(zip(), { 'X-Strikers-GPU': 'x'.repeat(129) }), 400, /too long/);
      await bad(await send(zip(), { 'X-Strikers-CPU': '%E0%A4%A' }), 400, /not readable/);
      const long = await send(zip(), { 'X-Strikers-Description': 'y'.repeat(5000) });
      assert.equal(long.status, 200, 'a long description is cut, not refused');
      await bad(await mf.dispatchFetch('https://reports.test/other'), 404, /Unknown/);
    });
    await t.test('two a minute from one address', async () => {
      assert.equal((await send(zip(), {}, '203.0.113.9')).status, 200);
      assert.equal((await send(zip(), {}, '203.0.113.9')).status, 200);
      const third = await send(zip(), {}, '203.0.113.9'); assert.equal(third.status, 429);
      assert.match((await third.json()).error, /Too many reports/);
      assert.equal((await send(zip(), {}, '203.0.113.10')).status, 200, 'another address is not held up');
    });
    await t.test('admin needs the token', async () => {
      assert.equal((await adminFetch('/v1/admin/reports', 'GET', null)).status, 401);
      assert.equal((await adminFetch('/v1/admin/reports', 'GET', 'test-admiN')).status, 401);
      assert.equal((await adminFetch('/v1/admin/reports', 'GET', 'test-admin-longer')).status, 401);
      assert.equal((await adminFetch('/v1/admin/report?key=../../etc')).status, 404);
    });
    await t.test('delete', async () => {
      const list = await (await adminFetch('/v1/admin/reports')).json();
      const key = list.reports.find(r => r.id === id).key;
      assert.equal((await adminFetch('/v1/admin/report?key=' + encodeURIComponent(key), 'DELETE')).status, 200);
      assert.equal((await adminFetch('/v1/admin/report?key=' + encodeURIComponent(key))).status, 404);
    });
  } finally { await mf.dispose(); }
});

test('reports: the daily cap', async () => {
  const mf = await worker({ DAILY_CAP: '2' });
  try {
    for (let i = 0; i < 2; i++) assert.equal((await mf.dispatchFetch('https://reports.test/v1/report', { method: 'POST', body: zip(),
      headers: { 'Content-Type': 'application/zip', 'CF-Connecting-IP': `192.0.2.${i}`, ...summary } })).status, 200);
    const r = await mf.dispatchFetch('https://reports.test/v1/report', { method: 'POST', body: zip(),
      headers: { 'Content-Type': 'application/zip', 'CF-Connecting-IP': '192.0.2.50', ...summary } });
    assert.equal(r.status, 503); assert.match((await r.json()).error, /full today/);
  } finally { await mf.dispose(); }
});

test('reports: the storage cap, and a delete frees its space', async () => {
  const mf = await worker({ STORE_CAP_MB: String(12000 / 1024 / 1024) });
  const send = (i, n = 5000) => mf.dispatchFetch('https://reports.test/v1/report', { method: 'POST', body: zip(n),
    headers: { 'Content-Type': 'application/zip', 'CF-Connecting-IP': `192.0.2.${i}`, ...summary } });
  const admin = (path, method = 'GET') => mf.dispatchFetch('https://reports.test' + path, { method, headers: { Authorization: 'Bearer test-admin' } });
  try {
    assert.equal((await send(1)).status, 200);
    assert.equal((await send(2)).status, 200);
    const full = await send(3); assert.equal(full.status, 503); assert.match((await full.json()).error, /full\. Your report was saved/);
    assert.equal((await send(4, 1500)).status, 200, 'a smaller report still fits');
    const list = await (await admin('/v1/admin/reports')).json();
    assert.equal(list.reports.length, 3, 'only the reports that fit are stored');
    const big = list.reports.find(r => r.size === 5000);
    assert.equal((await admin('/v1/admin/report?key=' + encodeURIComponent(big.key), 'DELETE')).status, 200);
    assert.equal((await admin('/v1/admin/report?key=' + encodeURIComponent(big.key), 'DELETE')).status, 404);
    assert.equal((await send(5)).status, 200, 'deleting a report frees its space');
  } finally { await mf.dispose(); }
});
