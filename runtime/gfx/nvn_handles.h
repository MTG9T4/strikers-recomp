/* SPDX-License-Identifier: GPL-3.0-or-later
 * Upstream: smo-recomp/runtime/gfx/nvn_handles.h (0414341471307605b8cb5d0c1a9928db5fe84e07).
 * Adapted for Strikers; see docs/phase2-runtime.md. */
/* NVN handle values: see nvn_handles.c. */
#ifndef NVN_HANDLES_H
#define NVN_HANDLES_H
#include "host_cpu.h"
#include "hostrt.h"
ImportFn nvn_gl_pick(const char *name);         /* nvn_gl.c */
/* Part of the SDK self-test: the five handle functions against the values the SDK's code gives (emulator on sdk.elf). */
int nvn_handles_selftest(Ctx *c);
#endif
