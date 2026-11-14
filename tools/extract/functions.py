"""The list of every function of a module, with start/end addresses, the evidence for each boundary, and names
(the module's own dynamic symbols, and for main's nerve states the name RTTI gives).

    python tools/extract/functions.py [--dump build/dump/update] [--out build/functions]

Boundary evidence, per function (column `src`):
    E  .eh_frame FDE (exact start and length, written by the compiler)
    S  dynamic symbol of type FUNC with a size (exact start and length, written by the linker)
    B  target of a BL somewhere in the module
    R  code address stored in data (R_AARCH64_RELATIVE addend into .text)
    T  target of an unconditional B that leaves the function it is in (tail call)
    P  validated PLT / resolver instruction template
    V  defined-symbol relocation or validated raw vtable code pointer
    U  defined FUNC symbol with zero size (entry evidence only, not an exact end)
The list is the union of E and S, plus B/R/T/P/V/U entries outside exact function interiors. The end of a function is the
E or S end when there is one (disagreements reported), else a PLT template end, a terminal before suffix padding,
or the next start. Addresses are module-relative
(main is loaded at 0x7100000000 on the console; add that for absolute addresses).

Names of main's nerve states from RTTI (column `rtti_name`; the game's main has almost no symbols): every class whose
typeinfo derives (through single-inheritance bases) from al::Nerve has a vtable whose first function is its
execute(al::NerveKeeper *) const; that function gets the mangled name the class implies. In the base game's main,
which has symbols, these names equal the symbols wherever both exist.

Writes <out>/<module>_functions.tsv (start, end, size, src, end_from, symbol, rtti_name) and <out>/summary.json. Output
is derived from the dump: it stays under build/ (ignored by git).
"""
import argparse
import bisect
import collections
import csv
import json
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T               # noqa: E402
OFF = 0x1000                     # image offset inside our ELF files (nso.nso_to_elf)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


class Module:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        shoff, = struct.unpack_from('<Q', d, 0x28)
        shnum, shstrndx = struct.unpack_from('<HH', d, 0x3c)
        secs = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * 64) for i in range(shnum)]
        st = secs[shstrndx]
        names = d[st[4]:st[4] + st[5]]
        self.sec = {}
        for s in secs:
            n = names[s[0]:names.index(b'\0', s[0])].decode()
            if n:
                self.sec[n] = (s[3], s[5])           # addr, size
        self.text_end = self.sec['.text'][1]
        # dynamic tags
        dyn, dsz = self.sec['.dynamic']
        self.dt = {}
        for i in range(dsz // 16):
            t, v = struct.unpack_from('<qQ', d, OFF + dyn + i * 16)
            if t == 0:
                break
            self.dt.setdefault(t, v)

    def u32(self, a):
        return struct.unpack_from('<I', self.d, OFF + a)[0]

    def words(self):
        return struct.unpack_from('<%dI' % (self.text_end // 4), self.d, OFF)

    def cstr(self, a):
        e = self.d.index(b'\0', OFF + a)
        return self.d[OFF + a:e].decode('latin-1')

    def symbols(self):
        symtab, strtab = self.dt[6], self.dt[5]
        n = self.sec['.dynsym'][1] // 24
        out = []
        for i in range(n):
            name_off, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', self.d, OFF + symtab + i * 24)
            out.append((self.cstr(strtab + name_off), info & 0xf, info >> 4, shndx, value, size))
        return out

    def relocs(self):
        if 7 not in self.dt:
            return []
        rela, sz = self.dt[7], self.dt[8]
        return [struct.unpack_from('<QQq', self.d, OFF + rela + i * 24) for i in range(sz // 24)]

    def plt_relocs(self):
        return [struct.unpack_from('<QQq', self.d, OFF + self.dt[23] + i * 24)
                for i in range(self.dt.get(2, 0) // 24)]

    def plt_entries(self):
        """Recognise linker stubs by their complete instruction template AND JUMP_SLOT.
        A RELATIVE addend may point to padding before PLT0: it is a GOT sentinel, not code."""
        syms = self.symbols()
        slots = {off: syms[info >> 32][0] for off, info, _ in self.plt_relocs() if info & 0xffffffff == 0x402}
        entries, resolvers = {}, set()
        words = self.words()
        for i in range(len(words)-3):
            w, ld, add, br = words[i:i+4]
            if w & 0x9f00001f != 0x90000010 or ld & 0xffc003ff != 0xf9400211 or br != 0xd61f0220:
                continue
            pc = i*4
            page = (pc & ~0xfff) + (sext(((w >> 5 & 0x7ffff) << 2) | (w >> 29 & 3), 21) << 12)
            slot = page + ((ld >> 10 & 0xfff)*8)
            if add != (0x91000210 | ((slot & 0xfff) << 10)):
                continue
            if slot in slots:
                entries[pc] = slots[slot]
            elif i and words[i-1] == 0xa9bf7bf0:  # stp x16,x30,[sp,#-16]!; PLT0 uses GOT[2]
                resolvers.add(pc-4)
        return entries, resolvers

    # ---- .eh_frame through .eh_frame_hdr (found from MOD0)
    def fdes(self):
        mod = self.u32(4)
        eh_hdr = mod + struct.unpack_from('<i', self.d, OFF + mod + 0x10)[0]
        ver, ptr_enc, cnt_enc, tbl_enc = self.d[OFF + eh_hdr:OFF + eh_hdr + 4]
        if ver != 1 or cnt_enc != 0x03 or tbl_enc != 0x3b:
            raise ValueError('unexpected .eh_frame_hdr encodings %x %x %x %x' % (ver, ptr_enc, cnt_enc, tbl_enc))
        count = self.u32(eh_hdr + 8)
        cie_cache = {}
        out = []
        for i in range(count):
            loc, fde_rel = struct.unpack_from('<ii', self.d, OFF + eh_hdr + 12 + i * 8)
            fde = eh_hdr + fde_rel
            length = self.u32(fde)
            assert length not in (0, 0xffffffff)
            cie = fde + 4 - self.u32(fde + 4)
            enc = cie_cache.get(cie)
            if enc is None:
                enc = cie_cache[cie] = self._cie_fde_encoding(cie)
            p = fde + 8
            begin, p = self._read_enc(p, enc)
            rng, p = self._read_enc(p, enc & 0x0f)
            assert begin == eh_hdr + loc, ('eh_frame_hdr / FDE disagree', hex(begin), hex(eh_hdr + loc))
            out.append((begin, begin + rng))
        return out

    def _uleb(self, a):
        r = s = 0
        while True:
            b = self.d[OFF + a]
            a += 1
            r |= (b & 0x7f) << s
            s += 7
            if not b & 0x80:
                return r, a

    def _cie_fde_encoding(self, cie):
        p = cie + 8                                   # length, CIE id
        version = self.d[OFF + p]
        p += 1
        e = self.d.index(b'\0', OFF + p)
        aug = self.d[OFF + p:e].decode()
        p = e - OFF + 1
        _, p = self._uleb(p)                          # code alignment
        _, p = self._uleb(p)                          # data alignment (sleb; skipping works the same)
        if version == 1:
            p += 1
        else:
            _, p = self._uleb(p)
        if not aug.startswith('z'):
            return 0
        _, p = self._uleb(p)
        for ch in aug[1:]:
            if ch == 'R':
                return self.d[OFF + p]
            if ch == 'P':
                penc = self.d[OFF + p]
                p += 1
                _, p = self._read_enc(p, penc)
            elif ch == 'L':
                p += 1
        return 0

    def _read_enc(self, p, enc):
        f = enc & 0x0f
        if f == 0x0b:
            v, n = struct.unpack_from('<i', self.d, OFF + p)[0], 4
        elif f == 0x03:
            v, n = self.u32(p), 4
        elif f in (0x00, 0x04, 0x0c):
            v, n = struct.unpack_from('<q', self.d, OFF + p)[0], 8
        else:
            raise ValueError('pointer encoding %x' % enc)
        if (enc & 0x70) == 0x10:
            v += p
        return v, p + n


def scan_code_pointers(mod, known, containing):
    """Audit aligned raw pointers outside relocation/unwind tables, including vtables.
    Numeric constants are not automatically function starts. A new raw target needs a
    validated Itanium RTTI vtable slot; all rejected candidates remain in the evidence file."""
    relocs = mod.relocs() + mod.plt_relocs()
    relocated = {o for o, _, _ in relocs}
    syms = mod.symbols()
    effective, typeinfos, absolute = {}, set(), set()
    for off, info, add in relocs:
        kind, idx = info & 0xffffffff, info >> 32
        if kind == 0x403:
            effective[off] = add
        elif kind in (0x101, 0x401) and idx < len(syms):
            name, typ, _, defined, value, _ = syms[idx]
            if name.startswith('_ZTVN10__cxxabiv1') and name.endswith('class_type_infoE'):
                typeinfos.add(off)
            if defined:
                effective[off] = value + add
                if typ == 2 and 0 < value + add < mod.text_end and (value+add) % 4 == 0:
                    absolute.add(value+add)
    skips = [(mod.dt[x], mod.dt[x]+mod.dt[y]) for x, y in ((7, 8), (23, 2)) if x in mod.dt]
    mo = mod.u32(4)
    header_end = mo+0x1c
    eh = [mo + struct.unpack_from('<i', mod.d, OFF+mo+o)[0] for o in (16, 20)]
    skips.append(tuple(eh))
    candidates, raw = [], {}
    for sec, (start, size) in mod.sec.items():
        if not sec.startswith(('.data', '.rodata')):
            continue
        for p in range((start+7)//8*8, start+size-7, 8):
            if any(b <= p < e for b, e in skips):
                continue
            value, = struct.unpack_from('<Q', mod.d, OFF+p)
            raw[p] = value
            if p in relocated or not (0 < value < mod.text_end and value % 4 == 0):
                continue
            candidates.append({'slot': p, 'target': value, 'section': sec})
    # A vtable begins with offset-to-top, a pointer to validated typeinfo, and code slots.
    # Include relocated slots when walking it, so a single unrelocated slot is visible.
    values = dict(raw)
    values.update(effective)
    vslots = set()
    for slot, value in values.items():
        if value not in typeinfos or slot-8 not in raw:
            continue
        offset_top = sext(raw[slot-8], 64)
        if not (-0x1000000 < offset_top <= 0):
            continue
        p = slot+8
        while p in values and 0 < values[p] < mod.text_end and values[p] % 4 == 0:
            vslots.add(p)
            p += 8
    found = set()
    for c in candidates:
        t = c['target']
        if t < header_end:
            status = 'padding-or-header'
        elif t in known:
            status = 'known-start'
        elif containing(t) is not None:
            status = 'interior-label'
        elif mod.u32(t) in (0, 0xd503201f):
            status = 'padding-or-header'
        elif c['slot'] in vslots:
            status = 'new-vtable-target'
            found.add(t)
        else:
            status = 'unconfirmed-numeric-value'
        c.update(status=status, vtable_slot=c['slot'] in vslots)
    return found | absolute, candidates, {'raw_candidates': len(candidates),
        'raw_status': dict(collections.Counter(c['status'] for c in candidates)),
        'validated_vtable_slots': len(vslots), 'raw_new_vtable_targets': len(found),
        'defined_symbol_relocation_targets': len(absolute), 'metadata_excluded': skips}


def analyse(mod):
    words = mod.words()
    text_end = mod.text_end
    exact = {}                                        # start -> {'E': end, 'S': end}
    fdes = mod.fdes()
    for b, e in fdes:
        exact.setdefault(b, {})['E'] = e
    syms = mod.symbols()
    unsized = set()
    names = collections.defaultdict(list)
    for name, typ, bind, shndx, value, size in syms:
        if shndx and value < text_end:
            if typ == 2 and size:
                exact.setdefault(value, {})['S'] = value + size
            elif typ == 2 and value % 4 == 0:
                unsized.add(value)
            if name:
                names[value].append(name)
    bl, rel, tail = set(), set(), set()
    for i, w in enumerate(words):
        if (w & 0xfc000000) == 0x94000000:
            t = i * 4 + sext(w & 0x3ffffff, 26) * 4
            if 0 <= t < text_end:
                bl.add(t)
    for off, info, addend in mod.relocs():
        if (info & 0xffffffff) == 0x403 and 0 <= addend < text_end and addend % 4 == 0:
            rel.add(addend)

    # exact functions first; disagreements between E and S are reported, E wins (it is what the unwinder uses)
    starts = sorted(exact)
    disagree = [(s, v['E'], v['S']) for s, v in exact.items() if 'E' in v and 'S' in v and v['E'] != v['S']]
    ends = {s: v.get('E', v.get('S')) for s, v in exact.items()}

    def containing(a):
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and a < ends[starts[i]]:
            return starts[i]
        return None
    # tail calls: unconditional B whose target is outside the exact function holding the branch
    for i, w in enumerate(words):
        if (w & 0xfc000000) == 0x14000000:
            pc = i * 4
            t = pc + sext(w & 0x3ffffff, 26) * 4
            f = containing(pc)
            if f is not None and 0 <= t < text_end and not (f <= t < ends[f]):
                tail.add(t)
    # Keep authoritative exact extents intact. Calls/pointers to their interiors are
    # secondary entries, not evidence for splitting the FDE/symbol range. In assembly
    # `bl next; .word x` can also read a PC-relative word (SDK UserExceptionHandler).
    secondary = sorted(a for a in (bl | rel | tail | unsized) - set(starts) if containing(a) is not None)
    # the NSO header: word 0 is either `b <entry>` (rtld) or 0, word 1 the MOD0 offset; when word 0 is not a branch the
    # bytes up to the end of the MOD0 record (7 words) are data, and a relocated pointer to the module base (R at 0)
    # is not a function
    header = []
    if (words[0] & 0xfc000000) != 0x14000000:
        hdr_end = words[1] + 0x1c
        header = sorted(a for a in (bl | rel | tail) if a < hdr_end and a not in exact)
    original_starts = (set(starts) | bl | rel | tail) - set(secondary) - set(header)
    pointers, pointer_audit, pointer_stats = scan_code_pointers(mod, original_starts, containing)
    plt, resolvers = mod.plt_entries()
    # RELATIVE GOT sentinels sometimes name the padding just before PLT0. Retain the
    # resolver itself (complete 20-byte template), never turn its padding into code.
    sentinels = {a for a in rel-set(exact)-bl-tail if any(0 < p-a <= 16 and
                 all(mod.u32(x) in (0, 0xd503201f) for x in range(a, p, 4)) for p in resolvers)}
    additions = (pointers | set(plt) | resolvers | unsized) - set(starts)
    hdr_end = words[1]+0x1c if words[0] & 0xfc000000 != 0x14000000 else 0
    additions = {a for a in additions if containing(a) is None and a >= hdr_end}
    allstarts = (original_starts | additions) - sentinels
    order = sorted(allstarts)
    rows = []
    for k, s in enumerate(order):
        src = ''.join(c for c, st in (('E', s in exact and 'E' in exact[s]), ('S', s in exact and 'S' in exact[s]),
                                      ('B', s in bl), ('R', s in rel), ('T', s in tail),
                                      ('P', s in plt or s in resolvers), ('V', s in pointers), ('U', s in unsized)) if st)
        nxt = order[k + 1] if k + 1 < len(order) else text_end
        if s == 0 and (words[0] & 0xfc000000) == 0x14000000:
            end, how = 4, 'nso-entry'             # `b start` followed by the MOD0 header words (data)
        elif s in ends:
            end, how = ends[s], 'exact'
        elif s in plt or s in resolvers:
            end, how = s + (20 if s in resolvers else 16), 'plt-template'
        else:
            end, how = nxt, 'next'
            # Trim only unreachable suffix padding after an unconditional terminator.
            # Never trim real instructions or conditional exits, and never alter E/S.
            trim = end
            while trim > s and mod.u32(trim-4) in (0, 0xd503201f):
                trim -= 4
            if trim > s and trim < end and is_terminal(mod.u32(trim-4)):
                end, how = trim, 'terminal-padding'
        rows.append({'start': s, 'end': end, 'src': src, 'end_from': how, 'names': names.get(s, [])})

    # coverage of .text by exact functions
    covered = 0
    last = 0
    for s in starts:
        e = ends[s]
        if e > last:
            covered += e - max(s, last)
            last = e
    stats = {'text_size': text_end, 'functions': len(rows), 'exact_E': sum(1 for v in exact.values() if 'E' in v),
             'exact_S': sum(1 for v in exact.values() if 'S' in v),
             'exact_both': sum(1 for v in exact.values() if 'E' in v and 'S' in v),
             'E_S_end_disagree': len(disagree), 'E_S_disagree_examples': ['%x: E %x S %x' % x for x in disagree[:10]],
             'only_heuristic': sum(1 for r in rows if not set(r['src']) & set('ES')),
             'secondary_entries': len(secondary),
             'header_refs_dropped': ['%x' % a for a in header],
             'secondary_examples': ['%x in %x' % (a, containing(a)) for a in secondary[:10]],
             'bl_targets': len(bl), 'reloc_targets': len(rel), 'tail_targets': len(tail),
             'text_bytes_in_exact_functions': covered, 'named_by_dynsym': sum(1 for r in rows if r['names']),
             'plt_entries': len(plt), 'plt_resolvers': sorted(resolvers),
             'plt_sentinels_dropped': sorted(sentinels), 'new_starts': sorted(allstarts-original_starts),
             'trimmed_padding_functions': sum(r['end_from'] == 'terminal-padding' for r in rows),
             'unsized_symbol_starts': len(unsized),
             'pointer_scan': pointer_stats}
    mod.analysis_details = {'pointers': pointer_audit, 'secondary': secondary, 'plt': plt,
                            'sentinels': sentinels, 'original_starts': original_starts}
    return rows, stats


def is_terminal(w):
    return (w & 0xfc000000 == 0x14000000 or  # B, not BL
            w & 0xfffffc1f in (0xd61f0000, 0xd65f0000) or  # BR/RET
            w & 0xffe0001f == 0xd4200000)  # BRK


def read_rows(path):
    with open(path, encoding='utf-8') as f:
        return [dict(start=int(r['start'],16), end=int(r['end'],16), src=r['src'],
                     end_from=r['end_from']) for r in csv.DictReader(f, delimiter='\t')]


def review_heuristics(mod, rows, baseline):
    """One evidence record per input heuristic, plus new entries. This is a static
    review, not a claim that arbitrary indirect control flow has been executed."""
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    final = {r['start']: r for r in rows}
    details = mod.analysis_details
    plt = details['plt']
    refs = collections.defaultdict(list)
    for i, w in enumerate(mod.words()):
        if w & 0xfc000000 == 0x94000000:
            refs[i*4 + sext(w & 0x3ffffff,26)*4].append(i*4)
    rrefs = collections.defaultdict(list)
    for off, info, add in mod.relocs():
        if info & 0xffffffff == 0x403:
            rrefs[add].append(off)
    no_return = {'abort', '_Exit', 'exit', '__stack_chk_fail', '_ZSt9terminatev',
                 '__cxa_throw', '__cxa_rethrow', 'nndetailDiagAbortFormatImpl'}
    audit = []
    for row in baseline:
        if set(row['src']) & set('ES'):
            continue
        s, e = row['start'], row['end']
        ins = list(md.disasm(mod.d[OFF+s:OFF+e], s))
        bypc = {i.address: i for i in ins}
        reachable, exits, fallthrough, invalid, pending = set(), [], [], [], [s]
        while pending:
            pc = pending.pop()
            if pc in reachable:
                continue
            if pc == e:
                fallthrough.append(pc)
                continue
            if not s <= pc < e:
                exits.append((pc, 'direct-outside'))
                continue
            reachable.add(pc)
            if pc not in bypc:
                invalid.append(pc)
                continue
            i, w = bypc[pc], mod.u32(pc)
            if w & 0xfc000000 in (0x14000000, 0x94000000):
                target = pc + sext(w & 0x3ffffff,26)*4
                if w & 0xfc000000 == 0x14000000:
                    if s <= target < e:
                        pending.append(target)
                    else:
                        exits.append((pc, 'tail-branch:%x'%target))
                elif plt.get(target) in no_return:
                    exits.append((pc, 'noreturn:'+plt[target]))
                else:
                    pending.append(pc+4)
            elif w & 0xff000010 == 0x54000000 or w & 0x7e000000 == 0x34000000:
                target = pc + sext(w >> 5 & 0x7ffff,19)*4
                pending.append(pc+4)
                if s <= target < e: pending.append(target)
                else: exits.append((pc, 'conditional-tail:%x'%target))
            elif w & 0x7e000000 == 0x36000000:
                target = pc + sext(w >> 5 & 0x3fff,14)*4
                pending.append(pc+4)
                if s <= target < e: pending.append(target)
                else: exits.append((pc, 'conditional-tail:%x'%target))
            elif is_terminal(w):
                exits.append((pc, i.mnemonic+(' '+i.op_str if i.op_str else '')))
            elif i.mnemonic == 'udf':
                invalid.append(pc)
            else:
                pending.append(pc+4)
        new = final.get(s)
        if s in details['sentinels']:
            category, reason = 'data-or-padding', 'RELATIVE GOT sentinel points before the validated PLT0 instruction template'
        elif new is None:
            category, reason = 'interior-label', 'start removed from final function list'
        elif new['end'] != e:
            category = 'end-wrong'
            reason = ('old next-start range merged distinct PLT stubs; split by validated template' if s in plt and e-s > 16 else
                      'old next-start range included unreachable suffix padding; exact terminal/template end retained')
        elif fallthrough:
            category, reason = 'end-wrong', 'reachable fallthrough crosses the proposed end; requires further boundary evidence'
        elif invalid:
            category, reason = 'data-or-padding', 'entry control flow reaches undecodable/padding words'
        else:
            category, reason = 'real-correctly-bounded', 'decoded code with return, tail/indirect exit, loop, or named noreturn call; no reachable end fallthrough'
        audit.append(dict(start=s, old_end=e, new_end=None if new is None else new['end'], src=row['src'],
            category=category, reason=reason, bl_reference_count=len(refs[s]), bl_examples=refs[s][:8],
            relocation_reference_count=len(rrefs[s]), relocation_examples=rrefs[s][:8],
            decoded_words=len(ins), range_words=(e-s)//4, reachable_words=len(reachable),
            exits=exits, fallthrough=fallthrough, invalid=invalid,
            first_instructions=['%x: %s %s' % (i.address,i.mnemonic,i.op_str) for i in ins[:6]],
            last_instructions=['%x: %s %s' % (i.address,i.mnemonic,i.op_str) for i in ins[-6:]],
            plt_symbol=plt.get(s)))
    return audit


def self_test():
    """Dump-independent regression for entry evidence, PLT padding and exact extents."""
    m = Module.__new__(Module)
    image = bytearray(OFF+0x500)
    def put(a, w): struct.pack_into('<I',image,OFF+a,w)
    put(4,0x100)
    put(0x200,0x94000002)  # BL into exact interior (0x208), not a new function
    put(0x204,0x9400000f)  # BL 0x240
    put(0x208,0x9400003a)  # BL 0x2f0 (leaf before resolver)
    put(0x20c,0xd65f03c0)
    put(0x240,0xd65f03c0)
    put(0x244,0xd503201f)
    put(0x260,0xd65f03c0)
    put(0x280,0xd65f03c0)
    put(0x2f0,0xd65f03c0)
    def stub(a, slot):
        for j,w in enumerate((0x90000010,0xf9400211 | ((slot//8)<<10),0x91000210 | (slot<<10),0xd61f0220)):
            put(a+j*4,w)
    put(0x300,0xa9bf7bf0)
    stub(0x304,0x800)
    stub(0x320,0x808)
    stub(0x330,0x810)
    struct.pack_into('<Q',image,OFF+0x400,0x10)  # numeric header hit is not a function
    m.d, m.text_end = bytes(image), 0x340
    m.sec, m.dt = {'.text':(0,0x340),'.data':(0x400,0x20)}, {}
    m.fdes = lambda: [(0x200,0x210)]
    m.symbols = lambda: [('sized',2,1,1,0x260,4),('unsized',2,1,1,0x280,0),('external',2,1,0,0,0)]
    m.relocs = lambda: [(0x418,0x403,0x2fc)]
    m.plt_relocs = lambda: [(0x808, (2<<32)|0x402,0),(0x810,(2<<32)|0x402,0)]
    rows, stats = analyse(m)
    indexed = {r['start']:r for r in rows}
    assert indexed[0x200]['end'] == 0x210 and indexed[0x260]['end'] == 0x264
    assert 0x208 not in indexed and 0x2fc not in indexed and 0x10 not in indexed
    assert indexed[0x240]['end'] == 0x244
    assert indexed[0x280]['src'] == 'U'
    assert indexed[0x300]['end'] == 0x314
    assert indexed[0x320]['end'] == 0x330 and indexed[0x330]['end'] == 0x340
    # A near-match with a wrong ADD immediate must not become a PLT entry.
    put(0x338,0x91000210)
    m.d = bytes(image)
    assert 0x330 not in m.plt_entries()[0]
    print('PASS: exact extents, secondary labels, sentinel removal, suffix padding, unsized symbols, PLT templates, false template rejection, numeric header rejection')


def rtti_nerves(mod, on_end=False):
    """{address: mangled name} of the execute functions of every class derived from al::Nerve, from RTTI. A typeinfo
    is a pointer to a __cxxabiv1 class_type_info vtable (+16, a symbol relocation), the name (a RELATIVE relocation to
    the type's mangled name) and, for single inheritance, the base typeinfo; a vtable is offset-to-top (no relocation),
    the typeinfo, then the virtual functions: slot 0 is al::Nerve's execute, slot 1 its executeOnEnd (run when the
    keeper leaves the state). on_end: also the classes' own executeOnEnd (not the default most classes share)."""
    syms = mod.symbols()
    rel, symrel = {}, {}
    for off, info, add in mod.relocs():
        kind = info & 0xffffffff
        if kind == 0x403:                                         # R_AARCH64_RELATIVE
            rel[off] = add
        elif kind in (0x101, 0x401):                              # R_AARCH64_ABS64, GLOB_DAT
            symrel[off] = syms[info >> 32][0]
    tinfo = {}
    for off, name in symrel.items():
        if name.startswith('_ZTVN10__cxxabiv1') and name.endswith('class_type_infoE') and off + 8 in rel:
            tinfo[off] = (mod.cstr(rel[off + 8]), rel.get(off + 16) if '_si_' in name else None)
    nerve = [t for t, (s, _) in tinfo.items() if s == 'N2al5NerveE']
    if len(nerve) != 1:
        return {}
    nerve = nerve[0]
    refs = collections.defaultdict(list)
    for off, p in rel.items():
        refs[p].append(off)
    out, ends = {}, []
    for t, (s, base) in tinfo.items():
        b, depth = base, 0
        while b is not None and b != nerve and b in tinfo and depth < 32:
            b, depth = tinfo[b][1], depth + 1
        if b != nerve:
            continue
        vts = [o - 8 for o in refs.get(t, []) if o - 8 not in rel and o - 8 not in symrel and o - 8 not in tinfo]
        fn = rel.get(vts[0] + 16) if len(vts) == 1 else None
        if fn is None:
            continue
        inner = s[1:-1] if s.startswith('N') and s.endswith('E') else s
        param = 'PNS_11NerveKeeperE' if inner.startswith('2al') else 'PN2al11NerveKeeperE'   # `al` is substitution S_
        out.setdefault(fn, '_ZNK' + inner + '7executeE' + param)
        if rel.get(vts[0] + 24) is not None:
            ends.append((rel[vts[0] + 24], '_ZNK' + inner + '12executeOnEndE' + param))
    if on_end and ends:
        shared = collections.Counter(a for a, _ in ends).most_common(1)[0][0]    # al::Nerve's own, which most keep
        for a, name in ends:
            if a != shared and a not in out:
                out.setdefault(a, name)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dump', default=T.DUMP)
    ap.add_argument('--out', default=T.build('functions'))
    ap.add_argument('--review-baseline', help='directory of original TSVs to classify against regenerated ranges')
    ap.add_argument('--self-test', action='store_true', help='run synthetic boundary regression without a dump')
    ap.add_argument('--no-review', action='store_true', help='generate identical function boundaries without developer Capstone review reports')
    a = ap.parse_args()
    if a.self_test:
        self_test()
        return
    os.makedirs(a.out, exist_ok=True)
    summary = {}
    elfdir = os.path.join(a.dump, 'elf')
    for fn in sorted(os.listdir(elfdir)):
        name = fn[:-4]
        mod = Module(os.path.join(elfdir, fn))
        rows, stats = analyse(mod)
        baseline = read_rows(os.path.join(a.review_baseline, name+'_functions.tsv')) if a.review_baseline else rows
        audit = [] if a.no_review else review_heuristics(mod, rows, baseline)
        prior_starts = {r['start'] for r in baseline}
        new_audit = [] if a.no_review else review_heuristics(mod, rows, [r for r in rows if r['start'] not in prior_starts])
        stats['capstone_review'] = 'skipped by --no-review' if a.no_review else 'performed'
        stats['review_classification'] = dict(collections.Counter(r['category'] for r in audit))
        stats['new_entry_classification'] = dict(collections.Counter(r['category'] for r in new_audit))
        json.dump(audit, open(os.path.join(a.out, name+'_heuristic_review.json'), 'w'), indent=1)
        json.dump(new_audit, open(os.path.join(a.out, name+'_new_entries_review.json'), 'w'), indent=1)
        with open(os.path.join(a.out, name+'_heuristic_review.tsv'), 'w', newline='', encoding='utf-8') as review:
            writer = csv.writer(review, delimiter='\t')
            writer.writerow(['start','old_end','new_end','src','category','reason','BL_refs','R_refs','first','last'])
            for r in audit:
                writer.writerow(['%x'%r['start'], '%x'%r['old_end'], '' if r['new_end'] is None else '%x'%r['new_end'],
                    r['src'],r['category'],r['reason'],r['bl_reference_count'],r['relocation_reference_count'],
                    '; '.join(r['first_instructions']),'; '.join(r['last_instructions'])])
        json.dump(mod.analysis_details['pointers'], open(os.path.join(a.out, name+'_code_pointers.json'),'w'), indent=1)
        rtti = {}                    # smo-recomp names al::Nerve states from RTTI here; Odyssey-only, off for Strikers
        summary[name] = stats
        with open(os.path.join(a.out, '%s_functions.tsv' % name), 'w', encoding='utf-8', newline='\n') as f:
            f.write('start\tend\tsize\tsrc\tend_from\tsymbol\trtti_name\n')
            for r in rows:
                f.write('%x\t%x\t%d\t%s\t%s\t%s\t%s\n' % (
                    r['start'], r['end'], r['end'] - r['start'], r['src'], r['end_from'], '|'.join(r['names']),
                    rtti.get(r['start'], '')))
        print('%-8s functions %6d  exact E %6d  S %6d  both %6d  E/S disagree %d  heuristic-only %d  secondary entries %d'
              '  .text covered %.2f%%' % (name, stats['functions'], stats['exact_E'], stats['exact_S'],
                                          stats['exact_both'], stats['E_S_end_disagree'], stats['only_heuristic'],
                                          stats['secondary_entries'],
                                          100.0 * stats['text_bytes_in_exact_functions'] / stats['text_size']))
    with open(os.path.join(a.out, 'summary.json'), 'w') as f:
        json.dump(summary, f, indent=1)


if __name__ == '__main__':
    main()
