/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native overlay group service. Independent of Nintendo account/NEX services.
 * Windows certificate validation; fixed configured origin; no guest URL access. */
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "group_client.h"
#include "group_json.h"
#include "zt_transport.h"
#include "../gfx/gfx_options.h"
#pragma comment(lib,"winhttp.lib")
#pragma comment(lib,"crypt32.lib")
#pragma comment(lib,"bcrypt.lib")
static SRWLOCK lock=SRWLOCK_INIT;
static GroupStatus view;
static INIT_ONCE once=INIT_ONCE_STATIC_INIT;
static HANDLE wake,thread;
static LONG stopping,ldn,match;
static int action;
static char join_code[16],join_name[129],target[40];
static void (*notify_ui)(void);
static char origin[256],profile[MAX_PATH],token[65],network[17],account_node[11];
static wchar_t hostname[256];static INTERNET_PORT port;static int secure;
static uint64_t cached_peers[128];
static unsigned cached_count;
static int cache_dirty;
typedef struct {char magic[8],origin[256],token[65],network[17],node[11],name[129];unsigned count;uint64_t peers[128];} Credential;
/* Audit 08: 15 s, one request. Two requests every 10 s came to 138,240 a day for 8 players left running, over the Workers Free
 * plan's 100,000; now 46,080. A removal reaches running clients within this period. */
#define GROUP_POLL_MS 15000
static void message(const char *s){AcquireSRWLockExclusive(&lock);snprintf(view.message,sizeof view.message,"%s",s);ReleaseSRWLockExclusive(&lock);}
static int hexdigits(const char *s,unsigned n){return strlen(s)==n&&strspn(s,"0123456789abcdef")==n;}
static int endpoint(void){
    wchar_t url[512];if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,origin,-1,url,512))return 0;
    URL_COMPONENTS c={0};c.dwStructSize=sizeof c;c.lpszHostName=hostname;c.dwHostNameLength=256;
    c.dwUserNameLength=c.dwPasswordLength=c.dwUrlPathLength=c.dwExtraInfoLength=(DWORD)-1;
    if(!WinHttpCrackUrl(url,0,0,&c)||!c.dwHostNameLength||c.dwUserNameLength||c.dwPasswordLength||c.dwExtraInfoLength||c.dwUrlPathLength>1||(c.dwUrlPathLength==1&&c.lpszUrlPath[0]!='/'))return 0;
    secure=c.nScheme==INTERNET_SCHEME_HTTPS;port=c.nPort;
#ifdef STRIKERS_GROUP_LOCAL_TEST
    if(!secure&&!(c.nScheme==INTERNET_SCHEME_HTTP&&!wcscmp(hostname,L"127.0.0.1")))return 0;
#else
    if(!secure)return 0;
#endif
    return 1;
}
/* Strict bounds, redirect refusal and no proxy/default credentials. WinHTTP's
 * default chain, hostname, expiry and revocation validation are never disabled. */
static int request(const wchar_t *method,const wchar_t *path,const char *body,char *out,unsigned cap){
    if(cap<1)return 0;out[0]=0;
    int result=0;HINTERNET session=NULL,connection=NULL,r=NULL;wchar_t headers[256]={0};
    session=WinHttpOpen(L"StrikersGroup/1",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);if(!session)goto done;
    if(!WinHttpSetTimeouts(session,3000,3000,3000,3000))goto done;
    DWORD tls=WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;if(!WinHttpSetOption(session,WINHTTP_OPTION_SECURE_PROTOCOLS,&tls,sizeof tls))goto done;
    connection=WinHttpConnect(session,hostname,port,0);if(!connection)goto done;
    r=WinHttpOpenRequest(connection,method,path,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,secure?WINHTTP_FLAG_SECURE:0);if(!r)goto done;
    DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;if(!WinHttpSetOption(r,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof redirects))goto done;
    DWORD autoLogon=WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;if(!WinHttpSetOption(r,WINHTTP_OPTION_AUTOLOGON_POLICY,&autoLogon,sizeof autoLogon))goto done;
    if(secure){DWORD features=WINHTTP_ENABLE_SSL_REVOCATION;if(!WinHttpSetOption(r,WINHTTP_OPTION_ENABLE_FEATURE,&features,sizeof features))goto done;}
    swprintf_s(headers,256,L"Content-Type: application/json\r\nAuthorization: Bearer %hs\r\n",token);
    DWORD n=body?(DWORD)strlen(body):0;
    if(!WinHttpSendRequest(r,headers,(DWORD)-1,(void*)body,n,n,0)||!WinHttpReceiveResponse(r,NULL))goto done;
    DWORD status=0,size=sizeof status;if(!WinHttpQueryHeaders(r,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,NULL,&status,&size,NULL))goto done;
    unsigned used=0;ULONGLONG deadline=GetTickCount64()+5000;
    for(;;){DWORD got=0;if(used+1>=cap||GetTickCount64()>deadline)goto done;
        if(!WinHttpReadData(r,out+used,cap-used-1,&got))goto done;if(!got)break;used+=got;}
    out[used]=0;result=(int)status;
done:
    SecureZeroMemory(headers,sizeof headers); /* audit 08: a crash report holds the thread stacks */
    if(!result)out[0]=0;
    if(r)WinHttpCloseHandle(r);if(connection)WinHttpCloseHandle(connection);if(session)WinHttpCloseHandle(session);return result;
}
static int save_credential(const char *name){
    Credential c={0};memcpy(c.magic,"SGROUP2",8);snprintf(c.origin,sizeof c.origin,"%s",origin);snprintf(c.token,sizeof c.token,"%s",token);snprintf(c.network,sizeof c.network,"%s",network);snprintf(c.node,sizeof c.node,"%s",account_node);snprintf(c.name,sizeof c.name,"%s",name);
    c.count=cached_count;memcpy(c.peers,cached_peers,cached_count*sizeof *cached_peers);
    DATA_BLOB plain={sizeof c,(BYTE*)&c},encrypted={0};
    if(!CryptProtectData(&plain,L"Strikers group",NULL,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&encrypted))return 0;
    char tmp[MAX_PATH+8];snprintf(tmp,sizeof tmp,"%s.tmp",profile);
    HANDLE f=CreateFileA(tmp,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);DWORD wrote=0;int ok=0;
    if(f!=INVALID_HANDLE_VALUE){ok=WriteFile(f,encrypted.pbData,encrypted.cbData,&wrote,NULL)&&wrote==encrypted.cbData&&FlushFileBuffers(f);CloseHandle(f);}
    LocalFree(encrypted.pbData);SecureZeroMemory(&c,sizeof c);
    if(ok)ok=MoveFileExA(tmp,profile,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok)DeleteFileA(tmp);return ok;
}
static void load_credential(void){
    FILE *f=fopen(profile,"rb");if(!f)return;BYTE data[4096];size_t n=fread(data,1,sizeof data,f);int extra=fgetc(f);fclose(f);if(extra!=EOF||!n)return;
    DATA_BLOB encrypted={(DWORD)n,data},plain={0};if(!CryptUnprotectData(&encrypted,NULL,NULL,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&plain))return;
    /* v1 had only the six character arrays, with no padding or peer directory. */
    const size_t legacy_size=offsetof(Credential,name)+sizeof view.name;
    if(plain.cbData==legacy_size||plain.cbData==sizeof(Credential)){Credential *c=(Credential*)plain.pbData;
        int v2=plain.cbData==sizeof(Credential)&&!memcmp(c->magic,"SGROUP2",8);
        int valid=v2&&c->count<=128;
        if(valid)for(unsigned i=0;i<c->count;i++){if(!c->peers[i]||c->peers[i]>0xffffffffffULL)valid=0;for(unsigned j=0;j<i;j++)if(c->peers[j]==c->peers[i])valid=0;}
        if(((plain.cbData==legacy_size&&!memcmp(c->magic,"SGROUP1",8))||valid)&&c->origin[255]==0&&c->token[64]==0&&c->network[16]==0&&c->node[10]==0&&c->name[128]==0&&!strcmp(c->origin,origin)&&hexdigits(c->token,64)&&hexdigits(c->node,10)&&(!c->network[0]||hexdigits(c->network,16))){
            strcpy_s(token,sizeof token,c->token);strcpy_s(network,sizeof network,c->network);strcpy_s(account_node,sizeof account_node,c->node);strcpy_s(view.name,sizeof view.name,c->name);
            if(valid){cached_count=c->count;memcpy(cached_peers,c->peers,cached_count*sizeof *cached_peers);}
        }
    }SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);
}
static int quote(const char *in,char *out,unsigned cap){unsigned n=0;
    for(unsigned i=0;in[i];i++){unsigned char ch=(unsigned char)in[i];if(ch<32)return 0;if(ch=='"'||ch=='\\'){if(n+2>=cap)return 0;out[n++]='\\';}if(n+1>=cap)return 0;out[n++]=(char)ch;}out[n]=0;return 1;}
static void response_error(int code,const char *out){char error[220];GjToken t[128];int n=gj_parse(out,t,128);
    if(n>0&&gj_string(out,t,gj_field(out,t,0,"error"),error,sizeof error))message(error);
    else message(code?"Group service returned an unexpected response. Keeping saved friends.":"Group service is unreachable. Keeping saved friends for play.");
}
/* Audit 08: only the Worker's own answer revokes. Cloudflare's edge has 401/403 pages of its own, and a bare status from one of
 * those (or an HTML body) must keep the saved friends: clearing them would end a match in progress. */
static int revoked(int code,const char *out){char value[8];GjToken t[8];
    return code==401&&gj_parse(out,t,8)>0&&gj_string(out,t,gj_field(out,t,0,"state"),value,sizeof value)&&!strcmp(value,"gone");}
static int friends(const char *out){
    GjToken t[4096];int n=gj_parse(out,t,4096);if(n<1)return 0;
    int a=gj_field(out,t,0,"friends"),b=gj_field(out,t,0,"invites");
    if(a<0||b<0||t[a].kind!='['||t[b].kind!='[')return 0;
    GroupStatus next;AcquireSRWLockShared(&lock);next=view;ReleaseSRWLockShared(&lock);next.count=0;next.toast[0]=0;next.invite_id[0]=0;
    if(account_node[0]){char self[11];if(!gj_string(out,t,gj_field(out,t,0,"self"),self,sizeof self)||strcmp(self,account_node))return 0;}
    uint64_t peers[128];
    for(int i=a+1;i<t[a].end;i=t[i].end){if(next.count==128)return 0;GroupFriend *f=&next.friends[next.count++];
        if(!gj_string(out,t,gj_field(out,t,i,"node"),f->node,sizeof f->node)||!hexdigits(f->node,10)||!gj_string(out,t,gj_field(out,t,i,"name"),f->name,sizeof f->name)||!gj_string(out,t,gj_field(out,t,i,"status"),f->status,sizeof f->status))return 0;
        unsigned at=next.count-1;peers[at]=_strtoui64(f->node,NULL,16);if(!peers[at])return 0;
        for(unsigned j=0;j<at;j++)if(peers[j]==peers[at])return 0;
        if(strcmp(f->status,"online")&&strcmp(f->status,"menus")&&strcmp(f->status,"hosting")&&strcmp(f->status,"match")&&strcmp(f->status,"offline"))return 0;
    }
    if(b+1<t[b].end){char name[129];int i=b+1;if(!gj_string(out,t,gj_field(out,t,i,"name"),name,sizeof name)||!gj_string(out,t,gj_field(out,t,i,"id"),next.invite_id,sizeof next.invite_id))return 0;
        /* Audit 06: the id goes back into a JSON body (group_dismiss): only the characters of the service's ids, never a quote */
        if(!next.invite_id[0]||strspn(next.invite_id,"0123456789abcdefABCDEF-")!=strlen(next.invite_id))return 0;
        snprintf(next.toast,sizeof next.toast,"%s is hosting a room: open Quick Battle > Local Wireless",name);
    }
    int changed=cached_count!=next.count||memcmp(cached_peers,peers,next.count*sizeof *peers);
    if(changed)cache_dirty=1;
    cached_count=next.count;memcpy(cached_peers,peers,next.count*sizeof *peers);
    zt_set_group_peers(cached_peers,cached_count);
    int saved=!cache_dirty||!token[0]||!profile[0]||save_credential(next.name);
    if(saved)cache_dirty=0;
    AcquireSRWLockExclusive(&lock);int fresh=next.invite_id[0]&&strcmp(view.invite_id,next.invite_id);void (*notify)(void)=notify_ui;view.count=next.count;memcpy(view.friends,next.friends,sizeof next.friends);strcpy_s(view.toast,sizeof view.toast,next.toast);strcpy_s(view.invite_id,sizeof view.invite_id,next.invite_id);if(!view.reachable)strcpy_s(view.message,sizeof view.message,"Friends updated. Open Quick Battle > Local Wireless.");view.reachable=1;view.revoked=0;ReleaseSRWLockExclusive(&lock);
    if(!saved)message("Friends updated, but could not save them for next launch. Check userdata permissions.");
    if(fresh&&notify)notify();return 1;
}
static DWORD WINAPI worker(void *unused){
    (void)unused;char out[65536],body[1024],previous_status[16]={0};ULONGLONG next=0;int loaded=0;
    while(!InterlockedCompareExchange(&stopping,0,0)){
        ZtStatus z;zt_status(&z);
        if(!z.node||z.node>0xffffffffffULL){WaitForSingleObject(wake,250);continue;}
        char node[11];snprintf(node,sizeof node,"%010llx",z.node);
        if(!loaded){loaded=1;
            if(token[0]&&strcmp(account_node,node)){token[0]=network[0]=0;cached_count=0;message("This profile's player ID changed. Ask your organizer before joining again.");}
            snprintf(account_node,sizeof account_node,"%s",node);
            zt_set_group_peers(cached_peers,cached_count);
            if(network[0]&&zt_set_group_network(network)){AcquireSRWLockExclusive(&lock);view.joined=1;ReleaseSRWLockExclusive(&lock);}
        }
        char code[16],name[129],to[40];int todo;
        AcquireSRWLockExclusive(&lock);todo=action;action=0;strcpy_s(code,sizeof code,join_code);strcpy_s(name,sizeof name,join_name);strcpy_s(to,sizeof to,target);int joined=view.joined;snprintf(view.self,sizeof view.self,"%s",node);ReleaseSRWLockExclusive(&lock);
        if(todo==1){
            if(!token[0]){BYTE random[32];if(BCryptGenRandom(NULL,random,sizeof random,BCRYPT_USE_SYSTEM_PREFERRED_RNG)){message("Could not create a player credential.");goto complete;}for(int i=0;i<32;i++)sprintf_s(token+2*i,3,"%02x",random[i]);SecureZeroMemory(random,sizeof random);}
            // Persist BEFORE enrollment: lost response/restart can retry with the same credential.
            if(!save_credential(name)){message("Could not save your private group credential. Check userdata permissions.");goto complete;}
            char escaped[260];if(!quote(name,escaped,sizeof escaped)){message("Use a display name without control characters.");goto complete;}
            snprintf(body,sizeof body,"{\"node\":\"%s\",\"name\":\"%s\",\"code\":\"%s\",\"credential\":\"%s\"}",node,escaped,code,token);
            int rc=request(L"POST",L"/join",body,out,sizeof out);SecureZeroMemory(body,sizeof body);
            AcquireSRWLockExclusive(&lock);SecureZeroMemory(join_code,sizeof join_code);ReleaseSRWLockExclusive(&lock);SecureZeroMemory(code,sizeof code);
            GjToken t[64];char received[17];
            if(rc!=200){response_error(rc,out);goto complete;}
            if(gj_parse(out,t,64)<1||!gj_string(out,t,gj_field(out,t,0,"network"),received,sizeof received)||!hexdigits(received,16)||!zt_set_group_network(received)){message("The service returned an invalid or different network. Check strikers.ini with your organizer.");goto complete;}
            strcpy_s(network,sizeof network,received);if(!save_credential(name)){message("Joined, but could not save enrollment. Keep this game open and check userdata permissions.");goto complete;}
            AcquireSRWLockExclusive(&lock);view.joined=1;strcpy_s(view.name,sizeof view.name,name);ReleaseSRWLockExclusive(&lock);
            puts("[group] enrolled via configured service");fflush(stdout);message("Joined your group. Open Quick Battle > Local Wireless.");joined=1;next=0;
        }else if(todo==2||todo==3){
            if(todo==2)snprintf(body,sizeof body,"{\"to\":\"%s\"}",to);else snprintf(body,sizeof body,"{\"id\":\"%s\"}",to);
            int rc=request(L"POST",todo==2?L"/invite":L"/invite/dismiss",body,out,sizeof out);
            if(rc!=200)response_error(rc,out);else message(todo==2?"Invitation sent.":"Invitation dismissed.");next=0;
        }
complete:
        AcquireSRWLockExclusive(&lock);if(todo)view.busy=0;ReleaseSRWLockExclusive(&lock);
        const char *current=InterlockedCompareExchange(&match,0,0)?"match":InterlockedCompareExchange(&ldn,0,0)==3?"hosting":"menus";
        if(strcmp(current,previous_status))next=0;
        if(joined&&GetTickCount64()>=next){
            const char *status=current;snprintf(previous_status,sizeof previous_status,"%s",current);
            snprintf(body,sizeof body,"{\"status\":\"%s\"}",status);
            int rc=request(L"POST",L"/presence",body,out,sizeof out),gone=0;  /* the answer carries the directory (audit 08) */
            if(InterlockedCompareExchange(&stopping,0,0))break;      /* audit 06: the game is closing */
            if(rc!=200||!friends(out)){
                /* Transport failures and malformed responses retain the last
                 * good directory. Explicit credential revocation fails closed. */
                gone=revoked(rc,out);
                if(gone){cached_count=0;zt_set_group_peers(NULL,0);char saved_name[129];AcquireSRWLockShared(&lock);strcpy_s(saved_name,sizeof saved_name,view.name);ReleaseSRWLockShared(&lock);save_credential(saved_name);}
                AcquireSRWLockExclusive(&lock);view.reachable=0;if(gone){view.count=0;view.revoked=1;}view.toast[0]=0;view.invite_id[0]=0;ReleaseSRWLockExclusive(&lock);response_error(rc,out);
            }else {printf("[group] friends refreshed\n");fflush(stdout);}
            next=GetTickCount64()+(gone?60000:GROUP_POLL_MS);
        }
        WaitForSingleObject(wake,250);
    }SecureZeroMemory(token,sizeof token);return 0;
}
/* Audit 06: at exit the worker gets 5 s (it was 30 s: a black-holed service held a closing game for up to 30 s). A request in
 * flight then ends with the process; the credential file is written to a temporary name and renamed, so it is never torn. */
static void stop(void){InterlockedExchange(&stopping,1);SetEvent(wake);if(thread){if(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0)CloseHandle(thread);}}
static BOOL CALLBACK initialize(PINIT_ONCE o,PVOID a,PVOID *c){
    (void)o;(void)a;(void)c;const GfxOptions *opt=gfx_options();if(!opt->group_service[0])return TRUE;
    snprintf(origin,sizeof origin,"%s",opt->group_service);snprintf(view.code,sizeof view.code,"%s",opt->group_code);view.configured=1;
    if(!endpoint()){message("Group service needs a valid HTTPS address in strikers.ini.");return TRUE;}
    if(!zt_requested()){message("Set [Network] mode = zerotier in strikers.ini, then restart.");return TRUE;}
    const char *root=getenv("STRIKERS_USERDATA");char local[MAX_PATH];snprintf(local,sizeof local,"%s",root&&*root?root:"../userdata");
    if(!_fullpath(profile,local,sizeof profile)||!strncmp(profile,"\\\\",2)){message("Choose a local userdata folder for your group credential.");return TRUE;}
    for(char *p=profile+3;*p;p++)if(*p=='/'||*p=='\\'){char ch=*p;*p=0;CreateDirectoryA(profile,NULL);*p=ch;}CreateDirectoryA(profile,NULL);
    strncat_s(profile,sizeof profile,"/group-credential.dat",_TRUNCATE);load_credential();
    wake=CreateEventA(NULL,FALSE,FALSE,NULL);if(!wake){message("Could not start group service.");return TRUE;}
    message("Enter your invite code and display name, then Join.");
    thread=CreateThread(NULL,0,worker,NULL,0,NULL);if(!thread){CloseHandle(wake);wake=NULL;message("Could not start group service.");return TRUE;}
    atexit(stop);zt_start();return TRUE;
}
void group_start(void){InitOnceExecuteOnce(&once,initialize,NULL,NULL);}
void group_status(GroupStatus *s){AcquireSRWLockShared(&lock);*s=view;s->hosting=InterlockedCompareExchange(&ldn,0,0)==3;s->in_match=InterlockedCompareExchange(&match,0,0)!=0;ReleaseSRWLockShared(&lock);}
void group_connection(char *text,unsigned capacity){
    GroupStatus g;group_status(&g);ZtStatus z;zt_status(&z);
    if(g.revoked){snprintf(text,capacity,"Group access removed. Contact your organizer.");return;}
    if(z.state!=2){snprintf(text,capacity,"%s",z.message[0]?z.message:"Preparing your connection...");return;}
    if(!g.joined){snprintf(text,capacity,"Enter your invitation to connect to friends.");return;}
    if(!g.reachable){snprintf(text,capacity,"Connected. Group service unavailable; using saved friends.");return;}
    unsigned online=0;for(unsigned i=0;i<g.count;i++)if(strcmp(g.friends[i].node,g.self)&&strcmp(g.friends[i].status,"offline"))online++;
    snprintf(text,capacity,"Connected. %u friend%s online.",online,online==1?"":"s");
}
void group_join(const char *code,const char *name){
    if(strlen(code)!=9||code[4]!='-'||strlen(name)<1||strlen(name)>128)return;
    for(unsigned i=0;i<9;i++)if(i!=4&&!strchr("ABCDEFGHJKLMNPQRSTUVWXYZ23456789",code[i]))return;
    AcquireSRWLockExclusive(&lock);if(thread&&!view.busy){strcpy_s(join_code,sizeof join_code,code);strcpy_s(join_name,sizeof join_name,name);view.busy=1;action=1;SetEvent(wake);}ReleaseSRWLockExclusive(&lock);
}
void group_invite(const char *node){if(!hexdigits(node,10))return;AcquireSRWLockExclusive(&lock);if(thread&&view.joined&&!view.busy){strcpy_s(target,sizeof target,node);action=2;view.busy=1;SetEvent(wake);}ReleaseSRWLockExclusive(&lock);}
void group_dismiss(void){AcquireSRWLockExclusive(&lock);if(thread&&view.invite_id[0]&&!view.busy){strcpy_s(target,sizeof target,view.invite_id);action=3;view.busy=1;view.toast[0]=0;SetEvent(wake);}ReleaseSRWLockExclusive(&lock);}
void group_ldn_state(int state){InterlockedExchange(&ldn,state);if(state!=3&&state!=5)InterlockedExchange(&match,0);}
void group_match(int active){InterlockedExchange(&match,active!=0);if(wake)SetEvent(wake);}
void group_notify(void (*notify)(void)){AcquireSRWLockExclusive(&lock);notify_ui=notify;ReleaseSRWLockExclusive(&lock);}
