/* SPDX-License-Identifier: GPL-3.0-or-later
 * Upstream: smo-recomp/runtime/os/os_threads.c (0414341471307605b8cb5d0c1a9928db5fe84e07).
 * Adapted for Strikers; see docs/phase2-runtime.md. */
/* nn::os threading and synchronisation on top of Win32.
 *
 * Guest synchronisation objects (MutexType, EventType, ...) are opaque here:
 * each one is identified by its guest address and backed by a host object
 * kept in a side table, so the guest layout never has to be known. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <malloc.h>
#include <string.h>
#include <windows.h>
#include <tlhelp32.h>
#include "host_cpu.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define TICK_HZ 19200000ULL

typedef struct Obj {
    uint64_t key;
    struct Obj *next;
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    int signaled, autoclear;                 /* events */
    uint64_t *buf, cap, len, head;           /* message queues */
    HANDLE thread;                           /* threads */
    uint64_t fn, arg, stack, stack_size, name;
    uint64_t name_kept;                      /* audit 06: the name copy of the thread's previous run (os_SetThreadName reuses it) */
    int priority, finished;
    int ideal_core, core_set;                /* threads: nn::os ideal core and affinity mask as the game set them */
    uint64_t core_mask;
    struct Obj *alias;                       /* system event attached by handle: the event object it shares */
    uint32_t rhandle, whandle;               /* kernel-style handles of a system event created here */
    void *fiber;
    Ctx *fiber_ctx;
    DWORD fiber_owner;
    int64_t timer_due, timer_period;
    int timer_running;
    int in_multi;                            /* events: linked to a multiwait holder (multi_link) */
} Obj;

#define BUCKETS 65536
static Obj *g_table[BUCKETS];
static SRWLOCK g_table_lock = SRWLOCK_INIT;
static __declspec(thread) Obj *t_current_fiber;
static SRWLOCK g_multi_lock = SRWLOCK_INIT;
static long long g_multi_wakes;                       /* WaitAny wakeups (os_thread_report) */
static volatile long long g_long_yields;              /* yields that gave the processor away after a millisecond of spinning */
static CONDITION_VARIABLE g_multi_cv = CONDITION_VARIABLE_INIT;
static void multi_notify(void) {
    AcquireSRWLockExclusive(&g_multi_lock);
    WakeAllConditionVariable(&g_multi_cv);
    ReleaseSRWLockExclusive(&g_multi_lock);
}

/* Every nn::os call looks its object up here (each LockMutex, UnlockMutex, SignalEvent, ...: thousands a frame from every
 * game thread). Lookups take the table lock shared; only the first use of an address (an insertion) takes it exclusively.
 * Before audit 04 every lookup took it exclusively: one game thread preempted inside it (at the end of a time slice on a
 * loaded PC) stopped every other game thread's next nn::os call until it ran again. Objects are never removed. */
static Obj *obj(uint64_t key) {
    unsigned h = (unsigned)((key >> 3) * 2654435761u) & (BUCKETS - 1);
    AcquireSRWLockShared(&g_table_lock);
    Obj *o = g_table[h];
    while (o && o->key != key) o = o->next;
    ReleaseSRWLockShared(&g_table_lock);
    if (o) return o->alias ? o->alias : o;
    AcquireSRWLockExclusive(&g_table_lock);
    o = g_table[h];                            /* again: another thread may have inserted it meanwhile */
    while (o && o->key != key) o = o->next;
    if (!o) {
        o = calloc(1, sizeof *o);
        o->key = key;
        InitializeCriticalSection(&o->cs);
        InitializeConditionVariable(&o->cv);
        o->next = g_table[h];
        g_table[h] = o;
        {   static LONG n; LONG k = InterlockedIncrement(&n);      /* audit 06: how many object addresses the table holds (never removed): a leak shows as steady growth */
            if (!(k & 1023)) { printf("nn::os: %ld object addresses known\n", (long)k); fflush(stdout); } }
    }
    ReleaseSRWLockExclusive(&g_table_lock);
    return o->alias ? o->alias : o;            /* a system event attached by handle shares its event */
}
/* the object itself, without following an alias (its own clear mode) */
static Obj *obj_raw(uint64_t key) {
    Obj *o = obj(key);
    unsigned h = (unsigned)((key >> 3) * 2654435761u) & (BUCKETS - 1);
    AcquireSRWLockShared(&g_table_lock);
    Obj *r = g_table[h];
    while (r && r->key != key) r = r->next;
    ReleaseSRWLockShared(&g_table_lock);
    return r ? r : o;
}
/* Forget an object so a new one created at the same address starts clean. */
static void obj_reset(uint64_t key) {
    Obj *o = obj(key);
    EnterCriticalSection(&o->cs);
    o->signaled = o->autoclear = 0;
    o->len = o->head = 0;
    LeaveCriticalSection(&o->cs);
}

static DWORD ns_to_ms(int64_t ns) { return ns <= 0 ? 0 : (DWORD)((ns + 999999) / 1000000); }

/* nn::os::SleepThread takes nanoseconds and the console's timer is good to a few microseconds. Sleep() counts whole
 * milliseconds on a 1 ms (or 15.6 ms) grid, so the wait runs on a high-resolution waitable timer (100 ns units,
 * Windows 10 1803 and later); Sleep() is only the fallback. A zero or negative span yields, as on the console. */
/* Audit 04 (F23): yields (YieldThread, SleepThread(0)) as on the console. The console's yield (svcSleepThread(0)) moves the
 * thread behind the other ready threads of ITS priority on ITS core: it never runs a lower-priority thread, and it returns at
 * once when no other thread of that priority is ready there. It was SwitchToThread, which hands the rest of the time slice to
 * ANY ready thread of that processor, lower priorities and other programs included (Sleep(0) does the same on this Windows 11:
 * measured, a priority-6 thread ran for ~5 ms after each Sleep(0) of a priority-9 thread on its processor). The game's job
 * loops poll with SleepThread(0) (the main thread waits for its job queue that way, f_7100687f80), so on a busy PC every
 * such yield gave milliseconds to another program's normal thread on the main thread's processor: with 32 busy programs 1,594
 * of 1,799 frames over 40 ms were the main thread "in SleepThread, not on a CPU" (STRIKERS_HITCH, a04-base-load-hitch1)
 * and the match ran at 6-20 fps. Now a yield calls SwitchToThread only when a game thread that may run on the same core at
 * the same or a higher nn::os priority is runnable (in lifted code or a fiber, not inside an import); otherwise it returns
 * at once, as the console's does, except after a millisecond of continuous yielding (os_yield). Without the console core
 * model, or with STRIKERS_OS_YIELD=switch: SwitchToThread always. */
static volatile LONG g_sched_gen;                     /* bumped when a thread starts or ends or changes priority or core mask */
static Ctx *volatile g_main_ctx_os;
static DWORD g_main_tid;                             /* audit 05: the game's main thread (nnMain), for the per-thread CPU lines */
void os_set_main_ctx(Ctx *c) { g_main_ctx_os = c; g_main_tid = GetCurrentThreadId(); }   /* boot.c, on the main guest thread: its context */
static void os_yield(void);
static __declspec(thread) HANDLE t_sleep_timer;      /* audit 06: closed when a guest thread ends (thread_main) */
static void sleep_ns(int64_t ns) {
#define timer t_sleep_timer
    static __declspec(thread) int no_timer;
    LARGE_INTEGER due;
    if (ns <= 0) { os_yield(); return; }
    if (!timer && !no_timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (!timer) no_timer = 1;
    }
    due.QuadPart = -((ns + 99) / 100);
    /* STRIKERS_SLEEP_LOG=1: a line for every sleep that lasted 2 ms longer than asked (first 200). */
    static int lg = -1; static LONG nl;
    if (lg < 0) lg = getenv("STRIKERS_SLEEP_LOG") && atoi(getenv("STRIKERS_SLEEP_LOG"));
    LARGE_INTEGER q0, q1, fq;
    if (lg) QueryPerformanceCounter(&q0);
    if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) Sleep(ns_to_ms(ns));
    else WaitForSingleObject(timer, INFINITE);
    if (lg) {
        extern long long nvn_front_frame_next(void);
        QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&fq);
        double ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)fq.QuadPart, asked = (double)ns / 1e6;
        {   /* per thread and frame: how many sleeps, asked and slept in all; a line when a frame after 1900 had over 8 ms of them */
            static __declspec(thread) long long fr0, cnt; static __declspec(thread) double a_sum, s_sum, a_max; static LONG nf;
            long long fr = nvn_front_frame_next();
            if (fr != fr0) {
                if (fr0 > 1900 && s_sum > 8.0 && InterlockedIncrement(&nf) <= 300) printf("sleep_log: frame %lld, thread %lu: %lld sleeps in the frame, asked %.3f ms in all (longest asked %.3f), slept %.3f ms\n", fr0, GetCurrentThreadId(), cnt, a_sum, a_max, s_sum);
                fr0 = fr; cnt = 0; a_sum = s_sum = a_max = 0;
            }
            cnt++; a_sum += asked; s_sum += ms; if (asked > a_max) a_max = asked;
        }
        if (ms > asked + 2.0 && InterlockedIncrement(&nl) <= 600) { printf("sleep_log: frame %lld, thread %lu: asked %.3f ms, slept %.3f ms (%s)\n", nvn_front_frame_next(), GetCurrentThreadId(), asked, ms, timer ? "high-resolution timer" : "Sleep"); fflush(stdout); }
    }
}
#undef timer

/* nn::os thread priorities run from 0 (highest) to 31 (lowest), 16 is the default; the application's main thread
 * starts at the priority of its NPDM (44 - 28 = 16 in nn::os terms). Windows has seven levels per process class:
 * the order is kept, the spacing is not. */
#define OS_DEFAULT_PRIORITY 16
#define OS_DEFAULT_CORE 0                    /* main.npdm: default core id 0 */
/* Console scheduling model (on by default; STRIKERS_OS_CONSOLE_CORES=0 turns it off). The game creates its threads on one
 * core of the console's three (mask 1, 2 or 4) or on all three (os_thread_report lists them at the end of every run),
 * and the console runs on each core only the highest-priority ready thread of that core. With the model:
 *   - three host processors stand for the console's cores 0..2 and every guest thread's affinity is its nn::os core mask
 *     mapped onto them (set at StartThread and at every SetThreadCoreMask), so two threads the console keeps on one core
 *     never run at the same time here either;
 *   - six priority levels (<= 4, 5..8, 9..15, 16, 17..18, 19..31; 18 was with 19..31 before audit 04, see host_priority),
 *     Windows' dynamic priority boost off: the game
 *     uses 0, 7, 8, 15, 16, 17, 18, 19, 20 and
 *     22; what must stay apart is 17 against 18 on core 2: the model draw buffer update (描画バッファ更新, 17) that every
 *     scene starts at the end of a frame and waits for at the start of the next, and the archive decompression
 *     (DecompThread, 18) busy while a stage loads. At one level the update waited behind the decompression for a whole
 *     time slice, long enough for the scene being left to be deleted under it (HakoniwaStateDeleteScene runs a frame or
 *     two after the scene's last frame): "pure virtual function called" in the update's list of actors on entering a
 *     stage from the title menu, 2 times in 91 runs of the stage stress test (tests/stress_stages.py, two games on the
 *     same three processors); with 18..31 a level lower, 0 times in 276 runs of it. 17 is the last level above
 *     Windows' lowest;
 *   - GetCurrentCoreNumber / GetCurrentProcessorNumber answer the core the thread is really on.
 * NOT the console's scheduler all the same: Windows time-slices equal priorities, other processes of the PC take the
 * three processors too, and host-side threads (graphics driver, audio device, movie decoding) are not part of it.
 * ON BY DEFAULT (STRIKERS_OS_CONSOLE_CORES=0 turns it off). Measured on 20 s boots, 6 runs each: with free
 * scheduling 4 of 6 runs failed in sead heap code on the loading path (sead::ExpHeap::tryAlloc failing inside
 * ParallelSZSDecompressor, or a sead::Heap destructor reading address -1); with the console core model 0 of 6 failed.
 * SMO depends on its threads being scheduled the console's way, so that is the default.
 * The three processors are taken from the top of a slot of six (three cores with their second hardware threads): every
 * game running at once on the PC takes the first slot no other game holds (a named mutex per slot, held while the
 * process lives), so games side by side (tests, or a test beside the copy being played) do not share processors. One
 * game alone takes slot 0, the top of the affinity mask: the processors it always had. Before the slots, four test
 * games at once all ran their console cores on the same three processors. STRIKERS_OS_HOST_CPUS=<hexadecimal mask> chooses
 * from those processors instead (and takes no slot). */
static int g_cc = -1;
static DWORD_PTR g_cc_core[3];
static DWORD_PTR g_slot_proc;                           /* the processors the slots are cut from (the process's at start) */
static int g_slot_own = -1, g_slot_n;                   /* this game's slot (-1: none), number of slots */
/* Audit 05 (F35): slots follow the processor topology. This PC's processor is a hybrid one: processors 0-15 are 8 P-cores with two
 * hardware threads, 16-31 are 16 E-cores without them. Slots of six processors cut from the top put the console cores of
 * the first game on E-cores 31/29/27 and left 30/28/26 idle, which are three more E-cores, not second hardware threads; the
 * same game work costs about 30% more CPU there than on P-cores (docs/audit-04.md, section 4). With STRIKERS_OS_SLOTS=pcores
 * (or =mixed, below; the default stays the old slots, see slots_mode) a slot is three PHYSICAL
 * cores of one efficiency class: the most performant class first (P-cores, from the top), keeping the two lowest cores of
 * that class for the runtime's own threads (the render thread) on a hybrid CPU, then the next class. A slot holds every
 * processor of its three cores (a P-core slot: six, an E-core slot: three), and the console core runs on the core's first
 * processor. On a PC whose cores are all of one class with two hardware threads each this is the old layout exactly (six
 * processors from the top per slot). Every game derives the same slots from the topology, so the named slot mutexes mean
 * the same processors in every game of this build. STRIKERS_OS_SLOTS=top: the old six-from-the-top slots. */
static struct { DWORD_PTR mask; int eff; } g_topo[64];   /* the physical cores of the process's processor group */
static int g_topo_n = -1;
static DWORD_PTR g_slot_core[64][3];                    /* the three cores of each slot (every processor of each) */
static int g_slot_layout;                               /* the slots follow the topology (g_slot_core) */
static void topo_read(void) {
    if (g_topo_n >= 0) return;
    int n = 0;
    GROUP_AFFINITY ga; memset(&ga, 0, sizeof ga);
    GetThreadGroupAffinity(GetCurrentThread(), &ga);
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    char *buf = len ? (char *)malloc(len) : NULL;
    if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len))
        for (DWORD at = 0; at < len && n < 64;) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + at);
            if (!p->Size) break;
            for (WORD g = 0; g < p->Processor.GroupCount && n < 64; g++)
                if (p->Processor.GroupMask[g].Group == ga.Group && p->Processor.GroupMask[g].Mask) {
                    g_topo[n].mask = p->Processor.GroupMask[g].Mask; g_topo[n].eff = p->Processor.EfficiencyClass; n++;
                }
            at += p->Size;
        }
    free(buf);
    g_topo_n = n;
}
static int top_bit(DWORD_PTR m) { int b = (int)(sizeof m * 8) - 1; while (b > 0 && !(m >> b & 1)) b--; return b; }
static DWORD_PTR core_of(DWORD_PTR bit) {               /* every processor of the physical core of this processor */
    topo_read();
    for (int i = 0; i < g_topo_n; i++) if (g_topo[i].mask & bit) return g_topo[i].mask;
    return bit | (bit & 0xaaaaaaaaaaaaaaaaull ? bit >> 1 : bit << 1);      /* no topology: pairs, as before */
}
/* 0 mixed, 1 top (the old slots: THE DEFAULT), 2 P-cores only. Audit 05 measured the topology slots against the old ones on
 * this PC while job 19 played two or three games beside them all afternoon (docs/audit-05.md, section 4): the game threads
 * need ~30% less CPU on P-cores, but play was less even (play-time frames over 40 ms: 260 with the mixed slot, 108 with the
 * old one, same executable, back to back; median p99 22.7 against 18.3 ms) and with all three on P-cores the audio thread
 * lost its processor to a System thread at priority 30 (2-6 underruns a match). Until a run on a quiet PC shows the P-core
 * slots as smooth, the old slots stay the default: STRIKERS_OS_SLOTS=mixed or =pcores chooses the topology slots. */
static int g_slots_mode = -1;
static int slots_mode(void) {
    if (g_slots_mode < 0) { const char *e = getenv("STRIKERS_OS_SLOTS"); g_slots_mode = e && !_stricmp(e, "mixed") ? 0 : e && !_stricmp(e, "pcores") ? 2 : 1; }
    return g_slots_mode;
}
static int slots_top(void) { return slots_mode() == 1; }
/* The slots of proc (g_slot_core, g_slot_n); 0 when the topology cannot be read or STRIKERS_OS_SLOTS=top (the old slots).
 * On a hybrid CPU the MIXED slot (STRIKERS_OS_SLOTS=mixed) puts console cores 0 and 1 (the game's main thread; the thread that presents) on two
 * P-cores and console core 2 (Wwise's audio thread AK::EventManager, worker core 2) on an E-core. Measured (a05-fix3-w1, all
 * three on P-cores): the game threads' CPU fell 25-33%, but a System thread at priority 30 took the audio thread's P-core
 * processor for up to 387 ms at a time (14 times in one match; 6 underruns, 950 ms of silence), as it did in audit 04's
 * three-game runs whenever an audio thread sat on a P-core, and never in a run with it on an E-core.
 * STRIKERS_OS_SLOTS=pcores: all three on P-cores. Neither is the default (slots_mode). */
static int slot_layout(DWORD_PTR proc) {
    topo_read();
    if (slots_top() || g_topo_n <= 0) return 0;
    int idx[64], n = 0, maxeff = 0;
    for (int i = 0; i < g_topo_n; i++) if (g_topo[i].mask & proc) { idx[n++] = i; if (g_topo[i].eff > maxeff) maxeff = g_topo[i].eff; }
    for (int i = 1; i < n; i++)                         /* most performant class first, then from the top of the mask */
        for (int j = i; j > 0; j--) {
            int a = idx[j - 1], b = idx[j];
            if (g_topo[b].eff > g_topo[a].eff || (g_topo[b].eff == g_topo[a].eff && top_bit(g_topo[b].mask & proc) > top_bit(g_topo[a].mask & proc))) { idx[j - 1] = b; idx[j] = a; }
        }
    int hybrid = n && g_topo[idx[n - 1]].eff != maxeff, slots = 0;
    if (hybrid && slots_mode() == 0) {
        int np = 0;
        while (np < n && g_topo[idx[np]].eff == maxeff) np++;
        /* the two lowest P-cores stay with the runtime's threads; the E-core of a mixed slot is taken from the BOTTOM of the
         * E-cores (builds before audit 05 cut their slots from the top: their first game's console cores are on 31/29/27) */
        int pu = np - 2, p = 0, e = np, eb = n - 1;
        while (p + 2 <= pu && e <= eb && slots < 64) {
            g_slot_core[slots][0] = g_topo[idx[p]].mask & proc; g_slot_core[slots][1] = g_topo[idx[p + 1]].mask & proc;
            g_slot_core[slots][2] = g_topo[idx[eb]].mask & proc; slots++; p += 2; eb--;
        }
        for (; e + 3 <= eb + 1 && slots < 64; e += 3, slots++)
            for (int j = 0; j < 3; j++) g_slot_core[slots][j] = g_topo[idx[e + j]].mask & proc;
    } else
    for (int i = 0; i < n && slots < 64;) {
        int eff = g_topo[idx[i]].eff, end = i;
        while (end < n && g_topo[idx[end]].eff == eff) end++;
        int usable = end - i - (hybrid && eff == maxeff ? 2 : 0);   /* the class's two lowest cores stay with the runtime's threads */
        for (int c = 0; c + 3 <= usable && slots < 64; c += 3, slots++)
            for (int j = 0; j < 3; j++) g_slot_core[slots][j] = g_topo[idx[i + c + j]].mask & proc;
        i = end;
    }
    if (!slots) return 0;
    g_slot_n = slots;
    return 1;
}
static DWORD_PTR slot_procs(DWORD_PTR proc, int k);
static DWORD_PTR slot_mask(int k) {                     /* every processor of slot k */
    if (!g_slot_layout) return slot_procs(g_slot_proc, k);
    return k >= 0 && k < g_slot_n ? g_slot_core[k][0] | g_slot_core[k][1] | g_slot_core[k][2] : 0;
}
static int core_class(DWORD_PTR core) { for (int i = 0; i < g_topo_n; i++) if (g_topo[i].mask & core) return g_topo[i].eff; return -1; }
static int slot_class(int k) {                          /* efficiency class of slot k's first core (topology slots) */
    return g_slot_layout && k >= 0 && k < g_slot_n ? core_class(g_slot_core[k][0]) : -1;
}
static DWORD_PTR slot_procs(DWORD_PTR proc, int k) {    /* the six processors of slot k: from the top of proc (STRIKERS_OS_SLOTS=top) */
    DWORD_PTR mask = 0;
    int i = 0;
    for (int b = (int)(sizeof(DWORD_PTR) * 8) - 1; b >= 0; b--)
        if (proc >> b & 1) { if (i >= 6 * k && i < 6 * k + 6) mask |= (DWORD_PTR)1 << b; i++; }
    return mask;
}
/* Topology slots have names of their own ("_v2_"): the old six-from-the-top slots of builds before audit 05 mean other
 * processors under the same number. Such a game is still seen (slot_others: its old slot's processors are left out of this
 * game's own threads' mask), and this game holds the old-name slots its console cores overlap, so that an old build neither
 * takes them for its console cores nor puts its own threads there (g_old_held). */
static void slot_name(char *name, size_t size, int k) { snprintf(name, size, g_slot_layout ? "Local\\strikers_console_cores_v2_%d" : "Local\\strikers_console_cores_%d", k); }
static void slot_name_old(char *name, size_t size, int k) { snprintf(name, size, "Local\\strikers_console_cores_%d", k); }
/* An old-name slot held by a game of this build (to keep older builds off its cores) carries a marker of the same number,
 * so that other games of this build do not take it for an older build's game. */
static int old_slot_by_older_build(int j) {
    char name[64]; HANDLE h;
    snprintf(name, sizeof name, "Local\\strikers_console_cores_%d", j);
    if (!(h = OpenMutexA(SYNCHRONIZE, FALSE, name))) return 0;
    CloseHandle(h);
    snprintf(name, sizeof name, "Local\\strikers_console_cores_v2held_%d", j);
    if ((h = OpenMutexA(SYNCHRONIZE, FALSE, name)) != NULL) { CloseHandle(h); return 0; }
    return 1;
}
static uint64_t g_old_held;                             /* old-name slots held by this game (topology slots only) */
static DWORD_PTR core_slot(DWORD_PTR proc) {           /* the first free slot (of the topology, or of six processors from the top of proc) */
    int nb = 0;
    for (int b = (int)(sizeof(DWORD_PTR) * 8) - 1; b >= 0; b--) if (proc >> b & 1) nb++;
    g_slot_proc = proc; g_slot_layout = slot_layout(proc);
    if (!g_slot_layout) g_slot_n = nb / 6;
    DWORD_PTR old_games = 0;                            /* audit 05: processors of old-name slots held by games of older builds */
    for (int j = 0; g_slot_layout && j < nb / 6 && j < 64; j++) if (old_slot_by_older_build(j)) old_games |= slot_procs(proc, j);
    for (int k = 0; k < g_slot_n; k++) {
        char name[64];
        if (g_slot_layout && (slot_mask(k) & old_games)) continue;     /* an older build's console cores are there */
        slot_name(name, sizeof name, k);
        HANDLE m = NULL;
        /* Audit 04: other games look at the slots (slot_others, a handle open for a few microseconds); a slot seen "held"
         * once is asked again 2 ms later, so a look from another game is not taken for a game holding it. */
        for (int tries = 0; tries < 2; tries++) {
            m = CreateMutexA(NULL, FALSE, name);        /* kept open: the slot is this process's until it ends */
            if (!m || GetLastError() != ERROR_ALREADY_EXISTS) break;
            CloseHandle(m); m = NULL;
            if (!tries) Sleep(2);
        }
        if (!m) continue;
        DWORD_PTR mask = slot_mask(k);
        g_slot_own = k;
        for (int j = 0; g_slot_layout && j < nb / 6 && j < 64; j++)       /* the old-name slots these processors fall in */
            if (slot_procs(proc, j) & mask) {
                char old[64]; slot_name_old(old, sizeof old, j);
                HANDLE h = CreateMutexA(NULL, FALSE, old);
                if (h && GetLastError() != ERROR_ALREADY_EXISTS) {        /* kept open, as the slot, with its marker */
                    g_old_held |= 1ull << j;
                    char mark[64]; snprintf(mark, sizeof mark, "Local\\strikers_console_cores_v2held_%d", j);
                    CreateMutexA(NULL, FALSE, mark);
                } else if (h) CloseHandle(h);
            }
        if (g_slot_layout)
            printf("nn::os: console cores in processor slot %d of %d (mask %llx: three physical cores, efficiency classes %d/%d/%d for console cores 0/1/2; other games running at once take other slots; STRIKERS_OS_SLOTS=top: six processors from the top, =pcores: P-cores only)\n",
                   k, g_slot_n, (unsigned long long)mask, core_class(g_slot_core[k][0]), core_class(g_slot_core[k][1]), core_class(g_slot_core[k][2]));
        else
        printf("nn::os: console cores in processor slot %d of %d (mask %llx; other games running at once take other slots)\n", k, g_slot_n, (unsigned long long)mask);
        return mask;
    }
    printf("nn::os: every processor slot is held by other games: sharing the top of the mask\n");
    return g_slot_layout ? slot_mask(0) : proc;
}
/* The processors of the slots other games hold now (their console cores and the cores' second hardware threads). */
static DWORD_PTR slot_others(void) {
    DWORD_PTR m = 0;
    for (int k = 0; k < g_slot_n; k++) {
        if (k == g_slot_own) continue;
        char name[64];
        slot_name(name, sizeof name, k);
        HANDLE h = OpenMutexA(SYNCHRONIZE, FALSE, name);
        if (h) { CloseHandle(h); m |= slot_mask(k); }
    }
    int nb = 0;
    for (int b = 0; g_slot_layout && b < (int)(sizeof(DWORD_PTR) * 8); b++) if (g_slot_proc >> b & 1) nb++;
    for (int j = 0; j < nb / 6 && j < 64; j++) {          /* games of builds before audit 05 (old-name slots not held here) */
        if (g_old_held >> j & 1) continue;
        if (old_slot_by_older_build(j)) m |= slot_procs(g_slot_proc, j);
    }
    return m;
}
static int console_cores(void) {
    if (g_cc < 0) {
        int n = 0, on = 0;
        const char *cc = getenv("STRIKERS_OS_CONSOLE_CORES"), *cpus = getenv("STRIKERS_OS_HOST_CPUS");
        if (!cc || atoi(cc) != 0) {
            DWORD_PTR proc = 0, sys = 0;
            GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
            if (cpus && cpus[0]) {
                DWORD_PTR want = (DWORD_PTR)_strtoui64(cpus, NULL, 16);
                printf("nn::os: STRIKERS_OS_HOST_CPUS=%s: the console's cores on these processors only (%llx of %llx)\n", cpus, (unsigned long long)(want & proc), (unsigned long long)proc);
                if (want & proc) proc &= want;
            } else proc = core_slot(proc);
            int k = g_slot_own >= 0 ? g_slot_own : 0;
            if (!(cpus && cpus[0]) && g_slot_layout)      /* audit 05: the first processor of each of the slot's three cores */
                for (; n < 3; n++) g_cc_core[n] = g_slot_core[k][n] & (~g_slot_core[k][n] + 1);
            else
            for (int b = (int)(sizeof(DWORD_PTR) * 8) - 1; b >= 0 && n < 3; b--)      /* from the top, every second processor: not two hardware threads of one core */
                if (proc >> b & 1) { g_cc_core[n++] = (DWORD_PTR)1 << b; b--; }
            on = n == 3;
            printf("nn::os: console core model %s (STRIKERS_OS_CONSOLE_CORES): host processor masks %llx %llx %llx for cores 0 1 2\n", on ? "ON" : "NOT possible (fewer than 3 usable processors)",
                   (unsigned long long)g_cc_core[0], (unsigned long long)g_cc_core[1], (unsigned long long)g_cc_core[2]);
        }
        g_cc = on;
    }
    return g_cc;
}
/* The processors for the runtime's own busy threads (the render thread, the pipeline workers and the helper threads of the
 * renderer): the process's minus the console cores' processors and the second hardware threads of their cores, so that
 * no game thread pinned there (the main thread, the presentation thread at priority 7 ...) preempts them and they do not
 * share a core with one. Without this the render thread was found "not on a CPU" in frames that missed the 60 Hz refresh
 * (STRIKERS_HITCH). 0 when that would leave nothing, or with STRIKERS_HOST_PIN=0; STRIKERS_HOST_CPUS=<hex mask> chooses them. */
/* Audit 04 (F22): and away from the console cores of every OTHER game running at once. Each game holds one slot of six
 * processors for its console cores; before, a game's own threads ran on every processor except its own slot, i.e. on the
 * other games' console cores: a second game compiling shaders put its 16 pipeline workers and its render thread (Windows
 * base 10) on the processor of the first game's audio thread (9) and of its other console-core threads (8..12), and that
 * game's underruns were caught in the act (docs/audit-03.md, section 4). Now the processors of the slots other games hold are
 * left out too, re-read every 250 ms by a small thread (slot_watch), which moves the threads already placed (they register in
 * os_pin_host_thread / os_pin_render_thread) when a game starts or ends. If that would leave fewer than four processors,
 * the other games' slots are not left out (and the log says so). STRIKERS_HOST_SLOTS=0: as before (only this game's own
 * slot left out). */
static volatile uint64_t g_host_mask;
static volatile LONG g_host_ready;                      /* g_host_mask computed */
static uint64_t g_host_base;                            /* without the other games' slots */
static int g_host_dynamic;                              /* the other games' slots are followed */
static SRWLOCK g_pin_lock = SRWLOCK_INIT;
static struct { HANDLE h; int render; } g_pin[128];
static int g_npin;
static uint64_t g_slots_seen;
static int popcount64(uint64_t m) { int n = 0; while (m) { m &= m - 1; n++; } return n; }
static uint64_t host_mask_with(uint64_t others) {
    uint64_t m = g_host_base & ~others;
    return popcount64(m) >= 4 ? m : g_host_base;
}
static void pin_apply(HANDLE h, int render);
void os_pin_host_thread(void);
/* Audit 05: names the calling host thread (SetThreadDescription, Windows 10 1607 and later; nothing before), so that the
 * runtime's "nvn_perf: cpu ms/frame by thread" lines, debuggers and profilers can tell the runtime's threads apart. */
void host_thread_name(const char *name) {
    static HRESULT (WINAPI *set)(HANDLE, PCWSTR);
    static volatile LONG looked;
    if (!looked) { *(FARPROC *)&set = GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription"); InterlockedExchange(&looked, 1); }
    WCHAR w[64];
    if (set && MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64)) set(GetCurrentThread(), w);
}
static volatile LONG g_slot_watch_stop;                 /* test harnesses only: ends slot_watch before their DLL is unloaded */
static HANDLE g_slot_watch_thread;
static DWORD WINAPI slot_watch(LPVOID unused) {
    (void)unused;
    host_thread_name("nn::os slot watch");
    os_pin_host_thread();
    while (!g_slot_watch_stop) {
        Sleep(250);
        uint64_t others = (uint64_t)slot_others();
        if (others == g_slots_seen) continue;
        g_slots_seen = others;
        uint64_t m = host_mask_with(others);
        AcquireSRWLockExclusive(&g_pin_lock);
        g_host_mask = m;
        int moved = 0;
        for (int i = 0; i < g_npin; i++) if (g_pin[i].h) { pin_apply(g_pin[i].h, g_pin[i].render); moved++; }
        ReleaseSRWLockExclusive(&g_pin_lock);
        printf("nn::os: other games' console-core slots now on processors %llx: the runtime's own threads (%d) moved to processors %llx%s\n",
               (unsigned long long)others, moved, (unsigned long long)m, m == g_host_base && others ? " (too few left without them: shared)" : "");
    }
}
uint64_t os_host_cpu_mask(void) {
    static volatile LONG once;
    if (!g_host_ready) {
        if (InterlockedCompareExchange(&once, 1, 0) != 0) { while (!g_host_ready) Sleep(1); return g_host_mask; }
        const char *pin = getenv("STRIKERS_HOST_PIN"), *cpus = getenv("STRIKERS_HOST_CPUS"), *slots = getenv("STRIKERS_HOST_SLOTS");
        DWORD_PTR proc = 0, sys = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        uint64_t mask = 0;
        if (pin && atoi(pin) == 0) {}
        else if (cpus && cpus[0]) mask = (uint64_t)_strtoui64(cpus, NULL, 16) & proc;
        else if (console_cores()) {
            uint64_t game = 0;
            for (int i = 0; i < 3; i++)      /* the console cores' whole physical cores (topology slots), or as before: each and its pair */
                game |= g_slot_layout ? core_of(g_cc_core[i]) : g_cc_core[i] | (g_cc_core[i] & 0xaaaaaaaaaaaaaaaaull ? g_cc_core[i] >> 1 : g_cc_core[i] << 1);
            mask = (uint64_t)proc & ~game;
            g_host_base = mask;
            g_host_dynamic = mask && g_slot_n > 1 && !(slots && atoi(slots) == 0);
            if (g_host_dynamic) { g_slots_seen = (uint64_t)slot_others(); mask = host_mask_with(g_slots_seen); }
        }
        printf("nn::os: the runtime's own threads (render, pipeline workers, helpers) on processors %llx%s\n", (unsigned long long)mask,
               mask ? " (away from the console cores and their second hardware threads; STRIKERS_HOST_PIN=0: anywhere)" : ": anywhere");
        if (g_host_dynamic)
            printf("nn::os: and away from other games' console-core slots (now on processors %llx; followed every 250 ms; STRIKERS_HOST_SLOTS=0: not)\n", (unsigned long long)g_slots_seen);
        g_host_mask = mask;
        MemoryBarrier();
        g_host_ready = 1;
        if (g_host_dynamic) g_slot_watch_thread = CreateThread(NULL, 64 << 10, slot_watch, NULL, 0, NULL);
    }
    return g_host_mask;
}
/* STRIKERS_HOST_LAYOUT=1: the render thread gets the lowest core of that mask to itself (one processor of it, the core's second
 * hardware thread left idle) and the runtime's other threads the rest, so that no helper shares a core with it. */
static int host_layout(void) { static int on = -1; if (on < 0) { const char *e = getenv("STRIKERS_HOST_LAYOUT"); on = e && atoi(e) > 0; } return on; }
static uint64_t render_core_mask(void) {          /* both processors of the lowest core in the host mask, or 0 */
    uint64_t m = os_host_cpu_mask();
    if (!m) return 0;
    int b = 0;
    while (!(m >> b & 1)) b++;
    return (3ull << (b & ~1)) & m;
}
static void pin_apply(HANDLE h, int render) {      /* g_pin_lock held */
    uint64_t m = os_host_cpu_mask(), rc = render_core_mask();
    if (render && host_layout() && rc) m = rc & (~rc + 1);                 /* the core's first processor only */
    else if (m && host_layout() && (m & ~rc)) m &= ~rc;
    if (m) SetThreadAffinityMask(h, (DWORD_PTR)m);
}
/* Place the calling thread and, when other games' slots are followed, remember it so that slot_watch can move it later. */
static void pin_self(int render) {
    if (!os_host_cpu_mask()) return;
    AcquireSRWLockExclusive(&g_pin_lock);
    pin_apply(GetCurrentThread(), render);
    if (g_host_dynamic) {
        int slot = -1;
        for (int i = 0; i < g_npin && slot < 0; i++)            /* a free entry, or one whose thread has ended */
            if (!g_pin[i].h || WaitForSingleObject(g_pin[i].h, 0) == WAIT_OBJECT_0) slot = i;
        if (slot < 0 && g_npin < (int)(sizeof g_pin / sizeof g_pin[0])) slot = g_npin++;
        if (slot >= 0) {
            if (g_pin[slot].h) CloseHandle(g_pin[slot].h);
            g_pin[slot].h = NULL; g_pin[slot].render = render;
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_pin[slot].h,
                            THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, 0);
        }
    }
    ReleaseSRWLockExclusive(&g_pin_lock);
}
void os_pin_host_thread(void) { pin_self(0); }
void os_pin_render_thread(void) {
    uint64_t rc = render_core_mask(), one = rc & (~rc + 1);
    pin_self(1);
    if (host_layout() && rc) printf("nn::os: STRIKERS_HOST_LAYOUT=1: the render thread alone on processor mask %llx, the runtime's other threads on %llx\n", (unsigned long long)one, (unsigned long long)(os_host_cpu_mask() & ~rc));
}
static void apply_sched(HANDLE h, uint64_t core_mask) {
    DWORD_PTR m = 0;
    InterlockedIncrement(&g_sched_gen);                /* after every change of a core mask (os_yield's siblings) */
    if (!console_cores() || !h) return;
    for (int i = 0; i < 3; i++) if (core_mask >> i & 1) m |= g_cc_core[i];
    SetThreadAffinityMask(h, m ? m : g_cc_core[0]);
    SetThreadPriorityBoost(h, TRUE);
}
/* Audit 04: 18 shares 17's level (BELOW_NORMAL, base 9 in the ABOVE_NORMAL class) instead of 19..31's (LOWEST, 8). 18 is
 * Wwise's audio thread (AK::EventManager, core 2: it appends every AudioOut buffer) and the three InterceptAnimExtractors
 * (one per core). At 8 the audio thread tied every normal thread of every other program and BundleDecompressCore2 (19, same
 * core) and lost whole time slices to them: underruns (docs/audit-03.md, F21, fixed there in nnaudio.c for that one thread;
 * the fix belongs here). At 9 it is above other programs' normal threads and above 19..31 again, still below everything the
 * console runs ahead of it (16 and better: 10 and up); the only change of order inside the game is that 18 ties 17 (the
 * file readers, BundleDecompressCore1), which the console runs first. STRIKERS_OS_PRIO18=0: 18 at LOWEST again (test only). */
static int host_priority(int prio) {
    static int split = -1, p18 = -1;                           /* TEST ONLY, STRIKERS_OS_PRIO17=0: 17 shares its level with 18..27 again */
    if (split < 0) { const char *e = getenv("STRIKERS_OS_PRIO17"); split = !e || atoi(e) != 0; }
    if (p18 < 0) { const char *e = getenv("STRIKERS_OS_PRIO18"); p18 = !e || atoi(e) != 0; }
    if (console_cores()) {
        if (prio <= 4) return THREAD_PRIORITY_TIME_CRITICAL;
        if (prio <= 8) return THREAD_PRIORITY_HIGHEST;
        if (prio == 18 && p18 && split) return THREAD_PRIORITY_BELOW_NORMAL;      /* with 17 (above) */
        if (prio >= 18 && (split || prio >= 28)) return THREAD_PRIORITY_LOWEST;     /* below the draw buffer update (17), see above */
    }
    if (prio <= 4) return THREAD_PRIORITY_HIGHEST;
    if (prio <= 15) return THREAD_PRIORITY_ABOVE_NORMAL;
    if (prio == 16) return THREAD_PRIORITY_NORMAL;
    if (prio <= 27) return THREAD_PRIORITY_BELOW_NORMAL;
    return THREAD_PRIORITY_LOWEST;
}

/* ---------------------------------------------------------------- threads */
static __declspec(thread) uint64_t t_thread;          /* guest ThreadType* of the running thread */
static __declspec(thread) uint64_t t_tls[64];
static uint64_t g_tls_dtor[64];                       /* guest destructor of each slot, 0 = none */
static volatile LONG g_tls_used[64];
static void tls_run_destructors(Ctx *c);
static uint8_t g_main_thread[0x400];
static volatile LONG g_thread_tag = 0x10000;
uint64_t os_main_thread_type(void) {
    /* SDK 13.3 @37f6cc/@37f824 reads the kernel mutex-owner tag at +1b0.
     * A zero tag would make the SDK's uncontended CAS lock write zero. */
    if (!rd32((uint64_t)(uintptr_t)g_main_thread + 0x1b0))
        wr32((uint64_t)(uintptr_t)g_main_thread + 0x1b0, (uint32_t)InterlockedIncrement(&g_thread_tag));
    return (uint64_t)(uintptr_t)g_main_thread;
}
uint64_t g_main_stack_base, g_main_stack_size;        /* set by boot.c */
static volatile LONG g_thread_count;

/* Audit 06: a guest thread's context and its 64 KB thread-local region were never freed when the thread ended (the movie's
 * three extractor threads end and are made again for every match: ~200 KB of commit a match). They are not freed now either
 * (diagnostics and the sibling lists of other threads may still read a finished thread's context): they are kept on a list
 * and given to the next thread, cleared. */
static SRWLOCK g_ctx_lock = SRWLOCK_INIT;
static Ctx *g_ctx_free[64];
static int g_ctx_nfree;
static LONG64 g_ctx_reused;
static Ctx *new_ctx(uint64_t stack_top) {
    Ctx *c = NULL;
    AcquireSRWLockExclusive(&g_ctx_lock);
    if (g_ctx_nfree) c = g_ctx_free[--g_ctx_nfree];
    ReleaseSRWLockExclusive(&g_ctx_lock);
    uint64_t tls = 0;
    if (c) { tls = c->tpidr; memset((void *)(uintptr_t)tls, 0, 0x10000); g_ctx_reused++; }
    else c = _aligned_malloc(sizeof *c, 16);
    memset(c, 0, sizeof *c);
    c->x[31] = stack_top & ~15ull;
    c->tpidr = tls ? tls : (uint64_t)VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return c;
}
/* Audit 06 debug aid STRIKERS_HANDLE_LOG=1: this process's handle count at a named point (thread start/end, movie objects). */
void strikers_handle_log(const char *what, const char *detail) {
    static int on = -1;
    if (on < 0) on = getenv("STRIKERS_HANDLE_LOG") && atoi(getenv("STRIKERS_HANDLE_LOG"));
    if (!on) return;
    DWORD n = 0; GetProcessHandleCount(GetCurrentProcess(), &n);
    printf("handles: %lu at %s%s%s (thread %lu)\n", (unsigned long)n, what, detail ? " " : "", detail ? detail : "", GetCurrentThreadId()); fflush(stdout);
}
static void ctx_retire(Ctx *c) {                      /* the thread that ran c has ended: c and its region go to the next thread */
    AcquireSRWLockExclusive(&g_ctx_lock);
    if (g_ctx_nfree < 64) { g_ctx_free[g_ctx_nfree++] = c; c = NULL; }
    ReleaseSRWLockExclusive(&g_ctx_lock);
    (void)c;                                          /* more than 64 ending at once: left allocated, as before */
}

/* Every guest thread's context, so a report can say where each one is. */
static struct { Obj *o; Ctx *c; } g_threads[256];
static volatile LONG g_threads_n;

void dump_guest_threads(FILE *out) {
    fprintf(out, "\nguest threads (last function entered, link register):\n");
    for (int i = 0; i < g_threads_n && i < 256; i++) {
        Obj *o = g_threads[i].o;
        Ctx *c = g_threads[i].c;
        fprintf(out, "  %-20s %s  f_%llx  lr=%llx  %s%s (from %llx)\n", o->name ? (const char *)(uintptr_t)o->name : "?",
                o->finished ? "finished" : "running ", (unsigned long long)c->fn, (unsigned long long)c->x[30],
                c->in_import ? "in " : "", c->in_import ? c->in_import : "guest code", (unsigned long long)c->import_from);
    }
}

/* hitch.c (STRIKERS_HITCH): guest thread i -> context, name, host handle; 0 past the end. */
int guest_thread_info(int i, Ctx **c, const char **name, HANDLE *h) {
    if (i < 0 || i >= g_threads_n || i >= 256 || !g_threads[i].o || !g_threads[i].c) return 0;
    *c = g_threads[i].c; *name = g_threads[i].o->name ? (const char *)(uintptr_t)g_threads[i].o->name : NULL; *h = g_threads[i].o->finished ? NULL : g_threads[i].o->thread;
    return 1;
}
/* Name of the guest thread running on host thread `tid`, or NULL (nvn_gl.c: CPU time per thread). */
const char *guest_thread_name_of(DWORD tid) {
    if (tid && tid == g_main_tid) return "nnMain";      /* boot.c's guest_main: not created through nn::os, so not in g_threads */
    for (int i = 0; i < g_threads_n && i < 256; i++) {
        Obj *o = g_threads[i].o;
        if (o && o->thread && GetThreadId(o->thread) == tid) return o->name ? (const char *)(uintptr_t)o->name : "?";
    }
    return NULL;
}

static DWORD WINAPI thread_main(LPVOID arg) {
    Obj *o = arg;
    t_thread = o->key;
    Ctx *c = new_ctx(o->stack + o->stack_size);
    wr64(c->tpidr + 0x1f8, o->key);                   /* Horizon thread-local region: the ThreadType pointer lives at +0x1f8 */
    /* audit 06: a thread made again with the same ThreadType (the movie's extractors, every match) takes its old slot back;
     * before, every start took a new one and after 256 starts no new thread was listed (reports, per-thread CPU names) */
    LONG slot = -1;
    for (LONG i = 0; i < g_threads_n && i < 256 && slot < 0; i++) if (g_threads[i].o == o) slot = i;
    if (slot >= 0) g_threads[slot].c = c;
    else { slot = InterlockedIncrement(&g_threads_n) - 1; if (slot < 256) { g_threads[slot].o = o; g_threads[slot].c = c; } }
    c->x[0] = o->arg;
    trace_printf("--- thread %llx started (entry %llx, name %s)\n", (unsigned long long)o->key, (unsigned long long)o->fn,
                 o->name ? (const char *)(uintptr_t)o->name : "?");
    strikers_handle_log("guest thread start", o->name ? (const char *)(uintptr_t)o->name : "?");
    call_guest(c, o->fn);
    tls_run_destructors(c);
    trace_printf("--- thread %llx finished\n", (unsigned long long)o->key);
    if (t_sleep_timer) { CloseHandle(t_sleep_timer); t_sleep_timer = NULL; }      /* audit 06: SleepThread's timer of this thread */
    ctx_retire(c);
    strikers_handle_log("guest thread end", o->name ? (const char *)(uintptr_t)o->name : "?");
    EnterCriticalSection(&o->cs);
    o->finished = 1;
    InterlockedIncrement(&g_sched_gen);
    wr8(o->key + 0x40, 4);                          /* SDK ThreadType::State_Terminated */
    WakeAllConditionVariable(&o->cv);
    LeaveCriticalSection(&o->cs);
    return 0;
}

/* SDK 13.3 ThreadManager::CreateThread (+0x377728) clears exactly 0x1c0 bytes,
 * sets the state at +0x40 to 1, priority as a halfword, suspension count to 1,
 * and fills the intrusive-list sentinels and stack/entry/argument/name fields.
 * StartThread (+0x377a40) changes state to 3. Middleware reads this state itself:
 * leaving it zero makes a successfully started thread appear uninitialized and
 * lets its owner free the entry argument while the host thread is running. */
static void thread_type_fill(uint64_t t, uint64_t fn, uint64_t arg, uint64_t stack, uint64_t size, int priority) {
    if (!t) return;
    memset((void *)(uintptr_t)t, 0, 0x1c0);
    wr64(t, t); wr64(t + 8, t);
    wr64(t + 0x10, t + 0x10); wr64(t + 0x18, t + 0x10);
    wr8(t + 0x40, 1);
    wr16(t + 0x44, (uint16_t)priority); wr16(t + 0x46, 1);
    wr64(t + 0x48, stack); wr64(t + 0x50, stack); wr64(t + 0x58, size);
    wr64(t + 0x60, arg); wr64(t + 0x68, fn);
    wr64(t + 0x1a0, t + 0x180);
    wr32(t + 0x1b0, (uint32_t)InterlockedIncrement(&g_thread_tag));
}
static void os_CreateThread(Ctx *c) {                 /* (ThreadType*, fn, arg, stack, stackSize, priority[, core]) */
    thread_type_fill(A0, A1, A2, A3, A4, (int)c->x[5]);
    Obj *o = obj(A0);
    o->fn = A1; o->arg = A2; o->stack = A3; o->stack_size = A4; o->priority = (int)c->x[5];
    o->finished = 0; o->thread = NULL; if (o->name) o->name_kept = o->name; o->name = 0;
    o->ideal_core = OS_DEFAULT_CORE; o->core_mask = 1ull << OS_DEFAULT_CORE; o->core_set = 1;
    RET(0);
}
static void os_CreateThreadOnCore(Ctx *c) {           /* (ThreadType*, fn, arg, stack, stackSize, priority, idealCore) */
    uint64_t t = A0;
    int core = (int)c->x[6];
    os_CreateThread(c);
    if (core >= 0 && core < 64) { Obj *o = obj(t); o->ideal_core = core; o->core_mask = 1ull << core; }
}
/* The thread log: every thread the game
 * starts, with the nn::os arguments it was given, and every later change of priority or core mask. Printed at the end
 * of a run by os_thread_report (hostrt.c report_and_exit). */
static char g_oslog[24000];
static volatile LONG g_oslog_n, g_oslog_lock;
static void oslog(const char *fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    while (InterlockedCompareExchange(&g_oslog_lock, 1, 0)) YieldProcessor();
    if (g_oslog_n + n < (LONG)sizeof g_oslog - 1) { memcpy(g_oslog + g_oslog_n, line, (size_t)n); g_oslog_n += n; }
    g_oslog_lock = 0;
}
void os_thread_report(FILE *out) {
    fprintf(out, "nn::os: WaitAny woke %lld times to rescan its holders; %lld yields gave the processor away after a millisecond of spinning\n", g_multi_wakes, (long long)g_long_yields);
    if (!g_oslog_n) return;
    fprintf(out, "nn::os threads of this run (priority 0 highest .. 31 lowest; cores 0..2 exist on the console):\n%.*s", (int)g_oslog_n, g_oslog);
}
static void os_StartThread(Ctx *c) {
    Obj *o = obj(A0);
    oslog("  start  %-28s entry %llx priority %2d ideal core %d mask %llx stack 0x%llx (called from %llx)\n", o->name ? (const char *)(uintptr_t)o->name : "?",
          (unsigned long long)o->fn, o->priority, o->ideal_core, (unsigned long long)o->core_mask, (unsigned long long)o->stack_size, (unsigned long long)c->x[30]);
    InterlockedIncrement(&g_thread_count);
    o->thread = CreateThread(NULL, (SIZE_T)256 << 20, thread_main, o, STACK_SIZE_PARAM_IS_A_RESERVATION | CREATE_SUSPENDED, NULL);
    if (o->thread) { SetThreadPriority(o->thread, host_priority(o->priority)); apply_sched(o->thread, o->core_mask); wr8(o->key + 0x40, 3); ResumeThread(o->thread); }
    InterlockedIncrement(&g_sched_gen);
}
/* The process runs in the ABOVE_NORMAL priority class (STRIKERS_PRIORITY_CLASS=normal: the NORMAL class, as before). With the
 * console core model every guest thread may run on one host processor only, and the game's helpers at nn::os priority
 * 17..27 (view update, clipping, draw buffer update: the main thread waits for them every frame) are BELOW_NORMAL
 * threads: in the NORMAL class any normal thread of another program on one of those three processors comes first. One
 * class up every level of the mapping is two higher (the six levels stay apart: 8 9 10 11 12 15), above the normal
 * threads of other programs (8); HIGH would put HIGHEST and TIME_CRITICAL together. Measured at Cascade Kingdom's first
 * checkpoint: with up to 8 busy programs (11 of 32 processors busy) Windows keeps them off those three processors and
 * the class changes nothing; with every processor busy (30 busy programs) the game in the NORMAL class got 44% of one
 * processor and had not reached play after 120 s, in ABOVE_NORMAL it played at 38-39 fps. */
static void process_priority(void) {
    const char *e = getenv("STRIKERS_PRIORITY_CLASS");
    if (e && !_stricmp(e, "normal")) { printf("nn::os: process priority class NORMAL (STRIKERS_PRIORITY_CLASS=normal)\n"); return; }
    BOOL ok = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    printf("nn::os: process priority class ABOVE_NORMAL%s (STRIKERS_PRIORITY_CLASS=normal for the NORMAL class)\n", ok ? "" : ": REFUSED");
}
/* boot.c, on the main guest thread before the game starts: main.npdm gives it core 0 and priority 44 - 28 = 16 */
/* Audit 04 (F23): threads other code starts in this process at THREAD_PRIORITY_IDLE (base 1). D3D12Core and the NVIDIA
 * driver (nvwgf2umx) start such threads here (the swapchain path); with every processor busy (32 programs at the normal
 * priority 8) a thread at 1 runs only when Windows' starvation boost reaches it. In one loaded match the render thread stood
 * still for 26-34 s three times (a04-fix2-load-hitch1: "render: submitted commands (render thread not on a CPU)", system
 * 100% busy, this process 0%), which fits a wait for such a thread (not proven: docs/audit-04.md, F23b). Once a second this thread raises every thread
 * of the process found at IDLE to LOWEST (base 8 in the ABOVE_NORMAL class: the level of other programs' normal threads, so
 * it gets its turn in their time slices). The runtime starts no IDLE thread of its own. Nothing else is changed: their work,
 * affinity and every other priority stay as their owner set them. STRIKERS_OS_IDLE_THREADS=0: left as they are. */
typedef LONG (WINAPI *NtQueryInformationThreadFn)(HANDLE, int, PVOID, ULONG, PULONG);
typedef LONG (WINAPI *NtGetNextThreadFn)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
static NtQueryInformationThreadFn g_qit;
static int idle_raise(HANDLE th, DWORD tid, int logged) {     /* 1 if the thread was at IDLE and is raised */
    if (GetThreadPriority(th) != THREAD_PRIORITY_IDLE || !SetThreadPriority(th, THREAD_PRIORITY_LOWEST)) return 0;
    void *start = NULL; char mod[MAX_PATH] = "?"; HMODULE hm = NULL;
    if (g_qit) g_qit(th, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, NULL);
    if (start && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)start, &hm))
        GetModuleFileNameA(hm, mod, sizeof mod);
    if (logged < 32) printf("nn::os: thread %lu (started at %s+%llx) raised from IDLE to LOWEST (STRIKERS_OS_IDLE_THREADS=0: not)\n", (unsigned long)tid,
                            strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod, (unsigned long long)((char *)start - (char *)hm));
    return 1;
}
/* Audit 05: the scan walks this process's own threads (NtGetNextThread, one call per thread of the process) instead of taking
 * a Toolhelp snapshot of every thread of the whole system once a second, which cost this watcher 2.0-3.5 ms of CPU per frame
 * (a05-base-cold1, sampled: Thread32Next and the snapshot's section mapping, under this PC's security software; the busiest
 * runtime thread after the render thread). The snapshot stays as the fallback where NtGetNextThread is missing. */
static DWORD WINAPI idle_watch(LPVOID unused) {
    (void)unused;
    DWORD seen[512]; int nseen = 0, logged = 0;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    g_qit = (NtQueryInformationThreadFn)(void *)GetProcAddress(ntdll, "NtQueryInformationThread");
    NtGetNextThreadFn next_thread = (NtGetNextThreadFn)(void *)GetProcAddress(ntdll, "NtGetNextThread");
    DWORD self = GetCurrentProcessId();
    host_thread_name("nn::os idle-priority watch");
    os_pin_host_thread();
    for (;;) {
        Sleep(1000);
        if (next_thread) {                           /* every thread of this process, every second: two cheap calls each */
            HANDLE cur = NULL, nx;
            while (next_thread(GetCurrentProcess(), cur, THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION, 0, 0, &nx) >= 0) {
                if (cur) CloseHandle(cur);
                cur = nx;
                logged += idle_raise(cur, GetThreadId(cur), logged);
            }
            if (cur) CloseHandle(cur);
            continue;
        }
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) continue;
        THREADENTRY32 te; te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != self || te.tpBasePri > 6) continue;     /* base 1..6: below every level the game uses */
            int known = 0;
            for (int i = 0; i < nseen; i++) if (seen[i] == te.th32ThreadID) { known = 1; break; }
            if (known) continue;
            HANDLE th = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION, FALSE, te.th32ThreadID);
            if (!th) continue;
            logged += idle_raise(th, te.th32ThreadID, logged);
            CloseHandle(th);
            if (nseen < 512) seen[nseen++] = te.th32ThreadID;
        }
        CloseHandle(snap);
    }
}
void os_apply_main_sched(void) {
    process_priority();
    {   const char *e = getenv("STRIKERS_OS_IDLE_THREADS");
        if (!e || atoi(e) != 0) { HANDLE t = CreateThread(NULL, 64 << 10, idle_watch, NULL, 0, NULL); if (t) CloseHandle(t); } }
    if (!console_cores()) return;
    apply_sched(GetCurrentThread(), 1ull << OS_DEFAULT_CORE);
    SetThreadPriority(GetCurrentThread(), host_priority(OS_DEFAULT_PRIORITY));
}
static void os_WaitThread(Ctx *c) { Obj *o = obj(A0); if (o->thread) WaitForSingleObject(o->thread, INFINITE); }
static void os_DestroyThread(Ctx *c) {
    Obj *o = obj(A0);
    if (o->thread) { WaitForSingleObject(o->thread, INFINITE); CloseHandle(o->thread); o->thread = NULL; }
    wr8(A0 + 0x40, 0);
}
static void os_SetThreadName(Ctx *c) {               /* the guest string may be a temporary: keep a copy */
    Obj *o = obj(A0);
    uint64_t old = o->name ? o->name : o->name_kept;
    if (old && A1 && !strcmp((const char *)(uintptr_t)old, (const char *)(uintptr_t)A1)) { o->name = old; wr64(A0 + 0x1a0, old); return; }      /* audit 06: the same name again (a thread made again): no new copy */
    char *copy = _strdup((const char *)(uintptr_t)A1);
    obj(A0)->name = (uint64_t)(uintptr_t)copy;
    wr64(A0 + 0x1a0, (uint64_t)(uintptr_t)copy);
}
static void os_GetCurrentThread(Ctx *c) {
    if (!t_thread && !rd64((uint64_t)(uintptr_t)g_main_thread + 0x50) && g_main_stack_base)      /* the main thread's ThreadType, once its stack is known (boot.c) */
        { thread_type_fill((uint64_t)(uintptr_t)g_main_thread, 0, 0, g_main_stack_base, g_main_stack_size, OS_DEFAULT_PRIORITY); wr8((uint64_t)(uintptr_t)g_main_thread + 0x40, 3); }
    RET(t_thread ? t_thread : (uint64_t)g_main_thread);
}
static Obj *thread_obj(uint64_t key);
static void os_GetThreadPriority(Ctx *c) { RET((int64_t)thread_obj(A0 == (uint64_t)(uintptr_t)g_main_thread ? 0 : A0)->priority); }
static void os_ChangeThreadPriority(Ctx *c) {
    Obj *o = obj(A0);
    int old = o->priority;
    oslog("  change priority of %s: %d -> %d (called from %llx)\n", A0 == (uint64_t)(uintptr_t)g_main_thread ? "the main thread" : o->name ? (const char *)(uintptr_t)o->name : "?", old, (int)A1, (unsigned long long)c->x[30]);
    o->priority = (int)A1;
    InterlockedIncrement(&g_sched_gen);
    wr16(A0 + 0x44, (uint16_t)A1);
    if (o->thread) SetThreadPriority(o->thread, host_priority(o->priority));
    else if (A0 == (uint64_t)(uintptr_t)g_main_thread || A0 == t_thread) SetThreadPriority(GetCurrentThread(), host_priority(o->priority));
    RET((int64_t)old);
}
static void os_GetThreadId(Ctx *c) { Obj *o = obj(A0); RET(o->thread ? GetThreadId(o->thread) : GetCurrentThreadId()); }
typedef struct { Ctx *c; struct Obj *o; } Sibling;
static __declspec(thread) LONG t_sib_gen = -1;
static __declspec(thread) int t_nsib;
static __declspec(thread) Sibling t_sib[64];
static int sibling_runnable(const Ctx *c) {         /* running lifted code (also inside a fiber), not waiting in an import */
    const char *n = c->in_import;
    return !n || strstr(n, "SwitchToFiber") != NULL;
}
static int yield_switches(void) {                   /* 1: SwitchToThread, 0: return at once */
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("STRIKERS_OS_YIELD"); mode = e && !_stricmp(e, "switch"); }
    if (mode || !console_cores()) return 1;
    LONG gen = g_sched_gen;
    if (gen != t_sib_gen) {                            /* the threads that may share this thread's core at its priority or above */
        Obj *self = thread_obj(t_thread ? t_thread : (uint64_t)(uintptr_t)g_main_thread), *m = thread_obj((uint64_t)(uintptr_t)g_main_thread);
        t_sib_gen = gen; t_nsib = 0;
        for (int i = 0; i < g_threads_n && i < 256 && t_nsib < 64; i++) {
            Obj *o = g_threads[i].o;
            if (o && o != self && !o->finished && (o->core_mask & self->core_mask) && o->priority <= self->priority)
                t_sib[t_nsib++] = (Sibling){g_threads[i].c, o};
        }
        if (self != m && g_main_ctx_os && (m->core_mask & self->core_mask) && m->priority <= self->priority && t_nsib < 64)
            t_sib[t_nsib++] = (Sibling){g_main_ctx_os, m};
    }
    for (int i = 0; i < t_nsib; i++)
        if (!t_sib[i].o->finished && sibling_runnable(t_sib[i].c)) return 1;
    return 0;
}
/* A thread that has been yielding without a break for over a millisecond (its yields less than 50 us apart) sleeps 0.25 ms
 * (the high-resolution timer), then again after the next millisecond. The console's job loops spin for
 * short times; here the job they wait for can be held up elsewhere (another program or game on the processor of the thread
 * that produces it), and a long spin must not starve the lower-priority game threads of its core: in a04-fix-x2-i2 (no such
 * bound) worker core 2 (16) spun for 240 ms while Wwise's audio thread (18, same core) waited, ready, for 199 ms. A short
 * spin, the usual case, never leaves the processor; a long one gives the rest of the core a turn every millisecond. A sleep,
 * not SwitchToThread: the waiting game threads of the core run first (they are above other programs' normal threads), an
 * other program's thread gets at most the rest of the 0.25 ms, and the spinner takes the processor back when its timer fires.
 * SwitchToThread there would give a busy program the rest of a time slice (~5 ms measured) for every long spin. */
static void os_yield(void) {
    static __declspec(thread) LONGLONG t_last, t_streak;
    static LONGLONG qpf_50us, qpf_1ms;
    LARGE_INTEGER now;
    if (yield_switches()) { SwitchToThread(); return; }
    QueryPerformanceCounter(&now);
    if (!qpf_1ms) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); qpf_50us = f.QuadPart / 20000; qpf_1ms = f.QuadPart / 1000; }
    if (now.QuadPart - t_last > qpf_50us) t_streak = now.QuadPart;           /* a new run of yields */
    t_last = now.QuadPart;
    if (now.QuadPart - t_streak > qpf_1ms) { g_long_yields++; sleep_ns(250000); QueryPerformanceCounter(&now); t_streak = t_last = now.QuadPart; return; }
    YieldProcessor();
}
static void os_YieldThread(Ctx *c) { (void)c; os_yield(); }
static void os_SleepThread(Ctx *c) { sleep_ns((int64_t)A0); }
static void os_GetCurrentStackInfo(Ctx *c) {
    Obj *o = t_current_fiber ? t_current_fiber : t_thread ? obj(t_thread) : NULL;
    if (A0) wr64(A0, o ? o->stack : g_main_stack_base);
    if (A1) wr64(A1, o ? o->stack_size : g_main_stack_size);
}
/* Core numbers and affinity masks. The console runs the application on cores 0..2 (GetThreadAvailableCoreMask = 7);
 * a thread is created on one core (the NPDM default core 0, or the one given to CreateThread) and stays there until
 * SetThreadCoreMask widens its mask. The values are kept per thread and returned as set; the Windows scheduler is
 * not told to pin anything (a PC has more than three cores and the game's threads would pile up on host CPU 0). */
static Obj *thread_obj(uint64_t key) {
    Obj *o = obj(key ? key : (uint64_t)(uintptr_t)g_main_thread);
    if (!o->core_set) { o->core_set = 1; o->ideal_core = OS_DEFAULT_CORE; o->core_mask = 1ull << OS_DEFAULT_CORE; if (!o->priority) o->priority = OS_DEFAULT_PRIORITY; }
    return o;
}
static void os_SetThreadCoreMask(Ctx *c) {            /* (ThreadType*, int idealCore, Bit64 affinityMask) */
    Obj *o = thread_obj(A0);
    int ideal = (int)A1;
    uint64_t mask = A2;
    oslog("  core mask of %s: ideal %d mask %llx (was ideal %d mask %llx; %s; called from %llx)\n", A0 == (uint64_t)(uintptr_t)g_main_thread || !A0 ? "the main thread" : o->name ? (const char *)(uintptr_t)o->name : "?",
          ideal, (unsigned long long)mask, o->ideal_core, (unsigned long long)o->core_mask, o->thread ? "running" : "not started", (unsigned long long)c->x[30]);
    int is_self = t_thread ? A0 == t_thread : (!A0 || A0 == (uint64_t)(uintptr_t)g_main_thread);      /* the calling thread names itself */
    HANDLE h = o->thread ? o->thread : is_self ? GetCurrentThread() : NULL;                             /* NULL: not started yet (applied at StartThread) */
    if (ideal == -2) { o->ideal_core = OS_DEFAULT_CORE; o->core_mask = 1ull << OS_DEFAULT_CORE; apply_sched(h, o->core_mask); return; }   /* IdealCoreUseDefaultValue: mask is ignored */
    if (ideal == -3) { o->core_mask = mask; apply_sched(h, o->core_mask); return; }                           /* IdealCoreNoUpdate */
    o->core_mask = mask;
    if (ideal == -1) { if (!(mask >> (o->ideal_core & 63) & 1)) o->ideal_core = mask ? (int)__builtin_ctzll(mask) : OS_DEFAULT_CORE; }   /* IdealCoreDontCare */
    else o->ideal_core = ideal;
    apply_sched(h, o->core_mask);
}
static void os_GetThreadCoreMask(Ctx *c) {            /* (int *outIdealCore, Bit64 *outMask, const ThreadType*) */
    Obj *o = thread_obj(A2);
    if (A0) wr32(A0, (uint32_t)o->ideal_core);
    if (A1) wr64(A1, o->core_mask);
}
static void os_GetCurrentCoreNumber(Ctx *c) {
    if (console_cores()) {                                /* the core the thread is on now, as the console answers */
        DWORD_PTR here = (DWORD_PTR)1 << GetCurrentProcessorNumber();
        for (int i = 0; i < 3; i++) if (g_cc_core[i] == here) { RET(i); return; }
    }
    RET((int64_t)thread_obj(t_thread)->ideal_core);
}
static void os_AvailableCoreMask(Ctx *c) { RET(7); }
static void nothing(Ctx *c) { (void)c; }

/* thread-local storage slots */
static volatile LONG g_tls_next;
/* (TlsSlot *out, void (*destructor)(uintptr_t value)) -> Result. The destructor of a slot runs when a thread ends
 * with a non-zero value in it; FreeTlsSlot makes the slot available again. */
static void os_AllocateTlsSlot(Ctx *c) {
    (void)g_tls_next;
    for (int i = 0; i < 64; i++)
        if (InterlockedCompareExchange(&g_tls_used[i], 1, 0) == 0) { g_tls_dtor[i] = A1; wr32(A0, (uint32_t)i); RET(0); return; }
    guest_trap("nn::os::AllocateTlsSlot: all 64 slots are in use (the console has 32)", c->x[30] - 4);
}
static void os_FreeTlsSlot(Ctx *c) { uint32_t i = (uint32_t)A0 & 63; g_tls_dtor[i] = 0; InterlockedExchange(&g_tls_used[i], 0); }
static void tls_run_destructors(Ctx *c) {
    for (int i = 0; i < 64; i++) {
        uint64_t v = t_tls[i], fn = g_tls_dtor[i];
        t_tls[i] = 0;
        if (v && fn && g_tls_used[i]) { c->x[0] = v; call_guest(c, fn); }
    }
}
static void os_GetTlsValue(Ctx *c) { RET(t_tls[A0 & 63]); }
static void os_SetTlsValue(Ctx *c) { t_tls[A0 & 63] = A1; }

/* ---------------------------------------------------------------- mutexes */
static void mtx_init(Ctx *c) { (void)obj(A0); }
static void mtx_lock(Ctx *c) { EnterCriticalSection(&obj(A0)->cs); }
static void mtx_unlock(Ctx *c) { LeaveCriticalSection(&obj(A0)->cs); }
static void mtx_trylock(Ctx *c) { RET(TryEnterCriticalSection(&obj(A0)->cs) ? 1 : 0); }
/* hitch.c (STRIKERS_HITCH): host thread id that owns the guest mutex `key` now, 0 = nobody. Diagnostics only. */
DWORD os_mutex_owner(uint64_t key) { return key ? (DWORD)(uintptr_t)obj(key)->cs.OwningThread : 0; }

/* ---------------------------------------------------------------- events (Event, LightEvent, SystemEvent) */
/* nn::os::EventClearMode: 0 = ManualClear, 1 = AutoClear */
static void ev_init(Ctx *c) { obj_reset(A0); Obj *o = obj(A0); o->signaled = (int)(A1 & 1); o->autoclear = A2 == 1; }
/* Audit 04: a signal wakes the multiwait waiters (WaitAny) only when the event is linked to a multiwait holder. Before, every
 * signal of every event (and the display's vsync event every refresh) woke them all to rescan their holders: the game's
 * MoviePlayerWorker (nn::os 14, above the worker thread of its core) woke for each one. in_multi is set and read under the
 * event's lock, so a signal that misses it came before the link, and WaitAny's first scan sees it. */
static void ev_signal(Ctx *c) {
    Obj *o = obj(A0);
    EnterCriticalSection(&o->cs);
    o->signaled = 1;
    WakeAllConditionVariable(&o->cv);
    int linked = o->in_multi;
    LeaveCriticalSection(&o->cs);
    if (linked) multi_notify();
}
/* Host-side access to guest events (used for the display's vsync event). */
void os_init_event(uint64_t key, int autoclear) { obj_reset(key); Obj *o = obj(key); o->signaled = 0; o->autoclear = autoclear; }
void os_signal_event(uint64_t key) {
    Obj *o = obj(key);
    EnterCriticalSection(&o->cs);
    o->signaled = 1;
    WakeAllConditionVariable(&o->cv);
    int linked = o->in_multi;
    LeaveCriticalSection(&o->cs);
    if (linked) multi_notify();
}
void os_clear_event(uint64_t key) { Obj *o = obj(key); EnterCriticalSection(&o->cs); o->signaled = 0; LeaveCriticalSection(&o->cs); }
static void ev_clear(Ctx *c) { Obj *o = obj(A0); EnterCriticalSection(&o->cs); o->signaled = 0; LeaveCriticalSection(&o->cs); }
static int ev_wait_ms(Obj *o, DWORD ms) {
    int ok = 1;
    EnterCriticalSection(&o->cs);
    while (!o->signaled) {
        if (!SleepConditionVariableCS(&o->cv, &o->cs, ms)) { ok = o->signaled; break; }
    }
    if (ok && o->autoclear) o->signaled = 0;
    LeaveCriticalSection(&o->cs);
    return ok;
}
static void ev_wait(Ctx *c) { ev_wait_ms(obj(A0), INFINITE); }
static void ev_trywait(Ctx *c) { RET(ev_wait_ms(obj(A0), 0)); }
static void ev_timedwait(Ctx *c) { RET(ev_wait_ms(obj(A0), ns_to_ms((int64_t)A1))); }

/* ---- system events shared through kernel handles.
 * [doc] A SystemEvent is a kernel event: CreateSystemEvent makes it with a readable and a writable handle; another
 * SystemEventType (in this or another process) can be attached to those handles, and a signal through one is seen by
 * every waiter on the event. SMO's audio server (lifted SDK code) creates the renderer's event and the client attaches
 * the game's SystemEvent to its readable handle. Model: handles name host event objects (g_handles); an attached
 * SystemEventType resolves to the event it shares (Obj.alias, followed by obj()). Each SystemEventType keeps ITS OWN
 * clear mode for its waits (AutoClear = a successful wait clears the shared event), as on the console. */
#define MAX_HANDLES 1024
static Obj *g_handles[MAX_HANDLES];
static LONG g_handle_next = 1;
static uint32_t handle_new(Obj *o) {
    LONG i = InterlockedIncrement(&g_handle_next) - 1;
    if (i >= MAX_HANDLES) guest_trap("os: too many system event handles (os_threads.c MAX_HANDLES)", 0);
    g_handles[i] = o;
    return 0x50000u + (uint32_t)i;
}
static Obj *handle_obj(uint32_t h) { uint32_t i = h - 0x50000u; return i < MAX_HANDLES ? g_handles[i] : NULL; }

/* The five SVC veneers retained by sdklift (sdk +3cce58/88/90/a8/c0).
 * Arbitration hands the guest mutex word to the next waiter. No native pointer
 * is ever treated as a kernel handle. Named-port IPC is explicitly unavailable. */
typedef struct ArbWaiter {
    uint64_t address;
    uint32_t tag;
    int ready;
    CONDITION_VARIABLE cv;
    struct ArbWaiter *next;
} ArbWaiter;
static SRWLOCK g_arb_lock = SRWLOCK_INIT;
static ArbWaiter *g_arb_waiters;
int os_dispatch_svc(Ctx *c, uint32_t number) {
    switch (number) {
    case 0x16: { /* CloseHandle */
        uint32_t i = (uint32_t)A0 - 0x50000u;
        void *old = i < MAX_HANDLES ? InterlockedExchangePointer((void *volatile *)&g_handles[i], NULL) : NULL;
        RET(old ? 0 : 0xe401); return 1;
    }
    case 0x1a: { /* ArbitrateLock(owner handle, mutex address, requester tag) */
        uint32_t owner = (uint32_t)A0, tag = (uint32_t)A2;
        uint64_t address = A1;
        ArbWaiter w = {0}; w.address = address; w.tag = tag; InitializeConditionVariable(&w.cv);
        AcquireSRWLockExclusive(&g_arb_lock);
        if ((rd32(address) & ~0x40000000u) == owner && (rd32(address) & 0x40000000u)) {
            ArbWaiter **tail = &g_arb_waiters;
            while (*tail) tail = &(*tail)->next;
            *tail = &w;
            while (!w.ready) SleepConditionVariableSRW(&w.cv, &g_arb_lock, INFINITE, 0);
        }
        ReleaseSRWLockExclusive(&g_arb_lock); RET(0); return 1;
    }
    case 0x1b: { /* ArbitrateUnlock(mutex address) */
        uint64_t address = A0;
        AcquireSRWLockExclusive(&g_arb_lock);
        ArbWaiter **p = &g_arb_waiters;
        while (*p && (*p)->address != address) p = &(*p)->next;
        ArbWaiter *w = *p;
        if (w) {
            *p = w->next;
            uint32_t value = w->tag;
            for (ArbWaiter *q = g_arb_waiters; q; q = q->next)
                if (q->address == address) { value |= 0x40000000u; break; }
            InterlockedExchange((volatile LONG *)(uintptr_t)address, (LONG)value);
            w->ready = 1; WakeConditionVariable(&w->cv);
        } else InterlockedExchange((volatile LONG *)(uintptr_t)address, 0);
        ReleaseSRWLockExclusive(&g_arb_lock); RET(0); return 1;
    }
    case 0x1f: /* ConnectToNamedPort: SVC returns handle in w1, veneer stores it. */
        A1 = 0; RET(0xf201); return 1;
    case 0x21: /* SendSyncRequest: no service sessions can have been created. */
        RET(0xe401); return 1;
    default: return 0;
    }
}
static int sev_wait(uint64_t key, DWORD ms) {
    Obj *raw = obj_raw(key), *o = obj(key);
    int ok = 1;
    EnterCriticalSection(&o->cs);
    while (!o->signaled) {
        if (!SleepConditionVariableCS(&o->cv, &o->cs, ms)) { ok = o->signaled; break; }
    }
    if (ok && raw->autoclear) o->signaled = 0;
    LeaveCriticalSection(&o->cs);
    return ok;
}
static void sev_create(Ctx *c) {               /* (SystemEventType*, EventClearMode, bool interprocess) -> Result */
    Obj *raw = obj_raw(A0);
    raw->alias = NULL;
    obj_reset(A0);
    raw->autoclear = A1 == 1;
    raw->rhandle = handle_new(raw);
    raw->whandle = handle_new(raw);
    RET(0);
}
static void sev_attach_readable(Ctx *c) {      /* (SystemEventType*, Handle, bool managed, EventClearMode) */
    Obj *raw = obj_raw(A0), *t = handle_obj((uint32_t)A1);
    if (!t) guest_trap("os: AttachReadableHandleToSystemEvent with a handle no event of this process has", c->x[30] - 4);
    raw->alias = t == raw ? NULL : t;
    raw->autoclear = A3 == 1;
}
/* Host side: a new system event with a handle (as a system module would create one and pass its readable handle);
 * returns the handle, *key receives a host key to signal it with os_signal_event. */
uint32_t os_system_event_new(int autoclear, uint64_t *key) {
    uint64_t k = (uint64_t)(uintptr_t)calloc(1, 64);            /* a unique key no guest object has */
    obj_reset(k);
    Obj *o = obj_raw(k);
    o->autoclear = autoclear;
    o->rhandle = handle_new(o);
    o->whandle = handle_new(o);
    if (key) *key = k;
    return o->rhandle;
}
static void sev_get_readable(Ctx *c) { Obj *o = obj(A0); if (!o->rhandle) o->rhandle = handle_new(o); RET(o->rhandle); }
static void sev_destroy(Ctx *c) { obj_raw(A0)->alias = NULL; }
static void sev_wait_h(Ctx *c) { sev_wait(A0, INFINITE); }
static void sev_trywait_h(Ctx *c) { RET(sev_wait(A0, 0)); }
static void sev_timedwait_h(Ctx *c) { RET(sev_wait(A0, ns_to_ms((int64_t)A1))); }

/* ---------------------------------------------------------------- condition variables */
static void cv_wait(Ctx *c) { SleepConditionVariableCS(&obj(A0)->cv, &obj(A1)->cs, INFINITE); }
static void cv_timedwait(Ctx *c) {                    /* returns ConditionVariableStatus: 0 timeout, 1 signalled */
    RET(SleepConditionVariableCS(&obj(A0)->cv, &obj(A1)->cs, ns_to_ms((int64_t)A2)) ? 1 : 0);
}
static void cv_signal(Ctx *c) { WakeConditionVariable(&obj(A0)->cv); }
static void cv_broadcast(Ctx *c) { WakeAllConditionVariable(&obj(A0)->cv); }

/* ---------------------------------------------------------------- message queues (of 64-bit values) */
static void mq_init(Ctx *c) {                         /* (queue, buffer, count): host-side storage is used */
    obj_reset(A0);
    Obj *o = obj(A0);
    free(o->buf);
    o->cap = A2 ? A2 : 1;
    o->buf = calloc((size_t)o->cap, 8);
}
static int mq_put(Obj *o, uint64_t v, int front, DWORD ms) {
    int ok = 1;
    EnterCriticalSection(&o->cs);
    while (o->len == o->cap) {
        if (!SleepConditionVariableCS(&o->cv, &o->cs, ms)) { ok = o->len < o->cap; break; }
    }
    if (ok) {
        if (front) { o->head = (o->head + o->cap - 1) % o->cap; o->buf[o->head] = v; }
        else o->buf[(o->head + o->len) % o->cap] = v;
        o->len++;
        WakeAllConditionVariable(&o->cv);
    }
    LeaveCriticalSection(&o->cs);
    return ok;
}
static int mq_get(Obj *o, uint64_t out, DWORD ms) {
    int ok = 1;
    EnterCriticalSection(&o->cs);
    while (o->len == 0) {
        if (!SleepConditionVariableCS(&o->cv, &o->cs, ms)) { ok = o->len > 0; break; }
    }
    if (ok) {
        wr64(out, o->buf[o->head]);
        o->head = (o->head + 1) % o->cap;
        o->len--;
        WakeAllConditionVariable(&o->cv);
    }
    LeaveCriticalSection(&o->cs);
    return ok;
}
static void mq_send(Ctx *c) { mq_put(obj(A0), A1, 0, INFINITE); }
static void mq_trysend(Ctx *c) { RET(mq_put(obj(A0), A1, 0, 0)); }
static void mq_jam(Ctx *c) { mq_put(obj(A0), A1, 1, INFINITE); }
static void mq_tryjam(Ctx *c) { RET(mq_put(obj(A0), A1, 1, 0)); }
static void mq_receive(Ctx *c) { mq_get(obj(A1), A0, INFINITE); }
static void mq_tryreceive(Ctx *c) { RET(mq_get(obj(A1), A0, 0)); }

/* ---------------------------------------------------------------- SDK 13.3 fibers
 * @3791e0: state +20, guarded +21, entry +28, arg +30, original/usable stack
 * +38/+40, size +48, saved SP +50. Host stacks preserve translated C frames;
 * each fiber has its own guest Ctx and guest stack. OS TLS remains per thread,
 * while the EH runtime's FLS gives each fiber independent exception state. */
static __declspec(thread) void *t_root_fiber;
static __declspec(thread) Ctx *t_root_ctx;
static void fib_switch(Ctx *c);
static void WINAPI fiber_entry(void *arg) {
    Obj *o = arg;
    call_guest(o->fiber_ctx, o->fn);
    uint64_t next = o->fiber_ctx->x[0];
    wr8(o->key + 0x20, 4); /* target SDK's terminated state @379270 */
    o->fiber_ctx->x[0] = next;
    fib_switch(o->fiber_ctx);
    guest_trap("terminated fiber resumed", o->key);
}
static void fib_init(Ctx *c) {
    Obj *o = obj(A0);
    if (o->fiber) guest_trap("InitializeFiber on a live fiber", A0);
    o->fn = A1; o->arg = A2; o->stack = A3; o->stack_size = A4;
    if (!o->fn || !o->stack || o->stack_size < 0x100 || ((o->stack | o->stack_size) & 15))
        guest_trap("invalid fiber entry or stack", A0);
    o->fiber_ctx = _aligned_malloc(sizeof(Ctx), 16);
    if (!o->fiber_ctx) guest_trap("fiber context allocation failed", A0);
    *o->fiber_ctx = *c;
    o->fiber_ctx->in_import = NULL;     /* the copy was taken inside this import: a fiber starts in its own lifted code (thread dumps, hitch log, sibling_runnable) */
    o->fiber_ctx->import_from = 0;
    o->fiber_ctx->x[0] = o->arg;
    o->fiber_ctx->x[30] = 0;
    o->fiber_ctx->x[31] = (o->stack + o->stack_size) & ~15ull;
    o->fiber_owner = 0; /* may initialize on one thread and start on another */
    wr16(A0 + 0x20, 1);
    wr64(A0 + 0x28, o->fn); wr64(A0 + 0x30, o->arg);
    wr64(A0 + 0x38, o->stack); wr64(A0 + 0x40, o->stack);
    wr64(A0 + 0x48, o->stack_size); wr64(A0 + 0x50, o->fiber_ctx->x[31]);
    o->fiber = CreateFiberEx(0x10000, 0x10000000, FIBER_FLAG_FLOAT_SWITCH, fiber_entry, o);
    if (!o->fiber) guest_trap("CreateFiberEx failed", A0);
}
/* STRIKERS_FIBERLOG=<ms>: every stretch a guest fiber ran before the thread's own code had it back, when longer than <ms>,
 * with how often the clock was read meanwhile: a fiber that never reads it cannot be keeping to a time budget. */
extern volatile LONG64 g_tick_calls;               /* nnsdk.c */
static __declspec(thread) LONG64 t_fib_t0, t_fib_ticks;
static __declspec(thread) uint64_t t_fib_from;
static void fiber_log(int entering, uint64_t key, uint64_t lr) {
    static int ms = -1;
    if (ms < 0) { const char *e = getenv("STRIKERS_FIBERLOG"); ms = e ? atoi(e) : 0; }
    if (!ms) return;
    LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    if (entering) { t_fib_t0 = q.QuadPart; t_fib_ticks = g_tick_calls; t_fib_from = lr; return; }
    if (!t_fib_t0) return;
    double d = (double)(q.QuadPart - t_fib_t0) * 1000.0 / (double)f.QuadPart;
    if (d >= ms) printf("fiber: %.1f ms in fiber(s) before the thread's own code ran again (entered from %llx, back with fiber %llx); clock read %lld times by any thread meanwhile\n",
                        d, (unsigned long long)t_fib_from, (unsigned long long)key, (long long)(g_tick_calls - t_fib_ticks));
    t_fib_t0 = 0;
}
static void fib_switch(Ctx *c) {
    Obj *source = t_current_fiber, *dest = A0 ? obj(A0) : NULL;
    if (!source && dest) fiber_log(1, dest->key, c->x[30]);
    if (source && !dest) fiber_log(0, source->key, c->x[30]);
    if (!source && !dest) return;
    if (dest && (!dest->fiber || rd8(dest->key + 0x20) != 1))
        guest_trap("SwitchToFiber target is not ready", A0);
    if (!t_root_fiber) {
        if (IsThreadAFiber()) guest_trap("guest thread already owned by another fiber scheduler", 0);
        t_root_fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
        if (!t_root_fiber) guest_trap("ConvertThreadToFiberEx failed", 0);
    }
    if (!source) t_root_ctx = c;
    if (source && rd8(source->key + 0x20) != 4) {
        wr8(source->key + 0x20, 1); wr64(source->key + 0x50, c->x[31]);
    }
    if (dest) {
        DWORD tid = GetCurrentThreadId();
        if (dest->fiber_owner && dest->fiber_owner != tid) guest_trap("fiber migrated between guest threads", dest->key);
        dest->fiber_owner = tid; dest->fiber_ctx->tpidr = c->tpidr;
        wr8(dest->key + 0x20, 2);
    }
    t_current_fiber = dest;
    if (c->tpidr) { uint64_t thread = rd64(c->tpidr + 0x1f8); if (thread) wr64(thread + 0x70, dest ? dest->key : 0); }
    host_exchange_context(dest ? dest->fiber_ctx : t_root_ctx);
    { void hitch_switch_ctx(Ctx *c, int fiber); hitch_switch_ctx(dest ? dest->fiber_ctx : t_root_ctx, dest != NULL); }
    SwitchToFiber(dest ? dest->fiber : t_root_fiber);
    host_exchange_context(c);
    { void hitch_switch_ctx(Ctx *c, int fiber); hitch_switch_ctx(c, t_current_fiber != NULL); }
}
static void fib_finalize(Ctx *c) {
    Obj *o = obj(A0);
    if (o == t_current_fiber || (o->fiber_owner && o->fiber_owner != GetCurrentThreadId()))
        guest_trap("FinalizeFiber on an active or foreign fiber", A0);
    if (o->fiber) DeleteFiber(o->fiber);
    o->fiber = NULL; _aligned_free(o->fiber_ctx); o->fiber_ctx = NULL;
    wr16(A0 + 0x20, 0);
}
static void fib_current(Ctx *c) { RET(t_current_fiber ? t_current_fiber->key : 0); }

/* ---------------------------------------------------------------- timers / multiwait
 * Target timer @372270: size 0x40, initialized/clear/signaled/running +10..13,
 * next/first/period +20/+28/+30. Holder @371c10: size 0x40, object +30,
 * user data +38. Multiwait @374e70: size 0x30, intrusive sentinel +8/+10. */
static int64_t monotonic_ns(void) {
    LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    return q.QuadPart / f.QuadPart * 1000000000LL + q.QuadPart % f.QuadPart * 1000000000LL / f.QuadPart;
}
static void timer_init(Ctx *c) {
    Obj *o = obj(A0); obj_reset(A0);
    EnterCriticalSection(&o->cs);
    o->autoclear = A1 == 1; o->timer_running = 0;
    memset((void *)(uintptr_t)A0, 0, 0x40);
    wr64(A0, A0); wr64(A0 + 8, A0); wr8(A0 + 0x10, 1); wr8(A0 + 0x11, o->autoclear);
    LeaveCriticalSection(&o->cs);
}
static void timer_start(Ctx *c) {
    Obj *o = obj(A0); int64_t first = (int64_t)A1, period = (int64_t)A2, now = monotonic_ns();
    if (first < 0 || period <= 0 || first > INT64_MAX - now) guest_trap("invalid periodic timer interval", A0);
    EnterCriticalSection(&o->cs);
    o->timer_due = now + first; o->timer_period = period; o->timer_running = 1;
    wr64(A0 + 0x20, o->timer_due); wr64(A0 + 0x28, first); wr64(A0 + 0x30, period); wr8(A0 + 0x13, 2);
    WakeAllConditionVariable(&o->cv); LeaveCriticalSection(&o->cs);
}
static void timer_wait(Ctx *c) {
    Obj *o = obj(A0); EnterCriticalSection(&o->cs);
    for (;;) {
        int64_t now = monotonic_ns();
        if (o->timer_running && now >= o->timer_due) {
            int64_t elapsed = (now - o->timer_due) / o->timer_period + 1;
            o->timer_due += elapsed * o->timer_period;
            o->signaled = 1; wr64(A0 + 0x20, o->timer_due); wr8(A0 + 0x12, 1);
        }
        if (o->signaled) break;
        DWORD ms = o->timer_running ? ns_to_ms(o->timer_due - now) : INFINITE;
        SleepConditionVariableCS(&o->cv, &o->cs, ms);
    }
    if (o->autoclear) { o->signaled = 0; wr8(A0 + 0x12, 0); }
    LeaveCriticalSection(&o->cs);
}
static void multi_init(Ctx *c) {
    memset((void *)(uintptr_t)A0, 0, 0x30);
    wr8(A0, 1); wr64(A0 + 8, A0 + 8); wr64(A0 + 0x10, A0 + 8);
}
static void holder_init(Ctx *c) {
    uint64_t sdk_symbol(const char *);
    memset((void *)(uintptr_t)A0, 0, 0x40);
    uint64_t vtable = sdk_symbol("_ZTVN2nn2os6detail22MultiWaitHolderOfEventE");
    if (vtable) wr64(A0, vtable + 0x10);
    wr64(A0 + 8, A0 + 8); wr64(A0 + 0x10, A0 + 8);
    wr64(A0 + 0x18, A0 + 0x18); wr64(A0 + 0x20, A0 + 0x18);
    wr64(A0 + 0x30, A1);
}
static void multi_link(Ctx *c) {
    uint64_t sentinel = A0 + 8, node = A1 + 8, prev = rd64(sentinel);
    if (rd64(A1 + 0x28)) guest_trap("multiwait holder already linked", A1);
    wr64(node, prev); wr64(node + 8, sentinel); wr64(prev + 8, node); wr64(sentinel, node);
    wr64(A1 + 0x28, sentinel);
    Obj *event = obj(rd64(A1 + 0x30));
    EnterCriticalSection(&event->cs);
    event->in_multi = 1;
    LeaveCriticalSection(&event->cs);
    multi_notify();
}
static void multi_wait(Ctx *c) {
    uint64_t sentinel = A0 + 8;
    AcquireSRWLockExclusive(&g_multi_lock);
    for (;;) {
        for (uint64_t node = rd64(sentinel + 8); node != sentinel; node = rd64(node + 8)) {
            uint64_t holder = node - 8;
            Obj *event = obj(rd64(holder + 0x30));
            EnterCriticalSection(&event->cs);
            int ready = event->signaled; /* multiwait observes; WaitEvent consumes AutoClear */
            LeaveCriticalSection(&event->cs);
            if (ready) { ReleaseSRWLockExclusive(&g_multi_lock); RET(holder); return; }
        }
        SleepConditionVariableSRW(&g_multi_cv, &g_multi_lock, INFINITE, 0);
        g_multi_wakes++;                                   /* under g_multi_lock */
    }
}

/* ---------------------------------------------------------------- misc */
static void os_ConvertToTick(Ctx *c) { RET((uint64_t)((double)(int64_t)A0 * (double)TICK_HZ / 1e9)); }
static void os_ConvertToTimeSpan(Ctx *c) { RET((uint64_t)((double)(int64_t)A0 * 1e9 / (double)TICK_HZ)); }
/* The console fills the buffer from a generator seeded with kernel entropy: unpredictable bytes. Same here, from the
 * system generator (RtlGenRandom). */
BOOLEAN NTAPI SystemFunction036(PVOID buffer, ULONG length);
/* Test switch STRIKERS_FIXED_SEED=<n>: the bytes come from a splitmix64 stream seeded with n instead, so
 * that a scripted run sees the same random choices in every run. Each call is logged (first 16). */
int host_fixed_random(void *buffer, uint64_t size, const char *who, uint64_t lr) {
    static int mode = -1;
    static volatile LONG lock, calls;
    static uint64_t state;
    if (mode < 0) { const char *e = getenv("STRIKERS_FIXED_SEED"); if (e && *e) { state = strtoull(e, NULL, 0) * 0x9E3779B97F4A7C15ull + 1; mode = 1; } else mode = 0; }
    if (!mode) return 0;
    while (InterlockedExchange(&lock, 1)) YieldProcessor();
    for (uint64_t i = 0; i < size; i += 8) {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
        memcpy((uint8_t *)buffer + i, &z, size - i < 8 ? (size_t)(size - i) : 8);
    }
    LONG n = ++calls;
    InterlockedExchange(&lock, 0);
    if (n <= 16) printf("fixed seed: %s call %ld, %llu bytes, caller %llx\n", who, n, (unsigned long long)size, (unsigned long long)lr);
    return 1;
}
static void os_GenerateRandomBytes(Ctx *c) {
    uint8_t *p = (uint8_t *)(uintptr_t)A0;
    if (host_fixed_random(p, A1, "nn::os::GenerateRandomBytes", c->x[30] - 4)) return;
    for (uint64_t done = 0; done < A1;) {
        ULONG n = A1 - done > 0x10000000 ? 0x10000000 : (ULONG)(A1 - done);
        if (!SystemFunction036(p + done, n)) guest_trap("nn::os::GenerateRandomBytes: the system random generator failed", c->x[30] - 4);
        done += n;
    }
}

static void sem_init(Ctx *c) { Obj *o=obj(A0); EnterCriticalSection(&o->cs); o->len=A1;o->cap=A2; LeaveCriticalSection(&o->cs); }
static void sem_acquire(Ctx *c) { Obj *o=obj(A0);EnterCriticalSection(&o->cs);while(!o->len) SleepConditionVariableCS(&o->cv,&o->cs,INFINITE);o->len--;LeaveCriticalSection(&o->cs); }
static void sem_try(Ctx *c) { Obj *o=obj(A0);EnterCriticalSection(&o->cs);int ok=o->len>0;if(ok)o->len--;LeaveCriticalSection(&o->cs);RET(ok); }
static void sem_release(Ctx *c) { Obj *o=obj(A0);EnterCriticalSection(&o->cs);if(o->len<o->cap)o->len++;WakeAllConditionVariable(&o->cv);LeaveCriticalSection(&o->cs); }
#define OS(n) "_ZN2nn2os" n
static const ImportDef thread_imports[] = {
    {OS("15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi"), fib_init},
    {OS("13FinalizeFiberEPNS0_9FiberTypeE"), fib_finalize},
    {OS("13SwitchToFiberEPNS0_9FiberTypeE"), fib_switch},
    {OS("20InitializeTimerEventEPNS0_14TimerEventTypeENS0_14EventClearModeE"), timer_init},
    {OS("23StartPeriodicTimerEventEPNS0_14TimerEventTypeENS_8TimeSpanES3_"), timer_start},
    {OS("14WaitTimerEventEPNS0_14TimerEventTypeE"), timer_wait},
    {OS("19InitializeMultiWaitEPNS0_13MultiWaitTypeE"), multi_init},
    {OS("25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE"), holder_init},
    {OS("19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE"), multi_link},
    {OS("7WaitAnyEPNS0_13MultiWaitTypeE"), multi_wait},
    {OS("14TimedWaitEventEPNS0_9EventTypeENS_8TimeSpanE"),ev_timedwait},
    {OS("17TryWaitLightEventEPNS0_14LightEventTypeE"),ev_trywait},
    {OS("18SdkAllocateTlsSlotEPNS0_7TlsSlotEPFvmE"),os_AllocateTlsSlot},
    {OS("24GetThreadCurrentPriorityEPKNS0_10ThreadTypeE"),os_GetThreadPriority},
    {OS("13LockBusyMutexEPNS0_13BusyMutexTypeE"),mtx_lock},
    {OS("15UnlockBusyMutexEPNS0_13BusyMutexTypeE"),mtx_unlock},
    {OS("17FinalizeBusyMutexEPNS0_13BusyMutexTypeE"),nothing},
    {OS("19InitializeBusyMutexEPNS0_13BusyMutexTypeE"),mtx_init},
    {OS("19InitializeSemaphoreEPNS0_13SemaphoreTypeEii"),sem_init},
    {OS("17FinalizeSemaphoreEPNS0_13SemaphoreTypeE"),nothing},
    {OS("16AcquireSemaphoreEPNS0_13SemaphoreTypeE"),sem_acquire},
    {OS("19TryAcquireSemaphoreEPNS0_13SemaphoreTypeE"),sem_try},
    {OS("16ReleaseSemaphoreEPNS0_13SemaphoreTypeE"),sem_release},

    {OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"), os_CreateThread},
    {OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii"), os_CreateThreadOnCore},
    {OS("20GetCurrentCoreNumberEv"), os_GetCurrentCoreNumber},
    {OS("11StartThreadEPNS0_10ThreadTypeE"), os_StartThread}, {OS("10WaitThreadEPNS0_10ThreadTypeE"), os_WaitThread},
    {OS("13DestroyThreadEPNS0_10ThreadTypeE"), os_DestroyThread}, {OS("13SetThreadNameEPNS0_10ThreadTypeEPKc"), os_SetThreadName},
    {OS("17SetThreadCoreMaskEPNS0_10ThreadTypeEim"), os_SetThreadCoreMask},
    {OS("17GetThreadCoreMaskEPiPmPKNS0_10ThreadTypeE"), os_GetThreadCoreMask},   /* not an import of the game: nnosGetThreadCoreMask reaches it (nnmisc.c FWD) */
    {OS("26GetThreadAvailableCoreMaskEv"), os_AvailableCoreMask},
    {OS("16GetCurrentThreadEv"), os_GetCurrentThread}, {OS("17GetThreadPriorityEPKNS0_10ThreadTypeE"), os_GetThreadPriority},
    {OS("20ChangeThreadPriorityEPNS0_10ThreadTypeEi"), os_ChangeThreadPriority}, {OS("11GetThreadIdEPKNS0_10ThreadTypeE"), os_GetThreadId},
    {OS("11YieldThreadEv"), os_YieldThread}, {OS("11SleepThreadENS_8TimeSpanE"), os_SleepThread},
    {OS("19GetCurrentStackInfoEPmS1_"), os_GetCurrentStackInfo},
    {OS("15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE"), os_AllocateTlsSlot},
    {OS("11FreeTlsSlotENS0_7TlsSlotE"), os_FreeTlsSlot}, {OS("11GetTlsValueENS0_7TlsSlotE"), os_GetTlsValue}, {OS("11SetTlsValueENS0_7TlsSlotEm"), os_SetTlsValue},
    {OS("15InitializeMutexEPNS0_9MutexTypeEbi"), mtx_init}, {OS("13FinalizeMutexEPNS0_9MutexTypeE"), nothing},
    {OS("9LockMutexEPNS0_9MutexTypeE"), mtx_lock}, {OS("11UnlockMutexEPNS0_9MutexTypeE"), mtx_unlock}, {OS("12TryLockMutexEPNS0_9MutexTypeE"), mtx_trylock},
    {OS("15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE"), ev_init}, {OS("13FinalizeEventEPNS0_9EventTypeE"), nothing},
    {OS("11SignalEventEPNS0_9EventTypeE"), ev_signal}, {OS("10ClearEventEPNS0_9EventTypeE"), ev_clear}, {OS("9WaitEventEPNS0_9EventTypeE"), ev_wait},
    {OS("20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE"), ev_init}, {OS("18FinalizeLightEventEPNS0_14LightEventTypeE"), nothing},
    {OS("16SignalLightEventEPNS0_14LightEventTypeE"), ev_signal}, {OS("15ClearLightEventEPNS0_14LightEventTypeE"), ev_clear},
    {OS("14WaitLightEventEPNS0_14LightEventTypeE"), ev_wait},
    {OS("18DestroySystemEventEPNS0_15SystemEventTypeE"), sev_destroy},
    {OS("18TryWaitSystemEventEPNS0_15SystemEventTypeE"), sev_trywait_h}, {OS("15WaitSystemEventEPNS0_15SystemEventTypeE"), sev_wait_h},
    {OS("20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE"), sev_timedwait_h},
    {OS("27InitializeConditionVariableEPNS0_21ConditionVariableTypeE"), mtx_init}, {OS("25FinalizeConditionVariableEPNS0_21ConditionVariableTypeE"), nothing},
    {OS("21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE"), cv_wait},
    {OS("26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE"), cv_timedwait},
    {OS("23SignalConditionVariableEPNS0_21ConditionVariableTypeE"), cv_signal}, {OS("26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE"), cv_broadcast},
    {OS("22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm"), mq_init}, {OS("20FinalizeMessageQueueEPNS0_16MessageQueueTypeE"), nothing},
    {OS("16SendMessageQueueEPNS0_16MessageQueueTypeEm"), mq_send}, {OS("19TrySendMessageQueueEPNS0_16MessageQueueTypeEm"), mq_trysend},
    {OS("15JamMessageQueueEPNS0_16MessageQueueTypeEm"), mq_jam}, {OS("18TryJamMessageQueueEPNS0_16MessageQueueTypeEm"), mq_tryjam},
    {OS("19ReceiveMessageQueueEPmPNS0_16MessageQueueTypeE"), mq_receive}, {OS("22TryReceiveMessageQueueEPmPNS0_16MessageQueueTypeE"), mq_tryreceive},
    {OS("15GetCurrentFiberEv"), fib_current},
    {OS("13ConvertToTickENS_8TimeSpanE"), os_ConvertToTick}, {OS("17ConvertToTimeSpanENS0_4TickE"), os_ConvertToTimeSpan},
    {OS("19GenerateRandomBytesEPvm"), os_GenerateRandomBytes},
};

/* SDK 3.5.1 forms the game imports. SetThreadNamePointer(ThreadType*, const char*): [doc] the thread's name is
 * the given string (kept by pointer); recorded for the thread reports. */
static void os_SetThreadNamePointer(Ctx *c) { obj(A0)->name = A1; wr64(A0 + 0x1a0, A1); }
/* GetThreadNamePointer(thread): the name set for it; a thread never named gets "Thread_0x<address>" as nn::os names it
 * (kept, so the pointer stays valid). Read by sead::nnsdk::PrintThreadInformation. */
static void os_GetThreadNamePointer(Ctx *c) {
    Obj *o = obj(A0);
    if (!o->name) { char *s = malloc(32); if (s) { snprintf(s, 32, "Thread_0x%llx", (unsigned long long)A0); o->name = (uint64_t)(uintptr_t)s; } }
    RET(o->name);
}
static const ImportDef strikers_thread_imports[] = {
    {OS("19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE"), ev_timedwait},
    {OS("20SetThreadNamePointerEPNS0_10ThreadTypeEPKc"), os_SetThreadNamePointer},
    {"nnosSetThreadNamePointer", os_SetThreadNamePointer},
    /* TryWaitEvent: the movie player (MovieDecoderPlayer::update
     * polls its decoder's buffer events with it) and the sound library's loaders (nn::atk ...Loader::IsInUse, Finalize). */
    {OS("12TryWaitEventEPNS0_9EventTypeE"), ev_trywait},
    {OS("20GetThreadNamePointerEPKNS0_10ThreadTypeE"), os_GetThreadNamePointer},
};
/* the system-event entry points, also for lifted SDK callers (sdklift_rules.txt cuts them; installed below with
 * sdk_host_override). Create, AttachReadableHandle, Signal, Clear and GetReadableHandle are not imports of the game:
 * only the lifted SDK code calls them (the audio renderer's events). */
static const ImportDef sev_all[] = {
    {OS("17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb"), sev_create},
    {OS("33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"), sev_attach_readable},
    {OS("18DestroySystemEventEPNS0_15SystemEventTypeE"), sev_destroy},
    {OS("15WaitSystemEventEPNS0_15SystemEventTypeE"), sev_wait_h},
    {OS("18TryWaitSystemEventEPNS0_15SystemEventTypeE"), sev_trywait_h},
    {OS("20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE"), sev_timedwait_h},
    {OS("17SignalSystemEventEPNS0_15SystemEventTypeE"), ev_signal},
    {OS("16ClearSystemEventEPNS0_15SystemEventTypeE"), ev_clear},
    {OS("30GetReadableHandleOfSystemEventEPKNS0_15SystemEventTypeE"), sev_get_readable},
};
void sdk_host_override(const char *name, void (*fn)(Ctx *));   /* sdklift_rt.c */
void register_thread_imports(void) {
    register_imports(thread_imports, (int)(sizeof thread_imports / sizeof thread_imports[0]));
    register_imports(sev_all, (int)(sizeof sev_all / sizeof sev_all[0]));
    register_imports(strikers_thread_imports, (int)(sizeof strikers_thread_imports / sizeof strikers_thread_imports[0]));
    for (int i = 0; i < (int)(sizeof sev_all / sizeof sev_all[0]); i++) sdk_host_override(sev_all[i].name, sev_all[i].fn);
}
int guest_thread_count(void) { return (int)g_thread_count; }
