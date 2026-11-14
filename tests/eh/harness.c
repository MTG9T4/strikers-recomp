/* SPDX-License-Identifier: GPL-3.0-or-later
 * Standalone host for actual nncxxrt/nnunwind/eh_resume. Including nncxxrt
 * exposes ABI adapters for assertions without copying exception logic.
 * Non-EH services below are a dispatcher or fail-fast stubs.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nncxxrt.c"
#include "fixture.h"
void register_unwind_imports(void);
void unwind_report(FILE *out);
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(80); } } while (0)

const FnEntry (*game_fn_table_p)[];
const int *game_fn_table_count_p;
const DynSym (*game_dyn_syms_p)[];
const int *game_dyn_sym_count_p;
typedef struct {
    void (*call_indirect)(Ctx *, uint64_t);
    void (*host_import)(Ctx *, int);
    void (*guest_trap)(const char *, uint64_t);
    void (*guest_unimpl)(uint64_t, uint32_t, const char *);
    void (*guest_svc)(Ctx *, uint32_t);
    void (*stx_diag)(Ctx *, uint64_t, uint64_t, uint64_t, uint64_t, int);
} TestHost;
typedef struct {
    const FnEntry *table;
    const int *count;
    const DynSym *syms;
    const int *sym_count;
} TestTables;
static ImportDef imports[512];
static int nimports, events, cleanups, catches;
static uint64_t entry_sp, thrown;
static uint64_t test_type[2];

void register_imports(const ImportDef *defs, int count) {
    CHECK(nimports + count < 512);
    for (int i = 0; i < count; i++) imports[nimports++] = defs[i];
}
ImportFn import_lookup(const char *name) {
    for (int i = nimports - 1; i >= 0; i--)
        if (!strcmp(imports[i].name, name)) return imports[i].fn;
    return NULL;
}
ImportFn handler_by_name(const char *name) { return import_lookup(name); }
void trace_printf(const char *fmt, ...) { (void)fmt; }
void report_and_exit(int code) { exit(code ? code : 81); }
void guest_trap(const char *why, uint64_t at) { fprintf(stderr, "TRAP %s at %llx\n", why, at); exit(82); }
void guest_unimpl(uint64_t pc, uint32_t word, const char *why) { (void)word; guest_trap(why, pc); }
void guest_svc(Ctx *c, uint32_t n) { (void)c; guest_trap("svc", n); }
void stx_diag(Ctx *c, uint64_t a, uint64_t b, uint64_t d, uint64_t e, int bits) {
    (void)c; (void)b; (void)d; (void)e; (void)bits; guest_trap("exclusive store", a);
}
void call_indirect(Ctx *c, uint64_t addr) {
    for (int i = 0; i < fn_table_count; i++)
        if (fn_table[i].addr == addr) { fn_table[i].fn(c); return; }
    guest_trap("missing guest function", addr);
}
void call_guest(Ctx *c, uint64_t addr) { call_indirect(c, addr); }
void host_import(Ctx *c, int sym) {
    CHECK(sym > 0 && sym < dyn_sym_count);
    const char *name = dyn_syms[sym].name;
    const char *previous = c->in_import;
    uint64_t from = c->import_from;
    c->in_import = name; c->import_from = c->x[30];
    if (!strcmp(name, "cleaned")) {
        CHECK(c->x[31] == entry_sp - 64); // pad must skip its prologue
        CHECK(c->x[0] == thrown - 0x20 && c->x[1] == 0);
        CHECK(rd64(entry_sp - 48) == 0x7777); // cleanup's saved caller x19
        CHECK(eh_uncaught_count(c) == 1);
        events = events * 10 + 1; cleanups++;
    } else if (!strcmp(name, "caught")) {
        CHECK(c->x[31] == entry_sp - 32 && c->x[19] == 0x7777);
        CHECK(c->x[0] == thrown && rd32(thrown) == 1234);
        CHECK(eh_uncaught_count(c) == 0);
        events = events * 10 + 2; catches++;
    } else if (!strcmp(name, "unexpected_return")) {
        guest_trap("throw returned through abandoned host frame", c->x[30]);
    } else {
        ImportFn fn = import_lookup(name); CHECK(fn != NULL); fn(c);
    }
    c->in_import = previous; c->import_from = from;
}

/* Library/loader services outside EH are not exercised. */
void libc2_install_data(void) {}
void register_libc2_imports(void) {}
void cxxlib_install_data(void) {}
void register_cxxlib_imports(void) {}
uint64_t import_address(int sym) { return IMPORT_BASE + (uint64_t)sym * IMPORT_SLOT; }
void import_set_address(const char *name, void *addr) { (void)name; (void)addr; guest_trap("unexpected data install", 0); }
uint64_t hostfn_address(const char *name, ImportFn fn) { (void)name; (void)fn; guest_trap("unexpected hostfn install", 0); return 0; }
const char *hostfn_current_name(void) { return "EH test"; }
uint64_t guest_function_of(uint64_t rip) { (void)rip; return 0; }
void cxx_string_init(Ctx *c, uint64_t s, const char *p, size_t n) { (void)c; (void)s; (void)p; (void)n; guest_trap("unexpected string ctor", 0); }
const char *cxx_string_data(uint64_t s, size_t *n) { (void)s; (void)n; guest_trap("unexpected string access", 0); return NULL; }

int main(int argc, char **argv) {
    CHECK(argc == 3);
    HMODULE dll = LoadLibraryA(argv[1]); CHECK(dll != NULL);
    void (*bind)(const TestHost *, TestTables *) = (void *)GetProcAddress(dll, "game_bind");
    CHECK(bind != NULL && GetProcAddress(dll, "game_eh_bind") != NULL);
    TestHost host = {call_indirect, host_import, guest_trap, guest_unimpl, guest_svc, stx_diag};
    TestTables tables; bind(&host, &tables);
    game_fn_table_p = (const FnEntry (*)[])tables.table;
    game_fn_table_count_p = tables.count;
    game_dyn_syms_p = (const DynSym (*)[])tables.syms;
    game_dyn_sym_count_p = tables.sym_count;
    FILE *f = fopen(argv[2], "rb"); CHECK(f != NULL);
    CHECK(fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f); CHECK(size > 0); rewind(f);
    void *image = VirtualAlloc((void *)GUEST_BASE, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(image == (void *)GUEST_BASE);
    CHECK(fread(image, 1, size, f) == (size_t)size); fclose(f);
    register_unwind_imports();
    register_cxxrt_imports(); // actual registration, game_eh_bind, sorted-table validation
    CHECK(g_main_resume_n == 2 && eh_main_resume_entry(TEST_CLEANUP_PAD) != NULL);
    CHECK(eh_main_resume_entry(TEST_CLEANUP_PAD + 4) == NULL);
    Ctx c = {0};
    uint64_t *stack = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(stack != NULL);
    entry_sp = (uint64_t)(uintptr_t)stack + 0x10000; c.x[31] = entry_sp;
    for (int i = 19; i <= 29; i++) c.x[i] = 0x9000 + i;
    for (int i = 8; i <= 15; i++) c.v[i].d[0] = 0xa000 + i;
    c.x[30] = 0x12345678;
    c.in_import = "outer host callback"; c.import_from = 0xabcdef;
    test_type[1] = (uint64_t)(uintptr_t)"i";
    for (int run = 0; run < 3; run++) {
        thrown = eh_allocate(4); wr32(thrown, 1234);
        c.x[0] = thrown; c.x[1] = (uint64_t)(uintptr_t)test_type; c.x[2] = 0;
        call_indirect(&c, TEST_CALLER);
        CHECK(c.x[0] == 42 && c.x[31] == entry_sp && c.x[30] == 0x12345678);
        for (int i = 19; i <= 29; i++) CHECK(c.x[i] == (uint64_t)(0x9000 + i));
        for (int i = 8; i <= 15; i++) CHECK(c.v[i].d[0] == (uint64_t)(0xa000 + i));
        CHECK(c.in_import && !strcmp(c.in_import, "outer host callback") && c.import_from == 0xabcdef);
        CHECK(eh_in_flight(&c) == 0 && eh_uncaught_count(&c) == 0 && eh_state(&c)->caught == 0);
        CHECK(exc_header(thrown) == NULL); // __cxa_end_catch released the allocation
    }
    CHECK(events == 121212 && cleanups == 3 && catches == 3);
    unwind_report(stdout);
    puts("PASS: 3 guest throws, 3 cleanups, 3 catches; registers restored, caller returned once per throw");
    eh_unregister_image(GUEST_BASE);
    VirtualFree(image, 0, MEM_RELEASE); VirtualFree(stack, 0, MEM_RELEASE); FreeLibrary(dll);
    return 0;
}
