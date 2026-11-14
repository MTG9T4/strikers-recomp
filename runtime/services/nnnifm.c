/* SPDX-License-Identifier: GPL-3.0-or-later
 * Upstream: smo-recomp/runtime/services/nnsocket.inc
 * (0414341471307605b8cb5d0c1a9928db5fe84e07). Offline network availability. */
#include <string.h>
#include "network_policy.h"
static int g_initialized;
static void nifm_init(Ctx *c) { g_initialized=1; RET(0); }
static void nifm_available(Ctx *c) { RET(strikers_online_enabled()); }
static void nifm_ip(Ctx *c) { if(!g_initialized) {RET(0xca6e);return;} if(A0) wr32(A0,strikers_online_enabled()?bswap32(strikers_online_config()->server_ip):0); RET(0); }
static void nifm_proxy(Ctx *c) { if(!g_initialized) {RET(0xca6e);return;} if(A0) memset(PTR(A0),0,0xaa); RET(0); }
static void nifm_reject(Ctx *c) { RET(strikers_online_enabled()?0:0x6e); } /* configured link, not a promise that a server is reachable */

static const ImportDef nnnifm_imports[] = {
    {"_ZN2nn4nifm10InitializeEv", nifm_init},
    {"_ZN2nn4nifm18IsNetworkAvailableEv", nifm_available},
    {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_proxy},
    {"_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", nifm_ip},
    {"_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", nifm_reject},
    {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", nifm_reject},
};
void register_nnnifm_imports(void) { register_imports(nnnifm_imports, sizeof nnnifm_imports / sizeof nnnifm_imports[0]); }
