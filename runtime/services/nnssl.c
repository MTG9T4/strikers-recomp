/* SPDX-License-Identifier: GPL-3.0-or-later
 * Upstream: smo-recomp/runtime/services/nnsocket.inc (0414341471307605b8cb5d0c1a9928db5fe84e07).
 * SSL object scaffolding. Network operations deliberately fail offline; no TLS session exists. */
#include <winsock2.h>
#include <stdlib.h>
#include "network_policy.h"
#include "nnssl_online.inc"
#define P(v) ((uint64_t)(uintptr_t)(v))
#define M32(a) (*(uint32_t *)(uintptr_t)(a))
#define M64(a) (*(uint64_t *)(uintptr_t)(a))
/* ================================================================== nn::ssl */
#define SSL_NOT_INIT 0xca7bu
#define SSL_BAD_CONTEXT 0xdc7bu
#define SSL_BAD_CONNECTION 0xda7bu
#define SSL_BAD_POINTER 0xe27bu
typedef struct SslCtx { uint32_t magic; int connections; } SslCtx;
typedef struct SslConn { uint32_t magic; SslCtx *ctx; int fd; int keep_socket; LocalTls *tls; } SslConn;
#define SSL_CTX_MAGIC 0x43584c53u
#define SSL_CONN_MAGIC 0x4e434c53u
static uint64_t g_ssl_count;                              /* [sdk] .bss 0xcd10a8 */
static int g_ssl_session;
static uint64_t ssl_scramble(uint64_t p) { return (p >> 48) | ((p >> 16) & 0xffff0000ull) | ((p << 16) & 0xffff00000000ull) | (p << 48); }
static void *ssl_chunk_object(uint64_t scrambled) {       /* the service object behind a context / connection value, or NULL */
    uint64_t chunk = ssl_scramble(scrambled);
    return chunk ? (void *)(uintptr_t)M64(chunk) : NULL;
}
static void nnssl_Initialize(Ctx *c) {
    int first = g_ssl_count == 0;
    g_ssl_count++;
    if (!first || g_ssl_session) { RET(0x1927bu); return; }
    g_ssl_session = 1;
    RET(0);
}
static void ssl_Context_Create(Ctx *c) {                  /* (this, SslVersion) */
    uint64_t self = A0;
    if(strikers_online_requested())printf("[online-tls] Context::Create service=%d version=%x\n",g_ssl_session,(unsigned)A1);
    if (!g_ssl_session) { RET(SSL_NOT_INIT); return; }
    uint64_t *chunk = calloc(1, 8);
    SslCtx *x = calloc(1, sizeof *x);
    x->magic = SSL_CTX_MAGIC;
    *chunk = P(x);
    M64(self) = ssl_scramble(P(chunk));
    RET(0);
}
/* audit 05: a use of a connection's TLS session for one call (nnssl_online.inc, users/closing) */
static LocalTls *tls_use(SslConn *n) {AcquireSRWLockShared(&g_tls_life);LocalTls *t=n?n->tls:NULL;if(t)InterlockedIncrement(&t->users);ReleaseSRWLockShared(&g_tls_life);return t;}
static void tls_unuse(LocalTls *t) {if(t)InterlockedDecrement(&t->users);}
static SslCtx *ssl_ctx(uint64_t value) { SslCtx *x = ssl_chunk_object(value); return x && x->magic == SSL_CTX_MAGIC ? x : NULL; }
static SslConn *ssl_conn(uint64_t value) { SslConn *x = ssl_chunk_object(value); return x && x->magic == SSL_CONN_MAGIC ? x : NULL; }
static void ssl_Context_ImportServerPki(Ctx *c) {         /* (this, uint64_t *outId, const char *data, uint32_t size, format) */
    if (!g_ssl_session) { RET(SSL_NOT_INIT); return; }
    if (!A1 || !A2) { RET(SSL_BAD_POINTER); return; }
    if (!ssl_ctx(M64(A0))) { RET(SSL_BAD_CONTEXT); return; }
    if(strikers_online_requested()) {
        if(!strikers_online_enabled()||!A3||A3>16384){RET(SSL_LOCAL_TLS_ERROR);return;}
        PCCERT_CONTEXT ca=tls_ca();if(!ca){RET(SSL_LOCAL_TLS_ERROR);return;}CertFreeCertificateContext(ca);
        printf("[online-tls] ImportServerPki bytes=%u replaced by process-private CA; verification mandatory\n",(unsigned)A3);
    }
    M64(A1) = 0;
    RET(0);
}
static void nnssl_ContextDestroy(Ctx *c) {
    uint64_t self = A0;
    if (!self) { RET(SSL_BAD_POINTER); return; }
    if (!g_ssl_session) { RET(SSL_NOT_INIT); return; }
    SslCtx *x = ssl_ctx(M64(self));
    if (!x) { RET(SSL_BAD_CONTEXT); return; }
    if (x->connections) { RET(0xd67bu); return; }
    free((void *)(uintptr_t)ssl_scramble(M64(self)));
    x->magic = 0; free(x);
    M64(self) = 0;
    RET(0);
}
static void nnssl_ContextGetContextId(Ctx *c) {
    if (!A0 || !A1) { RET(SSL_BAD_POINTER); return; }
    M64(A1) = M64(A0);
    RET(0);
}
static void nnssl_ConnectionCreate(Ctx *c) {              /* (connection, context) */
    uint64_t self = A0, ctx = A1;
    if(strikers_online_requested())printf("[online-tls] ConnectionCreate\n");
    if (!self || !ctx) { RET(SSL_BAD_POINTER); return; }
    M64(self) = 0; M64(self + 8) = 0; M32(self + 0x10) = 0xffffffffu;                 /* [sdk 0x383c70] constructor */
    if (!g_ssl_session) { RET(SSL_NOT_INIT); return; }
    SslCtx *x = ssl_ctx(M64(ctx));
    if (!x) { RET(SSL_BAD_CONTEXT); return; }
    uint64_t *chunk = calloc(1, 8);
    SslConn *n = calloc(1, sizeof *n);
    n->magic = SSL_CONN_MAGIC; n->ctx = x; n->fd = -1;
    x->connections++;
    *chunk = P(n);
    M64(self) = ssl_scramble(P(chunk));
    M64(self + 8) = M64(ctx);
    RET(0);
}
/* common front of every Connection call: null object, not initialised, invalid connection */
static SslConn *ssl_conn_enter(Ctx *c, uint64_t self) {
    if (!self) { RET(SSL_BAD_POINTER); return NULL; }
    if (!g_ssl_session) { RET(SSL_NOT_INIT); return NULL; }
    SslConn *n = ssl_conn(M64(self));
    if (!n) RET(SSL_BAD_CONNECTION);
    return n;
}
static void nnssl_ConnectionDestroy(Ctx *c) {
    uint64_t self = A0;
    SslConn *n = ssl_conn_enter(c, self);
    if (!n) return;
    AcquireSRWLockExclusive(&g_tls_life);LocalTls *t=n->tls;n->tls=NULL;ReleaseSRWLockExclusive(&g_tls_life);
    tls_free(t);                                           /* waits for the calls still using it (audit 05) */
    if(strikers_online_requested()&&n->fd>=0&&!n->keep_socket)strikers_online_socket_close(n->fd);
    if ((int32_t)M32(self + 0x10) >= 0) { /* offline: no descriptor exists */ M32(self + 0x10) = 0xffffffffu; }   /* [sdk 0x383e58] nn::socket::Close */
    if (n->fd >= 0 && n->fd != (int)M32(self + 0x10)) (void)n->fd;              /* the module closes its duplicate */
    n->ctx->connections--;
    free((void *)(uintptr_t)ssl_scramble(M64(self)));
    n->magic = 0; free(n);
    M64(self) = 0;
    RET(0);
}
static void nnssl_ConnectionSetSocketDescriptor(Ctx *c) { /* (connection, fd) */
    uint64_t self = A0;
    SslConn *n = ssl_conn_enter(c, self);
    if (!n) return;
    if(strikers_online_requested()) {
        if(n->tls&&n->tls->socket!=INVALID_SOCKET){RET(SSL_BAD_CONNECTION);return;}
        LocalTls *t=n->tls?n->tls:tls_new();if(!t){RET(SSL_LOCAL_TLS_ERROR);return;}
        t->socket=(SOCKET)strikers_online_socket_duplicate((int)A1);
        if(t->socket==INVALID_SOCKET){tls_free(t);n->tls=NULL;RET(0xce7bu);return;}
        n->tls=t;n->fd=(int)A1;M32(self+0x10)=(uint32_t)A1;RET(0);return;
    }
    RET(0xce7bu); return; /* offline: no connected socket */
    RET(0);
}
static void nnssl_ConnectionSetHostName(Ctx *c) {         /* (connection, const char *, uint32_t) */
    if (!A0) { RET(SSL_BAD_POINTER); return; }
    if (!g_ssl_session) { RET(SSL_NOT_INIT); return; }
    if (!A1) { RET(SSL_BAD_POINTER); return; }
    if (!ssl_conn(M64(A0))) { RET(SSL_BAD_CONNECTION); return; }
    if(strikers_online_requested()) {
        SslConn *n=ssl_conn(M64(A0));
        if(!A2||A2>=254){RET(SSL_BAD_POINTER);return;}
        char hostname[254];memcpy(hostname,PTR(A1),(size_t)A2);hostname[A2]=0;
        if(!strikers_online_hostname(hostname)||memchr(hostname,0,(size_t)A2-1)){RET(SSL_LOCAL_TLS_ERROR);return;}
        if(!n->tls)n->tls=tls_new();if(!n->tls){RET(SSL_LOCAL_TLS_ERROR);return;}
        snprintf(n->tls->hostname,sizeof n->tls->hostname,"%s",hostname);
    }
    RET(0);
}
static void nnssl_ConnectionSimpleOk(Ctx *c) { if (ssl_conn_enter(c, A0)) RET(0); }   /* SetVerifyOption, SetSessionCacheMode */
static void nnssl_ConnectionSetIoMode(Ctx *c) {           /* (connection, IoMode: 1 blocking, 2 non-blocking) */
    SslConn *n = ssl_conn_enter(c, A0);
    if (!n) return;
    if(strikers_online_requested()) {
        if(A1!=1&&A1!=2){RET(SSL_BAD_POINTER);return;}
        if(!n->tls)n->tls=tls_new();if(!n->tls){RET(SSL_BAD_CONNECTION);return;}
        n->tls->io_mode=(int)A1;RET(0);return;
    }
    if (n->fd < 0) { RET(0xce7bu); return; }
    RET(0);
}
static void nnssl_ConnectionSetOption(Ctx *c) {           /* (connection, OptionType, bool) */
    SslConn *n = ssl_conn_enter(c, A0);
    if (!n) return;
    if ((uint32_t)A1 == 0) n->keep_socket = (int)(A2 & 1);                             /* [suyu] OptionType::DoNotCloseSocket = 0 */
    RET(0);
}
static void ssl_needs_tls(Ctx *c, const char *what) {
    printf("\n%s on an nn::ssl connection needs a TLS session over a connected socket. The runtime has no TLS implementation\n"
           "(without a network only a loopback peer could be connected at all).\n", what);
    guest_trap("nn::ssl: TLS is not implemented", c->x[30] - 4);
}
static void nnssl_ConnectionDoHandshake(Ctx *c) {         /* (connection, uint32_t *outCertSize, uint32_t *outCertCount) */
    uint64_t self = A0, osize = A1, ocount = A2;
    if (!self) { RET(SSL_BAD_POINTER); return; }
    if (!g_ssl_session) { RET(SSL_NOT_INIT); return; }
    if (!ssl_conn(M64(self))) { RET(SSL_BAD_CONNECTION); return; }
    if(strikers_online_requested()) {
        LocalTls *t=tls_use(ssl_conn(M64(self)));int ok=tls_handshake(t);tls_unuse(t);
        if(osize)M32(osize)=0;if(ocount)M32(ocount)=0;RET(ok?0:SSL_LOCAL_TLS_ERROR);return;
    }
    if (ssl_conn(M64(self))->fd >= 0) ssl_needs_tls(c, "DoHandshake");
    if (!(M64(self + 0x18) && M32(self + 0x20))) {        /* [sdk 0x384be0..0x384c40] no certificate buffer: plain DoHandshake, outputs zeroed */
        if (osize) M32(osize) = 0;
        if (ocount) M32(ocount) = 0;
    }
    RET(0xce7bu);                                         /* [suyu] ResultNoSocket */
}
static void ssl_io(Ctx *c, const char *what) {            /* (connection, buffer, int *outSize, uint32_t size) */
    uint64_t self = A0;
    uint32_t r;
    if (!self) { RET(SSL_BAD_POINTER); return; }
    if (!g_ssl_session) r = SSL_NOT_INIT;
    else if (!A1 || !A2) r = SSL_BAD_POINTER;
    else if (!(uint32_t)A3) r = 0xe07bu;
    else if (!ssl_conn(M64(self))) r = SSL_BAD_CONNECTION;
    else if(strikers_online_requested()) {
        LocalTls *t=tls_use(ssl_conn(M64(self)));
        int n=!strcmp(what,"Write")?tls_write(t,PTR(A1),(unsigned)A3):tls_read(t,PTR(A1),(unsigned)A3,!strcmp(what,"Peek"));
        tls_unuse(t);
        /* main+ae788/+aee5c/+aeefc identify SDK13.3 WouldBlock=0x1987b. */
        if(n==-2)r=0x1987bu;else if(n<0)r=SSL_LOCAL_TLS_ERROR;else{M32(A2)=n;r=0;}
    } else { ssl_needs_tls(c, what); r = 0; }
    M32(self + 0x24) = r;
    RET(r);
}
static void nnssl_ConnectionRead(Ctx *c) { ssl_io(c, "Read"); }
static void nnssl_ConnectionWrite(Ctx *c) { ssl_io(c, "Write"); }
static void nnssl_ConnectionPending(Ctx *c) {             /* (connection, int *out) */
    uint64_t self = A0;
    uint32_t r;
    if (!self) { RET(SSL_BAD_POINTER); return; }
    if (!g_ssl_session) r = SSL_NOT_INIT;
    else if (!ssl_conn(M64(self))) r = SSL_BAD_CONNECTION;
    else if (!A1) r = SSL_BAD_POINTER;
    else { LocalTls *t = tls_use(ssl_conn(M64(self))); M32(A1) = tls_pending(t); tls_unuse(t); r = 0; }
    M32(self + 0x24) = r;
    RET(r);
}
static void nnssl_ConnectionGetVerifyCertError(Ctx *c) {  /* (connection, Result *out) */
    if (!ssl_conn_enter(c, A0)) return;
    if (!A1) { RET(SSL_BAD_POINTER); return; }
    LocalTls *t=ssl_conn(M64(A0))->tls;uint32_t r=strikers_online_requested()?(t&&t->verified?0:SSL_LOCAL_TLS_ERROR):0xce7bu;
    M32(A1)=r;RET(r);
}
static void nnssl_ConnectionGetConnectionId(Ctx *c) {
    if (!A0 || !A1) { RET(SSL_BAD_POINTER); return; }
    M64(A1) = M64(A0);
    RET(0);
}


static void nnssl_Finalize(Ctx *c) { if(g_ssl_count) g_ssl_count--; if(!g_ssl_count) g_ssl_session=0; RET(0); }
static void nnssl_Peek(Ctx *c) { ssl_io(c,"Peek"); }
static void nnssl_AlpnSet(Ctx *c) {
    SslConn *n=ssl_conn_enter(c,A0);if(!n)return;
    if(!strikers_online_requested()){RET(0xce7bu);return;}
    if(!A1||!A2||A2>255){RET(SSL_BAD_POINTER);return;}
    const uint8_t *p=PTR(A1);unsigned pos=0;
    while(pos<A2){unsigned size=p[pos++];if(!size||pos+size>A2){RET(SSL_BAD_POINTER);return;}pos+=size;}
    if(!n->tls)n->tls=tls_new();if(!n->tls||n->tls->ssl){RET(SSL_BAD_CONNECTION);return;}
    memcpy(n->tls->alpn,p,(size_t)A2);n->tls->alpn_size=(unsigned)A2;RET(0);
}
static void nnssl_AlpnGet(Ctx *c) {
    SslConn *n=ssl_conn_enter(c,A0);if(!n)return;
    if(!strikers_online_requested()){RET(0xce7bu);return;}
    if(!A1||!A2||!A3){RET(SSL_BAD_POINTER);return;}
    LocalTls *t=tls_use(n);
    if(!t||!t->verified){tls_unuse(t);RET(SSL_LOCAL_TLS_ERROR);return;}
    const unsigned char *data=NULL;unsigned size=0;AcquireSRWLockExclusive(&t->lock);TlsApi.SSL_get0_alpn_selected(t->ssl,&data,&size);
    int fits=size<=c->x[4];if(fits&&size)memcpy(PTR(A2),data,size);ReleaseSRWLockExclusive(&t->lock);tls_unuse(t);
    if(!fits){RET(SSL_BAD_POINTER);return;}
    wr32(A1,size?1:0);wr32(A3,size);RET(0);
}

static const ImportDef nnssl_imports[] = {
    {"_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ssl_Context_ImportServerPki},
    {"_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ssl_Context_Create},
    {"nnsslConnectionCreate", nnssl_ConnectionCreate},
    {"nnsslConnectionDestroy", nnssl_ConnectionDestroy},
    {"nnsslConnectionDoHandshakeWithCertBuffer", nnssl_ConnectionDoHandshake},
    {"nnsslConnectionGetConnectionId", nnssl_ConnectionGetConnectionId},
    {"nnsslConnectionGetNextAlpnProto", nnssl_AlpnGet},
    {"nnsslConnectionGetVerifyCertError", nnssl_ConnectionGetVerifyCertError},
    {"nnsslConnectionPeek", nnssl_Peek},
    {"nnsslConnectionPending", nnssl_ConnectionPending},
    {"nnsslConnectionRead", nnssl_ConnectionRead},
    {"nnsslConnectionSetHostName", nnssl_ConnectionSetHostName},
    {"nnsslConnectionSetIoMode", nnssl_ConnectionSetIoMode},
    {"nnsslConnectionSetNextAlpnProto", nnssl_AlpnSet},
    {"nnsslConnectionSetOption", nnssl_ConnectionSetOption},
    {"nnsslConnectionSetSessionCacheMode", nnssl_ConnectionSimpleOk},
    {"nnsslConnectionSetSocketDescriptor", nnssl_ConnectionSetSocketDescriptor},
    {"nnsslConnectionSetVerifyOption", nnssl_ConnectionSimpleOk},
    {"nnsslConnectionWrite", nnssl_ConnectionWrite},
    {"nnsslContextDestroy", nnssl_ContextDestroy},
    {"nnsslContextGetContextId", nnssl_ContextGetContextId},
    {"nnsslFinalize", nnssl_Finalize},
    {"nnsslInitialize", nnssl_Initialize},
};
void register_nnssl_imports(void) {
    register_imports(nnssl_imports, sizeof nnssl_imports / sizeof nnssl_imports[0]);
    /* The full SDK initializes its SSL service as part of platform startup.
     * This HLE replaces that service, but main imports only Create/ImportPki,
     * not Initialize (SDK13.3 Create checks its global session at +3cf908).
     * Provision the replacement service at registration, before guest threads.
     * Contexts still require explicit creation; certificate checks stay mandatory. */
    if(strikers_online_enabled()&&!g_ssl_session){Ctx init={0};nnssl_Initialize(&init);printf("[online-tls] local SSL service initialized by runtime\n");}
}
