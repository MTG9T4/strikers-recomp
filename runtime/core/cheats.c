/* SPDX-License-Identifier: GPL-3.0-or-later
 * Cheats in the Atmosphere format (the cheat files players already have): <game folder>\cheats\<build id>.txt, where
 * the build id is the game program's (the first 16 hex digits are enough). Sections "[Name]" hold codes; "{Name}" is the
 * master code, always on. Settings > Cheats switches the others; the choice is kept in cheats\enabled.txt.
 *
 * The codes run 60 times a second on their own thread, as the console's cheat process runs them, over the game's memory:
 * region 0 = the game program (the image at its load address), region 1 = the heap. Supported: 0 store static,
 * 1 compare memory, 2 end / else, 3 loop, 4 load register, 5 load register from memory, 6 store to a register address,
 * 7 legacy arithmetic, 8 keys held, 9 arithmetic, A store register, C0 compare register. Anything else stops that cheat.
 * A fault (a pointer that is not there yet) skips that cheat for the frame; repeated faults switch it off.
 * Never while in a Local Wireless room (it would change only this player's game): cheats wait until the room is left. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int strikers_heap_range(uint64_t *base, uint64_t *size);   /* os/switch_services.c */
int strikers_ldn_state(void);                              /* services/nnldn.c: 3 hosting a room, 5 in one */
void pc_pad_navigation(unsigned long long *buttons, int *lx, int *ly);   /* gfx/pc_controls.inc: every pad's buttons */
#define MAIN_BASE 0x7100000000ull

typedef struct { char name[96]; uint32_t *op; int n; int master, on, faults, broken; } Cheat;
static Cheat *g_ch; static int g_nch;
static char g_build[17], g_file[MAX_PATH], g_status[200];
static CRITICAL_SECTION g_lock; static volatile LONG g_ready;

static void game_folder(char *out, size_t n) {
    DWORD got = GetModuleFileNameA(NULL, out, (DWORD)n);
    char *s = got && got < n ? strrchr(out, '\\') : NULL;
    if (s) *s = 0; else snprintf(out, n, ".");
}
static int build_id(char out[17]) {
    char path[MAX_PATH]; game_folder(path, sizeof path);
    strncat(path, "\\build\\dump\\update\\exefs\\main", sizeof path - strlen(path) - 1);
    FILE *f = fopen(path, "rb"); unsigned char h[0x48];
    if (!f) return 0;
    int ok = fread(h, 1, sizeof h, f) == sizeof h && !memcmp(h, "NSO0", 4);
    fclose(f);
    if (ok) for (int i = 0; i < 8; i++) sprintf(out + i * 2, "%02X", h[0x40 + i]);
    return ok;
}

static void load(void) {
    char dir[MAX_PATH], pat[MAX_PATH + 8]; WIN32_FIND_DATAA d;
    game_folder(dir, sizeof dir); strncat(dir, "\\cheats", sizeof dir - strlen(dir) - 1);
    if (!build_id(g_build)) { snprintf(g_status, sizeof g_status, "No cheats: the game's build id is not readable."); return; }
    snprintf(pat, sizeof pat, "%s\\*.txt", dir);
    HANDLE h = FindFirstFileA(pat, &d);
    if (h != INVALID_HANDLE_VALUE) {
        do if (!_strnicmp(d.cFileName, g_build, 16)) { snprintf(g_file, sizeof g_file, "%s\\%s", dir, d.cFileName); break; } while (FindNextFileA(h, &d));
        FindClose(h);
    }
    if (!g_file[0]) { snprintf(g_status, sizeof g_status, "No cheats for this version: put %s.txt in the cheats folder.", g_build); return; }
    FILE *f = fopen(g_file, "rb");
    if (!f) return;
    char line[1024]; Cheat *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '[' || *p == '{') {
            char close = *p == '[' ? ']' : '}', *e = strchr(p + 1, close);
            if (!e) continue;
            g_ch = realloc(g_ch, sizeof(Cheat) * (size_t)(g_nch + 1)); cur = &g_ch[g_nch++]; memset(cur, 0, sizeof *cur);
            snprintf(cur->name, sizeof cur->name, "%.*s", (int)(e - p - 1), p + 1);
            cur->master = *p == '{'; cur->on = cur->master;
            continue;
        }
        if (!cur) continue;
        for (char *t = strtok(p, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
            if (strlen(t) != 8 || strspn(t, "0123456789abcdefABCDEF") != 8) break;
            cur->op = realloc(cur->op, sizeof(uint32_t) * (size_t)(cur->n + 1)); cur->op[cur->n++] = (uint32_t)strtoul(t, NULL, 16);
        }
    }
    fclose(f);
    char en[MAX_PATH]; snprintf(en, sizeof en, "%s\\enabled.txt", dir);       /* names switched on last time */
    if ((f = fopen(en, "rb"))) {
        while (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; for (int i = 0; i < g_nch; i++) if (!strcmp(g_ch[i].name, line)) g_ch[i].on = 1; }
        fclose(f);
    }
    snprintf(g_status, sizeof g_status, "%d cheats for this version (%s).", g_nch, strrchr(g_file, '\\') + 1);
    printf("cheats: %s\n", g_status);
}

/* ---- the code interpreter */
typedef struct { uint64_t reg[16]; } Vm;
static uint64_t region(int m) { uint64_t b = 0, s = 0; if (m == 0) return MAIN_BASE; if (m == 1 && strikers_heap_range(&b, &s)) return b; return 0; }
static int width_ok(int w) { return w == 1 || w == 2 || w == 4 || w == 8; }
static uint64_t rd(uint64_t a, int w) { uint64_t v = 0; memcpy(&v, (void *)(uintptr_t)a, (size_t)w); return v; }
static void wr(uint64_t a, int w, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, (size_t)w); }
static int cond(uint64_t a, uint64_t b, int c) { return c == 1 ? a > b : c == 2 ? a >= b : c == 3 ? a < b : c == 4 ? a <= b : c == 5 ? a == b : c == 6 ? a != b : 0; }
static uint64_t mask(uint64_t v, int w) { return w == 8 ? v : v & ((1ull << (w * 8)) - 1); }
static uint64_t arith(uint64_t a, uint64_t b, int op) {
    switch (op) { case 0: return a + b; case 1: return a - b; case 2: return a * b; case 3: return a << b; case 4: return a >> b;
                  case 5: return a & b; case 6: return a | b; case 7: return ~a; case 8: return a ^ b; case 9: return a; }
    return a;
}
/* Skip to the end (or else) of the conditional block starting after op i. Returns the index after it. */
static int skip_block(const Cheat *c, int i, int *stopped_at_else) {
    int depth = 0;
    while (i < c->n) {
        uint32_t w = c->op[i]; int t = w >> 28;
        int len = t == 0 ? (((w >> 24) & 0xF) == 8 ? 4 : 3) : t == 1 ? (((w >> 24) & 0xF) == 8 ? 4 : 3) : t == 3 ? ((w >> 24) & 1 ? 1 : 2)
                : t == 4 ? 3 : t == 5 ? 2 : t == 6 ? 3 : t == 7 ? 2 : t == 9 ? (((w >> 20) & 1) ? (((w >> 24) & 0xF) == 8 ? 3 : 2) : 1)
                : t == 0xA ? 1 + (((w >> 8) & 0xF) == 2 ? 1 : 0)
                : t == 0xC && ((w >> 24) & 0xF) == 0 && ((w >> 12) & 0xF) == 4 ? (((w >> 20) & 0xF) == 8 ? 3 : 2) : 1;
        if (t == 1 || t == 8 || (t == 0xC && ((w >> 24) & 0xF) == 0)) depth++;
        if (t == 2) {
            int els = ((w >> 24) & 0xF) == 1;
            if (!depth) { if (stopped_at_else) *stopped_at_else = els; return i + 1; }
            if (!els) depth--;
        }
        i += len;
    }
    return i;
}
static int run(Cheat *c, Vm *vm, unsigned long long keys) {
    int loop_start[16] = {0};
    for (int i = 0; i < c->n;) {
        uint32_t w = c->op[i]; int t = w >> 28, T = (w >> 24) & 0xF;
        if (t == 0) {                                          /* 0TMR00AA AAAAAAAA VVVVVVVV [VVVVVVVV] */
            if (!width_ok(T) || i + (T == 8 ? 3 : 2) >= c->n || ((w >> 20) & 0xF) > 1) return 0;
            int m = (w >> 20) & 0xF, r = (w >> 16) & 0xF;
            if (!region(m)) return 2;
            uint64_t a = region(m) + (((uint64_t)(w & 0xFF) << 32) | c->op[i + 1]) + vm->reg[r];
            uint64_t v = T == 8 ? ((uint64_t)c->op[i + 2] << 32 | c->op[i + 3]) : c->op[i + 2];
            wr(a, T, v); i += T == 8 ? 4 : 3;
        } else if (t == 1) {                                   /* 1TMC00AA AAAAAAAA VVVVVVVV [VVVVVVVV] */
            if (!width_ok(T) || i + (T == 8 ? 3 : 2) >= c->n || ((w >> 20) & 0xF) > 1) return 0;
            int m = (w >> 20) & 0xF, cc = (w >> 16) & 0xF;
            if (!region(m)) return 2;
            uint64_t a = region(m) + (((uint64_t)(w & 0xFF) << 32) | c->op[i + 1]);
            uint64_t v = T == 8 ? ((uint64_t)c->op[i + 2] << 32 | c->op[i + 3]) : c->op[i + 2];
            i += T == 8 ? 4 : 3;
            if (!cond(rd(a, T), v, cc)) i = skip_block(c, i, NULL);
        } else if (t == 2) {                                   /* 20000000 end, 21000000 else: reached from a true branch */
            if (T == 1) i = skip_block(c, i + 1, NULL); else i++;
        } else if (t == 3) {                                   /* 300R0000 VVVVVVVV start, 310R0000 end */
            int r = (w >> 16) & 0xF;
            if (!T) { if (i + 1 >= c->n) return 0; vm->reg[r] = c->op[i + 1]; loop_start[r] = i + 2; i += 2; }
            else { if (vm->reg[r] && --vm->reg[r]) i = loop_start[r]; else i++; }
        } else if (t == 4) {                                   /* 400R0000 VVVVVVVV VVVVVVVV */
            if (i + 2 >= c->n) return 0;
            vm->reg[(w >> 16) & 0xF] = (uint64_t)c->op[i + 1] << 32 | c->op[i + 2]; i += 3;
        } else if (t == 5) {                                   /* 5TMR00AA AAAAAAAA: R = [M+A]; 5T0R10AA: R = [R+A] */
            if (!width_ok(T) || i + 1 >= c->n) return 0;
            int m = (w >> 20) & 0xF, r = (w >> 16) & 0xF, from_reg = (w >> 12) & 0xF;
            uint64_t off = ((uint64_t)(w & 0xFF) << 32) | c->op[i + 1];
            if (from_reg != 1 && m > 1) return 0;
            if (from_reg != 1 && !region(m)) return 2;
            uint64_t a = from_reg == 1 ? vm->reg[r] + off : region(m) + off;
            vm->reg[r] = rd(a, T); i += 2;
        } else if (t == 6) {                                   /* 6T0RIor0 VVVVVVVV VVVVVVVV: [R (+ r)] = V, I: R += width */
            if (!width_ok(T) || i + 2 >= c->n) return 0;
            int r = (w >> 16) & 0xF, inc = (w >> 12) & 0xF, add = (w >> 8) & 0xF, r2 = (w >> 4) & 0xF;
            wr(vm->reg[r] + (add ? vm->reg[r2] : 0), T, (uint64_t)c->op[i + 1] << 32 | c->op[i + 2]);
            if (inc) vm->reg[r] += (uint64_t)T;
            i += 3;
        } else if (t == 7) {                                   /* 7T0RC000 VVVVVVVV: R = R op V */
            if (i + 1 >= c->n) return 0;
            int r = (w >> 16) & 0xF, op = (w >> 12) & 0xF;
            vm->reg[r] = mask(arith(vm->reg[r], c->op[i + 1], op), width_ok(T) ? T : 8); i += 2;
        } else if (t == 8) {                                   /* 8kkkkkkk: these keys held */
            uint32_t k = w & 0x0FFFFFFF; i++;
            if ((keys & k) != k) i = skip_block(c, i, NULL);
        } else if (t == 9) {                                   /* 9TCRS0s0 (register) / 9TCRS100 V... (value) */
            int op = (w >> 20) & 0xF, r = (w >> 16) & 0xF, s = (w >> 12) & 0xF, imm = (w >> 8) & 0xF, s2 = (w >> 4) & 0xF;
            uint64_t b;
            if (imm) { if (i + (T == 8 ? 2 : 1) >= c->n) return 0; b = T == 8 ? ((uint64_t)c->op[i + 1] << 32 | c->op[i + 2]) : c->op[i + 1]; i += T == 8 ? 3 : 2; }
            else { b = vm->reg[s2]; i++; }
            vm->reg[r] = mask(arith(vm->reg[s], b, op), width_ok(T) ? T : 8);
        } else if (t == 0xA) {                                 /* ATSRIOxa: [R (+ r / + a)] = S */
            int s = (w >> 20) & 0xF, r = (w >> 16) & 0xF, inc = (w >> 12) & 0xF, kind = (w >> 8) & 0xF, r2 = (w >> 4) & 0xF;
            if (!width_ok(T)) return 0;
            uint64_t a = vm->reg[r];
            if (kind == 1) a += vm->reg[r2];
            else if (kind == 2) { if (i + 1 >= c->n) return 0; a += ((uint64_t)(w & 0xF) << 32) | c->op[i + 1]; }
            else if (kind) return 0;
            wr(a, T, vm->reg[s]);
            if (inc) vm->reg[r] += (uint64_t)T;
            i += kind == 2 ? 2 : 1;
        } else if (t == 0xC && T == 0) {                       /* C0TcS4Xr / C0TcS5X0 VV..: compare register X with ... (value form) */
            int wdt = (w >> 20) & 0xF, cc = (w >> 16) & 0xF, x = (w >> 8) & 0xF, kind = (w >> 12) & 0xF;
            if (!width_ok(wdt) || kind != 4 || i + (wdt == 8 ? 2 : 1) >= c->n) return 0;
            uint64_t v = wdt == 8 ? ((uint64_t)c->op[i + 1] << 32 | c->op[i + 2]) : c->op[i + 1];
            i += wdt == 8 ? 3 : 2;
            if (!cond(mask(vm->reg[x], wdt), v, cc)) i = skip_block(c, i, NULL);
        } else return 0;                                        /* not supported: the cheat stops here */
    }
    return 1;
}

static int run_safe(Cheat *c, Vm *vm, unsigned long long keys) {      /* 1 ran, 2 not yet, -1 not supported, 0 a fault */
    __try { int r = run(c, vm, keys); return r == 0 ? -1 : r; }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return 0; }
}

static DWORD WINAPI cheat_thread(void *unused) {
    (void)unused;
    Vm vm; memset(&vm, 0, sizeof vm);
    int paused = 0;
    for (;;) {
        Sleep(16);
        int st = strikers_ldn_state();
        int room = st >= 2 && st <= 5;
        if (room != paused) { paused = room; printf("cheats: %s\n", room ? "paused: you are in a Local Wireless room" : "running again"); }
        if (room) continue;
        unsigned long long keys = 0; int lx, ly; pc_pad_navigation(&keys, &lx, &ly);
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_nch; i++) {
            Cheat *c = &g_ch[i];
            if (!c->on || c->broken) continue;
            int r = run_safe(c, &vm, keys);
            if (r < 0) { c->broken = 1; printf("cheats: \"%s\" uses a code this game's cheat support does not have: switched off\n", c->name); }
            else if (!r && ++c->faults == 600) { c->broken = 1; printf("cheats: \"%s\" kept pointing at memory that is not there (wrong version?): switched off\n", c->name); }
        }
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

void cheats_start(void) {
    if (InterlockedCompareExchange(&g_ready, 1, 0)) return;
    InitializeCriticalSection(&g_lock);
    load();
    g_ready = 2;
    if (g_nch) { HANDLE h = CreateThread(NULL, 0, cheat_thread, NULL, 0, NULL); if (h) CloseHandle(h); }
}

/* Settings > Cheats */
int cheats_list(char names[][96], int *on, int max, char *status, size_t sn) {
    if (g_ready != 2) { snprintf(status, sn, "Cheats load when the game starts."); return 0; }
    snprintf(status, sn, "%s", g_status);
    EnterCriticalSection(&g_lock);
    int n = 0;
    for (int i = 0; i < g_nch && n < max; i++) { if (g_ch[i].master) continue; snprintf(names[n], 96, "%s%s", g_ch[i].name, g_ch[i].broken ? " (switched off: not supported)" : ""); on[n++] = g_ch[i].on; }
    LeaveCriticalSection(&g_lock);
    return n;
}
void cheats_set(int index, int on) {
    if (g_ready != 2) return;
    char dir[MAX_PATH], en[MAX_PATH];
    EnterCriticalSection(&g_lock);
    int n = 0;
    for (int i = 0; i < g_nch; i++) { if (g_ch[i].master) continue; if (n++ == index) { g_ch[i].on = on; printf("cheats: \"%s\" %s\n", g_ch[i].name, on ? "on" : "off"); } }
    game_folder(dir, sizeof dir); snprintf(en, sizeof en, "%s\\cheats\\enabled.txt", dir);
    FILE *f = fopen(en, "wb");
    if (f) { for (int i = 0; i < g_nch; i++) if (g_ch[i].on && !g_ch[i].master) fprintf(f, "%s\r\n", g_ch[i].name); fclose(f); }
    LeaveCriticalSection(&g_lock);
}
