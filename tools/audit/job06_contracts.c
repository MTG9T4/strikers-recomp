/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host boundary tests, linked only into a temporary DLL by test_runtime_contracts.py. */
#include <string.h>
#include "os_threads.c"
/* stand-ins for the diagnostics os_threads.c reports to (core/hitch.c, os/nnsdk.c), not linked into this DLL */
void hitch_switch_ctx(Ctx *c, int fiber) { (void)c; (void)fiber; }
volatile LONG64 g_tick_calls;

static ImportDef test_defs[512];
static int test_count, test_traps, fiber_stage, fiber_bad, memory_changes;
static uint64_t test_fiber, test_stack;
static Ctx *active_context;
static DWORD test_fls;
static HANDLE test_thread_entered, test_thread_release;
static uint64_t test_thread_type, test_thread_stack;
static int test_thread_bad;
void register_imports(const ImportDef *d, int n) {
    for (int i = 0; i < n; i++) {
        int k; for (k = 0; k < test_count; k++) if (!strcmp(test_defs[k].name, d[i].name)) break;
        test_defs[k] = d[i]; if (k == test_count) test_count++;
    }
}
#ifndef JOB06_WITH_SDK
void sdk_host_override(const char *name, ImportFn fn) { (void)name; (void)fn; }
uint64_t sdk_symbol(const char *name) { (void)name; return 0; }
#endif
void guest_trap(const char *what, uint64_t pc) { (void)what; (void)pc; test_traps++; }
void trace_printf(const char *fmt, ...) { (void)fmt; }
long long nvn_front_frame_next(void) { return 0; }
Ctx *host_exchange_context(Ctx *c) { Ctx *old = active_context; active_context = c; return old; }
int strikers_heap_range(uint64_t *base, uint64_t *size) { *base = *size = 0; return 0; }
void cxxrt_guest_region(uint64_t b, uint64_t s) { (void)b; (void)s; }
void cxxrt_mem_changed(uint64_t b, uint64_t s) { (void)b; (void)s; memory_changes++; }
void register_main_imports(void);
void register_ngc_imports(void);
static void invoke(const char *name, Ctx *c) {
    for (int i = 0; i < test_count; i++) if (!strcmp(name, test_defs[i].name)) { test_defs[i].fn(c); return; }
    test_traps++;
}
#define REQUIRE(p) do { if (!(p)) return __LINE__; } while (0)
#define OS_CALL(n) invoke("_ZN2nn2os" n, &c)

static volatile LONG sib_stop, sib_started;
static const char *volatile sib_import;
void call_guest(Ctx *c, uint64_t addr) {
    if (addr == 0x9abc) {                     /* audit 04: a guest thread "in lifted code" or "in an import" until told to stop */
        InterlockedIncrement(&sib_started);
        while (!sib_stop) { c->in_import = sib_import; Sleep(1); }
        c->in_import = NULL;
        return;
    }
    if (addr == 0x5678) {
        if (c->x[0] != 0x12345678 || c->x[31] != test_thread_stack + 0x10000 ||
            rd64(c->tpidr + 0x1f8) != test_thread_type || rd8(test_thread_type + 0x40) != 3)
            test_thread_bad = 1;
        SetEvent(test_thread_entered);
        WaitForSingleObject(test_thread_release, INFINITE);
        return;
    }
    Ctx d = {0};
    if (addr != 0x1234 || c->x[0] != 42 || c->x[31] != test_stack + 0x10000 || FlsGetValue(test_fls)) fiber_bad = 1;
    fib_current(&d); if (d.x[0] != test_fiber || active_context != c) fiber_bad = 2;
    FlsSetValue(test_fls, (void *)202);
    c->x[19] = 0xaabbccdd; c->x[31] -= 32;
    fiber_stage = 1; c->x[0] = 0; fib_switch(c);
    if (c->x[19] != 0xaabbccdd || c->x[31] != test_stack + 0x10000 - 32 || FlsGetValue(test_fls) != (void *)202) fiber_bad = 3;
    fib_current(&d); if (d.x[0] != test_fiber || active_context != c) fiber_bad = 4;
    fiber_stage = 2; c->x[0] = 0; /* returned next-fiber pointer: null selects root */
}
static DWORD WINAPI wait_multi(void *p) { multi_wait((Ctx *)p); return 0; }
typedef struct { uint32_t *word; volatile LONG started; uint32_t result; } ArbTest;
static DWORD WINAPI wait_arb(void *p) {
    ArbTest *a = p; Ctx c = {0};
    c.x[0] = 10; c.x[1] = (uint64_t)(uintptr_t)a->word; c.x[2] = 20;
    InterlockedExchange(&a->started, 1); os_dispatch_svc(&c, 0x1a); a->result = (uint32_t)c.x[0]; return 0;
}

__declspec(dllexport) int job06_os_tests(void) {
    Ctx c = {0};
    register_thread_imports(); register_main_imports(); register_ngc_imports();
    REQUIRE(rd32(os_main_thread_type() + 0x1b0) != 0);
    /* SDK 13.3 ThreadType lifecycle, direct-field reads, and the real host start argument.
     * Wwise rejects a started thread whose state byte was left at zero. */
    uint8_t thread_storage[0x1d0]; memset(thread_storage, 0xcc, sizeof thread_storage);
    test_thread_type = (uint64_t)(uintptr_t)thread_storage;
    test_thread_stack = (uint64_t)(uintptr_t)VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    test_thread_entered = CreateEventA(NULL, TRUE, FALSE, NULL);
    test_thread_release = CreateEventA(NULL, TRUE, FALSE, NULL);
    c.x[0] = test_thread_type; c.x[1] = 0x5678; c.x[2] = 0x12345678;
    c.x[3] = test_thread_stack; c.x[4] = 0x10000; c.x[5] = 16;
    OS_CALL("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi");
    REQUIRE(c.x[0] == 0 && thread_storage[0x40] == 1 && thread_storage[0x1c0] == 0xcc);
    REQUIRE(rd16(test_thread_type + 0x44) == 16 && rd16(test_thread_type + 0x46) == 1);
    REQUIRE(rd64(test_thread_type + 0x10) == test_thread_type + 0x10);
    REQUIRE(rd64(test_thread_type + 0x60) == 0x12345678 && rd64(test_thread_type + 0x68) == 0x5678);
    c.x[0] = test_thread_type; OS_CALL("11StartThreadEPNS0_10ThreadTypeE");
    REQUIRE(WaitForSingleObject(test_thread_entered, 5000) == WAIT_OBJECT_0);
    REQUIRE(!test_thread_bad && thread_storage[0x40] == 3);
    SetEvent(test_thread_release);
    c.x[0] = test_thread_type; OS_CALL("10WaitThreadEPNS0_10ThreadTypeE");
    REQUIRE(thread_storage[0x40] == 4);
    OS_CALL("13DestroyThreadEPNS0_10ThreadTypeE");
    REQUIRE(thread_storage[0x40] == 0 && thread_storage[0x1c0] == 0xcc);
    CloseHandle(test_thread_entered); CloseHandle(test_thread_release);
    VirtualFree((void *)(uintptr_t)test_thread_stack, 0, MEM_RELEASE);
    /* Boundary guards catch writing a reference SDK's larger structs. */
    uint64_t timer[10]; memset(timer, 0xcc, sizeof timer);
    c.x[0] = (uint64_t)(uintptr_t)timer; c.x[1] = 1;
    OS_CALL("20InitializeTimerEventEPNS0_14TimerEventTypeENS0_14EventClearModeE");
    REQUIRE(timer[8] == 0xccccccccccccccccULL && ((uint8_t *)timer)[0x10] == 1 && ((uint8_t *)timer)[0x11] == 1);
    c.x[1] = 5000000; c.x[2] = 10000000;
    int64_t start = monotonic_ns();
    OS_CALL("23StartPeriodicTimerEventEPNS0_14TimerEventTypeENS_8TimeSpanES3_");
    OS_CALL("14WaitTimerEventEPNS0_14TimerEventTypeE");
    REQUIRE(monotonic_ns() - start >= 5000000 && ((uint8_t *)timer)[0x12] == 0);
    int64_t first_due = (int64_t)timer[4];
    OS_CALL("14WaitTimerEventEPNS0_14TimerEventTypeE"); REQUIRE((int64_t)timer[4] > first_due);

    uint64_t mw[8], holder[10], event[8] = {0};
    memset(mw, 0xcc, sizeof mw); memset(holder, 0xcc, sizeof holder);
    c.x[0] = (uint64_t)(uintptr_t)event; c.x[1] = 0; c.x[2] = 1;
    OS_CALL("15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE");
    c.x[0] = (uint64_t)(uintptr_t)mw; OS_CALL("19InitializeMultiWaitEPNS0_13MultiWaitTypeE");
    REQUIRE(mw[6] == 0xccccccccccccccccULL);
    c.x[0] = (uint64_t)(uintptr_t)holder; c.x[1] = (uint64_t)(uintptr_t)event;
    OS_CALL("25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE");
    REQUIRE(holder[8] == 0xccccccccccccccccULL && holder[6] == (uint64_t)(uintptr_t)event);
    holder[7] = 0x11223344; /* inline user data survives linking and waiting */
    c.x[0] = (uint64_t)(uintptr_t)mw; c.x[1] = (uint64_t)(uintptr_t)holder;
    OS_CALL("19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE");
    Ctx waiter = {0}; waiter.x[0] = (uint64_t)(uintptr_t)mw;
    HANDLE thread = CreateThread(NULL, 0, wait_multi, &waiter, 0, NULL);
    REQUIRE(thread && WaitForSingleObject(thread, 15) == WAIT_TIMEOUT);
    os_signal_event((uint64_t)(uintptr_t)event);
    REQUIRE(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0); CloseHandle(thread);
    REQUIRE(waiter.x[0] == (uint64_t)(uintptr_t)holder && holder[7] == 0x11223344);
    c.x[0] = (uint64_t)(uintptr_t)event; OS_CALL("12TryWaitEventEPNS0_9EventTypeE"); REQUIRE(c.x[0] == 1);
    c.x[0] = (uint64_t)(uintptr_t)event; OS_CALL("12TryWaitEventEPNS0_9EventTypeE"); REQUIRE(c.x[0] == 0);

    uint64_t fiber[40]; memset(fiber, 0xcc, sizeof fiber);
    test_fls = FlsAlloc(NULL); REQUIRE(test_fls != FLS_OUT_OF_INDEXES);
    FlsSetValue(test_fls, (void *)101);
    test_stack = (uint64_t)(uintptr_t)VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    test_fiber = (uint64_t)(uintptr_t)fiber;
    c.x[0] = test_fiber; c.x[1] = 0x1234; c.x[2] = 42; c.x[3] = test_stack; c.x[4] = 0x10000; c.x[5] = 1;
    OS_CALL("15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi");
    REQUIRE(fiber[5] == 0x1234 && fiber[6] == 42 && fiber[7] == test_stack && fiber[9] == 0x10000);
    c.x[19] = 0x55667788; active_context = &c;
    OS_CALL("13SwitchToFiberEPNS0_9FiberTypeE");
    REQUIRE(fiber_stage == 1 && !fiber_bad && c.x[19] == 0x55667788 && FlsGetValue(test_fls) == (void *)101 && active_context == &c);
    c.x[0] = test_fiber; OS_CALL("13SwitchToFiberEPNS0_9FiberTypeE");
    REQUIRE(fiber_stage == 2 && !fiber_bad && ((uint8_t *)fiber)[0x20] == 4);
    c.x[0] = test_fiber; OS_CALL("13FinalizeFiberEPNS0_9FiberTypeE");
    REQUIRE(((uint8_t *)fiber)[0x20] == 0); FlsFree(test_fls);
    REQUIRE(ConvertFiberToThread()); t_root_fiber = NULL; t_root_ctx = NULL;
    VirtualFree((void *)(uintptr_t)test_stack, 0, MEM_RELEASE);

    uint64_t out = 0, memory[8];
    c.x[0] = (uint64_t)(uintptr_t)&out; c.x[1] = 0x3000;
    OS_CALL("21AllocateAddressRegionEPmm"); REQUIRE(c.x[0] == 0 && out);
    uint64_t region = out;
    c.x[0] = region; c.x[1] = 0x2000; OS_CALL("19AllocateMemoryPagesEmm"); REQUIRE(c.x[0] == 0);
    memset((void *)(uintptr_t)region, 0x7b, 0x2000);
    c.x[0] = region + 0x1000; c.x[1] = 0x1000; OS_CALL("19AllocateMemoryPagesEmm"); REQUIRE(c.x[0] == 0);
    OS_CALL("36GetVirtualAddressMemoryResourceUsageEv"); REQUIRE(c.x[0] == 0x3000 && c.x[1] == 0x2000);
    memset(memory, 0xcc, sizeof memory); c.x[0] = (uint64_t)(uintptr_t)memory;
    OS_CALL("15QueryMemoryInfoEPNS0_10MemoryInfoE");
    REQUIRE(memory[0] == 0x2000 && memory[1] > memory[0] && (memory[6] >> 32) == 0xcccccccc && memory[7] == 0xccccccccccccccccULL);
    c.x[0] = region + 0x2000; c.x[1] = 0x2000; OS_CALL("19AllocateMemoryPagesEmm"); REQUIRE(c.x[0] == 0xe03);
    c.x[0] = region; OS_CALL("17FreeAddressRegionEm"); REQUIRE(c.x[0] == 0 && memory_changes >= 5);
    c.x[0] = region; OS_CALL("17FreeAddressRegionEm"); REQUIRE(c.x[0] == 0xe03);
    OS_CALL("36GetVirtualAddressMemoryResourceUsageEv"); REQUIRE(c.x[0] == 0 && c.x[1] == 0);

    /* SVC raw register ABI: arbitration must block and hand ownership over. */
    uint32_t word = 10 | 0x40000000u;
    ArbTest a = {&word, 0, 99}; thread = CreateThread(NULL, 0, wait_arb, &a, 0, NULL);
    REQUIRE(thread);
    ULONGLONG deadline = GetTickCount64() + 2000;
    int queued = 0;
    while (!queued && GetTickCount64() < deadline) {
        AcquireSRWLockShared(&g_arb_lock); queued = g_arb_waiters != NULL; ReleaseSRWLockShared(&g_arb_lock);
        if (!queued) Sleep(1);
    }
    REQUIRE(queued && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT);
    c.x[0] = (uint64_t)(uintptr_t)&word; REQUIRE(os_dispatch_svc(&c, 0x1b));
    REQUIRE(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0 && a.result == 0 && word == 20); CloseHandle(thread);
    c.x[0] = (uint64_t)(uintptr_t)&word; os_dispatch_svc(&c, 0x1b); REQUIRE(word == 0);
    uint64_t key; c.x[0] = os_system_event_new(0, &key); os_dispatch_svc(&c, 0x16); REQUIRE(c.x[0] == 0);
    c.x[0] = 0; os_dispatch_svc(&c, 0x16); REQUIRE(c.x[0] == 0xe401);
    c.x[0] = 123; c.x[1] = 456; os_dispatch_svc(&c, 0x1f); REQUIRE(c.x[0] == 0xf201 && c.x[1] == 0);
    os_dispatch_svc(&c, 0x21); REQUIRE(c.x[0] == 0xe401); REQUIRE(!os_dispatch_svc(&c, 0xffff));

    uint8_t random[32] = {0}; c.x[0] = (uint64_t)(uintptr_t)random; c.x[1] = sizeof random;
    invoke("_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", &c);
    int nonzero = 0; for (int i = 0; i < 32; i++) nonzero |= random[i]; REQUIRE(nonzero);
    c.x[0] = 1; invoke("_ZN2nn8settings12LanguageCode4MakeENS0_8LanguageE", &c);
    REQUIRE(c.x[0] == 0x53552d6e65ULL);
    uint32_t language = 1; char tag[8] = {'e','n','-','U','S',0,'x','x'};
    c.x[0] = (uint64_t)(uintptr_t)&language; c.x[1] = (uint64_t)(uintptr_t)tag;
    invoke("_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE", &c); REQUIRE(c.x[0] == 1);
    uint8_t ngc[0x38] = {0}; char text[] = "unfiltered"; int count = 99;
    c.x[0] = (uint64_t)(uintptr_t)ngc; c.x[1] = (uint64_t)(uintptr_t)random; c.x[2] = 32;
    invoke("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", &c); REQUIRE(c.x[0] == 0x892 && !ngc[9]);
    c.x[0] = (uint64_t)(uintptr_t)ngc; c.x[1] = (uint64_t)(uintptr_t)&count; c.x[2] = (uint64_t)(uintptr_t)text;
    invoke("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", &c); REQUIRE(c.x[0] == 0x292 && count == 99 && !strcmp(text,"unfiltered"));
    REQUIRE(test_traps == 0); return 0;
}

/* Audit 04: console-core slots and the runtime's own threads (F22), the nn::os priority mapping of 18, yields (F23). */
static volatile LONG yield_low_count, yield_stop;
static DWORD WINAPI pin_probe(void *p) { os_pin_host_thread(); WaitForSingleObject((HANDLE)p, INFINITE); return 0; }
static DWORD WINAPI yield_low(void *p) { (void)p; while (!yield_stop) InterlockedIncrement(&yield_low_count); return 0; }
__declspec(dllexport) int audit04_os_tests(void) {
    /* slot geometry: six processors from the top of the mask per slot */
    REQUIRE(slot_procs(0xffffffffull, 0) == 0xfc000000ull && slot_procs(0xffffffffull, 1) == 0x03f00000ull);
    REQUIRE(slot_procs(0xffffffffull, 4) == 0xfcull && slot_procs(0xf0f0ull, 0) == 0xf0c0ull);
    /* priority mapping with the console core model (the default): 18 with 17, above 19..31 */
    REQUIRE(console_cores());
    REQUIRE(host_priority(1) == THREAD_PRIORITY_TIME_CRITICAL && host_priority(8) == THREAD_PRIORITY_HIGHEST);
    REQUIRE(host_priority(15) == THREAD_PRIORITY_ABOVE_NORMAL && host_priority(16) == THREAD_PRIORITY_NORMAL);
    REQUIRE(host_priority(17) == THREAD_PRIORITY_BELOW_NORMAL && host_priority(18) == THREAD_PRIORITY_BELOW_NORMAL);
    REQUIRE(host_priority(19) == THREAD_PRIORITY_LOWEST && host_priority(31) == THREAD_PRIORITY_LOWEST);
    /* audit 05 (F35): topology slots are three whole physical cores of one efficiency class, disjoint, the most performant
     * class first; the console cores are the first processor of each core of this game's slot, and the runtime's own
     * threads stay off all of their processors */
    {   /* synthetic topologies (the live slot state saved and restored around them) */
        static DWORD_PTR saved_core[64][3]; int saved_n = g_slot_n, saved_topo_n = g_topo_n; static struct { DWORD_PTR mask; int eff; } saved_topo[64];
        memcpy(saved_core, g_slot_core, sizeof saved_core); memcpy(saved_topo, g_topo, sizeof saved_topo);
        /* 8 cores with two hardware threads, one class: exactly the old six-from-the-top slots */
        int saved_mode = slots_mode(); g_slots_mode = 0;   /* the mixed layout (opt-in, STRIKERS_OS_SLOTS=mixed) */
        g_topo_n = 8; for (int i = 0; i < 8; i++) { g_topo[i].mask = (DWORD_PTR)3 << (2 * i); g_topo[i].eff = 0; }
        REQUIRE(slot_layout(0xffff) && g_slot_n == 2);
        for (int k = 0; k < 2; k++) REQUIRE((g_slot_core[k][0] | g_slot_core[k][1] | g_slot_core[k][2]) == slot_procs(0xffff, k));
        /* hybrid: 8 P-cores (SMT, class 1) on 0-15, 16 E-cores (class 0) on 16-31, as this PC. Mixed (the default): two P-cores
         * from the top and one E-core per slot from the bottom (three such, the two lowest P-cores left), then E-cores in threes from the top */
        g_topo_n = 24; for (int i = 0; i < 8; i++) { g_topo[i].mask = (DWORD_PTR)3 << (2 * i); g_topo[i].eff = 1; }
        for (int i = 0; i < 16; i++) { g_topo[8 + i].mask = (DWORD_PTR)1 << (16 + i); g_topo[8 + i].eff = 0; }
        REQUIRE(slot_layout(0xffffffff) && g_slot_n == 7);
        REQUIRE(g_slot_core[0][0] == 0xc000 && g_slot_core[0][1] == 0x3000 && g_slot_core[0][2] == 0x10000);     /* E-cores from the bottom */
        REQUIRE((g_slot_core[2][0] | g_slot_core[2][1] | g_slot_core[2][2]) == 0x400f0 && (g_slot_core[3][0] | g_slot_core[3][1] | g_slot_core[3][2]) == 0xe0000000);
        g_slots_mode = 2; REQUIRE(slot_layout(0xffffffff) && g_slot_n == 7 && (g_slot_core[0][0] | g_slot_core[0][1] | g_slot_core[0][2]) == 0xfc00);   /* pcores */
        g_slots_mode = 1; REQUIRE(!slot_layout(0xffffffff));                                                          /* top: the old slots */
        g_slots_mode = saved_mode;
        memcpy(g_slot_core, saved_core, sizeof saved_core); memcpy(g_topo, saved_topo, sizeof saved_topo); g_slot_n = saved_n; g_topo_n = saved_topo_n;
    }
    if (g_slot_layout) {
        DWORD_PTR seen = 0;
        for (int i = 0; i < g_slot_n; i++) {
            REQUIRE(slot_mask(i) && !(seen & slot_mask(i)) && slot_class(i) <= slot_class(0));
            for (int j = 0; j < 3; j++) {           /* one physical core each, of the slot's class */
                DWORD_PTR first = g_slot_core[i][j] & (~g_slot_core[i][j] + 1);
                REQUIRE(first && (core_of(first) & g_slot_proc) == g_slot_core[i][j]);
            }
            seen |= slot_mask(i);
        }
        int own = g_slot_own >= 0 ? g_slot_own : 0;
        for (int j = 0; j < 3; j++) REQUIRE(g_cc_core[j] == (g_slot_core[own][j] & (~g_slot_core[own][j] + 1)) && !(os_host_cpu_mask() & core_of(g_cc_core[j])));
        fprintf(stderr, "audit05: %d topology slots; this game's slot %d = %llx (class %d)\n", g_slot_n, own, (unsigned long long)slot_mask(own), slot_class(own));
    } else fprintf(stderr, "audit05: no topology slots (STRIKERS_OS_SLOTS=top or no topology)\n");
    /* the runtime's own threads leave a slot another game takes, and come back when it is free again */
    uint64_t base = os_host_cpu_mask();
    REQUIRE(base && g_host_dynamic);
    int k = -1;
    if (g_slot_own >= 0) for (int i = 0; i < g_slot_n && k < 0; i++) if (i != g_slot_own && !(slot_others() & slot_mask(i))) k = i;
    uint64_t m2 = base;
    if (g_slot_own < 0 || k < 0) { fprintf(stderr, "audit04: every console-core slot is held by other games: the slot test is skipped\n"); }
    else {
    REQUIRE(!(base & slot_mask(g_slot_own)));
    HANDLE release = CreateEventA(NULL, TRUE, FALSE, NULL), t = CreateThread(NULL, 0, pin_probe, release, 0, NULL);
    REQUIRE(release && t);
    Sleep(100);
    uint64_t m0 = os_host_cpu_mask();
    REQUIRE(SetThreadAffinityMask(t, (DWORD_PTR)m0) == (DWORD_PTR)m0);       /* returns the mask it had: placed by os_pin_host_thread */
    char name[64]; slot_name(name, sizeof name, k);
    HANDLE other = CreateMutexA(NULL, FALSE, name);                          /* "another game" takes slot k */
    REQUIRE(other && GetLastError() != ERROR_ALREADY_EXISTS);
    Sleep(800);
    uint64_t m1 = os_host_cpu_mask();
    REQUIRE(m1 == host_mask_with(slot_others()) && (popcount64(m0 & ~slot_mask(k)) < 4 || !(m1 & slot_mask(k))));
    REQUIRE(SetThreadAffinityMask(t, (DWORD_PTR)m1) == (DWORD_PTR)m1);       /* the running thread was moved */
    CloseHandle(other);
    Sleep(800);
    m2 = os_host_cpu_mask();
    REQUIRE(m2 == host_mask_with(slot_others()) && (m2 & slot_mask(k)) == (base & slot_mask(k)));
    REQUIRE(SetThreadAffinityMask(t, (DWORD_PTR)m2) == (DWORD_PTR)m2);
    SetEvent(release); REQUIRE(WaitForSingleObject(t, 2000) == WAIT_OBJECT_0); CloseHandle(t); CloseHandle(release);
    }
    /* a yield does not hand the processor to a lower-priority thread (the console yields to the same priority only) */
    DWORD_PTR one = (DWORD_PTR)(m2 & (~m2 + 1));
    HANDLE self = GetCurrentThread();
    DWORD_PTR old_aff = SetThreadAffinityMask(self, one);
    int old_prio = GetThreadPriority(self);
    SetThreadPriority(self, THREAD_PRIORITY_ABOVE_NORMAL); SetThreadPriorityBoost(self, TRUE);
    HANDLE low = CreateThread(NULL, 0, yield_low, NULL, CREATE_SUSPENDED, NULL);
    REQUIRE(low);
    SetThreadAffinityMask(low, one); SetThreadPriority(low, THREAD_PRIORITY_LOWEST); SetThreadPriorityBoost(low, TRUE);
    ResumeThread(low);
    Sleep(1);                                                                /* lets it start once */
    /* A job loop's short spins: a yield after every 100 us of work, 200 ms long, with SwitchToThread (the old yield) and with
     * os_yield; then 200 ms of yields without a break (a long spin). How long the lower-priority thread ran in each. Only
     * comparisons are asserted: interrupts and other programs make the absolute counts vary. */
    LONG ran[3]; long yields[3] = {0, 0, 0};
    LARGE_INTEGER qf, q0, q1; QueryPerformanceFrequency(&qf);
    for (int pass = 0; pass < 3; pass++) {
        LONG before = yield_low_count;
        ULONGLONG until = GetTickCount64() + 200;
        while (GetTickCount64() < until) {
            if (pass == 0) SwitchToThread(); else os_yield();
            yields[pass]++;
            if (pass < 2) { QueryPerformanceCounter(&q0); do QueryPerformanceCounter(&q1); while ((q1.QuadPart - q0.QuadPart) * 10000 < qf.QuadPart); }
        }
        ran[pass] = yield_low_count - before;
    }
    yield_stop = 1;
    SetThreadPriority(self, old_prio); SetThreadAffinityMask(self, old_aff);
    WaitForSingleObject(low, 2000); CloseHandle(low);
    fprintf(stderr, "audit04 yield on processor mask %llx: short spins with SwitchToThread %ld yields, the lower-priority thread counted %ld; with os_yield %ld yields, it counted %ld; one long spin %ld yields, it counted %ld\n",
            (unsigned long long)one, yields[0], (long)ran[0], yields[1], (long)ran[1], yields[2], (long)ran[2]); fflush(stderr);
    REQUIRE(yields[1] > 100 && ran[0] > 0 && (LONGLONG)ran[1] * 20 < (LONGLONG)ran[0]);     /* short spins keep the processor */
    REQUIRE(ran[2] > 0);                                                                  /* a long one does not starve the core */
    /* whom a yield of the main thread (nn::os 16, core 0) gives the processor to: a runnable thread of its core at 16, not one
     * at 19, not one waiting in an import */
    REQUIRE(!yield_switches());
    static uint8_t types[2][0x1d0];
    uint64_t stack = (uint64_t)(uintptr_t)VirtualAlloc(NULL, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    int prios[2] = {19, 16};
    Ctx s = {0};
    sib_import = NULL;
    for (int i = 0; i < 2; i++) {
        s.x[0] = (uint64_t)(uintptr_t)types[i]; s.x[1] = 0x9abc; s.x[2] = i; s.x[3] = stack + 0x10000 * (uint64_t)i; s.x[4] = 0x10000; s.x[5] = prios[i];
        invoke("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi", &s); REQUIRE(s.x[0] == 0);
        s.x[0] = (uint64_t)(uintptr_t)types[i]; invoke("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE", &s);
        while (sib_started < i + 1) Sleep(1);
        if (i == 0) REQUIRE(!yield_switches());                             /* only a lower priority on the core */
    }
    REQUIRE(yield_switches());                                              /* a runnable thread at the same priority */
    sib_import = "_ZN2nn2os9WaitEventEPNS0_9EventTypeE"; Sleep(20);
    REQUIRE(!yield_switches());                                             /* it waits */
    sib_import = "_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE"; Sleep(20);
    REQUIRE(yield_switches());                                              /* it runs a fiber */
    sib_stop = 1;
    for (int i = 0; i < 2; i++) { s.x[0] = (uint64_t)(uintptr_t)types[i]; invoke("_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE", &s); }
    REQUIRE(!yield_switches());                                             /* both ended */
    /* audit 05: the slot watcher runs code of this DLL; the harness unloads it after the tests (a05-contracts2 crashed so) */
    InterlockedExchange(&g_slot_watch_stop, 1);
    if (g_slot_watch_thread) WaitForSingleObject(g_slot_watch_thread, 2000);
    return 0;
}
