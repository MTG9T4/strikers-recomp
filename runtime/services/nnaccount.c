/* SPDX-License-Identifier: GPL-3.0-or-later
 * Upstream: smo-recomp/runtime/fs/nnfs.c and runtime/services/nnonline.inc
 * (0414341471307605b8cb5d0c1a9928db5fe84e07). Offline local user + failed login.
 * No token cache, Nintendo login or account-linking applet is created. */
#include <stdlib.h>
#include <string.h>
#include "network_policy.h"
static void acc_init(Ctx *c) { RET(0); }
static void acc_uid(Ctx *c) { wr64(A0,1); wr64(A0+8,0); RET(0); }
static void acc_count(Ctx *c) { wr32(A0,1); RET(0); }
static void acc_list(Ctx *c) {
    int n = A2 && A1 ? 1 : 0;
    if (n) { wr64(A1,1); wr64(A1+8,0); }
    if (A0) wr32(A0,n); RET(0);
}
static void acc_exists(Ctx *c) { wr8(A0,rd64(A1)==1 && rd64(A1+8)==0); RET(0); }
static void acc_open(Ctx *c) {
    if (rd64(A1)!=1 || rd64(A1+8)) { RET(0x27c); return; }
    wr64(A0,1); wr64(A0+8,0); wr64(A0+16,0); RET(0);
}
static void acc_close(Ctx *c) { (void)c; }
static void acc_nick(Ctx *c) {
    const char *nick = getenv("STRIKERS_NICKNAME"); if (!nick || !*nick) nick="Player";
    size_t n=strlen(nick); if (n>32) { n=32; while (n && ((unsigned char)nick[n]&0xc0)==0x80) n--; }
    memset(PTR(A0),0,33); memcpy(PTR(A0),nick,n); RET(0);
}
static void acc_available(Ctx *c) { wr8(A0,0); RET(0); }
static void acc_cancelled(Ctx *c) { RET(ACC_CANCELLED); }
static void acc_no_internet(Ctx *c) { RET(ACC_NO_INTERNET); }
static void acc_no_id(Ctx *c) { if (A0) wr64(A0,0); RET(0x1907c); } /* exact unavailable code unverified */
static void acc_no_token(Ctx *c) { if (A0) wr64(A0,0); RET(ACC_NO_INTERNET); }
static void acc_async_ctor(Ctx *c) { memset(PTR(A0),0,0x20); }
static void acc_async_dtor(Ctx *c) { (void)c; }
static void acc_async_done(Ctx *c) { if (A1) wr8(A1,0); RET(ACC_NO_INTERNET); }

#include "nnaccount_online.inc"
static const ImportDef nnaccount_imports[] = {
    {"_ZN2nn7account10InitializeEv", acc_init},
    {"_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", acc_nick},
    {"_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", acc_no_internet},
    {"_ZN2nn7account12AsyncContext6CancelEv", acc_no_internet},
    {"_ZN2nn7account12AsyncContext7HasDoneEPb", acc_async_done},
    {"_ZN2nn7account12AsyncContext9GetResultEv", acc_no_internet},
    {"_ZN2nn7account12AsyncContextC1Ev", acc_async_ctor},
    {"_ZN2nn7account12AsyncContextD1Ev", acc_async_dtor},
    {"_ZN2nn7account12GetUserCountEPi", acc_count},
    {"_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi", acc_list},
    {"_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", acc_exists},
    {"_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE", acc_uid},
    {"_ZN2nn7account17GetLastOpenedUserEPNS0_3UidE", acc_uid},
    {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", acc_no_id},
    {"_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", acc_available},
    {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", acc_cancelled},
    {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", acc_no_token},
    {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", acc_no_internet},
    {"_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE", acc_open},
    {"_ZN2nn7account9CloseUserERKNS0_10UserHandleE", acc_close},
};
void register_nnaccount_imports(void) {
    register_imports(nnaccount_imports, sizeof nnaccount_imports / sizeof nnaccount_imports[0]);
    if(strikers_online_requested())register_online_account();
}
