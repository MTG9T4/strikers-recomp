/* SPDX-License-Identifier: GPL-3.0-or-later
 * Internet rooms: Local Wireless through the Strikers relay. See relay_transport.h and server/relay/strikers_relay.py
 * (the protocol). One UDP socket connected to the relay; no other address is ever sent to. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "relay_transport.h"
#include "zt_transport.h"
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"advapi32.lib")
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR,12)
#endif
/* The project's relay, host[:port]. strikers.ini [Network] relay = (or STRIKERS_RELAY) names another one. A name, not an
 * address: the relay can move (or come online after a release) by changing its DNS record, and the game looks the name
 * up again every 10 s until it answers (relay_resolver). */
#ifndef STRIKERS_RELAY_DEFAULT
#define STRIKERS_RELAY_DEFAULT "relay.mtg9t4.fun"
#endif
enum { R_HELLO=1, R_WELCOME, R_HOST, R_UNHOST, R_LIST, R_ROOM, R_LIST_END, R_SEND, R_DATA, R_PING, R_PONG, R_BYE, R_ERROR };
#define R_PROTOCOL 1
#define R_CLIENT_HEADER 20
#define R_SERVER_HEADER 12
#define R_MAX_BODY (4+64+4096)
#define R_HELLO_MS 1000
#define R_PING_MS 2000
#define R_LOST_MS 10000
extern int rand_s(unsigned int *);
static SRWLOCK relay_lock=SRWLOCK_INIT;
static struct {
    SOCKET fd;
    int open,resolved,state,ping,rooms,wsa;
    unsigned hellos;
    uint32_t token;
    uint64_t build,nonce,secret,last_hello,last_ping,heard;
    uint8_t room[16];
    char name[128],key[64],message[192];
    LONG generation;
} RS={.fd=INVALID_SOCKET,.ping=-1,.rooms=-1};
static uint32_t rl32(const uint8_t *p){uint32_t x;memcpy(&x,p,4);return x;}
static uint64_t rl64(const uint8_t *p){uint64_t x;memcpy(&x,p,8);return x;}
static void wl32(uint8_t *p,uint32_t x){memcpy(p,&x,4);}
static void wl64(uint8_t *p,uint64_t x){memcpy(p,&x,8);}
static uint64_t relay_random64(void){unsigned a=0,b=0;rand_s(&a);rand_s(&b);return ((uint64_t)a<<32)|b;}
static void relay_set_state(int state,const char *message) {
    if(RS.state!=state||strcmp(RS.message,message))printf("[relay] state=%d %s\n",state,message);
    RS.state=state;snprintf(RS.message,sizeof RS.message,"%s",message);
}
int relay_requested(void){return zt_relay_mode()&&!zt_requested();}

/* SHA-256("Strikers room key:" + key), first 16 bytes; zeros for no key. strikers_relay.py relay_room_hash does the same. */
static void relay_room_hash(const char *key,uint8_t out[16]) {
    memset(out,0,16);
    while(*key&&isspace((unsigned char)*key))key++;
    size_t n=strlen(key);while(n&&isspace((unsigned char)key[n-1]))n--;
    if(!n)return;
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;BYTE digest[32];DWORD size=sizeof digest;
    static const char prefix[]="Strikers room key:";
    if(CryptAcquireContextA(&provider,NULL,NULL,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)&&CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)&&
       CryptHashData(hash,(const BYTE*)prefix,(DWORD)(sizeof prefix-1),0)&&CryptHashData(hash,(const BYTE*)key,(DWORD)n,0)&&
       CryptGetHashParam(hash,HP_HASHVAL,digest,&size,0)&&size==32)memcpy(out,digest,16);
    else memset(out,0xff,16); /* no SHA-256: a room nobody else can be in, never the public room */
    if(hash)CryptDestroyHash(hash);if(provider)CryptReleaseContext(provider,0);
}
/* Lock held. A datagram to the relay: header, body. */
static int relay_datagram(int type,const void *a,unsigned an,const void *b,unsigned bn) {
    uint8_t buf[R_CLIENT_HEADER+R_MAX_BODY];
    if(!RS.open||!RS.resolved||RS.fd==INVALID_SOCKET||an+bn>R_MAX_BODY)return -1;
    memcpy(buf,"SRL1",4);buf[4]=(uint8_t)type;buf[5]=R_PROTOCOL;buf[6]=buf[7]=0;
    wl32(buf+8,type==R_HELLO?0:RS.token);wl64(buf+12,type==R_HELLO?0:RS.secret);
    if(an)memcpy(buf+R_CLIENT_HEADER,a,an);
    if(bn)memcpy(buf+R_CLIENT_HEADER+an,b,bn);
    return send(RS.fd,(const char*)buf,(int)(R_CLIENT_HEADER+an+bn),0);
}
static void relay_new_session(void){RS.token=0;RS.secret=0;RS.nonce=relay_random64();RS.hellos=0;RS.last_hello=0;}

typedef struct {LONG generation;char host[128],port[8];} RelayResolve;
/* Finds the relay's address, then connects the socket to it (only the relay's datagrams are received). Retries every
 * 10 s while it cannot; stops when the relay is closed or opened again (generation). */
static DWORD WINAPI relay_resolver(void *arg) {
    RelayResolve *r=arg;
    for(;;) {
        struct addrinfo hints={0},*found=NULL;hints.ai_family=AF_INET;hints.ai_socktype=SOCK_DGRAM;hints.ai_protocol=IPPROTO_UDP;
        int error=getaddrinfo(r->host,r->port,&hints,&found);
        AcquireSRWLockExclusive(&relay_lock);
        if(RS.generation!=r->generation||!RS.open) {ReleaseSRWLockExclusive(&relay_lock);if(found)freeaddrinfo(found);break;}
        if(!error&&found&&connect(RS.fd,found->ai_addr,(int)found->ai_addrlen)==0) {
            RS.resolved=1;relay_new_session();relay_set_state(1,"Connecting to the internet rooms server...");
            ReleaseSRWLockExclusive(&relay_lock);freeaddrinfo(found);break;
        }
        relay_set_state(3,"Can't reach the internet rooms server: it may not be online yet, or this PC has no internet. Rooms on the same network still work.");
        ReleaseSRWLockExclusive(&relay_lock);if(found)freeaddrinfo(found);
        for(int i=0;i<40;i++){Sleep(250);if(InterlockedCompareExchange(&RS.generation,0,0)!=r->generation)break;}
        if(InterlockedCompareExchange(&RS.generation,0,0)!=r->generation)break;
    }
    free(r);return 0;
}
int relay_open(uint64_t build) {
    AcquireSRWLockExclusive(&relay_lock);
    if(RS.open){ReleaseSRWLockExclusive(&relay_lock);return 1;}
    char name[128];const char *v=getenv("STRIKERS_RELAY");
    if(v)snprintf(name,sizeof name,"%s",v);else zt_ini_string("relay","",name,sizeof name);
    if(!name[0])snprintf(name,sizeof name,"%s",STRIKERS_RELAY_DEFAULT);
    snprintf(RS.name,sizeof RS.name,"%s",name);
    if(!RS.key[0]){v=getenv("STRIKERS_ROOM_KEY");if(v)snprintf(RS.key,sizeof RS.key,"%s",v);else zt_ini_string("room_key","",RS.key,sizeof RS.key);}
    relay_room_hash(RS.key,RS.room);RS.build=build;RS.ping=-1;RS.rooms=-1;
    WSADATA w;RS.wsa=WSAStartup(MAKEWORD(2,2),&w)==0;
    RS.fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    struct sockaddr_in any={0};any.sin_family=AF_INET;
    u_long nonblock=1;BOOL no=FALSE;DWORD ignored;
    if(RS.fd==INVALID_SOCKET||bind(RS.fd,(struct sockaddr*)&any,sizeof any)||ioctlsocket(RS.fd,FIONBIO,&nonblock)) {
        if(RS.fd!=INVALID_SOCKET)closesocket(RS.fd);RS.fd=INVALID_SOCKET;if(RS.wsa){WSACleanup();RS.wsa=0;}
        relay_set_state(3,"Internet rooms could not open a network socket.");ReleaseSRWLockExclusive(&relay_lock);return 0;
    }
    WSAIoctl(RS.fd,SIO_UDP_CONNRESET,&no,sizeof no,NULL,0,&ignored,NULL,NULL);
    RS.open=1;RS.resolved=0;relay_new_session();
    LONG generation=InterlockedIncrement(&RS.generation);
    RelayResolve *r=calloc(1,sizeof *r);char *colon=strrchr(name,':');
    if(!name[0]||!r) {free(r);relay_set_state(3,"No internet rooms server is set in strikers.ini. Rooms on the same network still work.");}
    else {
        r->generation=generation;
        if(colon){*colon=0;snprintf(r->port,sizeof r->port,"%s",colon+1);}else snprintf(r->port,sizeof r->port,"%d",RELAY_DEFAULT_PORT);
        snprintf(r->host,sizeof r->host,"%s",name);
        relay_set_state(1,"Connecting to the internet rooms server...");
        HANDLE t=CreateThread(NULL,0,relay_resolver,r,0,NULL);
        if(t)CloseHandle(t);else {free(r);relay_set_state(3,"Internet rooms could not start.");}
    }
    printf("[relay] open server=%s room=%s\n",RS.name,RS.key[0]?"key":"public");
    ReleaseSRWLockExclusive(&relay_lock);return 1;
}
void relay_close(void) {
    AcquireSRWLockExclusive(&relay_lock);
    if(RS.open) {
        if(RS.token)relay_datagram(R_BYE,NULL,0,NULL,0);
        closesocket(RS.fd);RS.fd=INVALID_SOCKET;if(RS.wsa){WSACleanup();RS.wsa=0;}
        RS.open=0;RS.resolved=0;RS.token=0;RS.ping=-1;RS.rooms=-1;InterlockedIncrement(&RS.generation);
        relay_set_state(0,"Internet rooms connect when you open Local Wireless.");
    }
    ReleaseSRWLockExclusive(&relay_lock);
}
SOCKET relay_socket(void){AcquireSRWLockShared(&relay_lock);SOCKET s=RS.open?RS.fd:INVALID_SOCKET;ReleaseSRWLockShared(&relay_lock);return s;}
void relay_tick(uint64_t now) {
    AcquireSRWLockExclusive(&relay_lock);
    if(RS.open&&RS.resolved) {
        if(!RS.token) {
            if(!RS.last_hello||now-RS.last_hello>=R_HELLO_MS) {
                uint8_t body[32];wl64(body,RS.build);memcpy(body+8,RS.room,16);wl64(body+24,RS.nonce);
                relay_datagram(R_HELLO,body,sizeof body,NULL,0);RS.last_hello=now;
                if(++RS.hellos==5)relay_set_state(3,"The internet rooms server does not answer. Rooms on the same network still work.");
            }
        } else {
            if(now-RS.last_ping>=R_PING_MS){uint8_t t[8];wl64(t,now);relay_datagram(R_PING,t,8,NULL,0);RS.last_ping=now;}
            if(now-RS.heard>R_LOST_MS){relay_new_session();RS.ping=-1;relay_set_state(1,"Lost the internet rooms server; reconnecting...");}
        }
    }
    ReleaseSRWLockExclusive(&relay_lock);
}
int relay_recv(uint32_t *token,void *data,unsigned cap) {
    uint8_t b[R_SERVER_HEADER+R_MAX_BODY+1];int result=-1;
    AcquireSRWLockExclusive(&relay_lock);
    while(RS.open&&RS.resolved&&result<0) {
        int n=recv(RS.fd,(char*)b,sizeof b,0);
        if(n<0){if(WSAGetLastError()==WSAECONNRESET||WSAGetLastError()==WSAEMSGSIZE)continue;break;}
        if(n<R_SERVER_HEADER||memcmp(b,"SRL1",4))continue;
        uint64_t now=GetTickCount64();int type=b[4];uint32_t field=rl32(b+8);unsigned size=(unsigned)n-R_SERVER_HEADER;const uint8_t *p=b+R_SERVER_HEADER;
        if(type==R_WELCOME) {
            if(!RS.token&&size>=16&&rl64(p+8)==RS.nonce&&RELAY_IS_TOKEN(field)) {
                RS.token=field;RS.secret=rl64(p);RS.heard=now;RS.last_ping=0;
                relay_set_state(2,RS.key[0]?"Connected. Internet rooms with your room key show up in Local Wireless.":"Connected. Internet rooms show up in Local Wireless.");
                printf("[relay] session %06x\n",field);
            }
            continue;
        }
        if(!RS.token)continue;
        RS.heard=now;
        if(type==R_PONG&&size>=8){uint64_t sent=rl64(p);if(sent<=now&&now-sent<60000)RS.ping=(int)(now-sent);}
        else if(type==R_LIST_END)RS.rooms=(int)field;
        else if(type==R_ERROR) {
            if(field==1){relay_new_session();relay_set_state(1,"Reconnecting to the internet rooms server...");}
            else if(field==2)relay_set_state(3,"The internet rooms server runs another version. Rooms on the same network still work.");
            else if(field==3)relay_set_state(3,"The internet rooms server is full. Try again later.");
        } else if((type==R_DATA||type==R_ROOM)&&RELAY_IS_TOKEN(field)&&size<=cap) {
            memcpy(data,p,size);*token=field;result=(int)size;
        }
    }
    ReleaseSRWLockExclusive(&relay_lock);return result;
}
int relay_send(uint32_t token,const void *data,unsigned size) {
    AcquireSRWLockExclusive(&relay_lock);int result=-1;
    if(RS.token&&RELAY_IS_TOKEN(token)){uint8_t dst[4];wl32(dst,token);if(relay_datagram(R_SEND,dst,4,data,size)>=0)result=(int)size;}
    ReleaseSRWLockExclusive(&relay_lock);return result;
}
int relay_endpoint(uint32_t token){AcquireSRWLockShared(&relay_lock);int ok=RS.open&&RELAY_IS_TOKEN(token);ReleaseSRWLockShared(&relay_lock);return ok;}
void relay_host(const void *advert,unsigned size){AcquireSRWLockExclusive(&relay_lock);if(RS.token)relay_datagram(R_HOST,advert,size,NULL,0);ReleaseSRWLockExclusive(&relay_lock);}
void relay_unhost(void){AcquireSRWLockExclusive(&relay_lock);if(RS.token)relay_datagram(R_UNHOST,NULL,0,NULL,0);ReleaseSRWLockExclusive(&relay_lock);}
void relay_list(void){AcquireSRWLockExclusive(&relay_lock);if(RS.token)relay_datagram(R_LIST,NULL,0,NULL,0);ReleaseSRWLockExclusive(&relay_lock);}
void relay_status(RelayStatus *out) {
    AcquireSRWLockShared(&relay_lock);
    memset(out,0,sizeof *out);
    out->state=RS.state;out->ping_ms=RS.ping;out->rooms=RS.rooms;out->keyed=RS.key[0]!=0;
    snprintf(out->server,sizeof out->server,"%s",RS.name);
    snprintf(out->message,sizeof out->message,"%s",RS.message[0]?RS.message:"Internet rooms connect when you open Local Wireless.");
    ReleaseSRWLockShared(&relay_lock);
}
void relay_set_room_key(const char *key) {
    AcquireSRWLockExclusive(&relay_lock);
    snprintf(RS.key,sizeof RS.key,"%s",key?key:"");relay_room_hash(RS.key,RS.room);
    if(RS.open&&RS.resolved){if(RS.token)relay_datagram(R_BYE,NULL,0,NULL,0);relay_new_session();RS.ping=-1;RS.rooms=-1;relay_set_state(1,"Connecting with the new room key...");}
    ReleaseSRWLockExclusive(&relay_lock);
}
void relay_room_key(char *out,unsigned cap) {
    AcquireSRWLockShared(&relay_lock);
    if(RS.key[0]||RS.open)snprintf(out,cap,"%s",RS.key);
    else {ReleaseSRWLockShared(&relay_lock);const char *v=getenv("STRIKERS_ROOM_KEY");if(v)snprintf(out,cap,"%s",v);else zt_ini_string("room_key","",out,cap);return;}
    ReleaseSRWLockShared(&relay_lock);
}
