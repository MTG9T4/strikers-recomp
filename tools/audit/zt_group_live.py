"""Job 22: native transport/client acceptance, never a game or Central client.

Only loopback HTTP and ZeroTier public roots. The optional supplied service
must be a fresh local ad-hoc Worker; outage tests then use a loopback gate so
the harness never stops a server it does not own.
"""
import ctypes
import _ctypes
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import uuid
from build_runtime import ROOT, msvc_environment

PROBE = r'''
#include "zt_transport.c"
#include "group_client.h"
#include "gfx_options.h"
static GfxOptions options;
const GfxOptions *gfx_options(void){return &options;}
int main(void) {
 snprintf(options.group_service,sizeof options.group_service,"%s",getenv("TEST_URL"));
 group_start();
 uint64_t until=GetTickCount64()+300000,last=0;int opened=0,enrolled=0,rx=0;
 char phase[32]="boot";unsigned attacks=0;
 while(GetTickCount64()<until) {
  int enroll=0;FILE *control=fopen(getenv("TEST_CONTROL"),"r");
  if(control){fscanf(control,"%31s %d",phase,&enroll);fclose(control);}
  if(!strcmp(phase,"exit"))break;
  if(enroll&&!enrolled){group_join(getenv("TEST_CODE"),getenv("TEST_NAME"));enrolled=1;}
  ZtStatus z;zt_status(&z);GroupStatus g;group_status(&g);
  if(zt_ready()&&!opened){uint16_t port;if(!(opened=zt_open(24872,&port)))return 11;}
  if(GetTickCount64()-last>500) {
   last=GetTickCount64();printf("STATUS phase=%s node=%010llx state=%d joined=%d reachable=%d friends=%u rx=%d attacks=%u\n",phase,z.node,z.state,g.joined,g.reachable,g.count,rx,attacks);
   if(opened){char data[32];snprintf(data,sizeof data,"ZTPROBE:%010llx",z.node);zt_discover(24872,data,18);
    /* Hostile client bypasses its OWN policy, with its own authenticated node.
     * The victim must reject it. Test target IDs never reach proper clients. */
    const char *targets=getenv("TEST_ATTACK_TARGETS");
    if(targets){char list[256];snprintf(list,sizeof list,"%s",targets);char *next=NULL;
     for(char *id=strtok_s(list,",",&next);id;id=strtok_s(NULL,",",&next)) {
      char ip[ZTS_IP_MAX_STR_LEN];struct zts_sockaddr_storage addr={0};zts_socklen_t len=sizeof addr;
      if(!p_zts_addr_compute_6plane_str(z.network,_strtoui64(id,NULL,16),ip,sizeof ip)&&!p_zts_util_ipstr_to_saddr(ip,24872,(struct zts_sockaddr*)&addr,&len))
       if(p_zts_bsd_sendto(fd,data,18,0,(struct zts_sockaddr*)&addr,len)==18)attacks++;
     }
    }
   }fflush(stdout);
  }
  if(opened)for(int i=0;i<128;i++){char data[64]={0};uint32_t token;uint16_t port;int n=zt_recv(&token,&port,data,63);if(n<0)break;
   if(n==18&&!memcmp(data,"ZTPROBE:",8)){rx++;printf("RECV phase=%s src=%.10s\n",phase,data+8);fflush(stdout);}
  }
  Sleep(5);
 }
 zt_close();printf("RESULT rx=%d\n",rx);return rx?0:12;
}
'''


def build(folder):
    env = msvc_environment()
    src = folder/'group_probe.c'; src.write_text(PROBE)
    exe = folder/'group_probe.exe'
    cmd = [str(Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe'), '/nologo','/MD','/O2','/std:c11',
           '/D_CRT_SECURE_NO_WARNINGS','/DWIN32_LEAN_AND_MEAN','/DSTRIKERS_GROUP_LOCAL_TEST=1',
           '/I'+str(ROOT/'runtime/services'),'/I'+str(ROOT/'runtime/gfx'),str(src),
           str(ROOT/'runtime/services/group_client.c'),str(ROOT/'runtime/services/group_json.c'),
           '/Fe:'+str(exe),'/link','ws2_32.lib']
    r = subprocess.run(cmd,cwd=folder,env=env,capture_output=True,text=True)
    assert r.returncode == 0,r.stdout+r.stderr
    shutil.copy2(ROOT/'build/bin/strikers_zt.dll',folder/'strikers_zt.dll')
    return exe


def run(count, out, service_url=None):
    from urllib.parse import urlsplit
    if service_url:
        u=urlsplit(service_url)
        assert u.scheme=='http' and u.hostname=='127.0.0.1' and not u.username and not u.password and u.path in ('','/') and not u.query and not u.fragment
    out=out.resolve();assert out.is_relative_to(ROOT/'build/run') and not out.exists()
    out.mkdir(parents=True)
    scratch=Path(tempfile.gettempdir())/('strikers-group-live-'+uuid.uuid4().hex);scratch.mkdir()
    base={k:v for k,v in os.environ.items() if not k.startswith(('STRIKERS_','TEST_','GROUP_'))}
    children=[];handles=[];server=None;checks=[];gate=None;sdk=None
    try:
        # Build ONLY the SDK, never build_host or the game/launcher. Ensure a
        # stale DLL cannot make a current-source test look successful.
        sys.path.insert(0,str(ROOT/'tools/lifter'))
        from build_exe import build_libzt
        assert build_libzt(msvc_environment())==0,'SDK-only build failed'
        exe=build(scratch)
        if not service_url:
            log=(out/'service.log').open('wb');handles.append(log)
            server=subprocess.Popen(['node','test/native-server.mjs'],cwd=ROOT/'cloudflare/group',env=dict(base,GROUP_TEST_PORT='0'),stdout=log,stderr=subprocess.STDOUT)
            end=time.monotonic()+10
            while 'READY' not in (out/'service.log').read_text(errors='replace'):
                assert server.poll() is None,(out/'service.log').read_text();assert time.monotonic()<end;time.sleep(.1)
        upstream=service_url or re.search(r'READY (http://127\.0\.0\.1:\d+)',(out/'service.log').read_text())[1]
        # A local failure gate exercises real WinHTTP outage behavior for either
        # backend; it has no forwarding target supplied by any HTTP request.
        class Gate(http.server.BaseHTTPRequestHandler):
            down=False
            def forward(self):
                if self.__class__.down:
                    self.send_response(503);self.send_header('Content-Length','2');self.end_headers();self.wfile.write(b'{}');return
                data=self.rfile.read(int(self.headers.get('Content-Length','0'))) if self.command=='POST' else None
                headers={k:v for k,v in self.headers.items() if k.lower() not in ('host','content-length')}
                req=urllib.request.Request(upstream.rstrip('/')+self.path,data,headers,method=self.command)
                try:r=urllib.request.urlopen(req,timeout=5)
                except urllib.error.HTTPError as error:r=error
                with r:
                    body=r.read();self.send_response(r.status);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
            do_GET=do_POST=forward
            def log_message(self,*args):pass
        gate=http.server.ThreadingHTTPServer(('127.0.0.1',0),Gate)
        threading.Thread(target=gate.serve_forever,daemon=True).start()
        origin=f'http://127.0.0.1:{gate.server_port}'
        def call(path,data):
            req=urllib.request.Request(upstream.rstrip('/')+path,json.dumps(data).encode(),{'Content-Type':'application/json','Authorization':'Bearer local-test-admin-only'})
            with urllib.request.urlopen(req,timeout=5) as r:return json.load(r)
        code=call('/admin/codes',{'uses':16})['code']
        sdk=ctypes.CDLL(str(scratch/'strikers_zt.dll'));ids=[]
        for i in range(count):
            folder=out/f'player{i}/zerotier';folder.mkdir(parents=True)
            key=ctypes.create_string_buffer(384);length=ctypes.c_uint(384)
            assert sdk.zts_id_new(key,ctypes.byref(length))==0
            (folder/'identity.secret').write_bytes(key.value);ids.append(key.value.decode().split(':')[0])
        def control(i,phase,enroll=1):
            (out/f'control{i}.txt').write_text(f'{phase} {enroll}')
        def start(i,restart=False):
            config=out/f'player{i}/strikers.ini'
            # Only the hostile third process knows its manual ad-hoc ID before
            # enrollment. Legitimate clients get network AND peers from Worker.
            config.write_text('[Network]\nmode=zerotier\nzerotier_network='+('ff61286137000000' if count==3 and i==2 else '')+'\ngroup_service='+origin+'\n')
            env=dict(base,STRIKERS_PC_CONFIG=config.as_posix(),STRIKERS_USERDATA=(out/f'player{i}').as_posix(),TEST_URL=origin,TEST_CODE=code,TEST_NAME=f'Player {i}',TEST_CONTROL=(out/f'control{i}.txt').as_posix())
            if i==count-1:env['TEST_ATTACK_TARGETS']=','.join(ids[:i])
            path=out/(f'player{i}-restart.log' if restart else f'player{i}.log')
            log=path.open('wb');handles.append(log)
            p=subprocess.Popen([str(exe)],env=env,cwd=scratch,stdout=log,stderr=subprocess.STDOUT)
            children.append(p);return p,path
        paths=[];active=[]
        for i in range(count):
            control(i,'outsider' if count==3 else 'initial',0 if count==3 and i==2 else 1)
            p,path=start(i);active.append(p);paths.append(path)
        def logs():return [p.read_text(errors='replace') for p in paths]
        def wait(predicate,description,seconds=65):
            end=time.monotonic()+seconds
            while time.monotonic()<end:
                assert all(p.poll() is None for p in active),logs()
                if predicate(logs()):checks.append(description);print('PASS '+description,flush=True);return
                time.sleep(.25)
            raise AssertionError(description+'; inspect retained player logs in '+str(out))
        def received(text,phase,node):return f'RECV phase={phase} src={node}' in text
        if count==3:
            wait(lambda ls:all(received(ls[i],'outsider',ids[1-i]) and f'[zt-group] rejected authenticated node={ids[2]}' in ls[i] for i in range(2)), 'two fresh members discover from Worker alone; SDK rejects authenticated outsider at ingress')
            time.sleep(3)
            assert all(not received(ls,'outsider',ids[2]) for ls in logs()[:2]),'outsider accepted'
            checks.append('unenrolled authenticated outsider ignored')
            for i in range(count):control(i,'enrolled')
            phase='enrolled'
        else:phase='initial'
        wait(lambda ls:all(all(received(ls[i],phase,ids[j]) for j in range(count) if i!=j) for i in range(count)),f'{count} real nodes discover each other through Worker, no peer hints')
        Gate.down=True
        for i in range(count):control(i,'outage')
        wait(lambda ls:all(re.search(r'STATUS phase=outage .*reachable=0',s) and all(received(s,'outage',ids[j]) for j in range(count) if j!=i) for i,s in enumerate(ls)), 'service down retains peers and datagrams',35)
        # Relaunch exactly one test probe using its unchanged identity/DPAPI
        # cache while service remains unavailable. Never launch a game build.
        control(0,'exit');active[0].wait(timeout=12);assert active[0].returncode==0
        control(0,'restart',0);active[0],paths[0]=start(0,True)
        wait(lambda ls:re.search(r'STATUS phase=restart .*joined=1 reachable=0',ls[0]) and all(received(ls[0],'restart',ids[j]) for j in range(1,count)), 'offline relaunch restores DPAPI peer cache and identity')
        assert (out/'player0/zerotier/identity.secret').read_bytes().decode().split(':')[0]==ids[0]
        Gate.down=False
        for i in range(count):control(i,'recovered',0 if i==0 else 1)
        wait(lambda ls:all(re.search(r'STATUS phase=recovered .*reachable=1',s) for s in ls),'service recovery refreshes without restart',35)
        removal_offsets=[len(s) for s in logs()]
        call('/admin/remove',{'node':ids[-1]})
        for i in range(count):control(i,'removing',0 if i==0 else 1)
        wait(lambda ls:all(re.search(rf'STATUS phase=removing .*friends={count-1} ',s) for s in ls[:-1]) and re.search(r'STATUS phase=removing .*reachable=0 friends=0 ',ls[-1]),'removal applied at next membership poll',35)
        for i in range(count):control(i,'removed',0 if i==0 else 1)
        time.sleep(4)
        ls=logs()
        assert all(not received(s,'removed',ids[-1]) for s in ls[:-1]),'removed node accepted'
        assert all(f'[zt-group] rejected authenticated node={ids[-1]}' in s[removal_offsets[i]:] for i,s in enumerate(ls[:-1])),'no authenticated ingress rejection after removal'
        assert int(re.findall(r'attacks=(\d+)',ls[-1])[-1])>0,'hostile node never sent'
        if count==3:assert all(received(ls[i],'removed',ids[1-i]) for i in range(2)),'remaining peers stopped'
        checks.append('removed node still sends but is ignored; remaining peers keep playing')
        for i in range(count):control(i,'exit')
        for p in active:p.wait(timeout=12);assert p.returncode==0
        result={'status':'PASS','processes':count,'nodes':ids,'checks':checks,'backend':'external loopback Worker' if service_url else 'production handler + Node/SQLite fallback, no Central','game_launched':False,'peer_hints_for_members':False}
        (out/'results.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2),flush=True)
    except BaseException as error:
        (out/'failure.txt').write_text(str(error));raise
    finally:
        for p in children:
            if p.poll() is None:p.kill();p.wait()
        if gate:gate.shutdown();gate.server_close()
        if server:server.terminate();server.wait()
        for f in handles:f.close()
        if sdk:_ctypes.FreeLibrary(sdk._handle)
        assert scratch.resolve().parent==Path(tempfile.gettempdir()).resolve();shutil.rmtree(scratch)
