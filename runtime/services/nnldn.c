/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nnldn_lan.inc"
#include "group_client.h"

/* Observe the existing locked state from a read-only snapshot. No LAN edits.
 * Keep group callbacks outside L.lock; they only publish atomics. */
static void group_ldn_observe(void) {
    AcquireSRWLockShared(&L.lock);int state=L.state;ReleaseSRWLockShared(&L.lock);
    group_ldn_state(state);
}
static void group_ldn_create(Ctx *c){ldn_create(c);group_ldn_observe();}
static void group_ldn_create_private(Ctx *c){ldn_create_private(c);group_ldn_observe();}
static void group_ldn_destroy(Ctx *c){ldn_destroy(c);group_ldn_observe();}
static void group_ldn_close_ap(Ctx *c){ldn_close_ap(c);group_ldn_observe();}
static void group_ldn_close_station(Ctx *c){ldn_close_station(c);group_ldn_observe();}
static void group_ldn_finalize(Ctx *c){ldn_finalize(c);group_ldn_observe();}
static void group_ldn_disconnect(Ctx *c){ldn_disconnect(c);group_ldn_observe();}

static const ImportDef nnldn_imports[] = {
    {"_ZN2nn3ldn10DisconnectEv", group_ldn_disconnect},
    {"_ZN2nn3ldn10InitializeEv", ldn_init},
    {"_ZN2nn3ldn11OpenStationEv", ldn_station},
    {"_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_scan_private},
    {"_ZN2nn3ldn12CloseStationEv", group_ldn_close_station},
    {"_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", group_ldn_create},
    {"_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_connect_private},
    {"_ZN2nn3ldn14DestroyNetworkEv", group_ldn_destroy},
    {"_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ldn_address},
    {"_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ldn_info},
    {"_ZN2nn3ldn15MakeIpv4AddressEhhhh", ldn_ip},
    {"_ZN2nn3ldn15OpenAccessPointEv", ldn_ap},
    {"_ZN2nn3ldn16CloseAccessPointEv", group_ldn_close_ap},
    {"_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", ldn_config},
    {"_ZN2nn3ldn16SetAdvertiseDataEPKvm", ldn_advertise},
    {"_ZN2nn3ldn19GetDisconnectReasonEv", ldn_reason},
    {"_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE", group_ldn_create_private},
    {"_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", ldn_security},
    {"_ZN2nn3ldn20MakeBroadcastAddressENS0_11Ipv4AddressENS0_10SubnetMaskE", ldn_broadcast},
    {"_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ldn_attach},
    {"_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", ldn_policy},
    {"_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_scan},
    {"_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", ldn_reject},
    {"_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_connect},
    {"_ZN2nn3ldn8FinalizeEv", group_ldn_finalize},
    {"_ZN2nn3ldn8GetStateEv", ldn_state},
};
static void ldn_trace(Ctx *c) {
    static LONG counts[sizeof nnldn_imports/sizeof nnldn_imports[0]][6];
    for(unsigned i=0;i<sizeof nnldn_imports/sizeof nnldn_imports[0];i++) if(c->in_import && !strcmp(c->in_import,nnldn_imports[i].name)) {
        int state=L.state;int log=InterlockedIncrement(&counts[i][state>=0&&state<6?state:0])<=16;
        if(log)printf("[lan-api] %s args=%llx,%llx,%llx,%llx\n",c->in_import,c->x[0],c->x[1],c->x[2],c->x[3]);
        nnldn_imports[i].fn(c);
        if(log)printf("[lan-api] %s result=%llx state=%d\n",c->in_import,c->x[0],L.state);
        return;
    }
    guest_trap("LDN trace: missing import context",c->x[30]-4);
}
void register_nnldn_imports(void) {
    if(getenv("STRIKERS_LDN_TRACE")) {
        ImportDef traced[sizeof nnldn_imports/sizeof nnldn_imports[0]];
        memcpy(traced,nnldn_imports,sizeof traced);
        for(unsigned i=0;i<sizeof traced/sizeof traced[0];i++)traced[i].fn=ldn_trace;
        register_imports(traced,sizeof traced/sizeof traced[0]);
    } else register_imports(nnldn_imports,sizeof nnldn_imports/sizeof nnldn_imports[0]);
}

/* core/cheats.c: cheats wait while the player is in (or making) a Local Wireless room. */
int strikers_ldn_state(void) { AcquireSRWLockShared(&L.lock); int s = L.state; ReleaseSRWLockShared(&L.lock); return s; }
