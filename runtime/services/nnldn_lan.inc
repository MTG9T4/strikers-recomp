/* SPDX-License-Identifier: GPL-3.0-or-later
 * See ldn_lan.h for attribution. Included by nnldn.c, not a separate TU.
 * All L fields (except the immutable configuration) are protected by lock.
 * Never invoke socket callbacks with this lock held: send takes S then L. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <stdlib.h>
#include <string.h>
#include "network_policy.h"
#include "ldn_lan.h"
#include "zt_transport.h"
#include "net_sim.h"
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR,12)
#endif
extern int rand_s(unsigned int *);
extern void os_init_event(uint64_t, int);
extern void os_signal_event(uint64_t);
extern void os_pin_host_thread(void);
typedef struct { struct sockaddr_in addr; uint64_t id, heard; } LanPeer;
typedef struct { uint8_t info[LDN_NI_SIZE]; LanPeer peer; uint64_t build, at; int private_room; } LanSeen;
static struct {
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    SOCKET fd;
    HANDLE worker;
    int stop, state, reason, node, answer, pending, private_room, policy, address_count, wsa;
    uint64_t event, id, build, last_tick, last_advert, last_scan_reply;
    uint32_t interface_ip, interface_mask, broadcast;
    uint16_t base_port, bound_port;
    uint8_t net[LDN_NI_SIZE], security[0x44], addresses[8*12], advertise[384];
    uint16_t advertise_size;
    uint32_t connect_result;
    LanPeer peers[8], joining;
    uint8_t join_session[16];
    LanSeen seen[32];
    unsigned long long received, malformed, dropped, data_tx, data_rx;
} L = { .lock=SRWLOCK_INIT, .cv=CONDITION_VARIABLE_INIT, .fd=INVALID_SOCKET, .node=-1 };
static uint16_t lu16(const void *p) { uint16_t x; memcpy(&x,p,2); return x; }
static uint32_t lu32(const void *p) { uint32_t x; memcpy(&x,p,4); return x; }
static uint64_t lu64(const void *p) { uint64_t x; memcpy(&x,p,8); return x; }
static void lp16(void *p,uint16_t x) { memcpy(p,&x,2); }
static void lp32(void *p,uint32_t x) { memcpy(p,&x,4); }
static void lp64(void *p,uint64_t x) { memcpy(p,&x,8); }
static uint64_t lan_random(void) {
    unsigned int a=0,b=0;
    if (rand_s(&a) || rand_s(&b)) return 0;
    return ((uint64_t)a<<32)|b;
}
static void lan_mac(uint8_t *m,uint64_t id) { m[0]=2; for(int i=1;i<6;i++) m[i]=(uint8_t)(id>>(8*(i-1))); }
static int lan_socket_errno(void) {
    switch(WSAGetLastError()) {
    case WSAEWOULDBLOCK:return 11;case WSAEMSGSIZE:return 90;case WSAEACCES:return 13;
    case WSAENOBUFS:return 105;case WSAENETDOWN:return 100;case WSAENETUNREACH:return 101;
    case WSAEHOSTUNREACH:return 113;case WSAEINTR:return 4;case WSAEINVAL:return 22;
    default:return 5;
    }
}
static int lan_active(void) { return L.state==LDN_CREATED || L.state==LDN_CONNECTED; }
static void lan_signal(void) { if(L.event) os_signal_event(L.event); WakeAllConditionVariable(&L.cv); }
static void lan_state(int s) {
    if(L.state!=s) printf("[lan] state %d -> %d reason=%d\n",L.state,s,L.reason);
    L.state=s; lan_signal();
}
static int lan_local_source(uint32_t ip) {
    if(zt_requested())return zt_endpoint(ip);
    if((ip>>24)==127) return 1;
    return (L.interface_ip>>24)!=127 && ip!=L.broadcast && ip!=0 &&
        (ip&L.interface_mask)==(L.interface_ip&L.interface_mask);
}
static int lan_same_peer(const LanPeer *p,const struct sockaddr_in *a,uint64_t id) {
    return p->id==id && p->addr.sin_addr.s_addr==a->sin_addr.s_addr && p->addr.sin_port==a->sin_port;
}
/* Header uses little endian integers; real endpoint fields are never supplied by
 * an untrusted payload. The peer endpoint always comes from recvfrom. */
static int lan_raw_send(uint32_t ip,uint16_t port,const void *data,unsigned size) {
    if(zt_requested()) {
        if(ip==0xffffffffu){zt_discover(L.base_port,data,size);return (int)size;}
        return zt_send(ip,port,data,size);
    }
    struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(ip);a.sin_port=htons(port);
    return sendto(L.fd,data,size,0,(const struct sockaddr*)&a,sizeof a);
}
static int lan_send(const struct sockaddr_in *to,int type,const void *body,unsigned size,
                    uint32_t src,uint32_t dst,uint16_t sport,uint16_t dport) {
    uint8_t b[LDN_WIRE_HEADER+LDN_MAX_DATA];
    if(size>LDN_MAX_DATA || L.fd==INVALID_SOCKET || !lan_local_source(ntohl(to->sin_addr.s_addr))) {
        /* Directed broadcast is allowed only by the discovery path below. */
        if(size>LDN_MAX_DATA || L.fd==INVALID_SOCKET || ntohl(to->sin_addr.s_addr)!=L.broadcast) return -1;
    }
    memset(b,0,LDN_WIRE_HEADER); memcpy(b,"SLDN",4);
    lp16(b+4,LDN_WIRE_VERSION); lp16(b+6,(uint16_t)type); lp32(b+8,size);
    lp64(b+12,L.build); lp64(b+20,L.id); memcpy(b+28,L.net+0x10,16);
    lp32(b+44,src); lp32(b+48,dst); lp16(b+52,sport); lp16(b+54,dport);
    if(size) memcpy(b+LDN_WIRE_HEADER,body,size);
    return net_sim_send(GetTickCount64(),ntohl(to->sin_addr.s_addr),ntohs(to->sin_port),b,LDN_WIRE_HEADER+size,lan_raw_send);
}
static void lan_discover(int type) {
    struct sockaddr_in a={0}; a.sin_family=AF_INET;
    uint8_t body[LDN_NI_SIZE+1];unsigned size=0;
    if(type==LP_ADVERT){memcpy(body,L.net,LDN_NI_SIZE);body[LDN_NI_SIZE]=(uint8_t)L.private_room;size=sizeof body;}
    if(zt_requested()){a.sin_addr.s_addr=htonl(L.broadcast);a.sin_port=htons(L.base_port);lan_send(&a,type,body,size,0,0,0,0);return;}
    /* Unicast loopback fanout is deterministic on Windows, unlike binding
     * several processes to one SO_REUSEADDR unicast discovery socket. */
    for(int i=0;i<LDN_PORT_SLOTS;i++) {
        a.sin_port=htons((uint16_t)(L.base_port+i)); a.sin_addr.s_addr=htonl(0x7f000001);
        lan_send(&a,type,body,size,0,0,0,0);
        if((L.interface_ip>>24)!=127) { a.sin_addr.s_addr=htonl(L.broadcast); lan_send(&a,type,body,size,0,0,0,0); }
    }
}
static void lan_advert(const struct sockaddr_in *to,int sync) {
    uint8_t body[LDN_NI_SIZE+1]; memcpy(body,L.net,LDN_NI_SIZE); body[LDN_NI_SIZE]=(uint8_t)L.private_room;
    lan_send(to,sync?LP_SYNC:LP_ADVERT,body,sizeof body,0,0,0,0);
}
static void lan_sync(void) {
    for(int i=1;i<8;i++) if(L.peers[i].id) lan_advert(&L.peers[i].addr,1);
}
static void lan_remove(int i) {
    printf("[lan] node-left id=%d identity=%016llx\n",i,(unsigned long long)L.peers[i].id);
    memset(&L.peers[i],0,sizeof L.peers[i]); memset(L.net+LDN_NODE(i),0,0x40);
    L.net[0x67]--; lan_sync();lan_signal();
}
static void lan_leave(int reason) {
    L.reason=reason; L.node=-1; L.pending=0; L.answer=-1; lan_state(LDN_STA);
}
static void lan_shutdown_room(void) {
    if(L.state==LDN_CREATED) {
        for(int i=1;i<8;i++) if(L.peers[i].id) lan_send(&L.peers[i].addr,LP_DESTROY,NULL,0,0,0,0,0);
        memset(L.peers,0,sizeof L.peers); L.node=-1; lan_state(LDN_AP);
    } else if(L.state==LDN_CONNECTED) {
        lan_send(&L.peers[0].addr,LP_LEAVE,NULL,0,0,0,0,0); lan_leave(1);
    }
}
static int lan_valid_info(const uint8_t *n) {
    if(n[0x26]>32 || n[0x27+n[0x26]] || n[0x66]<1 || n[0x66]>8 || n[0x67]<1 || n[0x67]>n[0x66] ||
       n[0x4b]!=2 || lu16(n+0x26a)>384 || !n[LDN_NODE(0)+11]) return 0;
    uint32_t base=lu32(n+LDN_NODE(0))&0xffffff00u; int count=0;
    if((base&0xffff0000u)!=0xa9fe0000u) return 0;
    for(int i=0;i<8;i++) if(n[LDN_NODE(i)+11]) {
        if(i>=n[0x66] || n[LDN_NODE(i)+10]!=i || lu32(n+LDN_NODE(i))!=(base|(i+1)) || !memchr(n+LDN_NODE(i)+12,0,33)) return 0;
        count++;
    }
    return count==n[0x67];
}
static int lan_accepts(const uint8_t *mac) {
    if(L.policy==0) return 1; if(L.policy==1) return 0;
    int listed=0;
    for(int i=0;i<L.address_count;i++) if(!memcmp(L.addresses+12*i+4,mac,6)) listed=1;
    return L.policy==2 ? !listed : listed;
}
static void lan_receive(const uint8_t *b,unsigned n,const struct sockaddr_in *from) {
    if(n<LDN_WIRE_HEADER || memcmp(b,"SLDN",4) || lu16(b+4)!=LDN_WIRE_VERSION ||
       lu32(b+8)!=n-LDN_WIRE_HEADER || !lan_local_source(ntohl(from->sin_addr.s_addr))) { L.malformed++; return; }
    uint64_t id=lu64(b+20),build=lu64(b+12); int type=lu16(b+6);
    if(!id || id==L.id) return;
    unsigned size=n-LDN_WIRE_HEADER; const uint8_t *p=b+LDN_WIRE_HEADER;
    int same_session=!memcmp(b+28,L.net+0x10,16), member=-1;
    for(int i=0;i<8;i++) if(lan_same_peer(&L.peers[i],from,id)) {member=i; break;}
    uint64_t now=GetTickCount64();
    if(type==LP_SCAN && !size && L.state==LDN_CREATED) {
        /* Aggregate rate limit also bounds reflection during a discovery flood. */
        if(now-L.last_scan_reply>=25) { lan_advert(from,0); L.last_scan_reply=now; }
    } else if(type==LP_ADVERT && size==LDN_NI_SIZE+1 && lan_valid_info(p) && !memcmp(b+28,p+0x10,16)) {
        int slot=0;
        for(int i=0;i<32;i++) {
            if(L.seen[i].peer.id==id && !memcmp(L.seen[i].info+0x10,p+0x10,16)) {slot=i;break;}
            if(L.seen[i].at<L.seen[slot].at) slot=i;
        }
        memcpy(L.seen[slot].info,p,LDN_NI_SIZE); L.seen[slot].peer.addr=*from;
        L.seen[slot].peer.id=id; L.seen[slot].at=now; L.seen[slot].build=build;
        L.seen[slot].private_room=p[LDN_NI_SIZE]!=0;
    } else if(type==LP_JOIN && size==0x78 && L.state==LDN_CREATED && same_session) {
        /* security[68], username[33], pad, version[2], MAC[6], pad[10]. */
        int why=0; uint16_t phrase=lu16(p+2);uint8_t expected_mac[6];lan_mac(expected_mac,id);
        if(memcmp(p+0x68,expected_mac,6)) {L.dropped++;return;}
        if(build!=L.build) why=3;
        else if(lu16(p+0x66)<lu16(L.net+LDN_NODE(0)+0x2e)) why=4;
        else if(lu16(p+0x66)>lu16(L.net+LDN_NODE(0)+0x2e)) why=6;
        else if(phrase>64 || lu16(p)!=lu16(L.security) || phrase!=lu16(L.security+2) || memcmp(p+4,L.security+4,phrase)) why=5;
        else if(!lan_accepts(p+0x68)) why=5;
        int node=member;
        if(!why && node<0) {
            for(int i=1;i<L.net[0x66];i++) if(!L.peers[i].id) {node=i;break;}
            if(node<0) why=2;
        }
        if(why) { uint8_t reason=(uint8_t)why; lan_send(from,LP_REFUSE,&reason,1,0,0,0,0); printf("[lan] join-refused reason=%d build=%016llx\n",why,(unsigned long long)build); return; }
        if(member<0) {
            uint8_t *nd=L.net+LDN_NODE(node); memset(nd,0,0x40);
            lp32(nd,(lu32(L.net+LDN_NODE(0))&0xffffff00u)|(node+1)); memcpy(nd+4,p+0x68,6);
            nd[10]=(uint8_t)node; nd[11]=1; memcpy(nd+12,p+0x44,33); nd[44]=0;
            memcpy(nd+0x2e,p+0x66,2); L.peers[node].id=id; L.peers[node].addr=*from; L.net[0x67]++;
            printf("[lan] node-joined id=%d identity=%016llx virtual=%08x\n",node,(unsigned long long)id,lu32(nd));
        }
        L.peers[node].heard=now; lan_sync();lan_signal();
    } else if(type==LP_SYNC && size==LDN_NI_SIZE+1 && build==L.build && lan_valid_info(p) &&
              !memcmp(b+28,p+0x10,16) &&
              ((L.state==LDN_STA && L.pending && lan_same_peer(&L.joining,from,id) && !memcmp(p+0x10,L.join_session,16)) ||
               (L.state==LDN_CONNECTED && member==0 && same_session))) {
        uint8_t mac[6]; lan_mac(mac,L.id); int node=-1;
        for(int i=1;i<8;i++) if(p[LDN_NODE(i)+11] && !memcmp(p+LDN_NODE(i)+4,mac,6)) node=i;
        if(node<0) { if(L.state==LDN_CONNECTED) lan_leave(5); return; }
        int changed=memcmp(L.net,p,LDN_NI_SIZE)!=0;
        memcpy(L.net,p,LDN_NI_SIZE); L.peers[0].id=id; L.peers[0].addr=*from; L.peers[0].heard=now;
        L.node=node; L.answer=1; L.reason=0;
        if(L.state!=LDN_CONNECTED) {lan_state(LDN_CONNECTED); printf("[lan] connected node=%d virtual=%08x\n",node,lu32(p+LDN_NODE(node)));}
        else if(changed) lan_signal();
    } else if(type==LP_REFUSE && size==1 && L.pending && lan_same_peer(&L.joining,from,id) && !memcmp(b+28,L.join_session,16)) {
        L.answer=-1; L.reason=5;
        L.connect_result=p[0]==2?LDN_R_FULL:p[0]==4?LDN_R_VERSION_LOW:p[0]==6?LDN_R_VERSION_HIGH:p[0]==5?LDN_R_AUTH:LDN_R_CONNECT;
        lan_signal();
    } else if(build==L.build && same_session && member>=0 && lan_active()) {
        L.peers[member].heard=now;
        if(type==LP_KEEP && !size) { /* periodic Sync is the host's heartbeat */ }
        else if(type==LP_LEAVE && !size && L.state==LDN_CREATED && member>0) lan_remove(member);
        else if((type==LP_DESTROY || type==LP_KICK) && !size && L.state==LDN_CONNECTED && member==0) lan_leave(type==LP_KICK?5:3);
        else if(type==LP_DATA && size<=LDN_MAX_DATA) {
            uint32_t src=lu32(b+44),dst=lu32(b+48),base=lu32(L.net+LDN_NODE(0))&0xffffff00u;
            int source_node=(int)(src&255)-1;
            if(source_node<0 || source_node>=8 || !L.net[LDN_NODE(source_node)+11] ||
               lu32(L.net+LDN_NODE(source_node))!=src || (L.state==LDN_CREATED && source_node!=member) ||
               ((dst&0xffffff00u)!=base && dst!=0xffffffffu)) {L.dropped++;return;}
            int broadcast=dst==(base|255) || dst==0xffffffffu;
            if(L.state==LDN_CREATED) for(int i=1;i<8;i++) if(i!=member && L.peers[i].id && (broadcast || dst==lu32(L.net+LDN_NODE(i))))
                lan_send(&L.peers[i].addr,LP_DATA,p,size,src,dst,lu16(b+52),lu16(b+54));
            if(broadcast || dst==lu32(L.net+LDN_NODE(L.node))) {
                /* The caller drops L before calling into the socket queue. */
                L.data_rx++; ReleaseSRWLockExclusive(&L.lock);
                socket_ldn_receive(src,dst,lu16(b+52),lu16(b+54),p,size);
                AcquireSRWLockExclusive(&L.lock);
            }
        }
    }
}
static DWORD WINAPI lan_worker(void *unused) {
    (void)unused; uint8_t b[LDN_WIRE_HEADER+LDN_MAX_DATA+1];
    os_pin_host_thread(); /* audit 04: with the runtime's own threads, off every game's console cores (os_threads.c) */
    for(;;) {
        /* audit 05: wait for a datagram (libzt: poll, not a 2 ms sleep loop), at most 20 ms or until a simulated one is due */
        unsigned wait_ms=net_sim_wait(GetTickCount64(),20);
        fd_set read; FD_ZERO(&read); FD_SET(L.fd,&read); struct timeval timeout={0,(long)wait_ms*1000};
        if(zt_requested())zt_wait(wait_ms);else select(0,&read,NULL,NULL,&timeout);
        AcquireSRWLockExclusive(&L.lock);
        if(L.stop) {ReleaseSRWLockExclusive(&L.lock);break;}
        /* A finite receive budget keeps timers/state changes responsive to flood. */
        for(int budget=0;budget<128;budget++) {
            struct sockaddr_in from; int len=sizeof from;
            int n;
            if(zt_requested()) {uint32_t ip;uint16_t port;n=zt_recv(&ip,&port,b,sizeof b);if(n>=0){memset(&from,0,sizeof from);from.sin_family=AF_INET;from.sin_addr.s_addr=htonl(ip);from.sin_port=htons(port);}}
            else n=recvfrom(L.fd,(char *)b,sizeof b,0,(struct sockaddr *)&from,&len);
            if(n<0) {if(!zt_requested()&&WSAGetLastError()!=WSAEWOULDBLOCK) L.malformed++;break;}
            L.received++; lan_receive(b,(unsigned)n,&from);
        }
        uint64_t now=GetTickCount64();
        net_sim_flush(now,lan_raw_send);
        if(now-L.last_tick>=500) {
            L.last_tick=now;
            if(L.state==LDN_CREATED) {
                for(int i=1;i<8;i++) if(L.peers[i].id && now-L.peers[i].heard>LDN_TIMEOUT_MS) lan_remove(i);
                lan_sync();
                if(now-L.last_advert>=1000){lan_discover(LP_ADVERT);L.last_advert=now;}
            } else if(L.state==LDN_CONNECTED) {
                lan_send(&L.peers[0].addr,LP_KEEP,NULL,0,0,0,0,0);
                if(now-L.peers[0].heard>LDN_TIMEOUT_MS) lan_leave(6);
            }
        }
        ReleaseSRWLockExclusive(&L.lock);
        socket_ldn_changed();
    }
    return 0;
}
static int lan_interface(void) {
    if(zt_requested()){L.interface_ip=0x64400000;L.interface_mask=0xffffffff;L.broadcast=0xffffffff;return zt_ready();}
    const char *wanted=getenv("STRIKERS_LDN_INTERFACE");
    if(wanted && !strcmp(wanted,"127.0.0.1")) {L.interface_ip=0x7f000001;L.interface_mask=0xff000000;L.broadcast=0x7fffffff;return 1;}
    SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP); INTERFACE_INFO interfaces[64]; DWORD bytes=0;
    if(s==INVALID_SOCKET) return 0;
    int ok=WSAIoctl(s,SIO_GET_INTERFACE_LIST,NULL,0,interfaces,sizeof interfaces,&bytes,NULL,NULL)==0; closesocket(s);
    uint32_t desired=0; if(wanted) {struct in_addr a; if(InetPtonA(AF_INET,wanted,&a)!=1) return 0; desired=ntohl(a.s_addr);}
    L.interface_ip=0;
    if(ok) for(unsigned i=0;i<bytes/sizeof interfaces[0];i++) {
        INTERFACE_INFO *p=&interfaces[i]; uint32_t ip=ntohl(p->iiAddress.AddressIn.sin_addr.s_addr),mask=ntohl(p->iiNetmask.AddressIn.sin_addr.s_addr);
        if(getenv("STRIKERS_LDN_TRACE") && (p->iiFlags&IFF_UP))
            printf("[lan] adapter candidate=%u.%u.%u.%u mask=%08x flags=%lx\n",ip>>24,(ip>>16)&255,(ip>>8)&255,ip&255,mask,(unsigned long)p->iiFlags);
        /* A point-to-point VPN /32 has no LAN broadcast domain. Do not let it
         * hide the actual Ethernet/Wi-Fi adapter (or loopback fallback). */
        if(!(p->iiFlags&IFF_UP) || !(p->iiFlags&IFF_BROADCAST) || (p->iiFlags&IFF_POINTTOPOINT) ||
           (ip>>24)==127 || !mask || mask>=0xfffffffeu || (desired && desired!=ip)) continue;
        L.interface_ip=ip;L.interface_mask=mask;break;
    }
    if(!L.interface_ip && !wanted) {printf("[lan] no usable LAN broadcast adapter; using loopback\n");L.interface_ip=0x7f000001;L.interface_mask=0xff000000;}
    if(!L.interface_ip && wanted) printf("[lan] requested interface %s is not an active LAN broadcast adapter\n",wanted);
    L.broadcast=L.interface_ip|~L.interface_mask;
    return L.interface_ip!=0;
}
static void ldn_init(Ctx *c) {
    if(!strikers_lan_enabled()) {RET(0x2ecb);return;}
    AcquireSRWLockExclusive(&L.lock);
    if(L.state!=LDN_NONE) {ReleaseSRWLockExclusive(&L.lock);RET(LDN_R_STATE);return;}
    WSADATA w; int ok=WSAStartup(MAKEWORD(2,2),&w)==0;L.wsa=ok;
    if(ok) ok=lan_interface();
    L.base_port=LDN_DEFAULT_PORT; const char *v=getenv("STRIKERS_LDN_PORT");
    if(v) { char *end; long x=strtol(v,&end,10); if(*end || x<1024 || x>65535-LDN_PORT_SLOTS) ok=0; else L.base_port=(uint16_t)x; }
    if(!L.id)L.id=lan_random(); v=getenv("STRIKERS_LDN_ID"); if(v) {char *end; L.id=_strtoui64(v,&end,16); if(*end || !L.id) ok=0;}
    L.build=STRIKERS_LDN_BUILD_DEFAULT; v=getenv("STRIKERS_LDN_BUILD"); if(v) {char *end;L.build=_strtoui64(v,&end,16);if(*end)ok=0;}
    if(!net_sim_init(getenv("STRIKERS_NET_SIM")))ok=0;
    if(ok && L.id && zt_requested()){ok=zt_open(L.base_port,&L.bound_port);if(ok)L.fd=0;}
    else if(ok && L.id) {
        L.fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        if(L.fd==INVALID_SOCKET) ok=0;
        else {
            BOOL yes=TRUE; setsockopt(L.fd,SOL_SOCKET,SO_BROADCAST,(char *)&yes,sizeof yes);
            setsockopt(L.fd,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char *)&yes,sizeof yes);
            BOOL no=FALSE;DWORD ignored;
            WSAIoctl(L.fd,SIO_UDP_CONNRESET,&no,sizeof no,NULL,0,&ignored,NULL,NULL);
            struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl((L.interface_ip>>24)==127?L.interface_ip:INADDR_ANY);
            ok=0; for(int i=0;i<LDN_PORT_SLOTS;i++) {a.sin_port=htons((uint16_t)(L.base_port+i)); if(bind(L.fd,(struct sockaddr *)&a,sizeof a)==0) {L.bound_port=L.base_port+i;ok=1;break;}}
            u_long nonblock=1; if(ok && ioctlsocket(L.fd,FIONBIO,&nonblock)) ok=0;
        }
    } else ok=0;
    if(ok) {
        L.stop=0;L.reason=0;L.node=-1;L.pending=0;L.answer=0;L.last_tick=0;L.last_advert=0;L.last_scan_reply=0;
        memset(L.net,0,sizeof L.net);memset(L.seen,0,sizeof L.seen);memset(L.peers,0,sizeof L.peers);
        memset(L.advertise,0,sizeof L.advertise);L.advertise_size=0;
        L.worker=CreateThread(NULL,0,lan_worker,NULL,0,NULL); if(!L.worker) ok=0;
    }
    if(ok) {lan_state(LDN_INIT);printf("[lan] initialized interface=%u.%u.%u.%u mask=%08x port=%u discovery=%u-%u identity=%016llx build=%016llx\n",L.interface_ip>>24,(L.interface_ip>>16)&255,(L.interface_ip>>8)&255,L.interface_ip&255,L.interface_mask,L.bound_port,L.base_port,L.base_port+LDN_PORT_SLOTS-1,(unsigned long long)L.id,(unsigned long long)L.build);}
    else {int error=WSAGetLastError();if(zt_requested())zt_close();else if(L.fd!=INVALID_SOCKET)closesocket(L.fd);L.fd=INVALID_SOCKET;if(L.wsa){WSACleanup();L.wsa=0;}printf("[lan] initialize failed (interface/port/Winsock error=%d)\n",error);}
    ReleaseSRWLockExclusive(&L.lock); RET(ok?0:0x2ecb);
}
static void ldn_finalize(Ctx *c) {
    AcquireSRWLockExclusive(&L.lock);lan_shutdown_room();L.stop=1;L.pending=0;L.answer=-1;lan_state(LDN_NONE);L.event=0;
    HANDLE thread=L.worker; L.worker=NULL; ReleaseSRWLockExclusive(&L.lock);
    if(thread) {WaitForSingleObject(thread,INFINITE);CloseHandle(thread);}
    AcquireSRWLockExclusive(&L.lock);net_sim_clear();if(zt_requested())zt_close();else if(L.fd!=INVALID_SOCKET)closesocket(L.fd);L.fd=INVALID_SOCKET;
    if(L.wsa){WSACleanup();L.wsa=0;}
    printf("[lan] finalized received=%llu malformed=%llu dropped=%llu data-tx=%llu data-rx=%llu\n",L.received,L.malformed,L.dropped,L.data_tx,L.data_rx);
    ReleaseSRWLockExclusive(&L.lock);socket_ldn_changed();RET(0);
}
static void ldn_state(Ctx *c) {AcquireSRWLockShared(&L.lock);RET(L.state);ReleaseSRWLockShared(&L.lock);}
static void ldn_attach(Ctx *c) {AcquireSRWLockExclusive(&L.lock);L.event=A0;os_init_event(A0,1);ReleaseSRWLockExclusive(&L.lock);RET(0);}
static void ldn_open(Ctx *c,int state) {AcquireSRWLockExclusive(&L.lock);int ok=L.state==LDN_INIT; if(ok){L.reason=0;if(state==LDN_AP)L.policy=0;lan_state(state);}ReleaseSRWLockExclusive(&L.lock);RET(ok?0:LDN_R_STATE);}
static void ldn_station(Ctx *c) {ldn_open(c,LDN_STA);}
static void ldn_ap(Ctx *c) {ldn_open(c,LDN_AP);}
static void ldn_close(Ctx *c,int ap) {AcquireSRWLockExclusive(&L.lock);int ok=ap?(L.state==LDN_AP||L.state==LDN_CREATED):(L.state==LDN_STA||L.state==LDN_CONNECTED);if(ok){lan_shutdown_room();lan_state(LDN_INIT);}ReleaseSRWLockExclusive(&L.lock);socket_ldn_changed();RET(ok?0:LDN_R_STATE);}
static void ldn_close_ap(Ctx *c) {ldn_close(c,1);}
static void ldn_close_station(Ctx *c) {ldn_close(c,0);}
static void ldn_create_common(Ctx *c,int private_room) {
    const uint8_t *cfg=PTR(A0),*sec=PTR(A1),*sp=private_room?PTR(A2):NULL,*user=PTR(private_room?A3:A2);
    if(!cfg||!sec||!user||cfg[0x12]<1||cfg[0x12]>8||lu16(sec+2)>64||lu16(sec)>2 ||
       (private_room && (!sp || c->x[4]>8 || (c->x[4] && !c->x[5])))) {RET(LDN_R_INVALID);return;}
    AcquireSRWLockExclusive(&L.lock); if(L.state!=LDN_AP) {ReleaseSRWLockExclusive(&L.lock);RET(LDN_R_STATE);return;}
    uint8_t *n=L.net;memset(n,0,sizeof L.net);memset(L.peers,0,sizeof L.peers);memcpy(n,cfg,16);
    for(int i=0;i<4;i++) lp64(n+0x10+i*8,lan_random());
    if(sp) {memcpy(n+0x50,sp,16);memcpy(n+0x10,sp+16,16);} else {lp64(n+0x50,lan_random());lp64(n+0x58,lan_random());}
    lan_mac(n+0x20,L.id);n[0x26]=32;
    for(int i=0;i<16;i++) {static const char h[]="0123456789abcdef";n[0x27+i*2]=h[n[0x10+i]>>4];n[0x28+i*2]=h[n[0x10+i]&15];}
    lp16(n+0x48,lu16(cfg+0x10)?lu16(cfg+0x10):6);n[0x4a]=3;n[0x4b]=2;memcpy(n+0x60,sec,2);
    n[0x62]=(uint8_t)L.policy;n[0x66]=cfg[0x12];n[0x67]=1;L.private_room=private_room;L.address_count=private_room?(int)c->x[4]:0;
    if(L.address_count)memcpy(L.addresses,PTR(c->x[5]),L.address_count*12);
    memcpy(L.security,sec,0x44);L.node=0;L.reason=0;
    lp16(n+0x26a,L.advertise_size);memcpy(n+0x26c,L.advertise,L.advertise_size);
    uint8_t *nd=n+LDN_NODE(0);uint32_t base=0xa9fe0000u|(((uint32_t)L.id%253+1)<<8);
    lp32(nd,base|1);lan_mac(nd+4,L.id);nd[11]=1;memcpy(nd+12,user,33);nd[44]=0;memcpy(nd+0x2e,cfg+0x14,2);
    lan_state(LDN_CREATED);printf("[lan] room-created private=%d lcid=%016llx scene=%u version=%u max=%u virtual=%08x\n",private_room,(unsigned long long)lu64(cfg),lu16(cfg+10),lu16(cfg+20),n[0x66],base|1);
    ReleaseSRWLockExclusive(&L.lock);RET(0);
}
static void ldn_create(Ctx *c) {ldn_create_common(c,0);}
static void ldn_create_private(Ctx *c) {ldn_create_common(c,1);}
static void ldn_destroy(Ctx *c) {AcquireSRWLockExclusive(&L.lock);int ok=L.state==LDN_CREATED;if(ok)lan_shutdown_room();ReleaseSRWLockExclusive(&L.lock);socket_ldn_changed();RET(ok?0:LDN_R_STATE);}
static int lan_filter(const uint8_t *n,const uint8_t *f) {
    uint32_t flags=lu32(f+0x5c);
    return !((flags&1)&&memcmp(n,f,8)) && !((flags&2)&&memcmp(n+16,f+16,16)) &&
        !((flags&4)&&lu32(f+32)!=3&&lu32(f+32)!=n[0x4b]) &&
        !((flags&16)&&(f[0x2a]!=n[0x26]||memcmp(n+0x27,f+0x2b,n[0x26]))) && !((flags&32)&&memcmp(n+10,f+10,2));
}
static void ldn_scan_common(Ctx *c,int private_room) {
    if(!A1 || !A3 || (int32_t)A2<0 || A2>256 || (A2&&!A0)) {RET(LDN_R_INVALID);return;}
    AcquireSRWLockExclusive(&L.lock); if(L.state==LDN_NONE) {ReleaseSRWLockExclusive(&L.lock);RET(LDN_R_STATE);return;}
    memset(L.seen,0,sizeof L.seen);lan_discover(LP_SCAN);ReleaseSRWLockExclusive(&L.lock);
    Sleep(300);AcquireSRWLockExclusive(&L.lock);lan_discover(LP_SCAN);ReleaseSRWLockExclusive(&L.lock);Sleep(300);
    AcquireSRWLockExclusive(&L.lock);int found=0;
    for(int i=0;i<32 && found<(int)A2;i++) if(L.seen[i].at && L.seen[i].private_room==private_room && lan_filter(L.seen[i].info,PTR(A3))) {
        memcpy(PTR(A0+(uint64_t)found*LDN_NI_SIZE),L.seen[i].info,LDN_NI_SIZE);found++;
    }
    wr32(A1,found);printf("[lan] scan private=%d flags=%x found=%d\n",private_room,lu32((uint8_t *)PTR(A3)+0x5c),found);
    ReleaseSRWLockExclusive(&L.lock);RET(0);
}
static void ldn_scan(Ctx *c) {ldn_scan_common(c,0);}
static void ldn_scan_private(Ctx *c) {ldn_scan_common(c,1);}
static void ldn_connect_common(Ctx *c,int private_room) {
    const uint8_t *cfg=PTR(A0),*sec=PTR(A1),*sp=private_room?PTR(A2):NULL,*user=PTR(private_room?A3:A2);
    if(!cfg||!sec||!user||(private_room&&!sp)||lu16(sec+2)>64) {RET(LDN_R_INVALID);return;}
    const uint8_t *session=private_room?sp+16:cfg+16;uint16_t version=(uint16_t)c->x[private_room?4:3];
    AcquireSRWLockExclusive(&L.lock);int seen=-1;
    for(int i=0;i<32;i++) if(L.seen[i].at && GetTickCount64()-L.seen[i].at<6000 && !memcmp(L.seen[i].info+16,session,16) && L.seen[i].private_room==private_room) {seen=i;break;}
    if(L.state!=LDN_STA || L.pending || seen<0) {ReleaseSRWLockExclusive(&L.lock);RET(LDN_R_CONNECT);return;}
    LanSeen *s=&L.seen[seen];
    if(s->build!=L.build ||
       (private_room && (memcmp(cfg,s->info,8)||memcmp(cfg+10,s->info+10,2)||memcmp(sp,s->info+0x50,16)))) {
        printf("[lan] connect-refused build/version/security-parameter mismatch\n");ReleaseSRWLockExclusive(&L.lock);RET(LDN_R_CONNECT);return;
    }
    uint16_t host_version=lu16(s->info+LDN_NODE(0)+0x2e);
    if(version!=host_version) {ReleaseSRWLockExclusive(&L.lock);RET(version<host_version?LDN_R_VERSION_LOW:LDN_R_VERSION_HIGH);return;}
    uint8_t request[0x78]={0};memcpy(request,sec,0x44);memcpy(request+0x44,user,33);lp16(request+0x66,version);lan_mac(request+0x68,L.id);
    L.joining=s->peer;memcpy(L.join_session,session,16);memcpy(L.net,s->info,LDN_NI_SIZE);L.answer=0;L.pending=1;L.connect_result=LDN_R_TIMEOUT;
    for(int tries=0;tries<8 && !L.answer && L.pending;tries++) {
        lan_send(&L.joining.addr,LP_JOIN,request,sizeof request,0,0,0,0);
        uint64_t until=GetTickCount64()+500,now;
        while(!L.answer && L.pending && (now=GetTickCount64())<until) SleepConditionVariableSRW(&L.cv,&L.lock,(DWORD)(until-now),0);
    }
    int ok=L.answer==1;uint32_t result=ok?0:L.connect_result;L.pending=0;
    printf("[lan] connect result=%s sdk-result=%x\n",ok?"joined":"refused/timeout",result);ReleaseSRWLockExclusive(&L.lock);RET(result);
}
static void ldn_connect(Ctx *c) {ldn_connect_common(c,0);}
static void ldn_connect_private(Ctx *c) {ldn_connect_common(c,1);}
static void ldn_disconnect(Ctx *c) {AcquireSRWLockExclusive(&L.lock);int ok=L.state==LDN_CONNECTED;if(ok)lan_shutdown_room();ReleaseSRWLockExclusive(&L.lock);socket_ldn_changed();RET(ok?0:LDN_R_STATE);}
static void ldn_info(Ctx *c) {AcquireSRWLockShared(&L.lock);int ok=lan_active()&&A0;if(ok)memcpy(PTR(A0),L.net,LDN_NI_SIZE);ReleaseSRWLockShared(&L.lock);RET(ok?0:LDN_R_STATE);}
static void ldn_config(Ctx *c) {AcquireSRWLockShared(&L.lock);int ok=lan_active()&&A0;if(ok){uint8_t *o=PTR(A0);memset(o,0,32);memcpy(o,L.net,16);memcpy(o+16,L.net+0x48,2);o[18]=L.net[0x66];memcpy(o+20,L.net+LDN_NODE(0)+0x2e,2);}ReleaseSRWLockShared(&L.lock);RET(ok?0:LDN_R_STATE);}
static void ldn_security(Ctx *c) {AcquireSRWLockShared(&L.lock);int ok=lan_active()&&A0;if(ok){memcpy(PTR(A0),L.net+0x50,16);memcpy(PTR(A0+16),L.net+16,16);}ReleaseSRWLockShared(&L.lock);RET(ok?0:LDN_R_STATE);}
uint32_t ldn_local_ip(void) {AcquireSRWLockShared(&L.lock);uint32_t ip=lan_active()&&L.node>=0?lu32(L.net+LDN_NODE(L.node)):0;ReleaseSRWLockShared(&L.lock);return ip;}
static void ldn_address(Ctx *c) {uint32_t ip=ldn_local_ip();if(ip&&A0&&A1){wr32(A0,ip);wr32(A1,0xffffff00);}RET(ip?0:LDN_R_STATE);}
static void ldn_reason(Ctx *c) {AcquireSRWLockShared(&L.lock);RET((uint32_t)L.reason);ReleaseSRWLockShared(&L.lock);}
static void ldn_advertise(Ctx *c) {
    if(A1>384){RET(0x14cb);return;}if(A1&&!A0){RET(LDN_R_INVALID);return;}
    AcquireSRWLockExclusive(&L.lock);int ok=L.state==LDN_AP||L.state==LDN_CREATED;
    if(ok){
        memset(L.advertise,0,sizeof L.advertise);if(A1)memcpy(L.advertise,PTR(A0),(size_t)A1);L.advertise_size=(uint16_t)A1;
        memset(L.net+0x26c,0,384);memcpy(L.net+0x26c,L.advertise,L.advertise_size);lp16(L.net+0x26a,L.advertise_size);
        if(L.state==LDN_CREATED){lan_sync();lan_signal();}
    }
    ReleaseSRWLockExclusive(&L.lock);RET(ok?0:LDN_R_STATE);
}
static void ldn_policy(Ctx *c) {AcquireSRWLockExclusive(&L.lock);int ok=(L.state==LDN_AP||L.state==LDN_CREATED)&&A0<=3;if(ok){L.policy=(int)A0;L.net[0x62]=(uint8_t)A0;if(L.state==LDN_CREATED)lan_sync();}ReleaseSRWLockExclusive(&L.lock);RET(ok?0:LDN_R_STATE);}
static void ldn_reject(Ctx *c) {AcquireSRWLockExclusive(&L.lock);int found=0;if(L.state==LDN_CREATED)for(int i=1;i<8;i++)if(L.peers[i].id&&lu32(L.net+LDN_NODE(i))==(uint32_t)A0){lan_send(&L.peers[i].addr,LP_KICK,NULL,0,0,0,0,0);lan_remove(i);found=1;break;}ReleaseSRWLockExclusive(&L.lock);RET(found?0:LDN_R_STATE);}
static void ldn_ip(Ctx *c) {RET(((A0&255)<<24)|((A1&255)<<16)|((A2&255)<<8)|(A3&255));}
static void ldn_broadcast(Ctx *c) {RET((uint32_t)A0|~(uint32_t)A1);}
int ldn_udp_send(uint32_t to,uint16_t sport,uint16_t port,const void *data,unsigned size) {
    if(size>LDN_MAX_DATA)return -90;
    AcquireSRWLockExclusive(&L.lock);int result=-100;
    if(lan_active()) {
        uint32_t src=lu32(L.net+LDN_NODE(L.node)),base=src&0xffffff00u;int broad=to==(base|255)||to==0xffffffffu;
        int valid=broad;for(int i=0;i<8;i++)if(L.net[LDN_NODE(i)+11]&&to==lu32(L.net+LDN_NODE(i)))valid=1;
        if(!valid) result=-13;
        else {
            result=(int)size;
            if(L.state==LDN_CONNECTED && to!=src) {if(lan_send(&L.peers[0].addr,LP_DATA,data,size,src,to,sport,port)<0)result=-lan_socket_errno();}
            else if(L.state==LDN_CREATED) for(int i=1;i<8;i++) if(L.peers[i].id&&(broad||to==lu32(L.net+LDN_NODE(i))))
                if(lan_send(&L.peers[i].addr,LP_DATA,data,size,src,to,sport,port)<0)result=-lan_socket_errno();
            L.data_tx++;if(L.data_tx<=8||L.data_tx%10000==0)printf("[lan] UDP tx=%llu src=%08x:%u dst=%08x:%u bytes=%u\n",L.data_tx,src,sport,to,port,size);
            if(to==src || broad) {ReleaseSRWLockExclusive(&L.lock);socket_ldn_receive(src,to,sport,port,data,size);return result;}
        }
    }
    ReleaseSRWLockExclusive(&L.lock);return result;
}
