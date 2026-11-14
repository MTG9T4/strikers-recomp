/* SPDX-License-Identifier: GPL-3.0-or-later
 * Strikers bounded datagram descriptors over the Winsock LDN transport.
 * No guest IP is ever passed to Winsock, including numeric DNS results.
 * BSD sockaddr_in/pollfd/option constants are translated explicitly. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <string.h>
#include "network_policy.h"
#include "ldn_lan.h"
#define SO_FDS 128
#define SO_QUEUE 64
typedef struct SoPacket { struct SoPacket *next; uint32_t from; uint16_t port; unsigned size; uint8_t data[1]; } SoPacket;
typedef struct {
    int used, flags, broadcast, reuse, shutdown_read, shutdown_write, error, generation, ttl;
    uint32_t local, remote, receive_ms, send_ms, receive_buffer, send_buffer;
    uint16_t port, remote_port;
    unsigned queued; SoPacket *head,*tail;
} SoFd;
static SoFd S[SO_FDS];
static SRWLOCK S_lock=SRWLOCK_INIT;
static CONDITION_VARIABLE S_cv=CONDITION_VARIABLE_INIT;
static unsigned S_ephemeral=49152;
static __declspec(thread) int g_errno;
static int *so_errno(Ctx *c) { return c->tpidr?(int *)PTR(c->tpidr+0x1d8):&g_errno; }
static void so_error(Ctx *c,int e) {
    static __declspec(thread) unsigned errors;
    if(strikers_lan_enabled() && e!=11 && errors++<32)
        printf("[lan] socket-error errno=%d import=%s caller=%llx\n",e,c->in_import?c->in_import:"contract-test",(unsigned long long)(c->x[30]-4));
    *so_errno(c)=e;RET((uint64_t)-1);
}
static void so_fail(Ctx *c) {so_error(c,strikers_lan_enabled()?95:50);}
static void so_badfd(Ctx *c) {so_error(c,9);}
static int so_valid(int fd) {return fd>=0 && fd<SO_FDS && S[fd].used;}
static int so_address(uint64_t p,uint64_t size,uint32_t *ip,uint16_t *port) {
    if(!p || size<16)return 22;
    const uint8_t *a=PTR(p);if(a[1]!=2)return 97;
    *ip=bswap32(rd32(p+4));*port=(uint16_t)((a[2]<<8)|a[3]);return 0;
}
static void so_address_out(uint64_t p,uint64_t lenp,uint32_t ip,uint16_t port) {
    uint8_t b[16]={16,2};b[2]=(uint8_t)(port>>8);b[3]=(uint8_t)port;
    uint32_t be=bswap32(ip);memcpy(b+4,&be,4);
    if(p && lenp) {uint32_t cap=rd32(lenp);memcpy(PTR(p),b,cap<16?cap:16);wr32(lenp,16);}
}
static void so_clear(SoFd *s) {SoPacket *p=s->head;while(p){SoPacket *next=p->next;free(p);p=next;}int generation=s->generation+1;memset(s,0,sizeof *s);s->generation=generation;}
static int so_port_free(int fd,uint16_t port) {for(int i=0;i<SO_FDS;i++)if(i!=fd&&S[i].used&&S[i].port==port&&!(S[i].reuse&&S[fd].reuse))return 0;return 1;}
static int so_auto_bind(int fd) {
    if(S[fd].port)return 1;
    for(unsigned i=0;i<16384;i++){if(S_ephemeral>65535)S_ephemeral=49152;uint16_t port=(uint16_t)S_ephemeral++;if(so_port_free(fd,port)){S[fd].port=port;return 1;}}
    return 0;
}
void socket_ldn_changed(void) {WakeAllConditionVariable(&S_cv);}
void socket_ldn_receive(uint32_t from,uint32_t to,uint16_t source_port,uint16_t port,const void *data,unsigned size) {
    if(size>LDN_MAX_DATA)return;
    AcquireSRWLockExclusive(&S_lock);
    for(int i=0;i<SO_FDS;i++) {
        SoFd *s=&S[i];if(!s->used||s->port!=port||s->shutdown_read||s->queued>=SO_QUEUE||
            (s->local && s->local!=to && (to&255)!=255 && to!=0xffffffffu)||
            (s->remote&&(s->remote!=from||s->remote_port!=source_port)))continue;
        SoPacket *p=malloc(sizeof *p+size);if(!p)continue;
        p->next=NULL;p->from=from;p->port=source_port;p->size=size;if(size)memcpy(p->data,data,size);
        if(s->tail)s->tail->next=p;else s->head=p;s->tail=p;s->queued++;
    }
    WakeAllConditionVariable(&S_cv);ReleaseSRWLockExclusive(&S_lock);
}
static void so_init(Ctx *c) {*so_errno(c)=0;RET(0);}
static void so_socket(Ctx *c) {
    if(!strikers_lan_enabled()){so_error(c,50);return;}
    int type=(int)A1;if(A0!=2){so_error(c,97);return;}
    if((type&~0x80800)!=2 || (A2!=0&&A2!=17)){so_error(c,93);return;}
    AcquireSRWLockExclusive(&S_lock);int fd=-1;
    for(int i=0;i<SO_FDS;i++)if(!S[i].used){fd=i;break;}
    if(fd>=0){so_clear(&S[fd]);S[fd].used=1;S[fd].ttl=64;S[fd].flags=type&0x800;S[fd].receive_buffer=S[fd].send_buffer=262144;}
    ReleaseSRWLockExclusive(&S_lock);if(fd<0)so_error(c,24);else {printf("[lan] socket fd=%d UDP\n",fd);RET(fd);}
}
static void so_close(Ctx *c) {AcquireSRWLockExclusive(&S_lock);int ok=so_valid((int)A0);if(ok)so_clear(&S[(int)A0]);WakeAllConditionVariable(&S_cv);ReleaseSRWLockExclusive(&S_lock);if(ok)RET(0);else so_badfd(c);}
static void so_bind(Ctx *c) {
    uint32_t ip;uint16_t port;int e=so_address(A1,A2,&ip,&port);if(e){so_error(c,e);return;}
    uint32_t me=ldn_local_ip();if(ip && ip!=me){so_error(c,99);return;}
    AcquireSRWLockExclusive(&S_lock);int fd=(int)A0;
    if(!so_valid(fd))e=9;else if(S[fd].port)e=22;else if(port&&!so_port_free(fd,port))e=98;
    else {S[fd].local=ip;S[fd].port=port;if(!port&&!so_auto_bind(fd))e=98;printf("[lan] bind fd=%d virtual=%08x:%u\n",fd,ip,S[fd].port);}
    ReleaseSRWLockExclusive(&S_lock);if(e)so_error(c,e);else RET(0);
}
static void so_connect(Ctx *c) {
    uint32_t ip;uint16_t port;int e=so_address(A1,A2,&ip,&port);if(e){so_error(c,e);return;}
    uint32_t me=ldn_local_ip();if(!me || (ip&0xffffff00u)!=(me&0xffffff00u)){so_error(c,13);return;}
    AcquireSRWLockExclusive(&S_lock);int fd=(int)A0;
    if(!so_valid(fd))e=9;else if(!so_auto_bind(fd))e=98;else {S[fd].remote=ip;S[fd].remote_port=port;}
    ReleaseSRWLockExclusive(&S_lock);if(e)so_error(c,e);else RET(0);
}
static void so_fcntl(Ctx *c) {int fd=(int)A0,e=0,result=0;AcquireSRWLockExclusive(&S_lock);if(!so_valid(fd))e=9;else if(A1==3)result=S[fd].flags;else if(A1==4)S[fd].flags=(int)A2&0x800;else e=22;ReleaseSRWLockExclusive(&S_lock);if(e)so_error(c,e);else RET(result);}
static void so_name_common(Ctx *c,int peer) {int fd=(int)A0,e=0;uint32_t ip=0;uint16_t port=0;AcquireSRWLockShared(&S_lock);if(!so_valid(fd))e=9;else if(!A1||!A2)e=14;else {ip=peer?S[fd].remote:S[fd].local;port=peer?S[fd].remote_port:S[fd].port;if(peer&&!ip)e=107;}ReleaseSRWLockShared(&S_lock);if(e)so_error(c,e);else{so_address_out(A1,A2,ip,port);RET(0);}}
static void so_name(Ctx *c){so_name_common(c,0);}
static void so_peer(Ctx *c){so_name_common(c,1);}
static void so_send_common(Ctx *c,int addressed) {
    uint32_t ip=0;uint16_t port=0,sport=0;int fd=(int)A0,e=0;
    if(A2>LDN_MAX_DATA){so_error(c,90);return;}if(A2&&!A1){so_error(c,14);return;}if(A3&~0x80u){so_error(c,95);return;}
    if(addressed){e=so_address(c->x[4],c->x[5],&ip,&port);if(e){so_error(c,e);return;}}
    AcquireSRWLockExclusive(&S_lock);
    if(!so_valid(fd))e=9;else if(S[fd].shutdown_write)e=32;else {
        if(!addressed){ip=S[fd].remote;port=S[fd].remote_port;}
        if(!port)e=89;else if((ip==0xffffffffu||(ip&255)==255)&&!S[fd].broadcast)e=13;
        else if(!so_auto_bind(fd))e=98;else sport=S[fd].port;
    }
    ReleaseSRWLockExclusive(&S_lock);if(e){so_error(c,e);return;}
    int n=ldn_udp_send(ip,sport,port,PTR(A1),(unsigned)A2);if(n<0)so_error(c,-n);else RET(n);
}
static void so_send(Ctx *c){so_send_common(c,0);}
static void so_sendto(Ctx *c){so_send_common(c,1);}
static void so_recv_common(Ctx *c,int addressed) {
    int fd=(int)A0,e=0;uint64_t buf=A1,size=A2,flags=A3;
    if((size&&!buf)||size>0x7fffffff || (addressed&&c->x[4]&&!c->x[5])){so_error(c,14);return;}
    if(flags&~0x82u){so_error(c,95);return;}
    AcquireSRWLockExclusive(&S_lock);
    if(!so_valid(fd)){ReleaseSRWLockExclusive(&S_lock);so_badfd(c);return;}
    int generation=S[fd].generation;uint64_t until=S[fd].receive_ms?GetTickCount64()+S[fd].receive_ms:UINT64_MAX;
    while(!S[fd].head && !S[fd].shutdown_read) {
        if((S[fd].flags&0x800)||(flags&0x80)){e=11;break;}
        if(!ldn_local_ip()){e=100;break;}
        uint64_t now=GetTickCount64();if(now>=until){e=11;break;}
        SleepConditionVariableSRW(&S_cv,&S_lock,(DWORD)(until-now>100?100:until-now),0);
        if(!so_valid(fd)||S[fd].generation!=generation){e=9;break;}
    }
    int n=0;uint32_t ip=0;uint16_t port=0;
    if(!e && S[fd].head) {
        SoPacket *p=S[fd].head;n=(int)(size<p->size?size:p->size);if(n)memcpy(PTR(buf),p->data,n);ip=p->from;port=p->port;
        if(!(flags&2)){S[fd].head=p->next;if(!S[fd].head)S[fd].tail=NULL;S[fd].queued--;free(p);}
    }
    ReleaseSRWLockExclusive(&S_lock);if(e)so_error(c,e);else{if(addressed)so_address_out(c->x[4],c->x[5],ip,port);RET(n);}
}
static void so_recv(Ctx *c){so_recv_common(c,0);}
static void so_recvfrom(Ctx *c){so_recv_common(c,1);}
static void so_poll(Ctx *c) {
    uint64_t ptr=A0,count=A1;int timeout=(int)A2;
    if(count>SO_FDS||(!ptr&&count)){so_error(c,22);return;}
    uint64_t until=timeout<0?UINT64_MAX:GetTickCount64()+(unsigned)timeout;int ready;
    AcquireSRWLockExclusive(&S_lock);
    do {
        ready=0;for(unsigned i=0;i<count;i++) {uint64_t p=ptr+i*8;int fd=(int)rd32(p);uint16_t events=rd16(p+4),r=0;
            if(fd>=0){if(!so_valid(fd))r=0x20;else{if(S[fd].head)r|=events&0x41;if(!S[fd].shutdown_write)r|=events&4;if(S[fd].shutdown_read)r|=0x10;}}
            wr16(p+6,r);if(r)ready++;
        }
        uint64_t now=GetTickCount64();if(ready||now>=until)break;
        SleepConditionVariableSRW(&S_cv,&S_lock,(DWORD)(until-now>100?100:until-now),0);
    }while(1);
    ReleaseSRWLockExclusive(&S_lock);RET(ready);
}
static void so_shutdown(Ctx *c) {int fd=(int)A0,e=0;AcquireSRWLockExclusive(&S_lock);if(!so_valid(fd))e=9;else if(A1>2)e=22;else {if(A1!=1)S[fd].shutdown_read=1;if(A1!=0)S[fd].shutdown_write=1;WakeAllConditionVariable(&S_cv);}ReleaseSRWLockExclusive(&S_lock);if(e)so_error(c,e);else RET(0);}
static void so_option(Ctx *c,int get) {
    int fd=(int)A0,level=(int)A1,option=(int)A2,e=0;uint32_t value=0,len=get?(c->x[4]?rd32(c->x[4]):0):(uint32_t)c->x[4];uint64_t p=A3;
    if(!p || (get&&!c->x[4])){so_error(c,14);return;}
    AcquireSRWLockExclusive(&S_lock);
    if(!so_valid(fd))e=9;
    else if(level==0 && option==4) { /* IP_TTL on the virtual link (one hop). */
        if(len<4)e=22;
        else if(get){wr32(p,(uint32_t)S[fd].ttl);wr32(c->x[4],4);}
        else {value=rd32(p);if(value<1||value>255)e=22;else S[fd].ttl=(int)value;}
    }
    else if(level!=0xffff)e=92;
    else {
        SoFd *s=&S[fd];
        if(len<4)e=22;
        else {
            if(!get)value=rd32(p);
            switch(option) {
            case 4: if(get)value=s->reuse;else s->reuse=value!=0;break;
            case 0x200:if(get)value=s->reuse;else s->reuse=value!=0;break;
            case 0x20:if(get)value=s->broadcast;else s->broadcast=value!=0;break;
            case 0x1001:if(get)value=s->send_buffer;else s->send_buffer=value;break;
            case 0x1002:if(get)value=s->receive_buffer;else s->receive_buffer=value;break;
            case 0x1005:case 0x1006: {
                uint32_t *ms=option==0x1005?&s->send_ms:&s->receive_ms;
                if(len<16){e=22;break;}
                if(get){wr64(p,*ms/1000);wr64(p+8,(*ms%1000)*1000);wr32(c->x[4],16);}
                else {int64_t sec=(int64_t)rd64(p),us=(int64_t)rd64(p+8);if(sec<0||us<0||us>=1000000||sec>86400)e=22;else *ms=(uint32_t)(sec*1000+(us+999)/1000);}
                break;
            }
            case 0x1007:if(get){value=s->error;s->error=0;}else e=92;break;
            case 0x1008:if(get)value=2;else e=92;break;
            default:e=92;break;
            }
            if(!e&&get&&option!=0x1005&&option!=0x1006){wr32(p,value);wr32(c->x[4],4);}
        }
    }
    ReleaseSRWLockExclusive(&S_lock);if(e)so_error(c,e);else RET(0);
}
static void so_getopt(Ctx *c){so_option(c,1);}
static void so_setopt(Ctx *c){so_option(c,0);}
static void so_geterr(Ctx *c){RET(*so_errno(c));}
static void so_seterr(Ctx *c){*so_errno(c)=(int)A0;}
static void so_dns(Ctx *c){if(A3)wr64(A3,0);*so_errno(c)=strikers_lan_enabled()?100:50;RET(strikers_lan_enabled()?2:(int64_t)-3);}
static void so_host(Ctx *c){*so_errno(c)=strikers_lan_enabled()?100:50;RET(0);}
static void so_free(Ctx *c){(void)c;}
static void so_gai_error(Ctx *c){static const char msg[]="Internet/DNS disabled (LDN LAN only)";RET(msg);}
static void so_swap16(Ctx *c){RET((uint16_t)(((A0&255)<<8)|((A0>>8)&255)));}
static void so_swap32(Ctx *c){RET(bswap32((uint32_t)A0));}
static void so_pton(Ctx *c){RET(InetPtonA((int)A0,(const char *)PTR(A1),PTR(A2)));}
static void so_ntop(Ctx *c){RET(InetNtopA((int)A0,PTR(A1),(char *)PTR(A2),(size_t)A3));}
static void so_aton(Ctx *c){RET(InetPtonA(AF_INET,(const char *)PTR(A0),PTR(A1))==1);}
static void so_ntoa(Ctx *c){static __declspec(thread) char buf[16];uint32_t ip=(uint32_t)A0;RET(InetNtopA(AF_INET,&ip,buf,sizeof buf));}
