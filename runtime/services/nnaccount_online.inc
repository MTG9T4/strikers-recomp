/* SPDX-License-Identifier: GPL-3.0-or-later
 * Local account provider boundary for job 18. The runtime receives an opaque
 * token; it does not mint Nintendo identities. The local auth server verifies
 * its MAC, audience, principal and expiry. Never log credential/file bytes. */
#include <time.h>
extern void os_init_event(uint64_t, int);
extern void os_signal_event(uint64_t);
typedef struct {uint64_t id;char token[3073];} LocalAccount;
static int acc_local(LocalAccount *out) {
    if(!strikers_online_enabled())return 0;
    FILE *f=fopen(strikers_online_config()->token_file,"rb");if(!f)return 0;
    char version[64],id[64],expiry[64];char *end;int ok=0;
    if(fgets(version,sizeof version,f)&&!strcmp(version,"STRIKERS-LOCAL-TOKEN-V1\n")&&
       fgets(id,sizeof id,f)&&fgets(expiry,sizeof expiry,f)&&fgets(out->token,sizeof out->token,f)) {
        out->id=_strtoui64(id,&end,10);
        if(out->id && *end=='\n' && end[1]==0) {
            uint64_t until=_strtoui64(expiry,&end,10);size_t n=strlen(out->token);
            if(n && out->token[n-1]=='\n')out->token[--n]=0;
            if(until>(uint64_t)time(NULL)&&*end=='\n'&&end[1]==0&&n>0&&n<3072&&fgetc(f)==EOF) {
                ok=1;for(size_t i=0;i<n;i++)if((unsigned char)out->token[i]<=32||(unsigned char)out->token[i]>=127)ok=0;
            }
        }
    }
    fclose(f);return ok;
}
static int acc_handle(uint64_t handle) {return handle&&rd64(handle)==1&&!rd64(handle+8);}
static void online_acc_available(Ctx *c) {LocalAccount a;if(!A0||!acc_handle(A1)){RET(ACC_CANCELLED);return;}wr8(A0,acc_local(&a));RET(0);}
static void online_acc_ensure(Ctx *c) {LocalAccount a;RET(acc_handle(A0)&&acc_local(&a)?0:ACC_NO_INTERNET);}
static void online_acc_id(Ctx *c) {LocalAccount a;if(A0)wr64(A0,0);if(!A0||!acc_handle(A1)||!acc_local(&a)){RET(ACC_NO_INTERNET);return;}wr64(A0,a.id);RET(0);}
static void online_acc_token(Ctx *c) {
    LocalAccount a;if(A0)wr64(A0,0);
    if(!A0||!A1||!acc_handle(A3)||!acc_local(&a)){RET(ACC_NO_INTERNET);return;}
    size_t n=strlen(a.token);if(A2<n+1){RET(ACC_CANCELLED);return;}
    memcpy(PTR(A1),a.token,n+1);wr64(A0,n);printf("[online-account] loaded local token principal=%llu bytes=%zu\n",(unsigned long long)a.id,n);RET(0);
}
#define ACC_LOCAL_ASYNC 0x4f414343u
static void online_acc_async(Ctx *c) {LocalAccount a;if(!A0){RET(ACC_CANCELLED);return;}uint32_t r=acc_handle(A1)&&acc_local(&a)?0:ACC_NO_INTERNET;wr32(A0+24,r);wr32(A0+28,ACC_LOCAL_ASYNC);RET(r);}
static void online_acc_done(Ctx *c) {if(!A0||!A1||rd32(A0+28)!=ACC_LOCAL_ASYNC){RET(ACC_NO_INTERNET);return;}wr8(A1,1);RET(0);}
static void online_acc_result(Ctx *c) {RET(A0&&rd32(A0+28)==ACC_LOCAL_ASYNC?rd32(A0+24):ACC_NO_INTERNET);}
static void online_acc_event(Ctx *c) {if(!A0||!A1||rd32(A0+28)!=ACC_LOCAL_ASYNC){RET(ACC_NO_INTERNET);return;}os_init_event(A1,0);os_signal_event(A1);RET(0);}
static void online_acc_cancel(Ctx *c) {if(A0&&rd32(A0+28)==ACC_LOCAL_ASYNC)wr32(A0+24,ACC_CANCELLED);RET(0);}
static void register_online_account(void) {
    static const ImportDef online[] = {
        {"_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE",online_acc_available},
        {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE",online_acc_ensure},
        {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE",online_acc_id},
        {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE",online_acc_token},
        {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE",online_acc_async},
        {"_ZN2nn7account12AsyncContext7HasDoneEPb",online_acc_done},
        {"_ZN2nn7account12AsyncContext9GetResultEv",online_acc_result},
        {"_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE",online_acc_event},
        {"_ZN2nn7account12AsyncContext6CancelEv",online_acc_cancel},
    };register_imports(online,sizeof online/sizeof online[0]);
}
