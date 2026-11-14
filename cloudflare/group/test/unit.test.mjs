// Production handler + actual SQLite. No package install or network access.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DatabaseSync } from 'node:sqlite';
import { timingSafeEqual } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import ts from 'typescript';
import { database } from './sqlite.mjs';
import { adhocContracts } from './adhoc-contracts.mjs';
crypto.subtle.timingSafeEqual=(a,b)=>timingSafeEqual(Buffer.from(a),Buffer.from(b));
const source=ts.transpileModule(await readFile('src/index.ts','utf8'),{compilerOptions:{target:ts.ScriptTarget.ES2022,module:ts.ModuleKind.ESNext}}).outputText;
const { default: worker }=await import('data:text/javascript;base64,'+Buffer.from(source).toString('base64'));
for (const mode of [undefined, 'adhoc']) test(`ad-hoc ${mode ?? 'default'}: no token, no Central, complete directory`,async()=>{
  const env={DB:await database(),ADMIN_TOKEN:'test-admin',CODE_PEPPER:'test-pepper',...(mode?{GROUP_MODE:mode}:{})};
  let ip=1,calls=0;const original=globalThis.fetch;
  globalThis.fetch=()=>{calls++;throw Error('Ad-hoc must never call upstream');};
  const req=async(path,data,token,expected=200)=>{
    const r=await worker.fetch(new Request('https://group.test'+path,{method:data?'POST':'GET',headers:{'Content-Type':'application/json','CF-Connecting-IP':`192.0.2.${ip++}`,...(token?{Authorization:'Bearer '+token}:{})},...(data?{body:JSON.stringify(data)}:{})}),env);
    const b=await r.json();assert.equal(r.status,expected,JSON.stringify(b));return b;
  };
  try {await adhocContracts(req,env.DB);assert.equal(calls,0);
    for(const invalid of [{GROUP_MODE:'bogus'},{GROUP_MODE:'central'},{GROUP_MODE:'adhoc',ZT_NETWORK:'aabbccddeeff0011'},{GROUP_MODE:'adhoc',ZT_NETWORK:'ff00010002000000'}]) {
      const r=await worker.fetch(new Request('https://group.test/friends'),{...env,...invalid});assert.equal(r.status,503);
    }
  } finally {globalThis.fetch=original;}
});
test('production handler with SQLite and strictly mocked Central',async t=>{
  const sqlite=new DatabaseSync(':memory:'); sqlite.exec(await readFile('migrations/0001_group.sql','utf8'));
  class Statement {
    constructor(q,args=[]){this.q=q;this.args=args;}
    bind(...args){return new Statement(this.q,args);}
    async first(){return sqlite.prepare(this.q).get(...this.args)??null;}
    async all(){return {results:sqlite.prepare(this.q).all(...this.args)};}
    async run(){return sqlite.prepare(this.q).run(...this.args);}
  }
  const DB={prepare:q=>new Statement(q),async batch(stmts){sqlite.exec('BEGIN');try {const r=[];for(const s of stmts)r.push(await s.run());sqlite.exec('COMMIT');return r;}catch(e){sqlite.exec('ROLLBACK');throw e;}}};
  const env={DB,GROUP_MODE: 'central', ZT_NETWORK:'aabbccddeeff0011',ZT_TOKEN:'mock-only',ADMIN_TOKEN:'test-admin',CODE_PEPPER:'test-pepper'};
  const original=globalThis.fetch,calls=[];let upstreamFailure=false,ip=1;
  globalThis.fetch=async(url,options)=>{assert.match(url,/^https:\/\/api\.zerotier\.com\/api\/v1\/network\/aabbccddeeff0011\/member\/[a-f0-9]{10}$/);assert.equal(options.headers.Authorization,'token mock-only');assert.equal(options.redirect,'manual');calls.push(JSON.parse(options.body));return new Response('{}',{status:upstreamFailure?503:200});};
  async function req(path,b,token,expected=200){const r=await worker.fetch(new Request('https://group.test'+path,{method:b?'POST':'GET',headers:{'Content-Type':'application/json','CF-Connecting-IP':`192.0.2.${ip++}`,...(token?{Authorization:'Bearer '+token}:{})},...(b?{body:JSON.stringify(b)}:{})}),env);const data=await r.json();assert.equal(r.status,expected,JSON.stringify(data));return data;}
  const make=(node,code)=>({node,code,name:node==='aaaaaaaaaa'?'Alice':'Bob',credential:node.padEnd(64,'1')});
  try {
    let code=(await req('/admin/codes',{},env.ADMIN_TOKEN)).code;
    const alice=make('aaaaaaaaaa',code),bob=make('bbbbbbbbbb',code);
    await t.test('join + retry + impersonation denial',async()=>{assert.equal((await req('/join',alice)).network,env.ZT_NETWORK);await req('/join',alice);assert.equal(calls.length,1);await req('/join',{...alice,credential:'f'.repeat(64)},null,403);await req('/friends',undefined,'f'.repeat(64),401);});
    await t.test('bad, expired, revoked and exhausted codes',async()=>{await req('/join',make('cccccccccc','AAAA-AAAA'),null,403);sqlite.exec('UPDATE codes SET expires=0');await req('/join',bob,null,403);code=(await req('/admin/codes',{uses:1},env.ADMIN_TOKEN)).code;await req('/join',{...bob,code});await req('/join',make('cccccccccc',code),null,403);code=(await req('/admin/codes',{},env.ADMIN_TOKEN)).code;await req('/admin/revoke-code',{},env.ADMIN_TOKEN);await req('/join',make('cccccccccc',code),null,403);});
    await t.test('rate limit is shared SQLite state',async()=>{let counts=[];for(let i=0;i<8;i++){const r=await worker.fetch(new Request('https://group.test/join',{method:'POST',headers:{'Content-Type':'application/json','CF-Connecting-IP':'198.51.100.1'},body:JSON.stringify(make('eeeeeeeeee','AAAA-AAAA'))}),env);counts.push(r.status);}assert.deepEqual(counts,[403,403,403,403,403,429,429,429]);});
    await t.test('presence and actual host-only invitation + recipient-only dismissal',async()=>{await req('/presence',{status:'menus'},bob.credential);await req('/invite',{to:bob.node},alice.credential,409);await req('/presence',{status:'hosting'},alice.credential);const i=await req('/invite',{to:bob.node},alice.credential);let f=await req('/friends',undefined,bob.credential);assert.equal(f.invites[0].name,'Alice');assert.equal(f.friends.find(x=>x.node===alice.node).status,'hosting');await req('/invite/dismiss',{id:i.id},alice.credential);assert.equal((await req('/friends',undefined,bob.credential)).invites.length,1);await req('/invite/dismiss',{id:i.id},bob.credential);assert.equal((await req('/friends',undefined,bob.credential)).invites.length,0);await req('/presence',{status:'match'},bob.credential);sqlite.exec("UPDATE players SET last_seen=0 WHERE node='aaaaaaaaaa'");assert.equal((await req('/friends',undefined,bob.credential)).friends.find(x=>x.node===alice.node).status,'offline');});
    await t.test('Central failure durable log and retry',async()=>{code=(await req('/admin/codes',{},env.ADMIN_TOKEN)).code;const c=make('cccccccccc',code);upstreamFailure=true;await req('/join',c,null,502);assert.equal(sqlite.prepare("SELECT state FROM players WHERE node='cccccccccc'").get().state,'pending');upstreamFailure=false;await req('/join',c);assert.ok((await req('/admin/audit',undefined,env.ADMIN_TOKEN)).audit.some(a=>a.outcome==='uncertain-retry'));});
    await t.test('removal blocks immediately even with failed upstream; retries deauthorize',async()=>{upstreamFailure=true;await req('/admin/remove',{node:alice.node},env.ADMIN_TOKEN,502);await req('/friends',undefined,alice.credential,401);upstreamFailure=false;await req('/admin/remove',{node:alice.node},env.ADMIN_TOKEN);assert.equal(calls.at(-1).config.authorized,false);await req('/join',alice,null,403);assert.ok(!(await req('/friends',undefined,bob.credential)).friends.some(p=>p.node===alice.node));});
    await t.test('validation, HTTP denial and bounded streaming input',async()=>{await req('/presence',{status:'invalid'},bob.credential,400);await req('/join',{...bob,node:'../etc'},null,400);let r=await worker.fetch(new Request('http://group.test/join'),env);assert.equal(r.status,403);r=await worker.fetch(new Request('https://group.test/join',{method:'POST',headers:{'Content-Type':'application/json'},body:' '.repeat(3000)}),env);assert.equal(r.status,413);await req('/admin/codes',{},'bad',401);});
  }finally{globalThis.fetch=original;sqlite.close();}
});
