"""MSVC DLL-only host checks. Does not link or start strikers_native.exe.

GPL-3.0-or-later. Strikers project code. Tests binary128 rounding against an
independent integer oracle, offline service results, and the game EH shim ABI.
Temporary compiler inputs and DLLs live in the system temporary directory.
"""
import ctypes
from contextlib import contextmanager
import json
import math
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import uuid
import sys
import zlib

from build_runtime import ROOT, msvc_environment

LOADED_DLLS = []


@contextmanager
def scratch_directory():
    # Python 3.13 TemporaryDirectory uses a restrictive Windows ACL that this
    # managed workspace's compiler process cannot open. Normal mkdir inherits
    # the approved system temporary directory permissions.
    base = Path(tempfile.gettempdir()).resolve()
    scratch = base / ('strikers-runtime-tests-' + uuid.uuid4().hex)
    scratch.mkdir()
    try:
        yield scratch
    finally:
        # Also unload after a failed assertion/link, before removing compiler
        # products. Otherwise a locked DLL hides the original test failure.
        while LOADED_DLLS:
            lib = LOADED_DLLS.pop()
            ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(lib._handle))
        if scratch.resolve().parent != base:
            raise RuntimeError('scratch cleanup escaped the temporary directory')
        shutil.rmtree(scratch)


def narrow(q):
    sign = (q >> 127) << 63
    e = (q >> 112) & 32767
    f = q & ((1 << 112) - 1)
    if e == 32767:
        return sign | (2047 << 52) | ((1 << 51) | (f >> 60) if f else 0)
    if not e and not f:
        return sign
    m = f | (1 << 112 if e else 0)
    power = (e or 1) - 16383 - 112
    top = m.bit_length() - 1 + power
    if top > 1023:
        return sign | (2047 << 52)
    quantum = max(top - 52, -1074)
    shift = quantum - power
    if shift > 0:
        r, lost = divmod(m, 1 << shift)
        half = 1 << (shift - 1)
        r += lost > half or lost == half and r & 1
    else:
        r = m << -shift
    if not r:
        return sign
    if quantum == -1074 and r < 1 << 52:
        return sign | r
    if r >= 1 << 53:
        r >>= 1
        quantum += 1
    exponent = quantum + 52 + 1023
    return sign | (2047 << 52) if exponent >= 2047 else sign | (exponent << 52) | (r & ((1 << 52) - 1))


def main():
    env = msvc_environment()
    compiler = str(Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe')
    includes = ['/I' + str(ROOT / p) for p in ('runtime/core', 'runtime/libc', 'runtime/os', 'runtime/services', 'build/gen')]
    result = {}
    with scratch_directory() as temp:
        tmp = Path(temp)

        def dll(name, code, sources=(), libs=()):
            src = tmp / (name + '.c')
            src.write_text(code, encoding='utf-8')
            command = [compiler, '/nologo', '/LD', '/MD', '/O2', '/std:c11', '/W3', '/we4013',
                       '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN', *includes,
                       str(src), *(str(ROOT / p) for p in sources), '/link',
                       '/OUT:' + str(tmp / (name + '.dll')), *libs]
            r = subprocess.run(command, cwd=tmp, env=env, capture_output=True, text=True)
            if r.returncode:
                raise RuntimeError(r.stdout + r.stderr)
            lib = ctypes.CDLL(str(tmp / (name + '.dll')))
            LOADED_DLLS.append(lib)
            return lib

        source = (ROOT / 'runtime/libc/nnlibc2.c').read_text()
        code = source[source.index('void quad_from_double'):source.index('/* ================================================================ the standard streams')]
        code = code.replace('void quad_from_double', '__declspec(dllexport) void quad_from_double')
        code = code.replace('double quad_to_double', '__declspec(dllexport) double quad_to_double')
        quad = dll('quad', '#include "host_cpu.h"\n' + code)
        quad.quad_from_double.argtypes = [ctypes.c_double, ctypes.POINTER(ctypes.c_uint64)]
        quad.quad_to_double.argtypes = [ctypes.POINTER(ctypes.c_uint64)]
        quad.quad_to_double.restype = ctypes.c_double
        rng = random.Random(0x19401051C000)
        cases = [0, 1, (1 << 64)-1, 1 << 127, 32767 << 112]
        cases += [(e << 112) | f for e in (0, 1, 15308, 15309, 15360, 16382, 16383, 17406, 17407, 32766, 32767)
                  for f in (0, 1, (1 << 59)-1, 1 << 59, (1 << 59)+1, (1 << 112)-1)]
        cases += [rng.getrandbits(128) for _ in range(20000)]
        for q in cases:
            words = (ctypes.c_uint64 * 2)(q & ((1 << 64)-1), q >> 64)
            got = struct.unpack('<Q', struct.pack('<d', quad.quad_to_double(words)))[0]
            assert got == narrow(q), f'binary128 narrowing: {q:032x}: {got:016x} != {narrow(q):016x}'
        for _ in range(20000):
            bits = rng.getrandbits(64)
            d = struct.unpack('<d', bits.to_bytes(8, 'little'))[0]
            words = (ctypes.c_uint64 * 2)()
            quad.quad_from_double(d, words)
            got = struct.unpack('<Q', struct.pack('<d', quad.quad_to_double(words)))[0]
            if math.isnan(d):
                assert math.isnan(struct.unpack('<d', got.to_bytes(8, 'little'))[0])
            else:
                assert got == bits
        result['binary128_narrow_vectors'] = len(cases)
        result['binary64_roundtrips'] = 20000

        sdk_source = (ROOT / 'runtime/os/nnsdk.c').read_text()
        errno_source = sdk_source[sdk_source.index('static __declspec(thread) int g_errno;'):
                                  sdk_source.index('/* ---- more libc */')]
        errno_dll = dll('guesterrno', '#include "hostrt.h"\n#define RET(v) (c->x[0]=(uint64_t)(v))\n' + errno_source + r'''
__declspec(dllexport) int errno_tests(void) {
    unsigned char ta[0x200]={0},tb[0x200]={0};Ctx a={0},b={0};
    a.tpidr=(uint64_t)(uintptr_t)ta;b.tpidr=(uint64_t)(uintptr_t)tb;
    c_errno_location(&a);c_errno_location(&b);
    if(a.x[0]!=(uint64_t)(uintptr_t)(ta+0x1d8)||b.x[0]!=(uint64_t)(uintptr_t)(tb+0x1d8)||a.x[0]==b.x[0])return 1;
    wr32(a.x[0],11);wr32(b.x[0],98);c_errno_location(&a);
    return rd32(a.x[0])!=11||rd32(b.x[0])!=98;
}
''')
        assert errno_dll.errno_tests() == 0
        result['guest_errno_cells'] = 'passed: libc errno follows the guest TLS; distinct guest contexts do not share a host TLS cell'
        misc_source = (ROOT / 'runtime/os/nnmisc.c').read_text()
        dialog_source = misc_source[misc_source.index('static void err_dialog('):misc_source.index('static void err_ShowErrorResult(')]
        dialog = dll('errorviewer', r'''
#include <stdlib.h>
#include <stdio.h>
#include <windows.h>
static int boxes;
static int test_box(HWND w,const char *t,const char *c,UINT flags) {(void)w;(void)t;(void)c;(void)flags;boxes++;return IDOK;}
#define MessageBoxA test_box
''' + dialog_source + r'''
__declspec(dllexport) int dialog_tests(void) {
    const char *modes[]={"STRIKERS_SEQ","STRIKERS_PLAY","STRIKERS_PRESS","STRIKERS_SCRIPT","STRIKERS_SDL_TEST_SEQ","STRIKERS_KEYBOARD_TEST_SEQ"};
    for(int i=0;i<6;i++)_putenv_s(modes[i],"");_putenv_s("STRIKERS_ERROR_DIALOG","1");
    err_dialog(2203,1);if(boxes!=1)return 1;
    _putenv_s("STRIKERS_SDL_TEST_SEQ","test.seq");err_dialog(2203,2);if(boxes!=1)return 2;
    _putenv_s("STRIKERS_SDL_TEST_SEQ","");_putenv_s("STRIKERS_KEYBOARD_TEST_SEQ","test.seq");err_dialog(2203,3);if(boxes!=1)return 3;
    _putenv_s("STRIKERS_KEYBOARD_TEST_SEQ","");_putenv_s("STRIKERS_ERROR_DIALOG","");return 0;
}
''')
        assert dialog.dialog_tests() == 0
        result['scripted_error_viewer'] = 'passed: normal viewer waits, SDL/keyboard replays acknowledge without opening a modal box'

        offline = dll('offline', r'''
#include <stdlib.h>
#include <string.h>
#include "network_policy.h"
static ImportDef defs[512]; static int used, traps;
void os_init_event(uint64_t key,int autoclear) { (void)key;(void)autoclear; }
void os_signal_event(uint64_t key) { (void)key; }
void group_ldn_state(int state) { (void)state; }
void group_start(void) {}
void os_pin_host_thread(void) {}
void register_imports(const ImportDef *d,int n) { for(int i=0;i<n;i++) { int k;for(k=0;k<used;k++)if(!strcmp(defs[k].name,d[i].name))break;defs[k]=d[i];if(k==used)used++;} }
void guest_trap(const char *why,uint64_t pc) { (void)why;(void)pc;traps++; }
static void invoke(const char *name,Ctx *c) { for(int i=0;i<used;i++)if(!strcmp(defs[i].name,name)){defs[i].fn(c);return;} traps++; }
#define CALL(n) invoke(n,&c)
#define REQUIRE(p) do { if(!(p)) return __LINE__; } while(0)
__declspec(dllexport) int offline_tests(void) {
    _putenv_s("STRIKERS_NETWORK","offline"); register_network_imports();
    Ctx c={0}; uint64_t out[8]={0};
    c.x[0]=(uint64_t)(uintptr_t)out;CALL("_ZN2nn7account17GetLastOpenedUserEPNS0_3UidE"); REQUIRE(out[0]==1 && out[1]==0 && c.x[0]==0);
    c.x[0]=(uint64_t)(uintptr_t)out;CALL("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE"); REQUIRE(*(unsigned char *)out==0);
    c.x[0]=(uint64_t)(uintptr_t)out;out[0]=99;CALL("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE"); REQUIRE(out[0]==0 && c.x[0]==ACC_NO_INTERNET);
    CALL("_ZN2nn4nifm18IsNetworkAvailableEv"); REQUIRE(c.x[0]==0);
    CALL("_ZN2nn3ldn10InitializeEv"); REQUIRE(c.x[0]==0x2ecb);
    CALL("_ZN2nn3ldn8GetStateEv"); REQUIRE(c.x[0]==0);
    CALL("nnsocketSocket"); REQUIRE(c.x[0]==(uint64_t)-1);
    CALL("nnsocketGetLastErrno"); REQUIRE(c.x[0]==50);
    CALL("nnsslInitialize"); REQUIRE(c.x[0]==0);
    uint64_t ctx=0,conn[5]={0};
    c.x[0]=(uint64_t)(uintptr_t)&ctx;c.x[1]=0;CALL("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE"); REQUIRE(c.x[0]==0 && ctx);
    c.x[0]=(uint64_t)(uintptr_t)conn;c.x[1]=(uint64_t)(uintptr_t)&ctx;CALL("nnsslConnectionCreate"); REQUIRE(c.x[0]==0);
    c.x[0]=(uint64_t)(uintptr_t)conn;c.x[1]=0;c.x[2]=0;CALL("nnsslConnectionDoHandshakeWithCertBuffer"); REQUIRE(c.x[0]==0xce7b);
    c.x[0]=(uint64_t)(uintptr_t)conn;CALL("nnsslConnectionDestroy"); REQUIRE(c.x[0]==0);
    c.x[0]=(uint64_t)(uintptr_t)&ctx;CALL("nnsslContextDestroy"); REQUIRE(c.x[0]==0 && !ctx);
    REQUIRE(traps==0); return 0;
}
''', sources=['runtime/services/' + p for p in ('zt_transport.c', 'net_sim.c', 'network_policy.c', 'nnaccount.c', 'nnfriends.c',
                     'nnldn.c', 'nnnifm.c', 'nnsocket.c', 'nnssl.c')], libs=['ws2_32.lib'])
        assert offline.offline_tests() == 0, 'offline service contract failure (C source line returned)'
        result['offline_services'] = 'passed: local UID, unavailable login/token/network, no sockets, LDN disabled, SSL no-socket failure'

        shim = dll('ehshim', r'''
#include "game_shim_eh.c"
static void resume(Ctx *c,uint64_t lp) { c->x[0]=lp; }
const FnEntry fn_table[]={{0,0}};const int fn_table_count=1;
const DynSym dyn_syms[]={{"",0,0}};const int dyn_sym_count=1;
const NroResume resume_table_main[]={{0x7100001000ull,resume},{0x7100002000ull,resume}};
const int resume_count_main=2;
static int called;
static void catch_call(Ctx *c,void (*body)(Ctx *,uint64_t)) {called++;body(c,0x7100001000ull);}
__declspec(dllexport) int eh_shim_tests(void) {
    const NroResume *tab=0;int count=0;Ctx c={0};GameHost h={0};GameTables t={0};
    game_bind(&h,&t);if(t.fn_table!=fn_table || *t.fn_table_count!=1) return 1;
    game_eh_bind(catch_call,&tab,&count);
    if(tab!=resume_table_main || count!=2) return 2;
    nro_catch_call(&c,resume);if(called!=1 || c.x[0]!=tab[0].lp)return 3;
    return 0;
}
''')
        assert shim.eh_shim_tests() == 0
        result['eh_shim_handshake'] = 'passed; not an end-to-end guest unwind test'
        registry = dll('ehregistry', r'''
#include "eh_runtime.h"
EhResumeFn eh_main_resume_entry(uint64_t lp) { (void)lp;return 0; }
static void resume(Ctx *c,uint64_t lp) {c->x[0]=lp;}
__declspec(dllexport) int registry_tests(void) {
    EhResumeEntry good[]={{0x7500000010ull,resume},{0x7500000020ull,resume}};
    EhResumeEntry bad[]={{0x7500000020ull,resume},{0x7500000010ull,resume}};
    if(eh_register_resume_table(0x7500000000ull,0x100,bad,2))return 1;
    if(!eh_register_resume_table(0x7500000000ull,0x100,good,2))return 2;
    if(eh_resume_entry(good[0].lp)!=resume || eh_resume_entry(good[1].lp)!=resume || eh_resume_entry(good[0].lp+1))return 3;
    eh_unregister_resume_table(0x7500000000ull);if(eh_resume_entry(good[0].lp))return 4;
    return 0;
}
''', sources=['runtime/libc/eh_resume.c'])
        assert registry.registry_tests() == 0
        result['eh_image_registry'] = 'passed: sorted table validation, exact lookup and unregister'
        job06 = dll('job06', (ROOT / 'tools/audit/job06_contracts.c').read_text(),
                    sources=['runtime/os/main_imports.c', 'runtime/services/nnngc.c'], libs=['advapi32.lib'])
        line = job06.job06_os_tests()
        assert line == 0, f'Job 06 host boundary failure at job06_contracts.c:{line}'
        result['job06_host_boundaries'] = 'passed: SDK 13.3 thread states/start argument/TLS/struct bounds, fiber resume/termination/FLS, periodic timers, blocking multiwait, memory layout/commit/free, all five SVCs, crypto entropy, language tags, unavailable profanity filter'
        line = job06.audit04_os_tests()
        assert line == 0, f'Audit 04 scheduling contract failure at job06_contracts.c:{line}'
        result['audit04_scheduling'] = "passed: console-core slot geometry, nn::os 18 mapped with 17 above 19..31, the runtime's own threads leave a slot another game takes and return when it is free, a yield never runs a lower-priority thread"
        # Compile and execute the real generated SDK code, using the same clang
        # CPU helpers as the game DLL (never substitute Windows wchar_t/va_list).
        sdk_object = tmp / 'sdklift.obj'
        command = [str(ROOT / 'deps/llvm/bin/clang.exe'), '-c', '-O1', '-ffp-contract=off',
                   '-fno-strict-aliasing', '-I' + str(ROOT / 'runtime/core'), '-I' + str(ROOT / 'build/gen'),
                   str(ROOT / 'build/gen/sdklift_gen.c'), '-o', str(sdk_object)]
        built = subprocess.run(command, cwd=tmp, env=env, capture_output=True, text=True, timeout=180)
        if built.returncode:
            raise RuntimeError(built.stdout + built.stderr)
        sdk_fixture = '#include "' + (ROOT / 'tools/audit/job06_sdklift_contracts.c').as_posix() + '"\n'
        if '--lan-zlib' in sys.argv:
            sdk_fixture = '#define JOB15_ZLIB\n' + sdk_fixture
        sdk_tests = dll('sdkcontracts', sdk_fixture,
                        sources=['runtime/os/main_imports.c', 'runtime/services/nnngc.c', 'runtime/core/sdklift_rt.c',
                                 'runtime/libc/clib.c', 'runtime/libc/nnlibc2.c'],
                        libs=[str(sdk_object), 'advapi32.lib'])
        sdk_tests.job06_sdk_tests.argtypes = [ctypes.c_char_p]
        try:
            line = sdk_tests.job06_sdk_tests(str(ROOT / 'build/dump/update/elf/sdk.elf').encode())
        except OSError as e:
            stage = ctypes.c_int.in_dll(sdk_tests, 'job06_sdk_stage').value
            raise RuntimeError(f'lifted SDK exception at test stage {stage}: {e}') from e
        assert line == 0, f'Job 06 lifted SDK failure at job06_sdklift_contracts.c:{line}'
        if '--lan-zlib' in sys.argv:
            original = bytes((ctypes.c_ubyte * 4096).in_dll(sdk_tests, 'job15_zip_input'))
            outputs = ((ctypes.c_ubyte * 8192) * 5).in_dll(sdk_tests, 'job15_zip_output')
            sizes = (ctypes.c_uint64 * 5).in_dll(sdk_tests, 'job15_zip_sizes')
            for i, level in enumerate((0, 1, 6, 9, 'Pia workspace wrapper')):
                assert zlib.decompress(bytes(outputs[i][:sizes[i]]), wbits=-15 if i==4 else 15) == original, f'SDK zlib level {level}'
            result['lan_sdk_zlib_strategies'] = 'passed levels 0, 1, 6, 9 and Pia workspace wrapper; original SDK output independently inflated by Python'
        result['job06_real_sdklift'] = 'passed: target data mapping, numeric Unicode counting, UTF-8/UTF-16 surrogate roundtrip, keyboard presets/buffer sizes, quaternion, ARM32 wchar_t/va_list formatting'
    out = ROOT / 'build/runtime-check/contract-tests.json'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
