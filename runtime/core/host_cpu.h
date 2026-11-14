/* SPDX-License-Identifier: GPL-3.0-or-later
 * Upstream: smo-recomp/runtime/core/runtime.h; Ctx matches the job 02/03 header.
 * Host-only MSVC view. No translated CPU arithmetic is compiled through this view.
 * tools/audit/build_runtime.py checks the shared declarations before compiling. */
#ifndef STRIKERS_HOST_CPU_H
#define STRIKERS_HOST_CPU_H
#if defined(_MSC_VER) && !defined(__clang__)
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <intrin.h>
typedef union {
    uint8_t b[16];
    uint16_t h[8];
    uint32_t w[4];
    uint64_t d[2];
    float f[4];
    double df[2];
} V128;

typedef struct {
    uint64_t x[32];          /* x[31] is SP */
    V128 v[32];
    uint32_t N, Z, C, V;
    uint64_t tpidr;
    uint64_t ex_addr, ex_val;    /* exclusive monitor: address and value seen by the last LDXR */
    uint64_t ex_fail;            /* consecutive failed STXR, for diagnostics */
    uint64_t fn;                 /* entry address of the lifted function entered most recently */
    const char *in_import;       /* name of the host import currently executing on this context, or NULL */
    uint64_t import_from;        /* guest return address of that import call */
    uint64_t ex_val2;            /* ssbu-recomp: high doubleword of a paired exclusive load */
} Ctx;

void call_indirect(Ctx *c, uint64_t addr);
void host_import(Ctx *c, int sym);          /* call to a symbol another module provides */
typedef struct { uint64_t addr; void (*fn)(Ctx *); } FnEntry;
typedef struct { const char *name; uint64_t value; int defined; } DynSym;
extern const FnEntry fn_table[];
extern const int fn_table_count;
extern const DynSym dyn_syms[];
extern const int dyn_sym_count;
void guest_trap(const char *what, uint64_t addr);
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what);
void guest_svc(Ctx *c, uint32_t number);

void stx_diag(Ctx *, uint64_t, uint64_t, uint64_t, uint64_t, int);
#define DEF_MEM(bits) \
    static inline uint##bits##_t rd##bits(uint64_t a) { uint##bits##_t v; memcpy(&v, (void *)(uintptr_t)a, sizeof v); return v; } \
    static inline void wr##bits(uint64_t a, uint##bits##_t v) { memcpy((void *)(uintptr_t)a, &v, sizeof v); }
DEF_MEM(8) DEF_MEM(16) DEF_MEM(32) DEF_MEM(64)


static inline void vzero(V128 *v) { memset(v, 0, sizeof *v); }
static inline V128 rdv(uint64_t a, int n) { V128 v = {0}; memcpy(&v, (void *)(uintptr_t)a, n); return v; }
static inline void wrv(uint64_t a, const V128 *v, int n) { memcpy((void *)(uintptr_t)a, v, n); }
static inline int host_ctz64(uint64_t v) { unsigned long n; return _BitScanForward64(&n, v) ? (int)n : 64; }
static inline int host_clz64(uint64_t v) { unsigned long n; return _BitScanReverse64(&n, v) ? 63-(int)n : 64; }
#define __builtin_ctzll host_ctz64
#define __builtin_clzll host_clz64
#define __builtin_bswap32 _byteswap_ulong
#define __builtin_bswap64 _byteswap_uint64
#define bswap32 _byteswap_ulong
#define bswap64 _byteswap_uint64
#else
#include "runtime.h"
#endif
#endif
