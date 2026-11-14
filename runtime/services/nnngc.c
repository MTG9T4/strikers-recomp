/* SPDX-License-Identifier: GPL-3.0-or-later
 * Offline profanity dictionary boundary. Target SDK 13.3 @3490b4/@34a690:
 * uninitialized = 0x292, bad buffer = 0x692, small buffer = 0x892.
 * The dictionary archive is a system service asset, absent from this dump.
 * Do not invent a clean-text verdict or a content version. CountNumbers and
 * the constructor are pure SDK code and are lifted instead. */
#include "hostrt.h"
static void ngc_initialize(Ctx *c) {
    uint64_t self = c->x[0];
    wr8(self + 9, 0);
    c->x[0] = !c->x[1] ? 0x692 : c->x[2] < 0x20000 ? 0x892 : 0x292;
}
static void ngc_finalize(Ctx *c) { wr8(c->x[0] + 9, 0); }
static void ngc_version(Ctx *c) { c->x[0] = 0; } /* target's unavailable-content answer */
static void ngc_mask(Ctx *c) { c->x[0] = 0x292; } /* text/count untouched on error, as SDK */
static const ImportDef ngc_imports[] = {
    {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", ngc_initialize},
    {"_ZN2nn3ngc15ProfanityFilterD1Ev", ngc_finalize},
    {"_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", ngc_version},
    {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", ngc_mask},
};
void register_ngc_imports(void) { register_imports(ngc_imports, sizeof ngc_imports / sizeof ngc_imports[0]); }
