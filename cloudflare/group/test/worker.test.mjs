import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import ts from 'typescript';
import { Miniflare, convertV4MiniflareOptions } from 'miniflare';
import { adhocContracts } from './adhoc-contracts.mjs';

for (const mode of [undefined,'adhoc']) test(`workerd ad-hoc ${mode ?? 'default'} without token or upstream`,async()=>{
  const compiled=ts.transpileModule(await readFile('src/index.ts','utf8'),{compilerOptions:{target:ts.ScriptTarget.ES2022,module:ts.ModuleKind.ESNext}}).outputText;
  let calls=0,ip=1;
  const mf=new Miniflare(convertV4MiniflareOptions({name:'adhoc',modules:true,script:compiled,stripCfConnectingIp:false,compatibilityDate:'2026-11-05',d1Databases:['DB'],bindings:{ADMIN_TOKEN:'test-admin',CODE_PEPPER:'test-pepper',...(mode?{GROUP_MODE:mode}:{})},outboundService:()=>{calls++;throw Error('Ad-hoc outbound forbidden');}}));
  try {
    const db=await mf.getD1Database('DB');
    for(const stmt of (await readFile('migrations/0001_group.sql','utf8')).split(';').map(s=>s.trim()).filter(Boolean))await db.prepare(stmt).run();
    const req=async(path,data,token,expected=200)=>{
      const r=await mf.dispatchFetch('https://group.test'+path,{method:data?'POST':'GET',headers:{'Content-Type':'application/json','CF-Connecting-IP':`192.0.2.${ip++}`,...(token?{Authorization:'Bearer '+token}:{})},...(data?{body:JSON.stringify(data)}:{})});
      const b=await r.json();assert.equal(r.status,expected,JSON.stringify(b));return b;
    };
    await adhocContracts(req,db);assert.equal(calls,0);
  } finally {await mf.dispose();}
});

// Run the production entry point in workerd with real local D1; all other fetches denied.
test('production Worker contracts with mocked Central', async t => {
  const compiled = ts.transpileModule(await readFile('src/index.ts','utf8'),{compilerOptions:{target:ts.ScriptTarget.ES2022,module:ts.ModuleKind.ESNext}}).outputText;
  const calls = []; let upstreamFailure = false;
  const mf = new Miniflare(convertV4MiniflareOptions({ name: 'group', modules: true, script: compiled, stripCfConnectingIp: false,
    compatibilityDate: '2026-11-05', d1Databases: ['DB'],
    bindings: { GROUP_MODE: 'central', ZT_NETWORK: 'aabbccddeeff0011', ZT_TOKEN: 'unit-test-only', ADMIN_TOKEN: 'test-admin', CODE_PEPPER: 'unit-test-pepper' },
    outboundService: async req => {
      assert.match(req.url, /^https:\/\/api\.zerotier\.com\/api\/v1\/network\/aabbccddeeff0011\/member\/[0-9a-f]{10}$/);
      assert.equal(req.headers.get('Authorization'), 'token unit-test-only');
      calls.push({ url: req.url, ...await req.json() });
      return new Response('{}', { status: upstreamFailure ? 503 : 200 });
    } }));
  try {
    const db = await mf.getD1Database('DB');
    for (const stmt of (await readFile('migrations/0001_group.sql','utf8')).split(';').map(s=>s.trim()).filter(Boolean)) await db.prepare(stmt).run();
    let ip = 1;
    async function req(path, data, token, expected = 200, fixedIp) {
      const r = await mf.dispatchFetch('https://group.test'+path, { method: data ? 'POST' : 'GET',
        headers: { 'Content-Type': 'application/json', 'CF-Connecting-IP': fixedIp || `192.0.2.${ip++}`, ...(token ? { Authorization: 'Bearer '+token } : {}) },
        ...(data ? { body: JSON.stringify(data) } : {}) });
      const b = await r.json(); assert.equal(r.status, expected, JSON.stringify(b)); return b;
    }
    const make = (node, code, credential = node.padEnd(64,'1')) => ({node,code,credential,name:node==='aaaaaaaaaa'?'Alice':'Bob'});
    const code = (await req('/admin/codes',{hours:24,uses:16},'test-admin')).code;
    const alice = make('aaaaaaaaaa',code), bob = make('bbbbbbbbbb',code);
    await t.test('join authorizes once, returns network; retries cannot steal membership', async()=>{
      assert.equal((await req('/join',alice)).network,'aabbccddeeff0011');
      await req('/join',alice); assert.equal(calls.length,1); assert.equal(calls[0].config.authorized,true);
      await req('/join',{...alice,credential:'f'.repeat(64)},null,403);
      await req('/friends',undefined,'f'.repeat(64),401);
    });
    await t.test('bad, expired, revoked, exhausted code and malformed input',async()=>{
      await req('/join',make('cccccccccc','AAAA-AAAA'),null,403);
      await db.prepare('UPDATE codes SET expires=0').run();
      await req('/join',make('cccccccccc',code),null,403);
      const one = (await req('/admin/codes',{uses:1},'test-admin')).code;
      await req('/join',make('cccccccccc',one)); await req('/join',make('dddddddddd',one),null,403);
      await req('/join',bob,null,403);
      await req('/join',{...bob,code:'bad',node:'../x'},null,400);
      await req('/admin/revoke-code',{},'bad',401);
    });
    await t.test('rate limit exact boundary and concurrent requests',async()=>{
      const attempts = await Promise.all(Array.from({length:10},()=>mf.dispatchFetch('https://group.test/join',{method:'POST',headers:{'CF-Connecting-IP':'198.51.100.1','Content-Type':'application/json'},body:JSON.stringify(make('eeeeeeeeee','AAAA-AAAA'))})));
      assert.equal(attempts.filter(r=>r.status===429).length,5);
    });
    await t.test('presence, friend status, hosting requirement, invite and dismissal',async()=>{
      bob.code=(await req('/admin/codes',{},'test-admin')).code; await req('/join',bob);
      await req('/presence',{status:'menus'},bob.credential);
      await req('/invite',{to:bob.node},alice.credential,409);
      await req('/presence',{status:'hosting'},alice.credential);
      const invite = await req('/invite',{to:bob.node},alice.credential);
      const f = await req('/friends',undefined,bob.credential);
      assert.equal(f.friends.find(p=>p.node===alice.node).status,'hosting'); assert.equal(f.invites[0].name,'Alice');
      await req('/invite/dismiss',{id:invite.id},bob.credential);
      assert.equal((await req('/friends',undefined,bob.credential)).invites.length,0);
      await db.prepare('UPDATE players SET last_seen=0 WHERE node=?').bind(alice.node).run();
      assert.equal((await req('/friends',undefined,bob.credential)).friends.find(p=>p.node===alice.node).status,'offline');
      await req('/presence',{status:'match'},bob.credential);
      await req('/presence',{status:'wrong'},bob.credential,400);
    });
    await t.test('Central failure is audited, retriable, and never returns successful enrollment',async()=>{
      upstreamFailure=true; const d=make('dddddddddd',bob.code);
      await req('/join',d,null,502); assert.equal((await db.prepare("SELECT state FROM players WHERE node='dddddddddd'").first()).state,'pending');
      upstreamFailure=false; await req('/join',d);
      const log = (await req('/admin/audit',undefined,'test-admin')).audit;
      assert.ok(log.some(a=>a.outcome==='uncertain-retry')); assert.ok(log.some(a=>a.action==='authorize'&&a.outcome==='confirmed'));
    });
    await t.test('removal revokes bearer, deauthorizes Central, and blocks re-enrollment',async()=>{
      upstreamFailure=true; await req('/admin/remove',{node:alice.node},'test-admin',502);
      await req('/friends',undefined,alice.credential,401);
      upstreamFailure=false; await req('/admin/remove',{node:alice.node},'test-admin');
      assert.equal(calls.at(-1).config.authorized,false); await req('/join',alice,null,403);
      assert.ok(!(await req('/friends',undefined,bob.credential)).friends.some(p=>p.node===alice.node));
    });
    await t.test('HTTPS and bounded bodies; no CORS or redirect path',async()=>{
      const r=await mf.dispatchFetch('http://group.test/join',{method:'POST'}); assert.equal(r.status,403);
      const large=await mf.dispatchFetch('https://group.test/join',{method:'POST',headers:{'Content-Type':'application/json','CF-Connecting-IP':'192.0.2.250'},body:' '.repeat(3000)}); assert.equal(large.status,413);
    });
  } finally { await mf.dispose(); }
});
