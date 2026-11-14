/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real generated SDK tests. Host boundary fixture from job06_contracts.c. */
#include <math.h>
#include "sdklift.h"
#define JOB06_WITH_SDK
#include "job06_contracts.c"

const DynSym dyn_syms[] = {
    {"_ZN2nn3hid13VibrationNode30DefaultVibrationSampleIntervalE", 0, 0},
    {"nvnPrivate0", 0, 0},
    {"_ZN2nn4util6detail7FloatPiE", 0, 0},
};
const int dyn_sym_count = sizeof dyn_syms / sizeof dyn_syms[0];
static struct { const char *name; void *address; } data_bindings[16];
static int data_bindings_n;
__declspec(dllexport) int job06_sdk_stage;
void import_set_address(const char *name, void *addr) {
    data_bindings[data_bindings_n].name = name;
    data_bindings[data_bindings_n++].address = addr;
}
void *import_data(const char *name) { (void)name; return (void *)1; }
ImportFn handler_by_name(const char *name) {
    for (int i = 0; i < test_count; i++) if (!strcmp(name, test_defs[i].name)) return test_defs[i].fn;
    return NULL;
}
ImportFn import_lookup(const char *name) { return handler_by_name(name); }
int eh_register_image(uint64_t b, uint64_t s, uint64_t h) { (void)b; (void)s; (void)h; return 1; }
void call_indirect(Ctx *c, uint64_t a) { sdk_dispatch(c, a); }
void report_and_exit(int code) { exit(code); }
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what) { (void)insn; guest_trap(what, pc); }
void guest_svc(Ctx *c, uint32_t n) { if (!os_dispatch_svc(c, n)) test_traps++; }
void stx_diag(Ctx *c, uint64_t a, uint64_t b, uint64_t d, uint64_t e, int bits) {
    (void)c; (void)a; (void)b; (void)d; (void)e; (void)bits;
}
static void test_memcpy(Ctx *c) { memcpy((void *)(uintptr_t)c->x[0], (void *)(uintptr_t)c->x[1], c->x[2]); }
static void test_memset(Ctx *c) { memset((void *)(uintptr_t)c->x[0], (int)c->x[1], c->x[2]); }
static void test_strlen(Ctx *c) { c->x[0] = strlen((const char *)(uintptr_t)c->x[0]); }
static void sdk_invoke(const char *name, Ctx *c) {
    ImportFn fn = sdk_lifted(name);
    if (!fn) { test_traps++; return; }
    fn(c);
}
#ifdef JOB15_ZLIB
#include "job15_zlib_contracts.inc"
#endif
__declspec(dllexport) int job06_sdk_tests(const char *path) {
    _putenv_s("STRIKERS_SDK_ELF", path);
    job06_sdk_stage = 1;
    sdk_prepare_data();
    job06_sdk_stage = 10;
    REQUIRE(data_bindings_n == 3);
    REQUIRE((uint64_t)(uintptr_t)data_bindings[0].address == sdk_symbol(dyn_syms[0].name));
    REQUIRE(rd64((uint64_t)(uintptr_t)data_bindings[0].address) == 5000000);
    REQUIRE((uint64_t)(uintptr_t)data_bindings[1].address == SDK_BASE + 0xd99f90);
    float pi; memcpy(&pi, data_bindings[2].address, 4); REQUIRE(pi > 3.14159f && pi < 3.14160f);
    register_sdklift_imports();
    static const ImportDef mem[] = {{"memcpy", test_memcpy}, {"memset", test_memset}, {"strlen", test_strlen}};
    void register_clib_imports(void);
    register_imports(mem, 3); register_thread_imports(); register_main_imports();
    register_clib_imports();
    job06_sdk_stage = 11;
    Ctx c = {0};
    uint8_t *stack = VirtualAlloc(NULL, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    REQUIRE(stack); c.x[31] = (uint64_t)(uintptr_t)(stack + 0x100000 - 16);
    uint8_t tls[0x400] = {0}; c.tpidr = (uint64_t)(uintptr_t)tls;
    wr64(c.tpidr + 0x1f8, os_main_thread_type());
    const char numbers[] = "a12b\xef\xbc\x93\xef\xbc\x94\xef\xbc\x95\xef\xbc\x96";
    c.x[0] = (uint64_t)(uintptr_t)numbers;
    job06_sdk_stage = 2;
    sdk_invoke("_ZN2nn3ngc12CountNumbersEPKc", &c); REQUIRE(c.x[0] == 6);
    uint16_t utf16[16] = {0}; char utf8[32] = {0};
    const char text[] = "A\xe2\x82\xac\xf0\x9f\x99\x82";
    c.x[0] = (uint64_t)(uintptr_t)utf16; c.x[1] = 16; c.x[2] = (uint64_t)(uintptr_t)text;
    job06_sdk_stage = 3;
    sdk_invoke("_ZN2nn4util30ConvertStringUtf8ToUtf16NativeEPtiPKc", &c);
    REQUIRE(utf16[0] == 0x41 && utf16[1] == 0x20ac && utf16[2] == 0xd83d && utf16[3] == 0xde42 && !utf16[4]);
    c.x[0] = (uint64_t)(uintptr_t)utf8; c.x[1] = sizeof utf8; c.x[2] = (uint64_t)(uintptr_t)utf16;
    sdk_invoke("_ZN2nn4util30ConvertStringUtf16NativeToUtf8EPciPKt", &c); REQUIRE(!strcmp(utf8, text));
    uint8_t config[0x4d8]; memset(config, 0xcc, sizeof config);
    c.x[0] = (uint64_t)(uintptr_t)config; c.x[1] = 0;
    job06_sdk_stage = 4;
    sdk_invoke("_ZN2nn5swkbd10MakePresetEPNS0_14KeyboardConfigENS0_6PresetE", &c);
    REQUIRE(rd32((uint64_t)(uintptr_t)config) == 2 && config[0x4d0] == 0xcc);
    c.x[0] = 0; sdk_invoke("_ZN2nn5swkbd25GetRequiredWorkBufferSizeEb", &c); REQUIRE(c.x[0] == 0x1000);
    c.x[0] = 1; sdk_invoke("_ZN2nn5swkbd25GetRequiredWorkBufferSizeEb", &c); REQUIRE(c.x[0] == 0xd000);
    sdk_invoke("_ZN2nn5swkbd27GetRequiredStringBufferSizeEv", &c); REQUIRE(c.x[0] == 0x7d4);
    uint8_t sensor[0x60] = {0}; float one = 1, quaternion[4] = {0};
    memcpy(sensor + 0x34, &one, 4); memcpy(sensor + 0x44, &one, 4); memcpy(sensor + 0x54, &one, 4);
    c.x[0] = (uint64_t)(uintptr_t)sensor; c.x[1] = (uint64_t)(uintptr_t)quaternion;
    job06_sdk_stage = 5;
    sdk_invoke("_ZNK2nn3hid18SixAxisSensorState13GetQuaternionEPNS_4util4neon12Vector4fTypeE", &c);
    REQUIRE(fabsf(quaternion[0]) < 0.0001f && fabsf(quaternion[1]) < 0.0001f && fabsf(quaternion[2]) < 0.0001f && fabsf(fabsf(quaternion[3]) - 1) < 0.0001f);
    /* ARM wchar_t is 32 bits; a Windows CRT vswprintf call would corrupt this. */
    uint32_t format[] = {'%','d',0}, wide[8] = {0}; uint64_t args[] = {42};
    struct { uint64_t stack, gr_top, vr_top; int32_t gr_offset, vr_offset; } va = {(uint64_t)(uintptr_t)args, 0, 0, 0, 0};
    c.x[0] = (uint64_t)(uintptr_t)wide; c.x[1] = 8; c.x[2] = (uint64_t)(uintptr_t)format; c.x[3] = (uint64_t)(uintptr_t)&va;
    job06_sdk_stage = 6;
    sdk_invoke("vswprintf", &c); REQUIRE(c.x[0] == 2 && wide[0] == '4' && wide[1] == '2' && wide[2] == 0);
    REQUIRE(test_traps == 0);
#ifdef JOB15_ZLIB
    job06_sdk_stage=15;
    { int line=job15_zip_tests(&c);if(line)return line; }
#endif
    VirtualFree(stack, 0, MEM_RELEASE);
    VirtualFree((void *)(uintptr_t)SDK_BASE, 0, MEM_RELEASE);
    return 0;
}
