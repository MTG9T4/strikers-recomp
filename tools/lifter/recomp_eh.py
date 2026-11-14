# SPDX-License-Identifier: GPL-3.0-or-later
"""Main EH metadata, adapted from ssbu-recomp/tools/lifter/recomp_eh.py.

No instruction translator or discovery heuristics live here. recomp.py supplies
the image and the TSV extents, and uses the result with its existing Fn class.
All reader addresses are image-relative; generated metadata uses guest VAs.
"""
import struct


class Reader:
    def __init__(self, image, file_off=0x1000):
        self.image, self.file_off = image, file_off

    def unpack(self, fmt, at):
        if at < 0 or self.file_off + at + struct.calcsize(fmt) > len(self.image):
            raise ValueError('EH read outside image at %#x' % at)
        return struct.unpack_from(fmt, self.image, self.file_off + at)[0]

    def byte(self, at):
        return self.unpack('<B', at)

    def leb(self, at, signed=False):
        value = shift = 0
        for _ in range(10):
            b = self.byte(at)
            at += 1
            value |= (b & 127) << shift
            shift += 7
            if not b & 128:
                if signed and b & 64:
                    value -= 1 << shift
                return value, at
        raise ValueError('oversized EH LEB128')

    def enc(self, at, enc, datarel=0):
        """Read a pointer without following indirection (personality not needed)."""
        if enc == 0xff:
            return 0, at
        start, form = at, enc & 15
        formats = {0: '<Q', 2: '<H', 3: '<I', 4: '<Q',
                   10: '<h', 11: '<i', 12: '<q'}
        if form in (1, 9):
            value, at = self.leb(at, form == 9)
        elif form in formats:
            fmt = formats[form]
            value = self.unpack(fmt, at)
            at += struct.calcsize(fmt)
        else:
            raise ValueError('unsupported EH encoding %#x' % enc)
        application = enc & 0x70
        if application not in (0, 0x10, 0x30):
            raise ValueError('unsupported EH pointer base %#x' % enc)
        if value:
            value += start if application == 0x10 else datarel if application == 0x30 else 0
        return value, at


def parse_main_eh(image, base, text_size, env, file_off=0x1000):
    r = Reader(image, file_off)
    mod0 = r.unpack('<I', 4)
    if r.unpack('<I', mod0) != 0x30444f4d:
        raise ValueError('main has no MOD0 header')
    hdr = mod0 + r.unpack('<i', mod0 + 0x10)
    if r.byte(hdr) != 1 or r.byte(hdr + 3) != 0x3b:
        raise ValueError('expected EH header v1 with datarel sdata4 table')
    _, p = r.enc(hdr + 4, r.byte(hdr + 1), hdr)
    count, p = r.enc(p, r.byte(hdr + 2), hdr)
    if count > (len(image) - file_off - p) // 8:
        raise ValueError('EH header count outside image')
    cies, functions, owners, catchers = {}, {}, {}, set()
    clause_functions, term_records = set(), 0
    terminate_cache = {}

    def bl_target(at):
        if not base <= at < base + text_size:
            return None
        w = r.unpack('<I', at - base)
        imm = w & 0x3ffffff
        return at + (imm - (1 << 26) if imm & (1 << 25) else imm) * 4 if w >> 26 == 0x25 else None

    def terminate_pad(pad):
        # Upstream's narrow __clang_call_terminate recognizer. Unrecognized
        # handlers get a wrapper conservatively, rather than losing a catch.
        for i in range(2):
            target = bl_target(pad + i * 4)
            if target is None:
                continue
            if target not in terminate_cache:
                calls = [bl_target(target + j * 4) for j in range(6)]
                names = [env.plt_symbol(t) for t in calls if t is not None]
                terminate_cache[target] = names[:2] == ['__cxa_begin_catch', '_ZSt9terminatev']
            return terminate_cache[target]
        return False

    for i in range(count):
        initial = hdr + r.unpack('<i', p + i * 8)
        fde = hdr + r.unpack('<i', p + i * 8 + 4)
        length = r.unpack('<I', fde)
        if length in (0, 0xffffffff):
            raise ValueError('unsupported FDE length at %#x' % fde)
        cie = fde + 4 - r.unpack('<I', fde + 4)
        if cie not in cies:
            version = r.byte(cie + 8)
            if version not in (1, 3):
                raise ValueError('unsupported CIE version %d' % version)
            q, aug = cie + 9, []
            while r.byte(q):
                aug.append(chr(r.byte(q)))
                q += 1
            q += 1
            _, q = r.leb(q)
            _, q = r.leb(q, True)
            if version == 1:
                q += 1
            else:
                _, q = r.leb(q)
            le, fe = 0xff, 0
            has_z = bool(aug and aug[0] == 'z')
            if has_z:
                size, q = r.leb(q)
                aug_end = q + size
                for ch in aug[1:]:
                    if ch == 'L':
                        le, q = r.byte(q), q + 1
                    elif ch == 'R':
                        fe, q = r.byte(q), q + 1
                    elif ch == 'P':
                        enc, q = r.byte(q), q + 1
                        _, q = r.enc(q, enc, hdr)
                    elif ch != 'S':
                        raise ValueError('unsupported CIE augmentation ' + ch)
                if q > aug_end:
                    raise ValueError('CIE augmentation overrun')
            elif aug:
                raise ValueError('unsupported CIE augmentation')
            cies[cie] = le, fe, has_z
        le, fe, has_z = cies[cie]
        start, q = r.enc(fde + 8, fe, hdr)
        size, q = r.enc(q, fe & 15)
        if start != initial:
            raise ValueError('EH header/FDE start mismatch')
        lsda = 0
        if has_z:
            aug_size, q = r.leb(q)
            if le != 0xff and aug_size:
                if le & 0x80:
                    raise ValueError('indirect LSDA pointer unsupported before relocation')
                lsda, _ = r.enc(q, le, hdr)
        if not lsda:
            continue
        entry, end = base + start, base + start + size
        if start % 4 or not 0 <= start < text_size or end > base + text_size:
            raise ValueError('LSDA function outside text')
        if entry not in env.ends or env.ends[entry] != end:
            raise ValueError('LSDA FDE disagrees with function TSV: %#x..%#x' % (entry, end))
        q = lsda
        lp_enc, q = r.byte(q), q + 1
        lp_start = start
        if lp_enc != 0xff:
            if lp_enc & 0x80:
                raise ValueError('indirect LPStart unsupported before relocation')
            lp_start, q = r.enc(q, lp_enc)
        tt_enc, q = r.byte(q), q + 1
        if tt_enc != 0xff:
            _, q = r.leb(q)
        cs_enc, q = r.byte(q), q + 1
        cs_size, q = r.leb(q)
        cs_end = q + cs_size
        pads = set()
        while q < cs_end:
            _, q = r.enc(q, cs_enc & 15)
            _, q = r.enc(q, cs_enc & 15)
            lp, q = r.enc(q, cs_enc & 15)
            action, q = r.leb(q)
            if not lp:
                continue
            pad = base + lp_start + lp
            if pad % 4 or not entry <= pad < end:
                raise ValueError('landing pad outside its TSV extent: %#x' % pad)
            if pad in owners and owners[pad] != entry:
                raise ValueError('landing pad has multiple owners: %#x' % pad)
            owners[pad] = entry
            pads.add(pad)
            if action:
                a, seen, clause = cs_end + action - 1, set(), False
                while True:
                    if a in seen:
                        raise ValueError('cyclic LSDA action chain')
                    seen.add(a)
                    filt, next_at = r.leb(a, True)
                    nxt, _ = r.leb(next_at, True)
                    clause |= filt != 0
                    if not nxt:
                        break
                    a = next_at + nxt
                if clause:
                    clause_functions.add(entry)
                    if terminate_pad(pad):
                        term_records += 1
                    else:
                        catchers.add(entry)
        if q != cs_end:
            raise ValueError('LSDA call-site table overrun')
        functions[entry] = sorted(pads)
    return {'header': base + hdr, 'fde_count': count, 'functions': functions,
            'pads': sorted(owners.items()), 'catchers': sorted(catchers),
            'clause_functions': sorted(clause_functions), 'terminate_records': term_records}
