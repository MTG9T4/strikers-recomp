/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nnsocket_lan.inc"
#include "nnsocket_online.inc"

static const ImportDef nnsocket_imports[] = {
    {"_ZN2nn6socket10GetSockOptEiiiPvPj", so_getopt},
    {"_ZN2nn6socket10InitializeEPvmmi", so_init},
    {"_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", so_setopt},
    {"_ZN2nn6socket10SetSockOptEiiiPKvj", so_setopt},
    {"_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", so_gai_error},
    {"_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", so_dns},
    {"_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", so_dns},
    {"_ZN2nn6socket11GetSockNameEiP8sockaddrPj", so_name},
    {"_ZN2nn6socket12FreeAddrInfoEP8addrinfo", so_free},
    {"_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", so_free},
    {"_ZN2nn6socket12GetLastErrnoEv", so_geterr},
    {"_ZN2nn6socket12GetLastErrorEv", so_geterr},
    {"_ZN2nn6socket4BindEiPK8sockaddrj", so_bind},
    {"_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", so_bind},
    {"_ZN2nn6socket4PollEP6pollfdmi", so_poll},
    {"_ZN2nn6socket4RecvEiPvmi", so_recv},
    {"_ZN2nn6socket4SendEiPKvmi", so_send},
    {"_ZN2nn6socket5CloseEi", so_close},
    {"_ZN2nn6socket5FcntlEiiz", so_fcntl},
    {"_ZN2nn6socket6AcceptEiP8sockaddrPj", so_fail},
    {"_ZN2nn6socket6ListenEii", so_fail},
    {"_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", so_sendto},
    {"_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", so_sendto},
    {"_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", so_socket},
    {"_ZN2nn6socket6SocketEiii", so_socket},
    {"_ZN2nn6socket7ConnectEiPK8sockaddrj", so_connect},
    {"_ZN2nn6socket8InetAtonEPKcPNS0_6InAddrE", so_aton},
    {"_ZN2nn6socket8InetNtoaE7in_addr", so_ntoa},
    {"_ZN2nn6socket8InetNtopEiPKvPcj", so_ntop},
    {"_ZN2nn6socket8InetPtonEiPKcPv", so_pton},
    {"_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", so_recvfrom},
    {"_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", so_recvfrom},
    {"_ZN2nn6socket8ShutdownEii", so_shutdown},
    {"_ZN2nn6socket9InetHtonlEj", so_swap32},
    {"_ZN2nn6socket9InetHtonsEt", so_swap16},
    {"_ZN2nn6socket9InetNtohlEj", so_swap32},
    {"_ZN2nn6socket9InetNtohsEt", so_swap16},
    {"nnsocketBind", so_bind},
    {"nnsocketClose", so_close},
    {"nnsocketConnect", so_connect},
    {"nnsocketFcntl", so_fcntl},
    {"nnsocketGetHostByNameWithOptions", so_host},
    {"nnsocketGetLastErrno", so_geterr},
    {"nnsocketGetPeerName", so_peer},
    {"nnsocketGetSockName", so_name},
    {"nnsocketGetSockOpt", so_getopt},
    {"nnsocketInetHtonl", so_swap32},
    {"nnsocketInetHtons", so_swap16},
    {"nnsocketInetNtohl", so_swap32},
    {"nnsocketInetNtohs", so_swap16},
    {"nnsocketPoll", so_poll},
    {"nnsocketRecv", so_recv},
    {"nnsocketResolverGetOption", so_fail},
    {"nnsocketResolverSetOption", so_fail},
    {"nnsocketSend", so_send},
    {"nnsocketSetLastErrno", so_seterr},
    {"nnsocketSetSockOpt", so_setopt},
    {"nnsocketSocket", so_socket},
};
static void online_perform(Ctx *c) {
    ImportFn fn=NULL;for(unsigned i=0;i<sizeof nnsocket_imports/sizeof nnsocket_imports[0];i++)if(c->in_import&&!strcmp(c->in_import,nnsocket_imports[i].name)){fn=nnsocket_imports[i].fn;break;}
    if(fn==so_socket)on_socket(c);else if(fn==so_close)on_close(c);else if(fn==so_bind)on_bind(c);else if(fn==so_connect)on_connect(c);
    else if(fn==so_send)on_io(c,0,0);else if(fn==so_sendto)on_io(c,0,1);else if(fn==so_recv)on_io(c,1,0);else if(fn==so_recvfrom)on_io(c,1,1);
    else if(fn==so_name)on_name(c,0);else if(fn==so_peer)on_name(c,1);else if(fn==so_fcntl)on_fcntl(c);else if(fn==so_poll)on_poll(c);
    else if(fn==so_setopt)on_option(c,0);else if(fn==so_getopt)on_option(c,1);else if(fn==so_dns)on_dns(c);else if(fn==so_host)on_host(c);
    else if(fn==so_free){free(PTR(A0));RET(0);}
    else if(fn==so_shutdown){AcquireSRWLockShared(&O_lock);if(on_valid((int)A0))on_result(c,shutdown(O[(int)A0].fd,(int)A1));else so_badfd(c);ReleaseSRWLockShared(&O_lock);}
    else if(fn==so_fail)so_error(c,95);else if(fn)fn(c);else guest_trap("online socket: missing import context",c->x[30]-4);
}
static void online_dispatch(Ctx *c) {
    static LONG counts[sizeof nnsocket_imports/sizeof nnsocket_imports[0]];
    int log=0;if(getenv("STRIKERS_ONLINE_TRACE"))for(unsigned i=0;i<sizeof counts/sizeof counts[0];i++)if(c->in_import&&!strcmp(c->in_import,nnsocket_imports[i].name)){log=InterlockedIncrement(&counts[i])<=32;break;}
    if(log)printf("[online-api] %s args=%llx,%llx,%llx,%llx,%llx caller=%llx\n",c->in_import,c->x[0],c->x[1],c->x[2],c->x[3],c->x[4],c->x[30]-4);
    online_perform(c);
    if(log)printf("[online-api] %s result=%llx errno=%d\n",c->in_import,c->x[0],*so_errno(c));
}
static void so_trace(Ctx *c) {
    static LONG counts[sizeof nnsocket_imports/sizeof nnsocket_imports[0]];
    for(unsigned i=0;i<sizeof nnsocket_imports/sizeof nnsocket_imports[0];i++) if(c->in_import && !strcmp(c->in_import,nnsocket_imports[i].name)) {
        int log=InterlockedIncrement(&counts[i])<=16;
        if(log)printf("[lan-api] %s args=%llx,%llx,%llx,%llx,%llx\n",c->in_import,c->x[0],c->x[1],c->x[2],c->x[3],c->x[4]);
        nnsocket_imports[i].fn(c);
        if(log)printf("[lan-api] %s result=%llx errno=%d\n",c->in_import,c->x[0],*so_errno(c));
        return;
    }
    guest_trap("socket trace: missing import context",c->x[30]-4);
}
void register_nnsocket_imports(void) {
    if(strikers_online_requested()) {
        ImportDef online[sizeof nnsocket_imports/sizeof nnsocket_imports[0]];memcpy(online,nnsocket_imports,sizeof online);
        for(unsigned i=0;i<sizeof online/sizeof online[0];i++)online[i].fn=online_dispatch;
        register_imports(online,sizeof online/sizeof online[0]);
    } else if(getenv("STRIKERS_LDN_TRACE")) {
        ImportDef traced[sizeof nnsocket_imports/sizeof nnsocket_imports[0]];
        memcpy(traced,nnsocket_imports,sizeof traced);
        for(unsigned i=0;i<sizeof traced/sizeof traced[0];i++)traced[i].fn=so_trace;
        register_imports(traced,sizeof traced/sizeof traced[0]);
    } else register_imports(nnsocket_imports,sizeof nnsocket_imports/sizeof nnsocket_imports[0]);
}
