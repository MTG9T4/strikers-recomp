/* Probe for runtime/core/cheats.c (tools/audit/test_cheats.py): the Atmosphere cheat codes against memory reserved at
 * the game's load address and a stand-in heap, checked value by value. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint8_t *g_test_heap;
int strikers_heap_range(uint64_t *base, uint64_t *size) { *base = (uint64_t)(uintptr_t)g_test_heap; *size = 65536; return g_test_heap != NULL; }
int strikers_ldn_state(void) { return 0; }
void pc_pad_navigation(unsigned long long *b, int *lx, int *ly) { *b = 0; *lx = *ly = 0; }
#include "../../runtime/core/cheats.c"

static int fails;
#define CHECK(c, what) do { if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } } while (0)
static int go(const char *codes, unsigned long long keys, Vm *vm) {
    Cheat c; memset(&c, 0, sizeof c);
    char buf[512]; snprintf(buf, sizeof buf, "%s", codes);
    for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) { c.op = realloc(c.op, sizeof(uint32_t) * (size_t)(c.n + 1)); c.op[c.n++] = (uint32_t)strtoul(t, NULL, 16); }
    int r = run_safe(&c, vm, keys);
    free(c.op);
    return r;
}
int main(void) {
    uint8_t *main_mem = VirtualAlloc((void *)(uintptr_t)MAIN_BASE, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (main_mem != (uint8_t *)(uintptr_t)MAIN_BASE) { printf("FAIL cannot reserve memory at the game's address\n"); return 1; }
    Vm vm; memset(&vm, 0, sizeof vm);
    CHECK(go("04100000 00000010 12345678", 0, &vm) == 2, "a heap code before the heap exists waits (2)");
    g_test_heap = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint32_t v32; uint64_t v64;
    go("04000000 00000010 12345678", 0, &vm); memcpy(&v32, main_mem + 0x10, 4);
    CHECK(v32 == 0x12345678, "0: 4 bytes into the program");
    go("08100000 00000020 AABBCCDD 11223344", 0, &vm); memcpy(&v64, g_test_heap + 0x20, 8);
    CHECK(v64 == 0xAABBCCDD11223344ull, "0: 8 bytes into the heap");
    go("02000000 00000030 0000BEEF", 0, &vm);
    CHECK(main_mem[0x30] == 0xEF && main_mem[0x31] == 0xBE && main_mem[0x32] == 0, "0: 2 bytes");
    go("14050000 00000010 12345678 04000000 00000014 00000001 20000000", 0, &vm); memcpy(&v32, main_mem + 0x14, 4);
    CHECK(v32 == 1, "1: a true condition runs its block");
    go("14050000 00000010 00000000 04000000 00000018 00000001 20000000 04000000 0000001C 00000007", 0, &vm);
    memcpy(&v32, main_mem + 0x18, 4); uint32_t after; memcpy(&after, main_mem + 0x1C, 4);
    CHECK(v32 == 0 && after == 7, "1: a false condition skips to its end, the rest runs");
    go("14050000 00000010 00000000 04000000 00000040 00000001 21000000 04000000 00000044 00000002 20000000", 0, &vm);
    uint32_t a1, a2; memcpy(&a1, main_mem + 0x40, 4); memcpy(&a2, main_mem + 0x44, 4);
    CHECK(a1 == 0 && a2 == 2, "2: else runs when the condition is false");
    uint64_t ptr = (uint64_t)(uintptr_t)(g_test_heap + 0x100); memcpy(main_mem + 0x80, &ptr, 8);
    go("58000000 00000080 40010000 00000000 0000CAFE A4100200 00000004", 0, &vm); memcpy(&v32, g_test_heap + 0x104, 4);
    CHECK(v32 == 0xCAFE, "5 and A: a pointer from the program, a register stored through it");
    memset(&vm, 0, sizeof vm);
    go("30020000 00000003 78030000 00000001 31020000", 0, &vm);
    CHECK(vm.reg[3] == 3, "3 and 7: a loop of three adds");
    go("40050000 00000000 00000010 98055100 00000000 00000004", 0, &vm);    /* 9: R5 = R5 + 4 (value form, 8 bytes) */
    CHECK(vm.reg[5] == 0x14, "9: arithmetic with a value");
    go("80000001 04000000 00000050 00000009 20000000", 0, &vm); memcpy(&v32, main_mem + 0x50, 4);
    CHECK(v32 == 0, "8: keys not held: skipped");
    go("80000001 04000000 00000050 00000009 20000000", 1, &vm); memcpy(&v32, main_mem + 0x50, 4);
    CHECK(v32 == 9, "8: A held: runs");
    memset(&vm, 0, sizeof vm); vm.reg[0] = (uint64_t)(uintptr_t)(g_test_heap + 0x200);
    go("64001000 00000000 00000011 64001000 00000000 00000022", 0, &vm);
    CHECK(g_test_heap[0x200] == 0x11 && g_test_heap[0x204] == 0x22 && vm.reg[0] == (uint64_t)(uintptr_t)(g_test_heap + 0x208), "6: stores through a register, moving it on");
    vm.reg[0] = 0; vm.reg[7] = 5;                               /* code 0 adds register R (here 0) to its address */
    go("C0454700 00000005 04000000 00000060 00000001 20000000", 0, &vm); memcpy(&v32, main_mem + 0x60, 4);
    CHECK(v32 == 1, "C0: a register compared with a value");
    CHECK(go("FF000000", 0, &vm) == -1, "an unsupported code is reported (-1)");
    memset(&vm, 0, sizeof vm);
    CHECK(go("58000000 00000F00 64000000 00000000 00000001", 0, &vm) == 0, "a null pointer faults (0), no crash");
    printf("%s: %d failed\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
