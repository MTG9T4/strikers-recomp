/* SPDX-License-Identifier: GPL-3.0-or-later
 * Strikers project code; offline registration boundary. */
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "network_policy.h"
#include "zt_transport.h"
#include "group_client.h"
static StrikersNetworkModule g_module;
static INIT_ONCE online_once=INIT_ONCE_STATIC_INIT;
static StrikersOnlineConfig online_config;
static void online_setting(const char *env,const char *key,const char *def,char *out,unsigned cap) {
    const char *v=getenv(env),*ini=getenv("STRIKERS_CONFIG");
    if(v) snprintf(out,cap,"%s",v);
    else GetPrivateProfileStringA("Network",key,def,out,cap,ini?ini:".\\strikers.ini");
}
static BOOL CALLBACK online_read(PINIT_ONCE once,PVOID param,PVOID *ctx) {
    (void)once;(void)param;(void)ctx;
    char addr[128],ports[256],*next=NULL;
    online_setting("STRIKERS_ONLINE_SERVER","online_server","",addr,sizeof addr);
    online_setting("STRIKERS_ONLINE_PORTS","online_ports","10025,10026,10027,10028,443",ports,sizeof ports);
    online_setting("STRIKERS_ONLINE_TOKEN_FILE","online_token_file","",online_config.token_file,sizeof online_config.token_file);
    online_setting("STRIKERS_ONLINE_CA_FILE","online_ca_file","",online_config.ca_file,sizeof online_config.ca_file);
    online_setting("STRIKERS_ONLINE_OPENSSL","online_openssl_file","",online_config.openssl_file,sizeof online_config.openssl_file);
    const char *files[]={online_config.token_file,online_config.ca_file,online_config.openssl_file};
    for(unsigned i=0;i<3;i++) {
        const char *p=files[i];
        if((p[0]=='\\'&&p[1]=='\\')||(p[0]=='/'&&p[1]=='/'))return TRUE;
        if(strlen(p)>1&&p[1]==':'){char drive[4]={p[0],':','\\',0};if(GetDriveTypeA(drive)==DRIVE_REMOTE)return TRUE;}
    }
    /* Intentionally narrower than 127/8 in this job. Never resolve the config. */
    if(strcmp(addr,"127.0.0.1")) {printf("[online-policy] refused server: requires numeric 127.0.0.1\n");return TRUE;}
    for(char *p=strtok_s(ports,",",&next);p;p=strtok_s(NULL,",",&next)) {
        char *end;unsigned long n=strtoul(p,&end,10);
        if(!*p||*end||!n||n>65535||online_config.port_count==8) {online_config.port_count=0;return TRUE;}
        online_config.ports[online_config.port_count++]=(uint16_t)n;
    }
    if(online_config.port_count)online_config.server_ip=0x7f000001;
    printf("[online-policy] loopback=%d allowed-ports=%u; external destinations blocked\n",!!online_config.server_ip,online_config.port_count);
    return TRUE;
}
int strikers_online_requested(void) {const char *v=getenv("STRIKERS_NETWORK");return v&&!strcmp(v,"online");}
const StrikersOnlineConfig *strikers_online_config(void) {InitOnceExecuteOnce(&online_once,online_read,NULL,NULL);return &online_config;}
int strikers_online_enabled(void) {return strikers_online_requested()&&strikers_online_config()->server_ip!=0;}
int strikers_online_destination(uint32_t ip,uint16_t port) {
    if(!strikers_online_enabled()||ip!=online_config.server_ip)return 0;
    for(unsigned i=0;i<online_config.port_count;i++)if(port==online_config.ports[i])return 1;
    return 0;
}
static int online_label(const char *s,size_t n) {
    if(!n||n>32)return 0;
    for(size_t i=0;i<n;i++)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='0'&&s[i]<='9')||s[i]=='-'||s[i]=='%'))return 0;
    return 1;
}
int strikers_online_hostname(const char *name) {
    /* Anchored shapes read from main's literals; no suffix-only allowlist.
     * Percent is NSD's unresolved environment placeholder (nnnet ResolveEx).
     * No native DNS, localhost aliases, Unicode, URL, port or path accepted. */
    if(!strikers_online_enabled()||!name)return 0;
    size_t n=strnlen(name,254);if(!n||n==254)return 0;
    const char *suffix=NULL,*label=NULL;
    if(n>10&&name[0]=='g'&&name[9]=='-') {
        for(int i=1;i<9;i++)if(!((name[i]>='0'&&name[i]<='9')||(name[i]>='a'&&name[i]<='f')))return 0;
        suffix=strchr(name+10,'.');label=name+10;
        if(!suffix||strcmp(suffix,".s.n.srv.nintendo.net"))return 0;
    } else if(!strncmp(name,"nncs1-",6)||!strncmp(name,"nncs2-",6)) {
        label=name+6;suffix=strchr(label,'.');if(!suffix||strcmp(suffix,".n.n.srv.nintendo.net"))return 0;
    } else if(!strncmp(name,"api-",4)) {
        label=name+4;suffix=strchr(label,'.');if(!suffix||strcmp(suffix,".znc.srv.nintendo.net"))return 0;
    } else {
        const char *dot=strchr(name,'.');if(!dot||!online_label(name,(size_t)(dot-name)))return 0;
        label=dot+1;suffix=strchr(label,'.');if(!suffix||strcmp(suffix,".p.srv.nintendo.net"))return 0;
    }
    return online_label(label,(size_t)(suffix-label));
}
int strikers_lan_enabled(void) {
    /* STRIKERS_NETWORK first, then strikers.ini's [Network] mode (zt_config) */
    return zt_lan_mode() || zt_requested();
}
/* main +14408c treats failure of the local-play history key as a failed room.
 * SDK +1cdd84 supplies a 0x40-byte output. This is an opaque runtime-local
 * identifier exchanged by Pia, never a Nintendo credential or online account.
 * History remains local-only and is deliberately discarded, as prepo is. */
static void lan_history_key(Ctx *c) {
    uint32_t ip=ldn_local_ip();
    if(!strikers_lan_enabled() || !ip || !A0 || !A1 || !A2 || rd64(A1)!=1 || rd64(A1+8)!=0) {RET(FR_NOT_CALLED);return;}
    memset(PTR(A0),0,64);memcpy(PTR(A0),"SLDNHIST",8);wr32(A0+8,1);wr32(A0+12,ip);
    memcpy(PTR(A0+16),PTR(A1),16);wr64(A0+32,STRIKERS_PROGRAM_ID);
    printf("[lan] local-history-key virtual=%08x (runtime-local, no account login)\n",ip);RET(0);
}
static void lan_history_add(Ctx *c) {
    if(!strikers_lan_enabled() || !A1 || memcmp(PTR(A1),"SLDNHIST",8) || rd32(A1+8)!=1 || rd64(A1+32)!=STRIKERS_PROGRAM_ID) {RET(FR_NOT_CALLED);return;}
    printf("[lan] local play-history notification discarded\n");RET(0);
}
static const ImportDef lan_local_history[] = {
    {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb",lan_history_key},
    {"_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_",lan_history_add},
};
void strikers_set_network_module(StrikersNetworkModule module) { g_module = module; }
void register_network_imports(void) {
    zt_start();
    group_start();
    register_nnaccount_imports(); register_nnfriends_imports();
    register_nnldn_imports(); register_nnsocket_imports();
    register_nnnifm_imports(); register_nnssl_imports();
    const char *mode = getenv("STRIKERS_NETWORK");
    if(strikers_lan_enabled()) register_imports(lan_local_history,sizeof lan_local_history/sizeof lan_local_history[0]);
    if (mode && strcmp(mode, "offline") && strcmp(mode, "0") && strcmp(mode, "lan") && strcmp(mode,"online") && !zt_requested() && !zt_lan_mode()) {
        if (!g_module) guest_trap("STRIKERS_NETWORK requested but no network module is installed", 0);
        g_module();
    }
}
