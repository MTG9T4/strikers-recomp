/* SPDX-License-Identifier: GPL-3.0-or-later
 * OpenSSL 3 from an explicitly selected existing local installation. Opaque ABI
 * declarations avoid a build dependency/download. No default trust paths are
 * loaded. Each connection has a fresh store containing only our configured CA,
 * mandatory chain/date/EKU/hostname verification and an already checked socket.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
#include <string.h>
#include <limits.h>
#pragma comment(lib,"crypt32.lib")
#define SSL_LOCAL_TLS_ERROR 0xc87bu
extern uintptr_t strikers_online_socket_duplicate(int fd);
extern int strikers_online_socket_close(int fd);
/* users/closing (audit 05): a Read/Write/Peek/Pending/DoHandshake in progress holds a use (taken under g_tls_life, shared,
 * with the connection lookup); ConnectionDestroy marks the session closing under g_tls_life, exclusive, and frees it only
 * after the uses are gone. Before, a blocking Read (up to 5 s of retries) on one thread and Destroy on another freed the
 * session under the reader. */
typedef struct LocalTls {SOCKET socket;char hostname[254];void *ctx,*ssl;int verified,io_mode;SRWLOCK lock;unsigned alpn_size;uint8_t alpn[255];volatile LONG users,closing;} LocalTls;
static SRWLOCK g_tls_life=SRWLOCK_INIT;
static struct {
    HMODULE ssl_dll,crypto_dll;int valid;
    const void *(*TLS_client_method)(void);
    void *(*SSL_CTX_new)(const void *);
    void (*SSL_CTX_free)(void *);
    void (*SSL_CTX_set_verify)(void *,int,void *);
    long (*SSL_CTX_ctrl)(void *,int,long,void *);
    void *(*SSL_CTX_get_cert_store)(void *);
    void *(*SSL_new)(void *);
    void (*SSL_free)(void *);
    int (*SSL_set_fd)(void *,int);
    int (*SSL_set1_host)(void *,const char *);
    long (*SSL_ctrl)(void *,int,long,void *);
    int (*SSL_connect)(void *);
    int (*SSL_get_error)(const void *,int);
    long (*SSL_get_verify_result)(const void *);
    int (*SSL_read)(void *,void *,int);
    int (*SSL_peek)(void *,void *,int);
    int (*SSL_write)(void *,const void *,int);
    int (*SSL_pending)(const void *);
    int (*SSL_set_alpn_protos)(void *,const unsigned char *,unsigned int);
    void (*SSL_get0_alpn_selected)(const void *,const unsigned char **,unsigned int *);
    void *(*d2i_X509)(void **,const unsigned char **,long);
    void (*X509_free)(void *);
    int (*X509_STORE_add_cert)(void *,void *);
    int (*OPENSSL_init_ssl)(uint64_t,const void *);
} TlsApi;
static INIT_ONCE TlsOnce=INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK tls_load(PINIT_ONCE once,PVOID p,PVOID *ctx) {
    (void)once;(void)p;(void)ctx;
    const char *path=strikers_online_config()->openssl_file;
    /* Never load from a working-directory search, PATH or a UNC share. */
    if(strlen(path)<4||path[1]!=':'||(path[2]!='/'&&path[2]!='\\'))return TRUE;
    char drive[4]={path[0],':','\\',0};if(GetDriveTypeA(drive)==DRIVE_REMOTE)return TRUE;
    char crypto[1024];snprintf(crypto,sizeof crypto,"%s",path);
    char *slash=strrchr(crypto,'/'),*back=strrchr(crypto,'\\');if(back&&(!slash||back>slash))slash=back;
    if(!slash)return TRUE;snprintf(slash+1,sizeof crypto-(size_t)(slash+1-crypto),"libcrypto-3.dll");
    TlsApi.crypto_dll=LoadLibraryExA(crypto,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    TlsApi.ssl_dll=LoadLibraryExA(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!TlsApi.crypto_dll||!TlsApi.ssl_dll){printf("[online-tls] explicit OpenSSL library unavailable\n");return TRUE;}
#define TLS_PROC(module,name) do {TlsApi.name=(void *)GetProcAddress(TlsApi.module,#name);if(!TlsApi.name)return TRUE;} while(0)
    TLS_PROC(ssl_dll,TLS_client_method);TLS_PROC(ssl_dll,SSL_CTX_new);TLS_PROC(ssl_dll,SSL_CTX_free);
    TLS_PROC(ssl_dll,SSL_CTX_set_verify);TLS_PROC(ssl_dll,SSL_CTX_ctrl);TLS_PROC(ssl_dll,SSL_CTX_get_cert_store);
    TLS_PROC(ssl_dll,SSL_new);TLS_PROC(ssl_dll,SSL_free);TLS_PROC(ssl_dll,SSL_set_fd);TLS_PROC(ssl_dll,SSL_set1_host);
    TLS_PROC(ssl_dll,SSL_ctrl);TLS_PROC(ssl_dll,SSL_connect);TLS_PROC(ssl_dll,SSL_get_error);TLS_PROC(ssl_dll,SSL_get_verify_result);
    TLS_PROC(ssl_dll,SSL_read);TLS_PROC(ssl_dll,SSL_peek);TLS_PROC(ssl_dll,SSL_write);TLS_PROC(ssl_dll,SSL_pending);
    TLS_PROC(ssl_dll,SSL_set_alpn_protos);TLS_PROC(ssl_dll,SSL_get0_alpn_selected);
    TLS_PROC(crypto_dll,d2i_X509);TLS_PROC(crypto_dll,X509_free);TLS_PROC(crypto_dll,X509_STORE_add_cert);
    TLS_PROC(ssl_dll,OPENSSL_init_ssl);
#undef TLS_PROC
    /* Audit 05: initialise OpenSSL without its configuration file (OPENSSL_INIT_NO_LOAD_CONFIG = 0x80) before any other
     * call. Otherwise the first call loads openssl.cnf from OPENSSL_CONF or the build's OPENSSLDIR, which can load provider
     * and engine modules (code from another path into this process) and set system_default cipher and protocol policy
     * for every context. The trust store and the TLS 1.2 minimum below are set per context in any case. */
    if(TlsApi.OPENSSL_init_ssl(0x80,NULL)!=1){printf("[online-tls] OpenSSL initialisation without its configuration file failed%c",10);return TRUE;}
    TlsApi.valid=1;return TRUE;
}
static LocalTls *tls_new(void) {LocalTls *t=calloc(1,sizeof *t);if(t){t->socket=INVALID_SOCKET;t->io_mode=1;}return t;}
static void tls_free(LocalTls *t) {   /* the caller has detached t from its connection under g_tls_life (exclusive) */
    if(!t)return;
    InterlockedExchange(&t->closing,1);
    /* uses in progress see closing within one 50 ms poll slice; never free under one (leak instead, after 2 s) */
    for(int i=0;i<2000&&InterlockedCompareExchange(&t->users,0,0);i++)Sleep(1);
    if(InterlockedCompareExchange(&t->users,0,0)){printf("[online-tls] session still in use at Destroy: left allocated%c",10);return;}
    if(t->ssl)TlsApi.SSL_free(t->ssl);if(t->ctx)TlsApi.SSL_CTX_free(t->ctx);
    if(t->socket!=INVALID_SOCKET)closesocket(t->socket);free(t);
}
/* The wait between two retries of a non-blocking OpenSSL call: until the socket is ready (WSAPoll), at most 50 ms. Before:
 * Sleep(1), a thousand wake-ups a second for a blocked reader. */
static void tls_wait(LocalTls *t,int error) {
    WSAPOLLFD p;p.fd=t->socket;p.events=error==3?POLLWRNORM:POLLRDNORM;p.revents=0;
    if(WSAPoll(&p,1,50)<0)Sleep(1);
}
static PCCERT_CONTEXT tls_ca(void) {
    FILE *f=fopen(strikers_online_config()->ca_file,"rb");if(!f)return NULL;
    uint8_t buf[16384];size_t n=fread(buf,1,sizeof buf,f);int extra=fgetc(f);fclose(f);
    if(!n||extra!=EOF||n==sizeof buf)return NULL;
    PCCERT_CONTEXT ca=CertCreateCertificateContext(X509_ASN_ENCODING,buf,(DWORD)n);
    if(!ca&&n>10&&!memcmp(buf,"-----BEGIN",10)) {
        DWORD size=0;if(CryptStringToBinaryA((char *)buf,(DWORD)n,CRYPT_STRING_BASE64HEADER,NULL,&size,NULL,NULL)&&size<=sizeof buf) {
            uint8_t der[16384];if(CryptStringToBinaryA((char *)buf,(DWORD)n,CRYPT_STRING_BASE64HEADER,der,&size,NULL,NULL))ca=CertCreateCertificateContext(X509_ASN_ENCODING,der,size);
        }
    }
    return ca;
}
static int tls_handshake(LocalTls *t) {
    if(t&&t->verified)return 1;
    if(!t||t->socket==INVALID_SOCKET||t->ssl||!strikers_online_hostname(t->hostname))return 0;
    InitOnceExecuteOnce(&TlsOnce,tls_load,NULL,NULL);if(!TlsApi.valid)return 0;
    PCCERT_CONTEXT ca=tls_ca();if(!ca)return 0;
    const unsigned char *bytes=ca->pbCertEncoded;void *cert=TlsApi.d2i_X509(NULL,&bytes,ca->cbCertEncoded);
    CertFreeCertificateContext(ca);if(!cert)return 0;
    t->ctx=TlsApi.SSL_CTX_new(TlsApi.TLS_client_method());int ok=0;
    if(t->ctx&&TlsApi.X509_STORE_add_cert(TlsApi.SSL_CTX_get_cert_store(t->ctx),cert)==1) {
        /* SSL_CTRL_SET_MIN_PROTO_VERSION=123; TLS 1.2 minimum. Fresh context:
         * no SSL_CTX_set_default_verify_paths and no system trust provider. */
        TlsApi.SSL_CTX_set_verify(t->ctx,1,NULL);
        if(TlsApi.SSL_CTX_ctrl(t->ctx,123,0x303,NULL)==1)t->ssl=TlsApi.SSL_new(t->ctx);
    }
    TlsApi.X509_free(cert);if(!t->ssl)return 0;
    if(t->alpn_size&&TlsApi.SSL_set_alpn_protos(t->ssl,t->alpn,t->alpn_size))return 0;
    if(t->socket>(SOCKET)INT_MAX||TlsApi.SSL_set_fd(t->ssl,(int)t->socket)!=1||TlsApi.SSL_set1_host(t->ssl,t->hostname)!=1)return 0;
    TlsApi.SSL_ctrl(t->ssl,33,3,NULL); /* partial writes, accept moving retry buffer; never alters verification */
    /* SSL_CTRL_SET_TLSEXT_HOSTNAME=55; TLSEXT_NAMETYPE_host_name=0. */
    if(TlsApi.SSL_ctrl(t->ssl,55,0,t->hostname)!=1)return 0;
    uint64_t until=GetTickCount64()+5000;
    while(GetTickCount64()<until&&!t->closing) {
        int r=TlsApi.SSL_connect(t->ssl);if(r==1){ok=TlsApi.SSL_get_verify_result(t->ssl)==0;break;}
        int error=TlsApi.SSL_get_error(t->ssl,r);if(error!=2&&error!=3)break;tls_wait(t,error);
    }
    t->verified=ok;
    printf("[online-tls] %s private-CA chain/date/EKU/hostname %s; OpenSSL verify=%ld (system roots excluded)\n",ok?"verified":"refused",t->hostname,TlsApi.SSL_get_verify_result(t->ssl));
    return ok;
}
static int tls_io(LocalTls *t,void *buf,unsigned n,int mode) {     /* the caller holds a use of t */
    if(!t||!t->verified||n>INT_MAX)return -1;
    uint64_t until=GetTickCount64()+5000;
    do {
        if(t->closing)return -1;
        AcquireSRWLockExclusive(&t->lock);
        int r=mode==2?TlsApi.SSL_write(t->ssl,buf,(int)n):mode==1?TlsApi.SSL_peek(t->ssl,buf,(int)n):TlsApi.SSL_read(t->ssl,buf,(int)n);
        int error=r>0?0:TlsApi.SSL_get_error(t->ssl,r);
        ReleaseSRWLockExclusive(&t->lock);
        if(r>0)return r;
        if(error==6)return 0;if(error!=2&&error!=3)return -1;
        if(t->io_mode==2)return -2;tls_wait(t,error);
    }while(GetTickCount64()<until);return -1;
}
static int tls_write(LocalTls *t,const void *buf,unsigned n) {return tls_io(t,(void *)buf,n,2);}
static int tls_read(LocalTls *t,void *buf,unsigned n,int peek) {return tls_io(t,buf,n,peek?1:0);}
static unsigned tls_pending(LocalTls *t) {if(!t||!t->verified)return 0;AcquireSRWLockExclusive(&t->lock);unsigned n=(unsigned)TlsApi.SSL_pending(t->ssl);ReleaseSRWLockExclusive(&t->lock);return n;}
