/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sdk_abi.h"
#include "sdklift_install.h" /* generated locally; never shipped */
static PlayerSdkHost host;
void call_indirect(Ctx *c,uint64_t a){host.indirect(c,a);}
void host_import(Ctx *c,int i){host.import(c,i);}
void guest_trap(const char *s,uint64_t a){host.trap(s,a);}
void guest_unimpl(uint64_t p,uint32_t i,const char *s){host.unimpl(p,i,s);}
void guest_svc(Ctx *c,uint32_t i){host.svc(c,i);}
void stx_diag(Ctx *c,uint64_t a,uint64_t e,uint64_t s,uint64_t v,int b){host.stx(c,a,e,s,v,b);}
void sdk_host_call(Ctx *c,int i){host.sdk_host(c,i);}
void sdk_trap_call(Ctx *c,const char *s){host.sdk_trap(c,s);}
void sdk_dispatch(Ctx *c,uint64_t a){host.sdk_dispatch(c,a);}
__declspec(dllexport) int player_sdk_bind(const PlayerSdkHost *h,PlayerSdkTables *t){
    if(!h||!t||h->abi!=PLAYER_SDK_ABI||h->size!=sizeof *h||t->size!=sizeof *t)return 0;
    host=*h;t->abi=PLAYER_SDK_ABI;
    t->functions=sdk_fns;t->function_count=&sdk_fn_count;
    t->host_names=sdk_host_names;t->host_name_count=&sdk_host_name_count;
    t->ctors=sdk_ctors;t->ctor_count=&sdk_ctor_count;
    t->data=sdk_data;t->data_count=&sdk_data_count;
    t->install_names=sdk_install_names;t->install_count=sizeof sdk_install_names/sizeof sdk_install_names[0];
    return 1;
}
