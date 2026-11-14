"""Group native contracts; production WinHTTP client and bounded parser.

Default: offline parser fuzz + HTTPS-only origin validation, no server required.
--integration: start local-only SQLite Worker fallback (not wrangler/D1 evidence),
then two independent native clients enroll, publish status, see friends and invite.
No Central/Cloudflare API calls or ZeroTier root traffic.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import urllib.request
import uuid
import http.server
import ssl
import re
import threading
import sys
from build_runtime import ROOT, msvc_environment

PROBE = r'''
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "group_client.h"
#include "group_json.h"
#include "zt_transport.h"
#include "gfx_options.h"
static GfxOptions options;
const GfxOptions *gfx_options(void){return &options;}
int zt_requested(void){return 1;}
void zt_start(void){}
void zt_set_group_peers(const uint64_t *nodes,unsigned count){(void)nodes;(void)count;}
void zt_status(ZtStatus *s){memset(s,0,sizeof *s);s->node=_strtoui64(getenv("TEST_NODE")?getenv("TEST_NODE"):"abcdef0123",NULL,16);}
int zt_set_group_network(const char *n){return strlen(n)==16&&(!strcmp(n,"aabbccddeeff0011")||!strcmp(n,"ff61286137000000"));}
int main(int argc,char **argv){
 if(argc>1&&!strcmp(argv[1],"parser")){
  GjToken t[64];char value[64];const char *s="{\"name\":\"Alice \\uD83D\\uDE00\",\"items\":[{},3]}";
  if(gj_parse(s,t,64)<1||!gj_string(s,t,gj_field(s,t,0,"name"),value,sizeof value)||strcmp(value,"Alice \xf0\x9f\x98\x80"))return 1;
  const char *bad[]={"{","[]x","{\"x\":}","[1,]","{\"a\":01}","\"\\u12","{\"x\":\"a\n\"}"};
  for(unsigned i=0;i<sizeof bad/sizeof *bad;i++)if(gj_parse(bad[i],t,64)>=0)return 2;
  if(gj_parse(s,t,2)>=0)return 3;
  unsigned seed=7;for(int i=0;i<50000;i++){char b[80];int len=i%79;for(int j=0;j<len;j++){seed=seed*1664525+1013904223;b[j]="[]{}\"\\u:,-0129abc \n"[seed%20];}b[len]=0;gj_parse(b,t,64);}
  puts("PASS JSON Unicode, malformed input, capacity and 50000 deterministic fuzz cases");return 0;
 }
 snprintf(options.group_service,sizeof options.group_service,"%s",getenv("TEST_URL")?getenv("TEST_URL"):"http://127.0.0.1:8787");
 group_start();
 if(argc>1&&!strcmp(argv[1],"policy")){GroupStatus s;group_status(&s);puts(s.message);return strstr(s.message,"HTTPS")?0:4;}
 group_join(getenv("TEST_CODE"),getenv("TEST_NAME"));
 if(argc>1&&!strcmp(argv[1],"rejection")){
  ULONGLONG end=GetTickCount64()+12000;
  while(GetTickCount64()<end){GroupStatus s;group_status(&s);if(!s.busy){printf("REJECT joined=%d %s\n",s.joined,s.message);return s.joined?6:0;}Sleep(20);}return 7;
 }
 int host=getenv("TEST_HOST")!=NULL;ULONGLONG until=GetTickCount64()+45000;int invited=0;
 while(GetTickCount64()<until){GroupStatus s;group_status(&s);
  if(s.joined&&host)group_ldn_state(3);
  int published_host=0;for(unsigned i=0;i<s.count;i++)if(!strcmp(s.friends[i].node,s.self)&&!strcmp(s.friends[i].status,"hosting"))published_host=1;
  if(s.count>=2){if(host&&!invited&&published_host){for(unsigned i=0;i<s.count;i++)if(strcmp(s.friends[i].node,s.self)){group_invite(s.friends[i].node);invited=1;puts("INVITE queued via actual native client");}}
   if(!host&&s.toast[0]){printf("INVITE received %s\n",s.toast);group_dismiss();puts("PASS two native clients, Friends, hosting and invitation");return 0;}
   if(host&&invited&&!strcmp(s.message,"Invitation sent.")){Sleep(12000);puts("PASS host Friends + Invite");return 0;}
  }Sleep(100);
 }GroupStatus s;group_status(&s);printf("FAIL %s joined=%d count=%u\n",s.message,s.joined,s.count);return 5;
}
'''

def build(folder, local=False):
    env = msvc_environment()
    cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
    source = folder / 'probe.c'; source.write_text(PROBE)
    exe = folder / ('local.exe' if local else 'production.exe')
    cmd = [str(cl), '/nologo', '/MD', '/O2', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS',
           '/I'+str(ROOT/'runtime/services'), '/I'+str(ROOT/'runtime/gfx'),
           *(['/DSTRIKERS_GROUP_LOCAL_TEST=1'] if local else []), str(source),
           str(ROOT/'runtime/services/group_client.c'), str(ROOT/'runtime/services/group_json.c'), '/Fe:'+str(exe)]
    r = subprocess.run(cmd, cwd=folder, env=env, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout+r.stderr
    return exe

def call(path, data, origin='http://127.0.0.1:8787'):
    from urllib.parse import urlsplit
    parsed=urlsplit(origin)
    assert parsed.scheme=='http' and parsed.hostname=='127.0.0.1' and not parsed.username and not parsed.password and parsed.path in ('','/') and not parsed.query and not parsed.fragment
    req = urllib.request.Request(origin.rstrip('/')+path, None if data is None else json.dumps(data).encode(),
        {'Content-Type':'application/json','Authorization':'Bearer local-test-admin-only'})
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.load(r)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--integration', action='store_true')
    ap.add_argument('--service-url',help='already-running wrangler local origin, numeric 127.0.0.1 HTTP only; no fallback server is started')
    ap.add_argument('--out', type=Path, default=ROOT/'build/run/job19-native')
    a=ap.parse_args(); a.out.mkdir(parents=True,exist_ok=True)
    server=None; children=[]; handles=[]
    folder=Path(tempfile.gettempdir())/('strikers-group-'+uuid.uuid4().hex)
    folder.mkdir()
    try:
        exe=build(folder)
        base={k:v for k,v in os.environ.items() if not k.startswith('STRIKERS_')}
        subprocess.run([str(exe),'parser'],check=True,env=base)
        subprocess.run([str(exe),'policy'],check=True,env=dict(base,TEST_URL='http://127.0.0.1:8787'))
        subprocess.run([str(exe),'policy'],check=True,env=dict(base,TEST_URL='https://group.test/path'))
        print('PASS production rejects HTTP and URL paths',flush=True)
        class Redirect(http.server.BaseHTTPRequestHandler):
            hits=0
            def do_POST(self):
                self.__class__.hits+=1;self.send_response(302);self.send_header('Location','http://127.0.0.1:1/forbidden');self.send_header('Content-Length','0');self.end_headers()
            def log_message(self,*args):pass
        http_server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Redirect)
        threading.Thread(target=http_server.serve_forever,daemon=True).start()
        local=build(folder,True)
        try:
            subprocess.run([str(local),'rejection'],check=True,env=dict(base,TEST_URL=f'http://127.0.0.1:{http_server.server_port}',TEST_CODE='ABCD-2345',TEST_NAME='Test',STRIKERS_USERDATA=(folder/'redirect').as_posix()))
            assert Redirect.hits==1
            print('PASS WinHTTP refuses redirect rather than following the target',flush=True)
        finally:http_server.shutdown();http_server.server_close()
        sys.path.insert(0,str(ROOT))
        from server.nex.generate_pki import generate
        pki=a.out.resolve()/('pki-'+uuid.uuid4().hex)
        generate(pki,['127.0.0.1'])
        tls=http.server.ThreadingHTTPServer(('127.0.0.1',0),Redirect)
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(pki/'server.pem',pki/'server.key')
        tls.socket=context.wrap_socket(tls.socket,server_side=True)
        threading.Thread(target=tls.serve_forever,daemon=True).start();before=Redirect.hits
        try:
            subprocess.run([str(exe),'rejection'],check=True,env=dict(base,TEST_URL=f'https://127.0.0.1:{tls.server_port}',TEST_CODE='ABCD-2345',TEST_NAME='Test',STRIKERS_USERDATA=(folder/'tls').as_posix()))
            assert Redirect.hits==before
            print('PASS Windows rejects an untrusted TLS certificate before HTTP',flush=True)
        finally:tls.shutdown();tls.server_close()
        if a.integration:
            try:
                log=(a.out/'server.log').open('wb');handles.append(log)
                if not a.service_url:
                    server=subprocess.Popen(['node','test/native-server.mjs'],cwd=ROOT/'cloudflare/group',env=dict(base,GROUP_TEST_PORT='0',GROUP_MODE='adhoc'),stdout=log,stderr=subprocess.STDOUT)
                    for _ in range(100):
                        if 'READY' in (a.out/'server.log').read_text(errors='replace'):break
                        assert server.poll() is None,(a.out/'server.log').read_text();time.sleep(.1)
                origin=a.service_url or re.search(r'READY (http://127\.0\.0\.1:\d+)',(a.out/'server.log').read_text())[1]
                code=call('/admin/codes',{},origin)['code'];local=build(folder,True)
                for i,name in enumerate(('Alice','Bob')):
                    userdata=folder/name;userdata.mkdir();log=(a.out/(name+'.log')).open('wb');handles.append(log)
                    env=dict(base,TEST_CODE=code,TEST_NAME=name,TEST_NODE=('aaaaaaaaaa','bbbbbbbbbb')[i],STRIKERS_USERDATA=userdata.as_posix())
                    env['TEST_URL']=origin
                    if i==0:env['TEST_HOST']='1'
                    children.append(subprocess.Popen([str(local)],env=env,cwd=folder,stdout=log,stderr=subprocess.STDOUT))
                codes=[p.wait(timeout=60) for p in children]
                assert codes==[0,0], [(a.out/(n+'.log')).read_text() for n in ('Alice','Bob')]
                kind='externally started loopback service' if a.service_url else 'production handler, SQLite fallback, ad-hoc (no Central)'
                print('PASS '+kind+' + two actual WinHTTP clients')
                (a.out/'results.json').write_text(json.dumps({'native_pair_exit':codes,'server':kind,'workerd':'not inferred by harness' if a.service_url else False},indent=2))
            finally:
                for p in children:
                    if p.poll() is None:p.kill();p.wait()
                if server is not None:server.terminate();server.wait()
                for h in handles:h.close()
    finally:
        assert folder.resolve().parent == Path(tempfile.gettempdir()).resolve()
        shutil.rmtree(folder)

if __name__=='__main__':main()
