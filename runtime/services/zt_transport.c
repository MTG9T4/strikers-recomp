/* SPDX-License-Identifier: GPL-3.0-or-later
 * Embedded libzt only. No native guest socket, adapter or routing changes.
 * Vendored libzt's Apache-2.0 C API; see runtime/provenance.json. */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zt_transport.h"
#include "../../deps/libzt/include/ZeroTierSockets.h"

/* Resolve from an absolute path beside the executable, never PATH/CWD. Loading
 * is deferred until Join so offline mode never starts a node or sends traffic. */
#define API(ret, name, args) static ret (__cdecl *p_##name) args
API(int,zts_init_from_storage,(const char*));
API(int,zts_init_set_event_handler,(void (*)(void*)));
API(int,zts_init_allow_port_mapping,(unsigned));
API(int,zts_init_allow_net_cache,(unsigned));
API(int,zts_node_start,(void));
API(int,zts_node_stop,(void));
API(int,zts_node_is_online,(void));
API(uint64_t,zts_node_get_id,(void));
API(int,zts_net_join,(uint64_t));
API(int,zts_net_transport_is_ready,(uint64_t));
API(int,zts_net_get_status,(uint64_t));
API(int,zts_net_get_name,(uint64_t,char*,unsigned));
API(int,zts_addr_get_str,(uint64_t,unsigned,char*,unsigned));
API(int,zts_addr_compute_6plane_str,(uint64_t,uint64_t,char*,unsigned));
API(int,zts_socket,(int,int,int));
API(int,zts_bind,(int,const char*,unsigned short));
API(int,zts_bsd_close,(int));
API(int,zts_bsd_setsockopt,(int,int,int,const void*,zts_socklen_t));
API(int,zts_bsd_fcntl,(int,int,int));
API(ssize_t,zts_bsd_sendto,(int,const void*,size_t,int,const struct zts_sockaddr*,zts_socklen_t));
API(ssize_t,zts_bsd_recvfrom,(int,void*,size_t,int,struct zts_sockaddr*,zts_socklen_t*));
API(int,zts_util_ipstr_to_saddr,(const char*,unsigned short,struct zts_sockaddr*,zts_socklen_t*));
API(int,zts_bsd_poll,(struct zts_pollfd*,zts_nfds_t,int));   /* optional (audit 05): zt_wait */
API(int,zts_strikers_group_peers,(int,uint64_t,const uint64_t*,unsigned));
static SRWLOCK zlock=SRWLOCK_INIT;
static LONG64 group_network;
static int group_enabled;
static uint64_t group_peers[128];
static unsigned group_count;
void zt_set_group_peers(const uint64_t *nodes,unsigned count) {
    if(count>128||(count&&!nodes))return;
    AcquireSRWLockExclusive(&zlock);
    group_enabled=1;group_count=count;
    if(count)memcpy(group_peers,nodes,count*sizeof *nodes);
    if(p_zts_strikers_group_peers)p_zts_strikers_group_peers(1,(uint64_t)group_network,group_peers,group_count);
    ReleaseSRWLockExclusive(&zlock);
}
static ZtStatus status;
static LONG started, stopping;
static HANDLE starter;
/* Audit 05: the SLDN socket is not closed here. It belongs to the LDN worker (opened, used and closed under nnldn's lock);
 * this runs at exit() on whichever thread ends the game, while that worker may be inside zts_bsd_recvfrom on it (lwIP is
 * built without full-duplex sockets): closing it from here raced the worker. Stopping the node is enough at exit. */
static void stop_node(void) {
    InterlockedExchange(&stopping,1);
    if(starter){WaitForSingleObject(starter,3000);CloseHandle(starter);starter=NULL;}
    if(p_zts_node_stop){p_zts_node_stop();Sleep(500);}
}
/* fd, family and the endpoint table are used only under nnldn's SLDN lock (ldn_init/finalize, the worker, lan_send), except
 * zt_wait (the worker, between its locked passes; fd does not change while the worker runs: it is opened before the worker
 * starts and closed after Finalize joined it). */
static int fd=-1, family;
/* Endpoint tokens (audit 05): an entry not used (received from or sent to) for 60 s is reused for a new address when the
 * table is full. Before, 128 distinct sources over a session (players coming and going, or a member forging IPv4 sources,
 * which ZeroTier does not check by default) filled it for good, and no new player could be heard until a restart. SLDN keeps
 * a token only while its peer is heard (peers and scan results expire after 6 s), so a 60 s idle token is not in use. */
#define ZT_ENDPOINT_IDLE_MS 60000
static struct {struct zts_sockaddr_storage addr;uint64_t used;} endpoints[128];
static unsigned endpoint_count;

void zt_config(char *mode,unsigned mc,char *network,unsigned nc) {
    char path[MAX_PATH],exe[MAX_PATH];
    const char *p=getenv("STRIKERS_PC_CONFIG");
    if(!p||!*p)p=getenv("STRIKERS_GRAPHICS_CONFIG");
    if(p&&*p)snprintf(path,sizeof path,"%s",p);
    else if((p=getenv("STRIKERS_CONFIG"))&&*p) {
        snprintf(path,sizeof path,"%s",p);char *a=strrchr(path,'/'),*b=strrchr(path,'\\');if(b&&(!a||b>a))a=b;
        if(a)snprintf(a+1,sizeof path-(size_t)(a+1-path),"strikers.ini");else snprintf(path,sizeof path,"strikers.ini");
    } else if((p=getenv("STRIKERS_USERDATA"))&&*p)snprintf(path,sizeof path,"%s/strikers.ini",p);
    else {GetModuleFileNameA(NULL,exe,sizeof exe);char *s=strrchr(exe,'\\');if(s)*s=0;snprintf(path,sizeof path,"%s/../config/strikers.ini",exe);}
    char absolute[MAX_PATH];if(_fullpath(absolute,path,sizeof absolute))snprintf(path,sizeof path,"%s",absolute);
    p=getenv("STRIKERS_NETWORK");if(p)snprintf(mode,mc,"%s",p);else GetPrivateProfileStringA("Network","mode","offline",mode,mc,path);
    p=getenv("STRIKERS_ZEROTIER_NETWORK");if(p)snprintf(network,nc,"%s",p);else GetPrivateProfileStringA("Network","zerotier_network","",network,nc,path);
}
static INIT_ONCE config_once=INIT_ONCE_STATIC_INIT;
static int requested;
static int service_configured(void) {
    /* Same canonical path resolution as the existing INI. Only detect here;
     * group_client performs the strict origin/HTTPS checks before any HTTP. */
    const char *p=getenv("STRIKERS_PC_CONFIG");
    if(!p||!*p)p=getenv("STRIKERS_GRAPHICS_CONFIG");
    char path[MAX_PATH],value[256];
    if(p&&*p)snprintf(path,sizeof path,"%s",p);
    else if((p=getenv("STRIKERS_CONFIG"))&&*p){snprintf(path,sizeof path,"%s",p);char *a=strrchr(path,'/'),*b=strrchr(path,'\\');if(b&&(!a||b>a))a=b;if(a)strcpy_s(a+1,sizeof path-(a+1-path),"strikers.ini");else strcpy_s(path,sizeof path,"strikers.ini");}
    else if((p=getenv("STRIKERS_USERDATA"))&&*p)snprintf(path,sizeof path,"%s/strikers.ini",p);
    else{GetModuleFileNameA(NULL,path,sizeof path);char *s=strrchr(path,'\\');if(s)*s=0;strncat_s(path,sizeof path,"/../config/strikers.ini",_TRUNCATE);}
    char absolute[MAX_PATH];if(_fullpath(absolute,path,sizeof absolute))snprintf(path,sizeof path,"%s",absolute);
    GetPrivateProfileStringA("Network","group_service","",value,sizeof value,path);
    return value[0]!=0;
}
int zt_set_group_network(const char *n) {
    if(strlen(n)!=16||strspn(n,"0123456789abcdefABCDEF")!=16||!_strtoui64(n,NULL,16))return 0;
    char mode[64],configured[64];zt_config(mode,sizeof mode,configured,sizeof configured);
    if(strlen(configured)==16&&strspn(configured,"0123456789abcdefABCDEF")==16&&_strtoui64(configured,NULL,16)!=_strtoui64(n,NULL,16))return 0;
    ZtStatus s;zt_status(&s);uint64_t v=_strtoui64(n,NULL,16);
    if(s.network&&s.network!=v)return 0;
    AcquireSRWLockExclusive(&zlock);
    InterlockedExchange64(&group_network,(LONG64)v);
    if(p_zts_strikers_group_peers)p_zts_strikers_group_peers(group_enabled,v,group_peers,group_count);
    ReleaseSRWLockExclusive(&zlock);return 1;
}
static BOOL CALLBACK read_mode(PINIT_ONCE o,PVOID a,PVOID *c) {
    (void)o;(void)a;(void)c;char m[64],n[64];zt_config(m,sizeof m,n,sizeof n);requested=!_stricmp(m,"zerotier");return TRUE;
}
int zt_requested(void) {InitOnceExecuteOnce(&config_once,read_mode,NULL,NULL);return requested;}
static void set_state(int state,const char *msg) {
    AcquireSRWLockExclusive(&zlock);
    if(status.state!=state||strcmp(status.message,msg))printf("[zerotier] state=%d %s\n",state,msg);
    status.state=state;snprintf(status.message,sizeof status.message,"%s",msg);
    ReleaseSRWLockExclusive(&zlock);fflush(stdout);
}
void zt_status(ZtStatus *out) {AcquireSRWLockShared(&zlock);*out=status;ReleaseSRWLockShared(&zlock);}
int zt_ready(void) {AcquireSRWLockShared(&zlock);int ready=status.state==2;ReleaseSRWLockShared(&zlock);return ready;}   /* per datagram: not a copy of the whole status (audit 05) */
static void event(void *ptr) {
    const zts_event_msg_t *m=ptr;
    if(!m->peer)return;
    AcquireSRWLockExclusive(&zlock);
    unsigned i;for(i=0;i<status.peers;i++)if(status.peer[i].id==m->peer->peer_id)break;
    if(m->peer->role!=ZTS_PEER_ROLE_LEAF) {
        if(i<status.peers){memmove(status.peer+i,status.peer+i+1,(status.peers-i-1)*sizeof status.peer[0]);status.peers--;}
        ReleaseSRWLockExclusive(&zlock);return;
    }
    if(i<64) {
        if(i==status.peers) {status.peers++;status.peer[i].path=-1;}
        int previous=status.peer[i].path;
        status.peer[i].id=m->peer->peer_id;status.peer[i].latency=m->peer->latency;
        if(m->event_code==ZTS_EVENT_PEER_DIRECT)status.peer[i].path=1;
        if(m->event_code==ZTS_EVENT_PEER_RELAY)status.peer[i].path=0;
        if(previous!=status.peer[i].path)printf("[zerotier] peer=%010llx path=%s latency=%d ms\n",m->peer->peer_id,status.peer[i].path?"direct":"relayed",m->peer->latency);
    }
    ReleaseSRWLockExclusive(&zlock);
}
static DWORD WINAPI start_worker(void *unused) {
    (void)unused;char mode[64],n[64],path[MAX_PATH],root[MAX_PATH];zt_config(mode,sizeof mode,n,sizeof n);
    int valid=strlen(n)==16&&strspn(n,"0123456789abcdefABCDEF")==16&&_strtoui64(n,NULL,16);
    if(!valid&&!service_configured()) {set_state(4,"Enter the group's 16-character network ID in strikers.ini, then restart.");return 0;}
    uint64_t network=valid?_strtoui64(n,NULL,16):0;
    GetModuleFileNameA(NULL,path,sizeof path);char *slash=strrchr(path,'\\');if(slash)*slash=0;
    strncat_s(path,sizeof path,"\\strikers_zt.dll",_TRUNCATE);
    HMODULE dll=LoadLibraryExA(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!dll){printf("[zerotier] LoadLibrary error=%lu path=%s\n",GetLastError(),path);set_state(4,"The game's strikers_zt.dll could not load. Restore it from your game package.");return 0;}
#define LOAD(name) if(!(p_##name=(void*)GetProcAddress(dll,#name))){set_state(4,"The bundled ZeroTier library is incompatible.");return 0;}
    LOAD(zts_init_from_storage);LOAD(zts_init_set_event_handler);LOAD(zts_init_allow_port_mapping);LOAD(zts_init_allow_net_cache);
    LOAD(zts_node_start);LOAD(zts_node_stop);LOAD(zts_node_is_online);LOAD(zts_node_get_id);LOAD(zts_net_join);LOAD(zts_net_transport_is_ready);
    LOAD(zts_net_get_status);LOAD(zts_net_get_name);LOAD(zts_addr_get_str);LOAD(zts_addr_compute_6plane_str);
    LOAD(zts_socket);LOAD(zts_bind);LOAD(zts_bsd_close);LOAD(zts_bsd_setsockopt);LOAD(zts_bsd_fcntl);
    LOAD(zts_bsd_sendto);LOAD(zts_bsd_recvfrom);LOAD(zts_util_ipstr_to_saddr);
#undef LOAD
    AcquireSRWLockExclusive(&zlock);
    p_zts_strikers_group_peers=(void*)GetProcAddress(dll,"zts_strikers_group_peers");
    group_enabled=group_enabled||service_configured();
    if(group_enabled&&!p_zts_strikers_group_peers){ReleaseSRWLockExclusive(&zlock);set_state(4,"Restore the current strikers_zt.dll to connect safely to your group.");return 0;}
    if(p_zts_strikers_group_peers)p_zts_strikers_group_peers(group_enabled,(uint64_t)group_network,group_peers,group_count);
    ReleaseSRWLockExclusive(&zlock);
    p_zts_bsd_poll=(void*)GetProcAddress(dll,"zts_bsd_poll");
    const char *user=getenv("STRIKERS_USERDATA");snprintf(root,sizeof root,"%s",user&&*user?user:"../userdata");
    if(!_fullpath(path,root,sizeof path)||!strncmp(path,"\\\\",2)){set_state(4,"Choose a local userdata folder for this player's identity.");return 0;}
    char drive[4]={path[0],':','\\',0};
    if(path[1]==':'&&GetDriveTypeA(drive)==DRIVE_REMOTE){set_state(4,"Choose a local userdata folder for this player's identity.");return 0;}
    for(char *p=path+3;*p;p++)if(*p=='/'||*p=='\\'){char ch=*p;*p=0;CreateDirectoryA(path,NULL);*p=ch;}
    CreateDirectoryA(path,NULL);strncat_s(path,sizeof path,"/zerotier",_TRUNCATE);CreateDirectoryA(path,NULL);
    /* A process owns its identity exclusively. Never share keys between players. */
    char lockpath[MAX_PATH];snprintf(lockpath,sizeof lockpath,"%s/instance.lock",path);
    HANDLE identity=CreateFileA(lockpath,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(identity==INVALID_HANDLE_VALUE){set_state(4,"This player profile is already open. Close the other copy or use a separate userdata folder.");return 0;}
    if(p_zts_init_from_storage(path)||p_zts_init_set_event_handler(event)||p_zts_init_allow_port_mapping(0)||p_zts_init_allow_net_cache(0)||p_zts_node_start()) {set_state(4,"ZeroTier could not start. Restart the game and try again.");return 0;}
    AcquireSRWLockExclusive(&zlock);status.network=network;ReleaseSRWLockExclusive(&zlock);
    int joined=0;
    while(!InterlockedCompareExchange(&stopping,0,0)) {
        uint64_t node=p_zts_node_get_id();
        AcquireSRWLockExclusive(&zlock);status.node=node&&node<=0xffffffffffULL?node:0;ReleaseSRWLockExclusive(&zlock);
        if(!network) {
            network=(uint64_t)InterlockedCompareExchange64(&group_network,0,0);
            if(!network){set_state(1,"Ready to join your group. Open F1 > Online and enter your invitation.");Sleep(250);continue;}
            AcquireSRWLockExclusive(&zlock);status.network=network;ReleaseSRWLockExclusive(&zlock);
        }
        /* Audit 05: "online" is the node having heard from a root in the last 30 s (ZT_SDK). Once connected, losing the roots
         * (a root outage, an ISP dropping their traffic) must not cut a match whose peers are reachable on direct or already
         * established paths: the state stays connected while the network's transport is ready, and only says so. */
        if(!p_zts_node_is_online()){
            ZtStatus now;zt_status(&now);
            if(joined&&now.state==2&&p_zts_net_transport_is_ready(network)){set_state(2,"Connected. ZeroTier's root servers are not answering; players already reached still are.");Sleep(250);continue;}
            set_state(1,"Joining. Waiting for a connection to ZeroTier...");Sleep(250);continue;
        }
        if(!joined){if(p_zts_net_join(network)){set_state(4,"Could not join this network. Check its ID and restart.");return 0;}joined=1;}
        node=p_zts_node_get_id();
        AcquireSRWLockExclusive(&zlock);status.node=node&&node<=0xffffffffffULL?node:0;ReleaseSRWLockExclusive(&zlock);
        if(p_zts_net_get_status(network)==ZTS_NETWORK_STATUS_ACCESS_DENIED)set_state(3,"Waiting for authorization. Ask your group organizer to authorize this player ID.");
        else if(p_zts_net_get_status(network)==ZTS_NETWORK_STATUS_NOT_FOUND)set_state(4,"Network not found. Check the network ID with your group organizer.");
        else if(p_zts_net_transport_is_ready(network)) {
            char addr[ZTS_IP_MAX_STR_LEN]={0},name[128]={0};
            p_zts_net_get_name(network,name,sizeof name);
            int fam=(network>>56)==255?ZTS_AF_INET6:ZTS_AF_INET;
            if(p_zts_addr_get_str(network,fam,addr,sizeof addr)) {set_state(1,"Waiting for a virtual address. Ask the organizer to enable IPv4 auto-assignment.");Sleep(250);continue;}
            AcquireSRWLockExclusive(&zlock);snprintf(status.address,sizeof status.address,"%s",addr);snprintf(status.name,sizeof status.name,"%s",name[0]?name:"Ad-hoc network");ReleaseSRWLockExclusive(&zlock);
            set_state(2,"Connected. Open Quick Battle > Local Wireless to host or join a room.");
        } else set_state(1,"Joining the group's network...");
        Sleep(250);
    }
    CloseHandle(identity);return 0;
}
void zt_start(void) {
    if(!zt_requested()||InterlockedCompareExchange(&started,1,0))return;
    set_state(1,"Joining ZeroTier...");HANDLE h=CreateThread(NULL,0,start_worker,NULL,0,NULL);
    if(h){starter=h;atexit(stop_node);}else {InterlockedExchange(&started,0);set_state(4,"Could not start the connection worker. Try Join again.");}
}
int zt_open(uint16_t base,uint16_t *bound) {
    if(!zt_ready())return 0;
    ZtStatus s;zt_status(&s);family=(s.network>>56)==255?ZTS_AF_INET6:ZTS_AF_INET;
    fd=p_zts_socket(family,ZTS_SOCK_DGRAM,0);if(fd<0)return 0;
    for(int i=0;i<16;i++)if(!p_zts_bind(fd,s.address,(unsigned short)(base+i))) {
        int yes=1;p_zts_bsd_setsockopt(fd,ZTS_SOL_SOCKET,ZTS_SO_BROADCAST,&yes,sizeof yes);
        if(p_zts_bsd_fcntl(fd,ZTS_F_SETFL,ZTS_O_NONBLOCK)){zt_close();return 0;}
        *bound=base+i;printf("[zerotier] SLDN bound=%s:%u node=%010llx network=%016llx\n",s.address,*bound,s.node,s.network);return 1;
    }
    zt_close();return 0;
}
void zt_close(void) {if(fd>=0){p_zts_bsd_close(fd);fd=-1;}}
/* Audit 05: the LDN worker waits here for a datagram (at most ms) instead of polling every 2 ms: 500 locked passes and 500
 * wake-ups of every guest thread blocked in a socket Recv/Poll a second, whether or not anything arrived. */
void zt_wait(unsigned ms) {
    if(fd<0||!p_zts_bsd_poll||!zt_ready()){Sleep(ms<2?ms:2);return;}
    struct zts_pollfd p;p.fd=fd;p.events=ZTS_POLLIN;p.revents=0;
    if(p_zts_bsd_poll(&p,1,(int)ms)<0)Sleep(ms<2?ms:2);
}
static uint32_t endpoint(const struct zts_sockaddr_storage *a) {
    /* the address part only (16 bytes for IPv4, 28 for IPv6; the rest of the zeroed storage is never written) */
    size_t n=a->ss_family==ZTS_AF_INET?sizeof(struct zts_sockaddr_in):sizeof(struct zts_sockaddr_in6);
    uint64_t now=GetTickCount64();
    for(unsigned i=0;i<endpoint_count;i++)if(!memcmp(&endpoints[i].addr,a,n)){endpoints[i].used=now;return 0x64400001+i;}
    unsigned slot=endpoint_count;
    if(slot==128) {
        slot=0;for(unsigned i=1;i<128;i++)if(endpoints[i].used<endpoints[slot].used)slot=i;
        if(now-endpoints[slot].used<ZT_ENDPOINT_IDLE_MS)return 0;
    } else endpoint_count++;
    endpoints[slot].addr=*a;endpoints[slot].used=now;return 0x64400001+slot;
}
int zt_endpoint(uint32_t token) {return token>=0x64400001&&token<0x64400001+endpoint_count;}
/* The SDK authenticated the sender and bound ad-hoc UDP to its 6plane address.
 * Recheck here so packets queued before a removal cannot enter SLDN afterward. */
static int allowed(const struct zts_sockaddr_storage *a) {
    int ok=1;AcquireSRWLockShared(&zlock);
    if(group_enabled) {
        ok=0;
        if((status.network>>56)!=255)ok=group_count!=0;
        else if(a->ss_family==ZTS_AF_INET6) {
            const unsigned char *ip=(const unsigned char*)&((const struct zts_sockaddr_in6*)a)->sin6_addr;
            uint64_t node=0;for(unsigned i=5;i<10;i++)node=(node<<8)|ip[i];
            uint32_t prefix=(uint32_t)(status.network^(status.network>>32));
            unsigned char expected[16]={0xfc,(unsigned char)(prefix>>24),(unsigned char)(prefix>>16),(unsigned char)(prefix>>8),(unsigned char)prefix};
            memcpy(expected+5,ip+5,5);expected[15]=1;
            if(!memcmp(ip,expected,16))for(unsigned i=0;i<group_count;i++)if(group_peers[i]==node){ok=1;break;}
        }
    }
    ReleaseSRWLockShared(&zlock);return ok;
}
int zt_send(uint32_t token,uint16_t port,const void *data,unsigned size) {
    if(fd<0||!zt_ready()||!zt_endpoint(token))return -1;
    if(!allowed(&endpoints[token-0x64400001].addr))return -1;
    endpoints[token-0x64400001].used=GetTickCount64();
    struct zts_sockaddr_storage a=endpoints[token-0x64400001].addr;
    ((struct zts_sockaddr_in*)&a)->sin_port=htons(port);
    return (int)p_zts_bsd_sendto(fd,data,size,0,(struct zts_sockaddr*)&a,a.ss_family==ZTS_AF_INET?sizeof(struct zts_sockaddr_in):sizeof(struct zts_sockaddr_in6));
}
int zt_recv(uint32_t *token,uint16_t *port,void *data,unsigned size) {
    if(fd<0||!zt_ready())return -1;
    /* Audit 08: -1 ends the SLDN worker's pass, so a datagram queued by a just-removed peer is skipped here, not reported. */
    for(int skipped=0;skipped<32;skipped++) {
        struct zts_sockaddr_storage a={0};zts_socklen_t len=sizeof a;
        int n=(int)p_zts_bsd_recvfrom(fd,data,size,ZTS_MSG_DONTWAIT,(struct zts_sockaddr*)&a,&len);
        if(n<0)return -1;
        if(a.ss_family!=family||len!=(family==ZTS_AF_INET?sizeof(struct zts_sockaddr_in):sizeof(struct zts_sockaddr_in6)))return -1;
        if(!allowed(&a))continue;
        *port=ntohs(((struct zts_sockaddr_in*)&a)->sin_port);((struct zts_sockaddr_in*)&a)->sin_port=0;
        *token=endpoint(&a);return *token?n:-1;
    }
    return -1;
}
void zt_discover(uint16_t base,const void *data,unsigned size) {
    if(fd<0||!zt_ready())return;
    ZtStatus s;zt_status(&s);
    if(family==ZTS_AF_INET) {
        struct zts_sockaddr_in a={0};a.sin_len=sizeof a;a.sin_family=ZTS_AF_INET;a.sin_addr.S_addr=0xffffffff;
        for(int j=0;j<16;j++){a.sin_port=htons(base+j);p_zts_bsd_sendto(fd,data,size,0,(struct zts_sockaddr*)&a,sizeof a);}
    } else {
        /* Ad-hoc is IPv6 unicast-only. Groups use the service's complete list;
         * manual mode retains its observed-leaf/test-hint bootstrap. */
        uint64_t ids[128];unsigned count=0;
        AcquireSRWLockShared(&zlock);int grouped=group_enabled;
        if(grouped){count=group_count;memcpy(ids,group_peers,count*sizeof *ids);}
        ReleaseSRWLockShared(&zlock);
        if(!grouped)for(unsigned i=0;i<s.peers;i++)ids[count++]=s.peer[i].id;
        const char *hint=getenv("STRIKERS_ZT_PEERS");char hints[1024];snprintf(hints,sizeof hints,"%s",hint?hint:"");
        char *next=NULL;for(char *h=strtok_s(hints,",",&next);!grouped&&h&&count<96;h=strtok_s(NULL,",",&next))if(strlen(h)==10&&strspn(h,"0123456789abcdefABCDEF")==10)ids[count++]=_strtoui64(h,NULL,16);
        /* Audit 08: a group member is a node of its own, with its own lwIP, so its one socket is on the base port. The other 15
         * ports only multiplied a host's 1.2 KB advert, sent every second for the whole match, to every member, offline ones too. */
        int ports=grouped?1:16;
        for(unsigned i=0;i<count;i++)if(ids[i]&&ids[i]!=s.node) {
            char ip[ZTS_IP_MAX_STR_LEN];if(p_zts_addr_compute_6plane_str(s.network,ids[i],ip,sizeof ip))continue;
            struct zts_sockaddr_storage a={0};zts_socklen_t len=sizeof a;
            if(p_zts_util_ipstr_to_saddr(ip,0,(struct zts_sockaddr*)&a,&len))continue;
            uint32_t token=endpoint(&a);for(int j=0;j<ports;j++)zt_send(token,base+j,data,size);
        }
    }
}
