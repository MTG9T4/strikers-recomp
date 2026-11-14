/* SPDX-License-Identifier: GPL-3.0-or-later
 * Upstream: smo-recomp/runtime/services/nnservices.c (0414341471307605b8cb5d0c1a9928db5fe84e07).
 * Adapted for Strikers; see docs/phase2-runtime.md. */
/* Service registration; target SDK installs are supplied by the pipeline. */
#include <stdlib.h>
#include <string.h>
#include "sdklift.h"
void register_nnapplet_imports(void);
void register_nnnet_imports(void);
void register_nnfsdir_imports(void);
void register_network_imports(void);
/* Imports whose host re-implementation is replaced by the real code lifted from sdk.elf (the list:
 * build/gen/sdklift_install.h, written by tools/lifter/sdklift.py). Debug aids: STRIKERS_LIFT_INSTALL=0 keeps every host handler;
 * STRIKERS_LIFT_SKIP=<text>[,<text>...] keeps the host handler of the names containing one of the texts. */
#include "sdklift_install.h"
static void install_lifted(void) {
    int n = (int)(sizeof sdk_install_names / sizeof sdk_install_names[0]), ok = 0, skipped = 0;
    const char *off = getenv("STRIKERS_LIFT_INSTALL"), *skip = getenv("STRIKERS_LIFT_SKIP");
    if (!sdk_image_ready() || (off && atoi(off) == 0)) return;
    for (int i = 0; i < n; i++) {
        int skip_this = 0;
        if (!sdk_install_names[i]) continue;       /* the generated list may be empty (a single NULL entry) */
        if (skip && skip[0]) {
            char buf[1024];
            snprintf(buf, sizeof buf, "%s", skip);
            for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) if (strstr(sdk_install_names[i], t)) skip_this = 1;
        }
        if (skip_this) { skipped++; continue; }
        if (sdk_install(sdk_install_names[i])) ok++;
        else printf("sdklift: %s is in sdklift_install.h but was not lifted or is not an import\n", sdk_install_names[i]);
    }
    printf("sdklift: %d host re-implementations replaced by the real SDK code (%d kept by STRIKERS_LIFT_SKIP)\n", ok, skipped);
}


void register_service_imports(void) {
    register_sdklift_imports();
    register_nnapplet_imports();
    register_nnnet_imports();
    register_nnfsdir_imports();
    install_lifted();
}
