/* SPDX-License-Identifier: GPL-3.0-or-later
 * Strikers host boundaries measured against NintendoSDK 13.3.0 (job 06).
 * Pure object/codec/math helpers remain in sdk.elf; no fake SDK object layouts. */
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sdklift.h"

#define A0 c->x[0]
#define A1 c->x[1]
#define RET(v) (c->x[0] = (uint64_t)(v))
BOOLEAN NTAPI SystemFunction036(PVOID, ULONG);
int strikers_heap_range(uint64_t *, uint64_t *);
void cxxrt_guest_region(uint64_t, uint64_t);
void cxxrt_mem_changed(uint64_t, uint64_t);

/* Cryptographic entropy deliberately bypasses STRIKERS_FIXED_SEED. */
static void crypto_random(Ctx *c) {
    uint8_t *p = (uint8_t *)(uintptr_t)A0;
    for (uint64_t left = A1; left;) {
        ULONG n = (ULONG)(left > 0x10000000 ? 0x10000000 : left);
        if (!SystemFunction036(p, n)) guest_trap("cryptographic entropy unavailable", c->x[30] - 4);
        p += n; left -= n;
    }
}

/* Reserve/commit track the host's own address-space resources, in guest bytes. */
typedef struct Region { uint64_t base, size, committed; struct Region *next; } Region;
static Region *regions;
static SRWLOCK region_lock = SRWLOCK_INIT;
static void allocate_region(Ctx *c) {
    uint64_t out = A0, size = A1;
    RET(0xe03);
    if (!out || !size || (size & 0xfff)) return;
    Region *r = calloc(1, sizeof *r);
    if (!r) { RET(0x1003); return; }
    void *p = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_READWRITE);
    if (!p) { free(r); RET(0x1003); return; }
    r->base = (uint64_t)(uintptr_t)p; r->size = size;
    AcquireSRWLockExclusive(&region_lock);
    r->next = regions; regions = r;
    ReleaseSRWLockExclusive(&region_lock);
    cxxrt_guest_region(r->base, size);
    wr64(out, r->base); RET(0);
}
static void allocate_pages(Ctx *c) {
    uint64_t addr = A0, size = A1;
    RET(0xe03);
    if (!size || ((addr | size) & 0xfff) || addr + size < addr) return;
    AcquireSRWLockExclusive(&region_lock);
    for (Region *r = regions; r; r = r->next) if (addr >= r->base && addr + size <= r->base + r->size) {
        /* Count newly committed pages, including overlapping commits. */
        uint64_t fresh = 0;
        for (uint64_t p = addr; p < addr + size;) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery((void *)(uintptr_t)p, &mi, sizeof mi)) break;
            uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
            if (end > addr + size) end = addr + size;
            if (mi.State != MEM_COMMIT) fresh += end - p;
            p = end;
        }
        cxxrt_mem_changed(addr, size);
        if (VirtualAlloc((void *)(uintptr_t)addr, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE)) {
            r->committed += fresh; RET(0);
        } else RET(0x1003);
        cxxrt_mem_changed(addr, size);
        break;
    }
    ReleaseSRWLockExclusive(&region_lock);
}
static void free_region(Ctx *c) {
    uint64_t addr = A0;
    RET(0xe03);
    AcquireSRWLockExclusive(&region_lock);
    Region **p = &regions;
    while (*p && (*p)->base != addr) p = &(*p)->next;
    if (*p) {
        Region *r = *p;
        cxxrt_mem_changed(addr, r->size);
        if (VirtualFree((void *)(uintptr_t)addr, 0, MEM_RELEASE)) { *p = r->next; free(r); RET(0); }
        cxxrt_mem_changed(addr, 0);
    }
    ReleaseSRWLockExclusive(&region_lock);
}
static void resource_usage(Ctx *c) {
    /* Target @37c990 returns a 16-byte aggregate in x0/x1 (total, used). */
    uint64_t reserved = 0, committed = 0;
    AcquireSRWLockShared(&region_lock);
    for (Region *r = regions; r; r = r->next) { reserved += r->size; committed += r->committed; }
    ReleaseSRWLockShared(&region_lock);
    A0 = reserved; A1 = committed;
}
static void query_memory(Ctx *c) {
    /* @37d0dc: seven fields, last a u32 at +0x30. No write to tail padding. */
    uint64_t out = A0, base, heap;
    strikers_heap_range(&base, &heap);
    resource_usage(c);
    uint64_t committed = A1;
    MEMORYSTATUSEX m = {sizeof m}; GlobalMemoryStatusEx(&m);
    wr64(out, heap + committed);
    wr64(out + 8, m.ullTotalPhys);
    wr64(out + 0x10, heap);
    wr64(out + 0x18, heap);
    wr64(out + 0x20, 0);       /* no console system resource arena */
    wr64(out + 0x28, 0);
    wr32(out + 0x30, 0);
}
static void virtual_memory_enabled(Ctx *c) { RET(1); }

/* SetDyingMessageRegion records the guest-owned diagnostic buffer, as @379170.
 * The crash reporter can inspect these without copying or owning the buffer. */
uint64_t strikers_dying_message_address, strikers_dying_message_size;
static void dying_message(Ctx *c) {
    strikers_dying_message_address = A0; strikers_dying_message_size = A1;
}

static volatile LONG exit_sections;
static void enter_exit_section(Ctx *c) { (void)c; InterlockedIncrement(&exit_sections); }
static void leave_exit_section(Ctx *c) {
    (void)c;
    LONG old;
    do { old = exit_sections; if (!old) return; }
    while (InterlockedCompareExchange(&exit_sections, old - 1, old) != old);
}
static void overclock_enabled(Ctx *c) { RET(0); } /* host has no console overclock mode */

/* settings::Language is a u32; LanguageCode is an eight-byte NUL-padded tag.
 * @397230 compares strings up to their first NUL, not arbitrary tail bytes. */
static const char languages[][8] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl",
    "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
static uint64_t language_code(uint32_t language) {
    uint64_t v = 0;
    if (language >= sizeof languages / sizeof languages[0]) guest_trap("invalid settings Language enum", language);
    memcpy(&v, languages[language], 8); return v;
}
static void language_make(Ctx *c) { RET(language_code((uint32_t)A0)); }
static void language_equal(Ctx *c) {
    uint64_t code = language_code(rd32(A0));
    RET(strncmp((const char *)&code, (const char *)(uintptr_t)A1, 8) == 0);
}
static void ro_range(Ctx *c) {
    uint64_t out = A0, end = A1, module = c->x[2], lo, hi;
    if (module >= GUEST_BASE && module < GUEST_BASE + GUEST_IMAGE_SIZE) {
        lo = GUEST_BASE + ((GUEST_TEXT_SIZE + 0xfff) & ~0xfffULL); hi = GUEST_BASE + GUEST_RO_END;
    } else if (module >= SDK_BASE && module < SDK_BASE + SDK_IMAGE_SIZE) {
        lo = SDK_BASE + 0x5d5000; hi = SDK_BASE + 0xc83000;
    } else { RET(0); return; }
    wr64(out, lo); wr64(end, hi); RET(1);
}
static void frame_history(Ctx *c) {
    /* @401710: FrameInfo stride 0x28; null/zero is a capacity query (8).
     * No console compositor records exist in this host, so filled count is 0. */
    RET(!A0 || !(int32_t)A1 ? 8 : 0);
}
static void musl_init_dso(Ctx *c) {
    /* Host CRT owns libc/TLS; registering a second guest musl DSO would create
     * incompatible FILE/TLS state. No guest musl code is used for those APIs. */
    RET(1); /* SDK @40d560: DSO initialized; @40d53c returns 2 on rejection. */
}

static const ImportDef main_imports[] = {
    {"_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", crypto_random},
    {"_ZN2nn2os21AllocateAddressRegionEPmm", allocate_region},
    {"_ZN2nn2os19AllocateMemoryPagesEmm", allocate_pages},
    {"_ZN2nn2os17FreeAddressRegionEm", free_region},
    {"_ZN2nn2os36GetVirtualAddressMemoryResourceUsageEv", resource_usage},
    {"_ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE", query_memory},
    {"_ZN2nn2os29IsVirtualAddressMemoryEnabledEv", virtual_memory_enabled},
    {"_ZN2nn2os21SetDyingMessageRegionEmm", dying_message},
    {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv", enter_exit_section},
    {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", leave_exit_section},
    {"_ZN2nn2oe21IsCpuOverclockEnabledEv", overclock_enabled},
    {"_ZN2nn8settings12LanguageCode4MakeENS0_8LanguageE", language_make},
    {"_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE", language_equal},
    {"_ZN2nn4diag27GetReadOnlyDataSectionRangeEPmS1_m", ro_range},
    {"_ZN2nn2vi16ListFrameHistoryEPNS0_9FrameInfoEiPKNS0_5LayerE", frame_history},
    {"__nnmusl_init_dso", musl_init_dso},
};
void register_main_imports(void) { register_imports(main_imports, sizeof main_imports / sizeof main_imports[0]); }
