/* SPDX-License-Identifier: GPL-3.0-or-later
 * Upstream: smo-recomp/runtime/services/nnonline.inc
 * (0414341471307605b8cb5d0c1a9928db5fe84e07). Deliberate empty/offline friends.
 * No successful async request is ever made. Returned profiles are always empty. */
#include <string.h>
#include "network_policy.h"
static void fr_empty_ctor(Ctx *c) { (void)c; } /* no profile is returned from the offline list */
static void fr_async_ctor(Ctx *c) { memset(PTR(A0),0,0x28); }
static void fr_async_dtor(Ctx *c) { (void)c; }
static void fr_done(Ctx *c) { if(A1) wr8(A1,0); RET(FR_NOT_CALLED); }
static void fr_no_request(Ctx *c) { RET(FR_NOT_CALLED); }
static void fr_list(Ctx *c) { if(A0) wr32(A0,0); RET(0); }
static void fr_no_network(Ctx *c) { RET(FR_NOT_CALLED); }
static void fr_id(Ctx *c) { RET(0); }
static void fr_nick(Ctx *c) { static const char empty[33]={0}; RET(empty); }

static const ImportDef nnfriends_imports[] = {
    {"_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", fr_no_request},
    {"_ZN2nn7friends12AsyncContext7HasDoneEPb", fr_done},
    {"_ZN2nn7friends12AsyncContextC1Ev", fr_async_ctor},
    {"_ZN2nn7friends12AsyncContextD1Ev", fr_async_dtor},
    {"_ZN2nn7friends13GetFriendListEPiPNS0_6FriendEiiRKNS0_12FriendFilterE", fr_list},
    {"_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", fr_no_network},
    {"_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi", fr_no_request},
    {"_ZN2nn7friends14ShowFriendListERKNS_7account3UidE", fr_no_network},
    {"_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextE", fr_no_request},
    {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", fr_no_network},
    {"_ZN2nn7friends6FriendC1Ev", fr_empty_ctor},
    {"_ZN2nn7friends7ProfileC1Ev", fr_empty_ctor},
    {"_ZNK2nn7friends12AsyncContext9GetResultEv", fr_no_request},
    {"_ZNK2nn7friends6Friend11GetNicknameEv", fr_nick},
    {"_ZNK2nn7friends6Friend12GetAccountIdEv", fr_id},
    {"_ZNK2nn7friends7Profile11GetNicknameEv", fr_nick},
    {"_ZNK2nn7friends7Profile12GetAccountIdEv", fr_id},
};
void register_nnfriends_imports(void) { register_imports(nnfriends_imports, sizeof nnfriends_imports / sizeof nnfriends_imports[0]); }
