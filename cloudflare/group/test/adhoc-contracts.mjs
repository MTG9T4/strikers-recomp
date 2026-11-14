import assert from 'node:assert/strict';
// Shared by SQLite unit and actual workerd/D1 suites, with no Central adapter.
export async function adhocContracts(req, db) {
  const code = (await req('/admin/codes', {}, 'test-admin')).code;
  const a = { node:'1111111111', name:'Alice', code, credential:'1'.repeat(64) };
  const b = { node:'2222222222', name:'Bob', code, credential:'2'.repeat(64) };
  const join = await req('/join', a);
  assert.equal(join.network, 'ff61286137000000'); assert.equal(join.mode, 'adhoc');
  await req('/join', a); await req('/join', b);
  const taken = await req('/join', {...a, credential:'3'.repeat(64)}, undefined, 403);
  const invalid = await req('/join', {...a,node:'3333333333',code:'AAAA-AAAA'}, undefined, 403);
  assert.equal(taken.error, invalid.error); // no oracle for which node IDs are enrolled
  assert.equal((await req('/friends', undefined, 'f'.repeat(64), 401)).state, 'gone');
  assert.equal((await req('/friends', undefined, undefined, 401)).state, undefined);
  // Malformed joins spend no shared allowance.
  const used = async () => (await db.prepare("SELECT sum(count) AS n FROM limits WHERE key LIKE 'join-global:%'").first()).n;
  const allowance = await used(); await req('/join', {...a,node:'../etc'}, undefined, 400); assert.equal(await used(), allowance);
  await db.prepare('UPDATE players SET last_seen=0 WHERE node=?').bind(b.node).run();
  const directory = await req('/friends', undefined, a.credential);
  assert.equal(directory.self,a.node);
  assert.deepEqual(directory.friends.map(p=>p.node), [a.node,b.node]);
  assert.equal(directory.friends.find(p=>p.node===b.node).status,'offline');
  // One request per poll: presence answers with the directory.
  const poll = await req('/presence', {status:'menus'}, a.credential);
  assert.equal(poll.self,a.node); assert.deepEqual(poll.friends.map(p=>p.node),[a.node,b.node]); assert.deepEqual(poll.invites,[]);
  assert.equal(poll.friends.find(p=>p.node===a.node).status,'menus');
  // A fresh poll writes nothing; a stale one refreshes.
  const seen = async () => (await db.prepare('SELECT last_seen FROM players WHERE node=?').bind(a.node).first()).last_seen;
  await db.prepare('UPDATE players SET last_seen=last_seen-5 WHERE node=?').bind(a.node).run();
  const fresh = await seen(); await req('/presence', {status:'menus'}, a.credential); assert.equal(await seen(), fresh);
  await db.prepare('UPDATE players SET last_seen=last_seen-100 WHERE node=?').bind(a.node).run();
  const stale = await seen(); await req('/presence', {status:'menus'}, a.credential); assert.ok(await seen() > stale);
  // Status changes are the only presence write a client can repeat: limited, and refusing writes nothing.
  const minute = Math.floor(Date.now()/60000);
  for (const bucket of [minute, minute+1]) await db.prepare('INSERT OR REPLACE INTO limits(key,until,count) VALUES(?,?,12)').bind(`presence:${a.node}:${bucket}`, (bucket+1)*60).run();
  await req('/presence', {status:'hosting'}, a.credential, 429);
  assert.equal((await db.prepare("SELECT max(count) AS n FROM limits WHERE key LIKE 'presence:%'").first()).n, 12);
  await req('/presence', {status:'menus'}, a.credential);
  await req('/admin/remove', {node:b.node}, 'test-admin');
  await req('/admin/remove', {node:b.node}, 'test-admin'); // idempotent removal
  assert.deepEqual((await req('/friends',undefined,a.credential)).friends.map(p=>p.node),[a.node]);
  assert.equal((await req('/friends',undefined,b.credential,401)).state,'gone'); await req('/join',b,undefined,403);
  assert.ok((await req('/admin/audit',undefined,'test-admin')).audit.some(x=>x.action==='deauthorize'&&x.outcome==='confirmed'));
}
