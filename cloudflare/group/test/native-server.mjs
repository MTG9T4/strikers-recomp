// Loopback-only fallback. No external fetch implementation is available here.
// Not shipped as production entry point and not a replacement for wrangler dev acceptance.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { timingSafeEqual } from 'node:crypto';
import ts from 'typescript';
import { database } from './sqlite.mjs';
crypto.subtle.timingSafeEqual=(a,b)=>timingSafeEqual(Buffer.from(a),Buffer.from(b));
const compiled=ts.transpileModule(await readFile(new URL('../src/index.ts',import.meta.url),'utf8'),{compilerOptions:{target:ts.ScriptTarget.ES2022,module:ts.ModuleKind.ESNext}}).outputText;
const {createHandler}=await import('data:text/javascript;base64,'+Buffer.from(compiled).toString('base64'));
const handler=createHandler(async (_e,node,authorized)=>console.log(JSON.stringify({mock_central:true,node,authorized})),true);
globalThis.fetch=()=>{throw Error('Outbound network forbidden in native fallback');};
const env={DB:await database(),GROUP_MODE:process.env.GROUP_MODE || 'adhoc',...(process.env.GROUP_MODE==='central'?{ZT_TOKEN:'mock-only'}:{}),ZT_NETWORK:process.env.GROUP_MODE==='central'?'aabbccddeeff0011':'ff61286137000000',ADMIN_TOKEN:'local-test-admin-only',CODE_PEPPER:'local-test-pepper-only'};
const traceStart=performance.now();
const server=createServer(async(req,res)=>{
  try {
    let data=Buffer.alloc(0);
    for await(const chunk of req){data=Buffer.concat([data,chunk]);if(data.length>2048){res.writeHead(413);res.end('{}');return;}}
    const r=await handler(new Request('http://127.0.0.1:8787'+req.url,{method:req.method,headers:req.headers,...(data.length?{body:data}:{})}),env);
    const responseText=await r.text();
    if(process.env.GROUP_TEST_TRACE==='1') {
      let node;
      try {const value=JSON.parse(responseText);const candidate=value.self??value.node;if(typeof candidate==='string'&&/^[0-9a-f]{10}$/.test(candidate))node=candidate;}catch{}
      // Public route/status/node metadata only. Never headers, bodies or codes.
      console.log(JSON.stringify({event:'request',seconds:(performance.now()-traceStart)/1000,method:req.method,path:new URL('http://127.0.0.1'+req.url).pathname,status:r.status,node}));
    }
    res.writeHead(r.status,Object.fromEntries(r.headers));res.end(responseText);
  }catch{res.writeHead(503);res.end('{}');}
});
server.listen(Number(process.env.GROUP_TEST_PORT ?? 8787),'127.0.0.1',()=>console.log('READY http://127.0.0.1:'+server.address().port+' (SQLite fallback, '+env.GROUP_MODE+')'));
