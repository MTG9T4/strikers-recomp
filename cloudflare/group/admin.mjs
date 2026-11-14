// Interactive organizer helper. Secrets are read from a masked prompt, never argv or INI.
import { createInterface } from 'node:readline/promises';
import { Writable } from 'node:stream';
const [origin, command, arg] = process.argv.slice(2);
const local = process.env.STRIKERS_GROUP_LOCAL_TEST === '1';
const u = new URL(origin);
if (u.username || u.password || u.search || u.hash || u.pathname !== '/' || (u.protocol !== 'https:' && !(local && u.origin === 'http://127.0.0.1:8787'))) throw Error('Use the Worker HTTPS origin');
const routes = { code: ['/admin/codes', {hours:24,uses:16}], revoke: ['/admin/revoke-code', {}], remove: ['/admin/remove', {node:arg}], players:['/admin/players'], audit:['/admin/audit'] };
if (!routes[command]) throw Error('Usage: npm run admin -- https://WORKER code|revoke|players|audit|remove NODE');
process.stdout.write('Organizer token (hidden): ');
const sink=new Writable({write(_chunk,_encoding,callback){callback();}});
const rl=createInterface({input:process.stdin,output:sink,terminal:true});
const token=await rl.question(''); rl.close(); process.stdout.write('\n');
const [path,body]=routes[command];
const r=await fetch(u.origin+path,{method:body?'POST':'GET',redirect:'error',headers:{Authorization:'Bearer '+token,'Content-Type':'application/json'},...(body?{body:JSON.stringify(body)}:{})});
console.log(JSON.stringify(await r.json(),null,2)); if(!r.ok)process.exitCode=1;
