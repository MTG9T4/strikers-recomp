/* SPDX-License-Identifier: GPL-3.0-or-later
 * Online sockets are a separate descriptor table. Every Connect/SendTo checks
 * network_policy before any Winsock call. No native resolver is ever invoked.
 * SDK sockaddr/pollfd/errno use the same translations as the LAN implementation.
 */
typedef struct {SOCKET fd;int used,type,flags,connected;unsigned timeout,generation;} OnlineFd;
static OnlineFd O[SO_FDS];
static SRWLOCK O_lock=SRWLOCK_INIT;
static INIT_ONCE O_once=INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK on_init(PINIT_ONCE o,PVOID p,PVOID *ctx) {(void)o;(void)p;(void)ctx;WSADATA w;return WSAStartup(MAKEWORD(2,2),&w)==0;}
static int on_error_code(void) {
    switch(WSAGetLastError()) {
    case WSAEWOULDBLOCK:return 11;case WSAEINPROGRESS:return 115;case WSAEALREADY:return 114;
    case WSAECONNREFUSED:return 111;case WSAECONNRESET:return 104;case WSAETIMEDOUT:return 110;
    case WSAENETUNREACH:return 101;case WSAEHOSTUNREACH:return 113;case WSAENOTCONN:return 107;
    case WSAEADDRINUSE:return 98;case WSAEADDRNOTAVAIL:return 99;case WSAEMSGSIZE:return 90;
    case WSAENOBUFS:return 105;case WSAEACCES:return 13;case WSAEINVAL:return 22;
    case WSAENOTSOCK:return 9;case WSAEISCONN:return 106;case WSAESHUTDOWN:return 32;
    default:return 5;
    }
}
static int on_valid(int fd) {return fd>=0&&fd<SO_FDS&&O[fd].used;}
static void on_result(Ctx *c,int n) {if(n==SOCKET_ERROR)so_error(c,on_error_code());else RET(n);}
static void on_socket(Ctx *c) {
    if(!strikers_online_enabled()){so_error(c,13);return;}
    if(A0!=2){so_error(c,97);return;}
    int type=(int)A1&~0x80800;if((type!=1&&type!=2)||(A2&&A2!=(type==1?6:17))){so_error(c,93);return;}
    if(!InitOnceExecuteOnce(&O_once,on_init,NULL,NULL)){so_error(c,100);return;}
    AcquireSRWLockExclusive(&O_lock);int fd=-1;
    for(int i=0;i<SO_FDS;i++)if(!O[i].used){fd=i;break;}
    if(fd<0){ReleaseSRWLockExclusive(&O_lock);so_error(c,24);return;}
    SOCKET native=socket(AF_INET,type,0);int e=native==INVALID_SOCKET?on_error_code():0;
    if(!e) {
        /* Bind all outgoing sockets to loopback before Connect or first SendTo.
         * Guest Bind below only confirms/chooses its logical local address. */
        struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(0x7f000001);
        if(bind(native,(struct sockaddr *)&a,sizeof a))e=on_error_code();
        u_long nonblock=1;if(!e&&ioctlsocket(native,FIONBIO,&nonblock))e=on_error_code();
    }
    if(!e){unsigned generation=O[fd].generation+1;O[fd]=(OnlineFd){native,1,type,(int)A1&0x800,0,30000,generation};printf("[online-socket] fd=%d type=%d loopback\n",fd,type);}
    else if(native!=INVALID_SOCKET)closesocket(native);
    ReleaseSRWLockExclusive(&O_lock);if(e)so_error(c,e);else RET(fd);
}
int strikers_online_socket_close(int fd) {int e=0;AcquireSRWLockExclusive(&O_lock);if(!on_valid(fd))e=9;else{unsigned generation=O[fd].generation+1;closesocket(O[fd].fd);memset(&O[fd],0,sizeof O[fd]);O[fd].generation=generation;}ReleaseSRWLockExclusive(&O_lock);return e;}
static void on_close(Ctx *c) {int e=strikers_online_socket_close((int)A0);if(e)so_error(c,e);else RET(0);}
static void on_bind(Ctx *c) {
    uint32_t ip;uint16_t port;int e=so_address(A1,A2,&ip,&port);if(e){so_error(c,e);return;}
    if(ip&&ip!=0x7f000001){so_error(c,13);return;}
    AcquireSRWLockShared(&O_lock);int valid=on_valid((int)A0);ReleaseSRWLockShared(&O_lock);
    /* One ephemeral native endpoint per descriptor avoids collisions between
     * game processes using identical guest ports. GetSockName returns that
     * actual endpoint, including the remapped port, to NAT/Pia. */
    if(!valid)so_badfd(c);else {printf("[online-socket] bind guest-port=%u -> exclusive loopback ephemeral\n",port);RET(0);}
}
static void on_connect(Ctx *c) {
    uint32_t ip;uint16_t port;int e=so_address(A1,A2,&ip,&port);if(e){so_error(c,e);return;}
    if(!strikers_online_destination(ip,port)){printf("[online-policy] refused Connect %08x:%u\n",ip,port);so_error(c,13);return;}
    struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(ip);a.sin_port=htons(port);
    AcquireSRWLockExclusive(&O_lock);int fd=(int)A0;
    if(!on_valid(fd))e=9;else {
        int n=connect(O[fd].fd,(struct sockaddr *)&a,sizeof a);e=n?on_error_code():0;
        if(e==11)e=115;
        if(!e||e==115)O[fd].connected=1;
    }
    ReleaseSRWLockExclusive(&O_lock);if(e)so_error(c,e);else RET(0);
}
static void on_name(Ctx *c,int peer) {
    struct sockaddr_in a={0};int len=sizeof a,e=0,fd=(int)A0;
    if(!A1||!A2){so_error(c,14);return;}
    AcquireSRWLockShared(&O_lock);
    if(!on_valid(fd))e=9;else if((peer?getpeername(O[fd].fd,(struct sockaddr *)&a,&len):getsockname(O[fd].fd,(struct sockaddr *)&a,&len)))e=on_error_code();
    ReleaseSRWLockShared(&O_lock);if(e)so_error(c,e);else {so_address_out(A1,A2,ntohl(a.sin_addr.s_addr),ntohs(a.sin_port));RET(0);}
}
static void on_io(Ctx *c,int receive,int addressed) {
    if((A2&&!A1)||A2>65507||(A3&~(receive?0x82u:0x80u))){so_error(c,A2>65507?90:22);return;}
    struct sockaddr_in a={0};a.sin_family=AF_INET;int len=sizeof a,e=0,n=-1,fd=(int)A0;
    if(addressed&&!receive) {
        uint32_t ip;uint16_t port;e=so_address(c->x[4],c->x[5],&ip,&port);if(e){so_error(c,e);return;}
        if(!strikers_online_destination(ip,port)){printf("[online-policy] refused SendTo %08x:%u\n",ip,port);so_error(c,13);return;}
        a.sin_addr.s_addr=htonl(ip);a.sin_port=htons(port);
    }
    if(receive&&addressed&&c->x[4]&&!c->x[5]){so_error(c,14);return;}
    uint64_t until=GetTickCount64()+30000;int receive_type=0;unsigned generation;SOCKET native=INVALID_SOCKET;
    AcquireSRWLockShared(&O_lock);
    if(!on_valid(fd)){ReleaseSRWLockShared(&O_lock);so_badfd(c);return;}
    generation=O[fd].generation;until=GetTickCount64()+(O[fd].timeout?O[fd].timeout:30000);
    ReleaseSRWLockShared(&O_lock);
    do {
        AcquireSRWLockShared(&O_lock);
        if(!on_valid(fd)||O[fd].generation!=generation)e=9;
        else if(!receive&&!addressed&&!O[fd].connected)e=107;
        else {
            receive_type=O[fd].type;native=O[fd].fd;
            if(receive)n=recvfrom(O[fd].fd,PTR(A1),(int)A2,(A3&2)?MSG_PEEK:0,(struct sockaddr *)&a,&len);
            else if(addressed)n=sendto(O[fd].fd,PTR(A1),(int)A2,0,(struct sockaddr *)&a,len);
            else n=send(O[fd].fd,PTR(A1),(int)A2,0);
            e=n==SOCKET_ERROR?on_error_code():0;
            if(e==11&&((O[fd].flags&0x800)||(A3&0x80))) {ReleaseSRWLockShared(&O_lock);break;}
        }
        ReleaseSRWLockShared(&O_lock);
        uint64_t now=GetTickCount64();if(e!=11||now>=until)break;
        /* audit 05: wait until the socket is ready (at most 50 ms, then the descriptor is checked again), not Sleep(1):
         * a blocked Recv woke a thousand times a second. A descriptor closed meanwhile ends the wait (its generation). */
        WSAPOLLFD w;w.fd=native;w.events=receive?POLLRDNORM:POLLWRNORM;w.revents=0;
        if(WSAPoll(&w,1,(INT)(until-now<50?until-now:50))<0)Sleep(1);
    }while(1);
    /* Reject forged datagrams even on loopback if source is not an allowed
     * server endpoint. There is no peer bypass or LAN forwarding in online. */
    if(!e&&receive&&receive_type==2&&!strikers_online_destination(ntohl(a.sin_addr.s_addr),ntohs(a.sin_port)))e=13;
    if(e)so_error(c,e);else {if(receive&&addressed)so_address_out(c->x[4],c->x[5],ntohl(a.sin_addr.s_addr),ntohs(a.sin_port));RET(n);}
}
static void on_fcntl(Ctx *c) {int fd=(int)A0,e=0,n=0;AcquireSRWLockExclusive(&O_lock);if(!on_valid(fd))e=9;else if(A1==3)n=O[fd].flags;else if(A1==4)O[fd].flags=(int)A2&0x800;else e=22;ReleaseSRWLockExclusive(&O_lock);if(e)so_error(c,e);else RET(n);}
static void on_poll(Ctx *c) {
    if(A1>SO_FDS||(!A0&&A1)){so_error(c,22);return;}
    uint64_t until=(int)A2<0?UINT64_MAX:GetTickCount64()+(unsigned)A2;int ready=0;
    do {
        WSAPOLLFD p[SO_FDS];unsigned count=0;uint16_t indices[SO_FDS];ready=0;
        AcquireSRWLockShared(&O_lock);
        for(unsigned i=0;i<A1;i++) {uint64_t addr=A0+8*i;int fd=(int)rd32(addr);wr16(addr+6,0);if(fd<0)continue;
            if(!on_valid(fd)){wr16(addr+6,0x20);ready++;continue;}
            uint16_t events=rd16(addr+4);indices[count]=(uint16_t)i;p[count].fd=O[fd].fd;p[count].events=(events&0x41?POLLRDNORM:0)|(events&4?POLLWRNORM:0);p[count++].revents=0;
        }
        int n=count?WSAPoll(p,count,0):0;
        if(n>0)for(unsigned j=0;j<count;j++){uint16_t native=p[j].revents,events=rd16(A0+8*indices[j]+4),r=0;if(native&POLLRDNORM)r|=events&0x41;if(native&POLLWRNORM)r|=events&4;if(native&POLLHUP)r|=0x10;if(native&POLLERR)r|=8;if(native&POLLNVAL)r|=0x20;wr16(A0+8*indices[j]+6,r);if(r)ready++;}
        ReleaseSRWLockShared(&O_lock);if(n<0){so_error(c,on_error_code());return;}
        uint64_t now=GetTickCount64();if(ready||now>=until)break;
        /* audit 05: wait for one of them (at most 50 ms; the set is built again under the lock), not Sleep(1) */
        INT slice=(INT)(until-now<50?until-now:50);
        if(!count||WSAPoll(p,count,slice)<0)Sleep(count?1:(DWORD)slice);
    }while(1);RET(ready);
}
static void on_option(Ctx *c,int get) {
    int fd=(int)A0,e=0,level=(int)A1,option=(int)A2,native=0;
    if(!A3||(get&&!c->x[4])){so_error(c,14);return;}
    unsigned len=get?rd32(c->x[4]):(unsigned)c->x[4];int value=0;
    if(len<4){so_error(c,22);return;}
    if(level==0xffff)switch(option){case 4:case 0x200:native=SO_REUSEADDR;break;case 0x1001:native=SO_SNDBUF;break;case 0x1002:native=SO_RCVBUF;break;case 0x1007:native=SO_ERROR;break;case 0x1008:native=SO_TYPE;break;case 0x1005:case 0x1006:break;default:e=92;break;}
    else if(level==0&&option==4)native=IP_TTL;else if(level==6&&option==1)native=TCP_NODELAY;else e=92;
    AcquireSRWLockExclusive(&O_lock);
    if(!on_valid(fd))e=9;
    if(!e&&(option==0x1005||option==0x1006)) {
        if(len<16)e=22;else if(get){wr64(A3,O[fd].timeout/1000);wr64(A3+8,(O[fd].timeout%1000)*1000);wr32(c->x[4],16);}
        else {int64_t sec=(int64_t)rd64(A3),us=(int64_t)rd64(A3+8);if(sec<0||sec>30||us<0||us>=1000000)e=22;else O[fd].timeout=(unsigned)(sec*1000+us/1000);}
    }else if(!e) {
        if(get){int size=4;if(getsockopt(O[fd].fd,level==0?IPPROTO_IP:level==6?IPPROTO_TCP:SOL_SOCKET,native,(char *)&value,&size))e=on_error_code();else{if(level==0xffff&&native==SO_ERROR&&value){WSASetLastError(value);value=on_error_code();}wr32(A3,value);wr32(c->x[4],4);}}
        else if(native==SO_TYPE||native==SO_ERROR)e=92;
        else {value=(int)rd32(A3);if(setsockopt(O[fd].fd,level==0?IPPROTO_IP:level==6?IPPROTO_TCP:SOL_SOCKET,native,(char *)&value,4))e=on_error_code();}
    }
    ReleaseSRWLockExclusive(&O_lock);if(e)so_error(c,e);else RET(0);
}
typedef struct OnlineDns {uint8_t info[48],address[16];} OnlineDns;
static void on_dns(Ctx *c) {
    if(!A3){so_error(c,14);return;}wr64(A3,0);
    if(!strikers_online_hostname(PTR(A0))){printf("[online-policy] DNS refused (unlisted host)\n");RET(8);return;}
    uint16_t port=0;if(A1){char *end;unsigned long p=strtoul(PTR(A1),&end,10);if(!* (char *)PTR(A1)||*end||p>65535){RET(9);return;}port=(uint16_t)p;}
    OnlineDns *d=calloc(1,sizeof *d);if(!d){RET(6);return;}
    uint64_t base=(uint64_t)(uintptr_t)d;wr32(base+4,2);wr32(base+8,A2?rd32(A2+8):0);wr32(base+12,A2?rd32(A2+12):0);
    wr32(base+16,16);wr64(base+32,base+48);d->address[0]=16;d->address[1]=2;d->address[2]=(uint8_t)(port>>8);d->address[3]=(uint8_t)port;d->address[4]=127;d->address[7]=1;
    wr64(A3,base);printf("[online-policy] DNS %s -> configured loopback\n",(char *)PTR(A0));RET(0);
}
static void on_host(Ctx *c) {
    static __declspec(thread) uint64_t entry[4],addresses[2],aliases[1];static __declspec(thread) uint32_t ip;
    if(!strikers_online_hostname(PTR(A0))){RET(0);return;}ip=htonl(strikers_online_config()->server_ip);addresses[0]=(uint64_t)(uintptr_t)&ip;addresses[1]=0;aliases[0]=0;
    entry[0]=A0;entry[1]=(uint64_t)(uintptr_t)aliases;entry[2]=((uint64_t)4<<32)|2;entry[3]=(uint64_t)(uintptr_t)addresses;RET(entry);
}
/* Return an independent handle for TLS. It stays bound to the already checked
 * connected TCP peer. No new Connect occurs in the TLS layer. */
uintptr_t strikers_online_socket_duplicate(int fd) {
    SOCKET result=INVALID_SOCKET;AcquireSRWLockShared(&O_lock);
    if(on_valid(fd)&&O[fd].type==1&&O[fd].connected){WSAPROTOCOL_INFOA p;if(!WSADuplicateSocketA(O[fd].fd,GetCurrentProcessId(),&p))result=WSASocketA(FROM_PROTOCOL_INFO,FROM_PROTOCOL_INFO,FROM_PROTOCOL_INFO,&p,0,0);}
    ReleaseSRWLockShared(&O_lock);return (uintptr_t)result;
}
