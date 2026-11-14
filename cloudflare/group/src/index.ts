// SPDX-License-Identifier: GPL-3.0-or-later
// The only production outbound URL is the legacy Central member endpoint below.
export type Secrets = { GROUP_MODE?: string; ZT_TOKEN?: string; ZT_NETWORK?: string; ADMIN_TOKEN: string; CODE_PEPPER: string };
type Bindings = Env & Secrets;
type Central = (env: Bindings, node: string, authorized: boolean) => Promise<void>;
class Failure extends Error { constructor(public status: number, message: string, public state?: string) { super(message); } }
const fail = (status: number, message: string): never => { throw new Failure(status, message); };
const encoder = new TextEncoder();
// Client poll is 15 s; last_seen is rewritten at most every 20 s, so it is up to 30 s old on a healthy client. Sixty seconds of
// silence survives one lost poll and says offline. Workers Free allows 100,000 requests and D1 100,000 rows written a day.
const FRESH = 60, PRESENCE_WRITE = 20;
function group(e: Secrets) {
  const mode = e.GROUP_MODE ?? 'adhoc';
  const network = e.ZT_NETWORK || (mode === 'adhoc' ? 'ff61286137000000' : '');
  if (!['adhoc', 'central'].includes(mode) || !/^[0-9a-f]{16}$/.test(network) ||
      (mode === 'central' && (!e.ZT_TOKEN || network.startsWith('ff'))) ||
      (mode === 'adhoc' && (!/^ff[0-9a-f]{8}000000$/.test(network) ||
        parseInt(network.slice(2,6),16) > 24872 || parseInt(network.slice(6,10),16) < 24887)))
    fail(503, 'Organizer configuration missing');
  return { mode, network };
}
const hex = (b: ArrayBuffer) => [...new Uint8Array(b)].map(x => x.toString(16).padStart(2, '0')).join('');
const hash = async (s: string) => hex(await crypto.subtle.digest('SHA-256', encoder.encode(s)));
async function codeHash(env: Bindings, s: string) {
  const key = await crypto.subtle.importKey('raw', encoder.encode(env.CODE_PEPPER), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  return hex(await crypto.subtle.sign('HMAC', key, encoder.encode(s)));
}
async function equal(a: string, b: string) {
  // Fixed-length digests plus timingSafeEqual (Workers extension).
  const x = await crypto.subtle.digest('SHA-256', encoder.encode(a));
  const y = await crypto.subtle.digest('SHA-256', encoder.encode(b));
  return crypto.subtle.timingSafeEqual(x, y);
}
function str(b: Record<string, unknown>, k: string, re: RegExp): string {
  const s = b[k]; if (typeof s !== 'string' || !re.test(s)) return fail(400, `Invalid ${k}`); return s;
}
const nodeId = (b: Record<string, unknown>, k = 'node') => str(b, k, /^[0-9a-f]{10}$/);
async function body(r: Request): Promise<Record<string, unknown>> {
  if (!r.headers.get('Content-Type')?.startsWith('application/json')) return fail(415, 'Use application/json');
  const reader = r.body?.getReader(); let s = ''; let bytes = 0;
  if (!reader) return fail(400, 'Missing body');
  const decoder = new TextDecoder('utf-8', { fatal: true, ignoreBOM: false });
  try {
    for (;;) { const { done, value } = await reader.read(); if (done) break;
      bytes += value.length; if (bytes > 2048) { await reader.cancel(); return fail(413, 'Body too large'); }
      s += decoder.decode(value, { stream: true });
    }
    s += decoder.decode(); const v = JSON.parse(s);
    if (!v || typeof v !== 'object' || Array.isArray(v)) return fail(400, 'Invalid JSON'); return v;
  } catch (e) { if (e instanceof Failure) throw e; return fail(400, 'Invalid JSON'); }
}
function json(v: unknown, status = 200) {
  return Response.json(v, { status, headers: { 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', 'Strict-Transport-Security': 'max-age=31536000' } });
}
function sql(e: Bindings, q: string, ...args: (string | number)[]) { return e.DB.prepare(q).bind(...args); }
async function rate(e: Bindings, key: string, now: number, seconds: number, maximum: number) {
  const bucket = Math.floor(now / seconds); const k = `${key}:${bucket}`;
  // A refused request writes nothing: rows written are the Free plan's scarcest quota, and a flood must not spend them.
  const row = await sql(e, `INSERT INTO limits(key,until,count) VALUES(?,?,1)
    ON CONFLICT(key) DO UPDATE SET count=count+1 WHERE count<? RETURNING count`, k, (bucket + 1) * seconds, maximum).first<{count: number}>();
  if (!row) fail(429, 'Please wait before trying again');
}
type Player = { node: string; name: string; credential: string; state: string; removed: number; lease: number; status: string; last_seen: number };
async function player(r: Request, e: Bindings) {
  const token = r.headers.get('Authorization')?.match(/^Bearer ([0-9a-f]{64})$/)?.[1];
  if (!token) return fail(401, 'Join your group first');
  const p = await sql(e, 'SELECT * FROM players WHERE credential=? AND state=? AND removed=0', await hash(token), 'active').first<Player>();
  // state marks this as the Worker's own verdict: clients clear their saved friends for this answer and no other (audit 08).
  if (!p) throw new Failure(401, 'Membership is missing or removed. Contact your organizer.', 'gone');
  return p;
}
async function directory(e: Bindings, p: Player, now: number) {
  const friends = (await sql(e, `SELECT node,name,CASE WHEN last_seen<? THEN 'offline' ELSE status END AS status,last_seen
    FROM players WHERE state='active' AND removed=0 ORDER BY node LIMIT 128`, now-FRESH).all()).results;
  const invites = (await sql(e, `SELECT i.id,p.node,p.name,i.expires FROM invites i JOIN players p ON p.node=i.sender
    WHERE i.recipient=? AND i.expires>? AND p.removed=0 AND p.status='hosting' AND p.last_seen>=? ORDER BY i.expires DESC LIMIT 16`, p.node, now, now-FRESH).all()).results;
  return { self: p.node, friends, invites };
}
async function central(e: Bindings, node: string, authorized: boolean) {
  group(e);
  const r = await fetch(`https://api.zerotier.com/api/v1/network/${e.ZT_NETWORK}/member/${node}`, {
    // 'manual', not 'error': the Workers runtime only accepts 'follow' or 'manual' (fetch throws on 'error', which
    // made every join fail with 502 under workerd). A 3xx is not r.ok, so a redirect is still never followed.
    method: 'POST', redirect: 'manual', signal: AbortSignal.timeout(8000),
    headers: { Authorization: `token ${e.ZT_TOKEN}`, 'Content-Type': 'application/json' },
    body: JSON.stringify({ config: { authorized } })
  });
  await r.body?.cancel(); if (!r.ok) fail(502, 'Central did not confirm the change. Ask your organizer.');
}
async function audit(e: Bindings, node: string, action: string, outcome: string, now: number, id = crypto.randomUUID()) {
  await sql(e, 'INSERT INTO audit(id,at,node,action,outcome) VALUES(?,?,?,?,?)', id, now, node, action, outcome).run(); return id;
}
// Test injection lives in test/local.ts; production never accepts a mock flag from bindings/requests.
export function createHandler(change: Central = central, local = false) {
  return async (r: Request, e: Bindings): Promise<Response> => {
    const now = Math.floor(Date.now() / 1000); const u = new URL(r.url);
    try {
      if (u.protocol !== 'https:' && !(local && u.hostname === '127.0.0.1')) fail(403, 'HTTPS required');
      if (!e.ADMIN_TOKEN || !e.CODE_PEPPER) fail(503, 'Organizer configuration missing');
      const { mode, network } = group(e);
      const route = `${r.method} ${u.pathname}`;
      if (u.pathname.startsWith('/admin/')) {
        await rate(e, `admin:${r.headers.get('CF-Connecting-IP') || 'local'}`, now, 60, 30);
        if (!(await equal(r.headers.get('Authorization') || '', `Bearer ${e.ADMIN_TOKEN}`))) fail(401, 'Organizer credential required');
        if (route === 'POST /admin/codes') {
          const b = await body(r); const hours = b.hours ?? 24, uses = b.uses ?? 16;
          if (!Number.isInteger(hours) || Number(hours) < 1 || Number(hours) > 168 || !Number.isInteger(uses) || Number(uses) < 1 || Number(uses) > 32) fail(400, 'Invalid hours or uses');
          const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
          const bytes = crypto.getRandomValues(new Uint8Array(8));
          const raw = [...bytes].map(x => alphabet[x & 31]).join(''); const code = raw.slice(0,4) + '-' + raw.slice(4);
          // Rotation is atomic: all earlier codes become invalid for new enrollments.
          await e.DB.batch([sql(e, 'UPDATE codes SET revoked=1'), sql(e, 'INSERT INTO codes(hash,expires,remaining) VALUES(?,?,?)', await codeHash(e, code), now + Number(hours)*3600, Number(uses))]);
          return json({ code, expires: now + Number(hours)*3600, uses });
        }
        if (route === 'POST /admin/revoke-code') { await sql(e, 'UPDATE codes SET revoked=1').run(); return json({ ok: true }); }
        if (route === 'GET /admin/players') return json({ players: (await sql(e, 'SELECT node,name,state,removed,status,last_seen FROM players ORDER BY node LIMIT 128').all()).results });
        if (route === 'GET /admin/audit') return json({ audit: (await sql(e, 'SELECT * FROM audit ORDER BY at DESC,id DESC LIMIT 200').all()).results });
        if (route === 'POST /admin/remove') {
          const node = nodeId(await body(r));
          // Deny credentials immediately even if Central is down or another operation is in flight.
          await sql(e, 'UPDATE players SET removed=1 WHERE node=?', node).run();
          const claimed = await sql(e, 'UPDATE players SET lease=? WHERE node=? AND lease<? RETURNING node', now+60, node, now).first();
          if (!claimed) fail(409, 'Unknown player or change in progress. Retry removal in one minute.');
          const id = await audit(e, node, 'deauthorize', 'pending', now);
          try { if (mode === 'central') await change(e, node, false); }
          catch { await sql(e, 'UPDATE players SET lease=0 WHERE node=?', node).run(); await sql(e, 'UPDATE audit SET outcome=? WHERE id=?', 'uncertain-retry-removal', id).run(); return fail(502, 'Player blocked in service; retry removal to confirm Central deauthorization.'); }
          await e.DB.batch([sql(e, "UPDATE players SET state='removed',lease=0,status='offline' WHERE node=?", node), sql(e, "UPDATE audit SET outcome='confirmed' WHERE id=?", id), sql(e, 'DELETE FROM invites WHERE sender=? OR recipient=?', node, node)]);
          return json({ ok: true });
        }
        return fail(404, 'Unknown organizer route');
      }
      if (route === 'POST /join') {
        const b = await body(r), node = nodeId(b), name = str(b, 'name', /^[\p{L}\p{N} ._'\-]{1,32}$/u).trim();
        if (!name) fail(400, 'Invalid name');
        const code = str(b, 'code', /^[A-Z2-9]{4}-[A-Z2-9]{4}$/), secret = str(b, 'credential', /^[0-9a-f]{64}$/);
        // After validation: malformed junk must not use up the shared hourly allowance real friends need.
        await rate(e, `join-ip:${r.headers.get('CF-Connecting-IP') || 'local'}`, now, 600, 5);
        await rate(e, 'join-global', now, 3600, 60);
        const credential = await hash(secret);
        const codehash = await codeHash(e, code);
        // INSERT and use-count decrement are in one D1 transaction. Only one enrollment per node.
        await e.DB.batch([
          sql(e, `INSERT OR IGNORE INTO players(node,name,credential,code_hash)
            SELECT ?,?,?,? FROM codes WHERE hash=? AND revoked=0 AND expires>? AND remaining>0
            AND (SELECT count(*) FROM players)<128`, node, name, credential, codehash, codehash, now),
          sql(e, 'UPDATE codes SET remaining=remaining-1 WHERE hash=? AND changes()=1', codehash)
        ]);
        const p = await sql(e, 'SELECT * FROM players WHERE node=?', node).first<Player>();
        // One answer for both: a different one would tell anyone, with no code, which player IDs are enrolled.
        const refused = 'Code invalid, expired or used up, group full, or this player cannot join. Ask your organizer.';
        if (!p) fail(403, refused);
        if (p!.removed || !(await equal(p!.credential, credential))) fail(403, refused);
        // Retry with the original credential is allowed after code expiry; never replaces another credential.
        if (p!.state === 'active') return json({ network, node, mode });
        const claimed = await sql(e, 'UPDATE players SET lease=? WHERE node=? AND removed=0 AND lease<? RETURNING node', now+60, node, now).first();
        if (!claimed) fail(409, 'Join in progress. Try again in one minute.');
        const id = await audit(e, node, 'authorize', 'pending', now);
        try { if (mode === 'central') await change(e, node, true); }
        catch { await e.DB.batch([sql(e, 'UPDATE players SET lease=0 WHERE node=?', node), sql(e, "UPDATE audit SET outcome='uncertain-retry' WHERE id=?", id)]); return fail(502, 'Group service could not reach Central. Try Join again.'); }
        await e.DB.batch([sql(e, "UPDATE players SET state='active',lease=0,last_seen=? WHERE node=?", now, node), sql(e, "UPDATE audit SET outcome='confirmed' WHERE id=?", id)]);
        const removed = await sql(e, 'SELECT removed FROM players WHERE node=?', node).first<{removed:number}>();
        if (removed?.removed) { // Removal raced this call: compensate before returning, organizer can also retry.
          const undo = await audit(e, node, 'deauthorize', 'pending', now);
          if (mode === 'central') await change(e,node,false); await sql(e, "UPDATE audit SET outcome='confirmed' WHERE id=?",undo).run();
          fail(403, 'Player removed. Contact your organizer.');
        }
        console.log(JSON.stringify({ event: 'authorization', node, result: 'confirmed', audit: id }));
        return json({ network, node, mode });
      }
      const p = await player(r,e);
      if (route === 'POST /presence') {
        const b = await body(r), status = str(b, 'status', /^(online|menus|hosting|match)$/);
        // A poll costs one row written every 20 s at most, and none between. The refresh limits itself; a status change is the
        // only write a client can repeat at will, so that alone is counted. The answer is the directory: one request per poll.
        if (status !== p.status) await rate(e, `presence:${p.node}`, now, 60, 12);
        if (status !== p.status || p.last_seen < now-PRESENCE_WRITE)
          await sql(e, 'UPDATE players SET status=?,last_seen=? WHERE node=? AND removed=0', status, now, p.node).run();
        return json(await directory(e, p, now));
      }
      if (route === 'GET /friends') return json(await directory(e, p, now));
      if (route === 'POST /invite') {
        await rate(e, `invite:${p.node}`, now, 60, 6);
        if (p.status !== 'hosting' || p.last_seen < now-FRESH) fail(409, 'Create a Local Wireless room first');
        const to = nodeId(await body(r), 'to'); if (to === p.node) fail(400, 'Choose a friend');
        const target = await sql(e, "SELECT node FROM players WHERE node=? AND removed=0 AND state='active' AND last_seen>=?", to, now-FRESH).first();
        if (!target) fail(404, 'Friend is offline');
        const id = crypto.randomUUID();
        await e.DB.batch([sql(e, 'DELETE FROM invites WHERE sender=? AND recipient=?',p.node,to), sql(e, 'INSERT INTO invites(id,sender,recipient,expires) VALUES(?,?,?,?)',id,p.node,to,now+120)]);
        return json({ id, expires: now+120 });
      }
      if (route === 'POST /invite/dismiss') {
        const id = str(await body(r),'id',/^[0-9a-f-]{36}$/); await sql(e,'DELETE FROM invites WHERE id=? AND recipient=?',id,p.node).run(); return json({ ok: true });
      }
      return fail(404, 'Unknown route');
    } catch (error) {
      if (error instanceof Failure) return json({ error: error.message, ...(error.state ? { state: error.state } : {}) }, error.status);
      // Never log request bodies, Authorization headers, invite codes or upstream responses.
      console.error(JSON.stringify({ event: 'group_error', path: u.pathname })); return json({ error: 'Service unavailable. Manual networking still works.' },503);
    }
  };
}
export default {
  fetch: createHandler(),
  async scheduled(_controller: ScheduledController, e: Bindings) {
    const now = Math.floor(Date.now()/1000);
    await e.DB.batch([sql(e,'DELETE FROM limits WHERE until<?',now),sql(e,'DELETE FROM invites WHERE expires<?',now),sql(e,'DELETE FROM codes WHERE expires<?',now-86400),sql(e,'DELETE FROM audit WHERE at<?',now-90*86400)]);
  }
};
