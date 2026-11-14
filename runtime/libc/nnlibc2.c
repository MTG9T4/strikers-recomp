/* SPDX-License-Identifier: GPL-3.0-or-later
 * Upstream: smo-recomp/runtime/libc/nnlibc2.c (0414341471307605b8cb5d0c1a9928db5fe84e07).
 * Adapted for Strikers; see docs/phase2-runtime.md. */
/* C library additions: conversions between binary128 (long double on AArch64) and double, the standard streams with
 * fclose / feof / ferror, and gmtime.
 *
 * Only names that have no handler yet are installed (see register_libc2_imports). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <windows.h>
#include "host_cpu.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define RET(v) (c->x[0] = (uint64_t)(v))

/* ================================================================ binary128 <-> double
 * long double is IEEE binary128 on AArch64: clib.c reads it for printf's %Lf and writes it for sscanf's %Lf.
 * Narrowing rounds to nearest even. */
/* MSVC represents the binary128 significand as two words. Only conversion is needed
 * here, never host long double (which is binary64 on Windows). Same nearest-even
 * narrowing algorithm as upstream, with explicit cross-word shifts. */
void quad_from_double(double d, uint64_t out[2]) {
    uint64_t bits; memcpy(&bits, &d, 8);
    uint64_t sign = bits & 0x8000000000000000ull;
    int e = (int)((bits >> 52) & 0x7ff);
    uint64_t f = bits & 0xfffffffffffffull;
    if (e == 0x7ff) {
        out[0] = f << 60;
        out[1] = sign | 0x7fff000000000000ull | (f >> 4) | (f ? 0x800000000000ull : 0);
        return;
    }
    if (!e && !f) { out[0] = 0; out[1] = sign; return; }
    if (!e) { int sh = __builtin_clzll(f) - 11; f <<= sh; e = 1-sh; f &= 0xfffffffffffffull; }
    out[0] = f << 60;
    out[1] = sign | ((uint64_t)(e-1023+16383) << 48) | (f >> 4);
}
double quad_to_double(const uint64_t in[2]) {
    uint64_t lo = in[0], hi = in[1], sign = hi & 0x8000000000000000ull;
    int e = (int)((hi >> 48) & 0x7fff);
    hi &= 0xffffffffffffull;
    uint64_t result;
    if (e == 0x7fff) {
        result = sign | 0x7ff0000000000000ull;
        if (hi || lo) result |= 0x8000000000000ull | (hi << 4) | (lo >> 60);
    } else if (!e && !hi && !lo) result = sign;
    else {
        if (!e) {
            int sh = hi ? __builtin_clzll(hi)-15 : 49+__builtin_clzll(lo);
            if (sh >= 64) { hi = lo << (sh-64); lo = 0; }
            else if (sh) { hi = (hi << sh) | (lo >> (64-sh)); lo <<= sh; }
            e = 1-sh;
        } else hi |= 1ull << 48;
        e = e-16383+1023;
        uint64_t sig = (hi << 7) | (lo >> 57) | ((lo & ((1ull << 57)-1)) != 0);
        if (e <= 0) {
            int sh = 1-e;
            if (sh > 56) sig = sig != 0;
            else { uint64_t lost = sig & ((1ull << sh)-1); sig = (sig >> sh) | (lost != 0); }
            e = 0;
        }
        unsigned grs = (unsigned)sig & 7; sig >>= 3;
        if (grs > 4 || (grs == 4 && (sig & 1))) sig++;
        if (!e) { if (sig >> 52) e = 1; }
        else if (sig >> 53) { sig >>= 1; e++; }
        result = e >= 2047 ? sign | 0x7ff0000000000000ull : sign | ((uint64_t)e << 52) | (sig & 0xfffffffffffffull);
    }
    double d; memcpy(&d, &result, 8); return d;
}

/* ================================================================ the standard streams
 * The game imports the variables stdin, stdout and stderr and the functions fclose, feof and ferror, but nothing that
 * opens a file: a guest FILE * is one of the three host objects below (the game only passes it back). */
typedef struct {
    uint32_t magic;                  /* 'FILE' */
    int kind;                        /* 1 stdin, 2 stdout, 3 stderr */
    int eof, err, open;
} GFile;
#define GFILE_MAGIC 0x454c4946u
static GFile g_std[3] = {{GFILE_MAGIC, 1, 0, 0, 1}, {GFILE_MAGIC, 2, 0, 0, 1}, {GFILE_MAGIC, 3, 0, 0, 1}};
static uint64_t g_std_ptr[3];        /* the objects behind the imported `stdin`, `stdout`, `stderr` (FILE * variables) */

static GFile *gfile(uint64_t p) {
    GFile *f = (GFile *)(uintptr_t)p;
    return f && f->magic == GFILE_MAGIC ? f : NULL;
}
static void c_fclose(Ctx *c) {
    GFile *f = gfile(A0);
    if (!f) { RET(-1); return; }
    f->open = 0;
    RET(0);
}
static void c_feof(Ctx *c) { GFile *f = gfile(A0); RET(f ? f->eof : 0); }
static void c_ferror(Ctx *c) { GFile *f = gfile(A0); RET(f ? f->err : 0); }

/* ================================================================ gmtime
 * musl struct tm: nine ints (sec, min, hour, mday, mon, year, wday, yday, isdst), long gmtoff at +40, const char *zone at +48. */
int libc2_gmtime(int64_t t, int32_t tm[9]) {
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    if (days < -1000000000ll || days > 1000000000ll) return 0;        /* year would overflow an int */
    tm[2] = (int32_t)(rem / 3600); tm[1] = (int32_t)(rem % 3600 / 60); tm[0] = (int32_t)(rem % 60);
    int64_t wd = (days + 4) % 7;                                      /* 1970-01-01 was a Thursday */
    tm[6] = (int32_t)(wd < 0 ? wd + 7 : wd);
    /* civil date from day count (proleptic Gregorian), era-based */
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    tm[3] = (int32_t)d; tm[4] = (int32_t)(m - 1); tm[5] = (int32_t)(y - 1900);
    tm[7] = cum[m - 1] + (int32_t)d - 1 + (leap && m > 2);
    tm[8] = 0;
    return 1;
}
static void tm_store(uint64_t dst, const int32_t tm[9]) {
    static const char utc[] = "UTC";
    memcpy((void *)(uintptr_t)dst, tm, 36);
    wr32(dst + 36, 0);
    wr64(dst + 40, 0);
    wr64(dst + 48, (uint64_t)(uintptr_t)utc);
}
static void c_gmtime(Ctx *c) {
    static __declspec(thread) uint8_t buf[56];
    int32_t tm[9];
    if (!libc2_gmtime((int64_t)rd64(A0), tm)) { RET(0); return; }
    tm_store((uint64_t)(uintptr_t)buf, tm);
    RET(buf);
}

static const ImportDef libc2_imports[] = {
    {"fclose", c_fclose},
    {"feof", c_feof}, {"ferror", c_ferror},
    {"gmtime", c_gmtime},
};

/* Called before runtime_init (from cxxrt_install_data): the imported FILE * variables. */
void libc2_install_data(void) {
    for (int i = 0; i < 3; i++) g_std_ptr[i] = (uint64_t)(uintptr_t)&g_std[i];
    import_set_address("stdin", &g_std_ptr[0]);
    import_set_address("stdout", &g_std_ptr[1]);
    import_set_address("stderr", &g_std_ptr[2]);
}

void register_libc2_imports(void) {
    int n = (int)(sizeof libc2_imports / sizeof libc2_imports[0]);
    for (int i = 0; i < n; i++)
        if (!import_lookup(libc2_imports[i].name)) register_imports(&libc2_imports[i], 1);
}
