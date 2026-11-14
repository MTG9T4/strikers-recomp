/* SPDX-License-Identifier: GPL-3.0-or-later
 * Upstream: smo-recomp/runtime/services/nnnet.c; services/nnsocket.inc (0414341471307605b8cb5d0c1a9928db5fe84e07).
 * Offline NSD only. glslc weak imports are deliberately left unbound/null. */
#include <string.h>
#include "network_policy.h"
static void nsd_resolve(Ctx *c) {
    char fqdn[256]={0};if(!A0||!A1){RET(0x6e);return;}
    memcpy(fqdn,PTR(A1),255);
    /* NEX formats %% to %, then ResolveEx expands that SDK environment
     * placeholder. The offline passthrough left a literal % in TLS SNI;
     * pair08 correctly refused its certificate (hostname verification 62).
     * Expansion is confined to the known online name shapes. Native DNS is
     * still forbidden: socket policy maps the result only to our server. */
    if(strikers_online_hostname(fqdn)&&strchr(fqdn,'%')) {
        char resolved[256]={0};unsigned at=0;
        for(unsigned i=0;fqdn[i];i++) {
            const char *part=fqdn[i]=='%'?"lp1":NULL;
            if(at+(part?3:1)>=sizeof resolved){RET(0x6e);return;}
            if(part){memcpy(resolved+at,part,3);at+=3;}else resolved[at++]=fqdn[i];
        }
        memcpy(fqdn,resolved,sizeof fqdn);
    }
    memcpy(PTR(A0),fqdn,256);RET(0);
}
static void aoc_empty(Ctx *c) { RET(0); } /* ListAddOnContent returns number of indices written */
static const ImportDef net_imports[] = {
    {"_ZN2nn3aoc16ListAddOnContentEPiii",aoc_empty},
    {"_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_",nsd_resolve},
};
void register_nnnet_imports(void) { register_imports(net_imports,sizeof net_imports/sizeof net_imports[0]); }
