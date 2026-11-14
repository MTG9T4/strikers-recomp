"""Sampling profiler for a running strikers_native process (audit 01). No install, no system change.

    python -I tools/audit/sample_profile.py <pid> <tid>[,<tid>...] <seconds> [--hz 500] [--depth 32] [--out file.txt]

Every 1/hz seconds each named thread is suspended, its stack walked (dbghelp StackWalk64 with the
modules' unwind tables) and resumed. At the end: self (leaf) and inclusive sample counts per function
(dbghelp names: the host exe's PDB, exports elsewhere, else module+offset). The thread ids are the
bracketed numbers of the runtime's "nvn_perf: cpu ms/frame by thread" lines.
"""
import argparse
import bisect
import re
import collections
import ctypes
import ctypes.wintypes as W
import sys
import time

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
dbg = ctypes.WinDLL('dbghelp', use_last_error=True)
PROCESS_ALL_ACCESS = 0x1FFFFF
THREAD_ACCESS = 0x0002 | 0x0008 | 0x0040 | 0x0010   # suspend/resume, get context, query information
CONTEXT_FULL = 0x10000B


class Ctx(ctypes.Structure):
    _fields_ = [('raw', ctypes.c_ubyte * 1232)]


class SymInfo(ctypes.Structure):
    _fields_ = [('SizeOfStruct', ctypes.c_ulong), ('TypeIndex', ctypes.c_ulong), ('Reserved', ctypes.c_ulonglong * 2),
                ('Index', ctypes.c_ulong), ('Size', ctypes.c_ulong), ('ModBase', ctypes.c_ulonglong), ('Flags', ctypes.c_ulong),
                ('Value', ctypes.c_ulonglong), ('Address', ctypes.c_ulonglong), ('Register', ctypes.c_ulong),
                ('Scope', ctypes.c_ulong), ('Tag', ctypes.c_ulong), ('NameLen', ctypes.c_ulong), ('MaxNameLen', ctypes.c_ulong),
                ('Name', ctypes.c_char * 512)]


class ModInfo(ctypes.Structure):          # IMAGEHLP_MODULE64 prefix we need; the struct is sized generously
    _fields_ = [('SizeOfStruct', ctypes.c_ulong), ('BaseOfImage', ctypes.c_ulonglong), ('ImageSize', ctypes.c_ulong),
                ('TimeDateStamp', ctypes.c_ulong), ('CheckSum', ctypes.c_ulong), ('NumSyms', ctypes.c_ulong),
                ('SymType', ctypes.c_ulong), ('ModuleName', ctypes.c_char * 32), ('ImageName', ctypes.c_char * 256),
                ('LoadedImageName', ctypes.c_char * 256), ('rest', ctypes.c_ubyte * 4096)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pid', type=int)
    ap.add_argument('tids')
    ap.add_argument('seconds', type=float)
    ap.add_argument('--hz', type=float, default=500)
    ap.add_argument('--depth', type=int, default=32)
    ap.add_argument('--top', type=int, default=60)
    ap.add_argument('--out')
    ap.add_argument('--map', action='append', default=[],
                    help='module=linker map (audit 04): name the PCs of that module from the map, e.g. the lifted functions of '
                         'strikers_game.dll, whose only exports are game_bind/game_eh_bind (relink it with -Wl,/MAP: same layout)')
    a = ap.parse_args()
    hp = k32.OpenProcess(PROCESS_ALL_ACCESS, False, a.pid)
    if not hp:
        raise SystemExit('OpenProcess failed %d' % ctypes.get_last_error())
    dbg.SymSetOptions(0x2 | 0x4 | 0x10)       # undecorated names, deferred loads, load lines off
    dbg.SymInitialize.argtypes = [W.HANDLE, ctypes.c_char_p, W.BOOL]
    if not dbg.SymInitialize(hp, None, True):
        raise SystemExit('SymInitialize failed %d' % ctypes.get_last_error())
    dbg.SymRefreshModuleList(hp)
    fta = ctypes.cast(dbg.SymFunctionTableAccess64, ctypes.c_void_p).value
    gmb = ctypes.cast(dbg.SymGetModuleBase64, ctypes.c_void_p).value
    dbg.StackWalk64.argtypes = [W.DWORD, W.HANDLE, W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    k32.OpenThread.restype = W.HANDLE
    k32.SuspendThread.argtypes = [W.HANDLE]
    k32.ResumeThread.argtypes = [W.HANDLE]
    k32.GetThreadContext.argtypes = [W.HANDLE, ctypes.c_void_p]
    threads = {}
    for t in a.tids.split(','):
        h = k32.OpenThread(THREAD_ACCESS, False, int(t))
        if not h:
            raise SystemExit('OpenThread %s failed %d' % (t, ctypes.get_last_error()))
        threads[int(t)] = h
    ctx_buf = ctypes.create_string_buffer(1232 + 16)
    ctx_addr = (ctypes.addressof(ctx_buf) + 15) & ~15
    frame = ctypes.create_string_buffer(1024)
    stacks = {t: collections.Counter() for t in threads}
    n = {t: 0 for t in threads}
    period = 1.0 / a.hz
    t_end = time.perf_counter() + a.seconds
    nxt = time.perf_counter()
    while time.perf_counter() < t_end:
        for tid, h in threads.items():
            if k32.SuspendThread(h) == 0xFFFFFFFF:
                continue
            try:
                ctypes.memset(ctx_addr, 0, 1232)
                ctypes.c_uint32.from_address(ctx_addr + 0x30).value = CONTEXT_FULL
                if not k32.GetThreadContext(h, ctx_addr):
                    continue
                rip = ctypes.c_uint64.from_address(ctx_addr + 0xF8).value
                rsp = ctypes.c_uint64.from_address(ctx_addr + 0x98).value
                rbp = ctypes.c_uint64.from_address(ctx_addr + 0xA0).value
                ctypes.memset(frame, 0, 1024)
                for off, val in ((0, rip), (32, rbp), (48, rsp)):
                    ctypes.c_uint64.from_buffer(frame, off).value = val
                    ctypes.c_uint32.from_buffer(frame, off + 12).value = 3      # AddrModeFlat
                pcs = []
                while len(pcs) < a.depth and dbg.StackWalk64(0x8664, hp, h, frame, ctx_addr, None, fta, gmb, None):
                    pc = ctypes.c_uint64.from_buffer(frame, 0).value
                    if not pc:
                        break
                    pcs.append(pc)
            finally:
                k32.ResumeThread(h)
            stacks[tid][tuple(pcs)] += 1
            n[tid] += 1
        nxt += period
        d = nxt - time.perf_counter()
        if d > 0:
            time.sleep(d)
        else:
            nxt = time.perf_counter()
    names = {}
    si = SymInfo()
    mi = ModInfo()
    disp = ctypes.c_ulonglong()
    dbg.SymFromAddr.argtypes = [W.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(ctypes.c_ulonglong), ctypes.c_void_p]
    dbg.SymGetModuleInfo64.argtypes = [W.HANDLE, ctypes.c_ulonglong, ctypes.c_void_p]

    maps = {}
    for spec in a.map:
        mod, path = spec.split('=', 1)
        data = open(path, errors='replace').read()
        base = int(re.search(r'Preferred load address is ([0-9a-fA-F]+)', data)[1], 16)
        syms = sorted((int(m[2], 16) - base, m[1]) for m in re.finditer(r'^\s+[0-9a-fA-F]{4}:[0-9a-fA-F]+\s+(\S+)\s+([0-9a-fA-F]{16})\s', data, re.M))
        maps[mod.lower()] = ([x[0] for x in syms], [x[1] for x in syms])

    def name(pc):
        if pc in names:
            return names[pc]
        si.SizeOfStruct = 88
        si.MaxNameLen = 500
        mi.SizeOfStruct = ctypes.sizeof(ModInfo) - 4096 + 4096
        mod = '?'
        mi.SizeOfStruct = 0x690     # IMAGEHLP_MODULE64 (v3) size
        if dbg.SymGetModuleInfo64(hp, pc, ctypes.byref(mi)):
            mod = mi.ModuleName.decode(errors='replace')
        if mod.lower() in maps and mod != '?':
            rvas, syms = maps[mod.lower()]
            i = bisect.bisect_right(rvas, pc - mi.BaseOfImage) - 1
            r = '%s!%s' % (mod, syms[i] if i >= 0 else '+%x' % (pc - mi.BaseOfImage))
        elif dbg.SymFromAddr(hp, pc, ctypes.byref(disp), ctypes.byref(si)):
            r = '%s!%s' % (mod, si.Name.decode(errors='replace'))
        else:
            r = '%s!+%x' % (mod, pc - mi.BaseOfImage if mod != '?' else pc)
        names[pc] = r
        return r
    out = open(a.out, 'w') if a.out else sys.stdout
    for tid in threads:
        total = n[tid]
        self_c, incl, modc = collections.Counter(), collections.Counter(), collections.Counter()
        for st, c in stacks[tid].items():
            if not st:
                continue
            nm = [name(pc) for pc in st]
            self_c[nm[0]] += c
            modc[nm[0].split('!')[0]] += c
            for x in set(nm):
                incl[x] += c
        print('thread %d: %d samples (%.0f Hz asked)' % (tid, total, a.hz), file=out)
        print('  by module (self):', ', '.join('%s %.1f%%' % (m, 100.0 * c / total) for m, c in modc.most_common(12)), file=out)
        print('  self:', file=out)
        for x, c in self_c.most_common(a.top):
            print('    %5.1f%%  %s' % (100.0 * c / total, x), file=out)
        print('  inclusive:', file=out)
        for x, c in incl.most_common(a.top):
            print('    %5.1f%%  %s' % (100.0 * c / total, x), file=out)
    if a.out:
        out.close()


if __name__ == '__main__':
    main()
