"""Audit 06: the group service client's JSON handling under AddressSanitizer, with every input ending at a guard page.

    .venv/Scripts/python -I tools/audit/test_group_json_fuzz.py [--cases 200000] [--out build/run/<name>]

Builds runtime/services/group_json.c and group_client.c (included, so its response handler `friends` and `response_error` are
called directly; WinHTTP is never reached) with MSVC /fsanitize=address, then:
  * mutates a corpus of service answers (friends lists, invitations, errors, enrolment) byte by byte (flip, insert from the
    JSON alphabet, delete, truncate, duplicate a span) and parses each with a random token capacity;
  * checks every token of a parse that succeeded (start/length inside the text, `end` inside the token list and after the
    token itself), decodes every string token into buffers of random size (result NUL-terminated inside its buffer) and looks
    up keys in every object;
  * hands every mutation of a friends answer to the client's own handler and checks the status it publishes (at most 128
    friends, every field terminated, node ids of 10 hex digits, an invitation id of id characters only, so it can never break
    out of the JSON body group_dismiss sends back).
Each input is copied so that its terminating NUL is the last byte before a PAGE_NOACCESS page: a read past the end faults.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_runtime import ROOT, msvc_environment

PROBE = r'''
#define _CRT_SECURE_NO_WARNINGS
#include "group_client.c"
static GfxOptions options;
const GfxOptions *gfx_options(void){return &options;}
int zt_requested(void){return 1;}
void zt_start(void){}
static uint64_t applied_peers[128];static unsigned applied_count,apply_calls;
void zt_set_group_peers(const uint64_t *nodes,unsigned count){applied_count=count;if(count)memcpy(applied_peers,nodes,count*sizeof *nodes);apply_calls++;}
void zt_status(ZtStatus *s){memset(s,0,sizeof *s);s->state=2;}
int zt_set_group_network(const char *n){(void)n;return 1;}
static unsigned seed=12345;
static unsigned rnd(void){seed=seed*1664525u+1013904223u;return seed>>8;}
static char *page;static size_t page_cap;
static const char *guarded(const char *s,size_t n){            /* s[0..n) + NUL, the NUL right before a no-access page */
    size_t need=(n+1+4095)&~(size_t)4095;
    if(need>page_cap){if(page)VirtualFree(page,0,MEM_RELEASE);page=VirtualAlloc(NULL,need+4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);DWORD old;VirtualProtect(page+need,4096,PAGE_NOACCESS,&old);page_cap=need;}
    char *d=page+page_cap-(n+1);memcpy(d,s,n);d[n]=0;return d;
}
static int fail(const char *what,const char *s){printf("FAIL %s: input [%s]\n",what,s);return 1;}
static int terminated(const char *b,size_t cap){return memchr(b,0,cap)!=NULL;}
static int check_parse(const char *s){
    static GjToken t[4096];int cap=1+(int)(rnd()%(rnd()%4?64:4096));
    int n=gj_parse(s,t,cap);if(n<0)return 0;if(n==0||n>cap)return fail("token count",s);
    int len=(int)strlen(s);
    for(int i=0;i<n;i++){
        if(t[i].start<0||t[i].length<1||t[i].start+t[i].length>len)return fail("token outside the text",s);
        if(t[i].end<=i||t[i].end>n)return fail("subtree end",s);
        char out[300];unsigned oc=1+rnd()%300;memset(out,0x5a,sizeof out);
        if(gj_string(s,t,i,out,oc)&&!terminated(out,oc))return fail("string not terminated in its buffer",s);
        if(t[i].kind=='{'){static const char *keys[]={"name","node","friends","invites","error","id","network","status"};gj_field(s,t,i,keys[rnd()%8]);}
    }
    return 0;
}
static int check_client(const char *s){
    GroupStatus v;
    AcquireSRWLockExclusive(&lock);memset(&view,0,sizeof view);ReleaseSRWLockExclusive(&lock);
    uint64_t old_peers[128];unsigned old_count=cached_count,old_calls=apply_calls;
    memcpy(old_peers,cached_peers,sizeof old_peers);
    int ok=friends(s);
    group_status(&v);
    if(!ok){if(cached_count!=old_count||memcmp(old_peers,cached_peers,sizeof old_peers)||apply_calls!=old_calls)return fail("invalid response changed last-good allowlist",s);return 0;}
    if(applied_count!=v.count||memcmp(applied_peers,cached_peers,v.count*sizeof *cached_peers))return fail("directory not applied atomically",s);
    if(v.count>128)return fail("more than 128 friends",s);
    for(unsigned i=0;i<v.count;i++){
        if(!terminated(v.friends[i].node,sizeof v.friends[i].node)||!terminated(v.friends[i].name,sizeof v.friends[i].name)||!terminated(v.friends[i].status,sizeof v.friends[i].status))return fail("friend field not terminated",s);
        if(!hexdigits(v.friends[i].node,10))return fail("friend node not 10 hex digits",s);
    }
    if(!terminated(v.toast,sizeof v.toast)||!terminated(v.invite_id,sizeof v.invite_id))return fail("toast or invitation not terminated",s);
    if(strspn(v.invite_id,"0123456789abcdefABCDEF-")!=strlen(v.invite_id))return fail("invitation id with characters that would break the dismiss body",s);
    return 0;
}
int main(int argc,char **argv){
    long cases=argc>1?atol(argv[1]):200000;
    static const char *corpus[]={
        "{\"friends\":[{\"node\":\"abcdef0123\",\"name\":\"Alice\",\"status\":\"menus\"},{\"node\":\"0123456789\",\"name\":\"Bob \\uD83D\\uDE00\",\"status\":\"hosting\"}],\"invites\":[{\"id\":\"3f2a9c1e-0b7d-4e5f-9a8b-7c6d5e4f3a2b\",\"name\":\"Bob\"}]}",
        "{\"friends\":[],\"invites\":[]}",
        "{\"friends\":[{\"node\":\"abcdef0123\",\"name\":\"\\\"quoted\\\\\",\"status\":\"match\"}],\"invites\":[{\"id\":\"x\\\"},\\\"to\\\":\\\"y\",\"name\":\"evil\"}]}",
        "{\"error\":\"That invite code is not valid.\"}",
        "{\"network\":\"aabbccddeeff0011\",\"node\":\"abcdef0123\"}",
        "[1,-0.5e+3,true,false,null,\"\\u0041\\/\",{\"a\":[[[]]]}]",
    };
    const int nc=(int)(sizeof corpus/sizeof *corpus);
    static char buf[16384];
    long parsed=0,accepted=0;
    /* the corpus itself first: the invitation whose id carries a quote must be refused, the plain ones accepted */
    for(int k=0;k<nc;k++){const char *s=guarded(corpus[k],strlen(corpus[k]));if(check_parse(s)||check_client(s))return 1;}
    {GroupStatus v;const char *s=guarded(corpus[2],strlen(corpus[2]));AcquireSRWLockExclusive(&lock);memset(&view,0,sizeof view);ReleaseSRWLockExclusive(&lock);
     if(friends(s))return fail("an invitation id with a quote was accepted",s);
     s=guarded(corpus[0],strlen(corpus[0]));if(!friends(s))return fail("a well-formed friends answer was refused",s);group_status(&v);if(v.count!=2||strcmp(v.invite_id,"3f2a9c1e-0b7d-4e5f-9a8b-7c6d5e4f3a2b"))return fail("friends answer read wrongly",s);}
    /* Audit 08: only the Worker's own verdict clears the allowlist, never an edge page. */
    {const char *gone="{\"error\":\"Membership is missing\",\"state\":\"gone\"}";
     if(!revoked(401,guarded(gone,strlen(gone))))return fail("the Worker's revocation was not recognised",gone);
     static const char *edge[]={"<html><body>error code: 1027</body></html>","","{\"error\":\"x\"}","{\"state\":\"other\"}","{\"state\":1}","[\"gone\"]"};
     for(unsigned i=0;i<sizeof edge/sizeof *edge;i++)if(revoked(401,guarded(edge[i],strlen(edge[i]))))return fail("an edge answer revoked",edge[i]);
     if(revoked(403,gone)||revoked(429,gone)||revoked(503,gone)||revoked(0,gone)||revoked(200,gone))return fail("a status other than 401 revoked",gone);}
    /* Plain UI status uses current presence, never stale outage counts or IDs. */
    {char text[256];view.joined=1;group_connection(text,sizeof text);
     if(strcmp(text,"Connected. 2 friends online."))return fail("online summary",text);
     view.reachable=0;group_connection(text,sizeof text);if(!strstr(text,"using saved friends"))return fail("outage summary",text);
     view.revoked=1;group_connection(text,sizeof text);if(!strstr(text,"access removed"))return fail("revoked summary",text);}
    for(long c=0;c<cases;c++){
        const char *src=corpus[rnd()%nc];size_t n=strlen(src);memcpy(buf,src,n);
        int muts=1+(int)(rnd()%8);
        for(int m=0;m<muts;m++){
            static const char alpha[]="[]{}\":,\\u0123456789abcdefABCDEF-+.eE tnrfl\x01\x7f\xc3\xa9";
            unsigned op=rnd()%6;size_t at=n?rnd()%n:0;
            if(op==0&&n)buf[at]^=(char)(1u<<(rnd()%8));
            else if(op==1&&n+1<sizeof buf){memmove(buf+at+1,buf+at,n-at);buf[at]=alpha[rnd()%(sizeof alpha-1)];n++;}
            else if(op==2&&n){memmove(buf+at,buf+at+1,n-at-1);n--;}
            else if(op==3)n=at;
            else if(op==4&&n){size_t l=1+rnd()%16;if(at+l>n)l=n-at;if(n+l<sizeof buf){memmove(buf+at+l,buf+at,n-at);n+=l;}}
            else if(op==5&&n){buf[at]=0;}
        }
        size_t z=strnlen(buf,n);
        const char *s=guarded(buf,z);
        GjToken t[8];if(gj_parse(s,t,8)>=0)accepted++;
        parsed++;
        if(check_parse(s)||check_client(s))return 1;
    }
    printf("PASS %ld mutated inputs (%ld parse with 8 tokens), every one ending at a guard page under ASan; tokens, strings, fields and the friends handler checked\n",parsed,accepted);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cases', type=int, default=200000)
    p.add_argument('--out', default=str(ROOT / 'build/run/group-json-fuzz'))
    a = p.parse_args()
    out = Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = msvc_environment()
    bindir = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64'
    src = out / 'fuzz.c'
    src.write_text(PROBE)
    exe = out / 'fuzz.exe'
    cmd = [str(bindir / 'cl.exe'), '/nologo', '/MD', '/Zi', '/Od', '/fsanitize=address', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS',
           '/I' + str(ROOT / 'runtime/services'), '/I' + str(ROOT / 'runtime/gfx'), str(src), str(ROOT / 'runtime/services/group_json.c'),
           '/Fe:' + str(exe), '/Fo:' + str(out) + os.sep, '/Fd:' + str(out / 'fuzz.pdb')]
    r = subprocess.run(cmd, cwd=out, env=env, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        print('FAIL: the fuzz probe did not build')
        return 1
    run_env = dict(env, PATH=str(bindir) + os.pathsep + env.get('PATH', ''), ASAN_OPTIONS='halt_on_error=1')
    r = subprocess.run([str(exe), str(a.cases)], cwd=out, env=run_env, capture_output=True, text=True, errors='replace', timeout=600)
    print((r.stdout + r.stderr)[-6000:])
    if r.returncode or 'PASS' not in r.stdout:
        print('FAIL: exit %d' % r.returncode)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
