# SPDX-License-Identifier: GPL-3.0-or-later
"""Dump-independent assembly vectors for the union of the upstream CPU decoders.

Assembled by the same local clang used to build the host, targeting AArch64.
Expected results always come from Unicorn, never from these generators.
"""


def cases():
    out = []

    def add(family, *asm):
        out.append((family, list(asm)))

    shapes = ['8b', '16b', '4h', '8h', '2s', '4s', '2d']
    for op in ['sqdmulh', 'sqrdmulh']:
        for sh in ['4h', '8h', '2s', '4s']:
            for rd in [0, 1, 2]:
                add('saturating-dsp', f'{op} v{rd}.{sh}, v1.{sh}, v2.{sh}')
                for lane in range(8 if sh[-1] == 'h' else 4):
                    add('saturating-dsp', f'{op} v{rd}.{sh}, v1.{sh}, v2.{sh[-1]}[{lane}]')
        for sh in ['h', 's']:
            add('saturating-dsp', f'{op} {sh}1, {sh}1, {sh}2')
    for op in ['sqabs', 'sqneg']:
        for sh in shapes:
            add('saturating-dsp', f'{op} v1.{sh}, v1.{sh}')
        for sh in 'bhsd':
            add('saturating-dsp', f'{op} {sh}1, {sh}1')
    for op in ['orr', 'bic']:
        for sh, shifts in [('4h', [0, 8]), ('8h', [0, 8]), ('2s', [0, 8, 16, 24]), ('4s', [0, 8, 16, 24])]:
            for shift in shifts:
                add('modified-immediate', f'{op} v1.{sh}, #0x81, lsl #{shift}')
    for op in ['sqshl', 'uqshl', 'sqrshl', 'uqrshl']:
        for sh in shapes:
            for rd in [0, 1, 2]:
                add('saturating-register-shift', f'{op} v{rd}.{sh}, v1.{sh}, v2.{sh}')
        for scalar in 'bhsd':
            add('saturating-register-shift', f'{op} {scalar}1, {scalar}1, {scalar}2')
    for op in ['sqxtn', 'uqxtn', 'sqxtun']:
        for low, high, src in [('8b', '16b', '8h'), ('4h', '8h', '4s'), ('2s', '4s', '2d')]:
            for rd in [0, 1]:
                add('saturating-narrow-no-shift', f'{op} v{rd}.{low}, v1.{src}')
                add('saturating-narrow-no-shift', f'{op}2 v{rd}.{high}, v1.{src}')
                add('saturating-narrow-no-shift', f'{op} {low[-1]}{rd}, {src[-1]}1')
    for op in ['ssubl', 'usubl', 'saddl', 'uaddl']:
        for upper in ['', '2']:
            for dst, low, high in [('8h', '8b', '16b'), ('4s', '4h', '8h'), ('2d', '2s', '4s')]:
                src = high if upper else low
                for rd in [0, 1, 2]:
                    add('add-sub-long', f'{op}{upper} v{rd}.{dst}, v1.{src}, v2.{src}')
    for op in ['fminp', 'fmaxp', 'fminnmp', 'fmaxnmp']:
        for sh in ['2s', '4s', '2d']:
            for rd in [0, 1, 2]:
                add('float-pairwise', f'{op} v{rd}.{sh}, v1.{sh}, v2.{sh}')
        for scalar, src in [('s', '2s'), ('d', '2d')]:
            add('float-pairwise', f'{op} {scalar}1, v1.{src}')
    for op in ['fcvtzs', 'fcvtzu']:
        for sh, bits in [('2s', 32), ('4s', 32), ('2d', 64)]:
            for scale in [1, bits//2, bits-1, bits]:
                for rd in [0, 1]:
                    add('float-fixed-point', f'{op} v{rd}.{sh}, v1.{sh}, #{scale}')
            add('float-fixed-point', f'{op} {sh[-1]}1, {sh[-1]}1, #{bits}')
    for op in ['frintn', 'frintp', 'frintm', 'frintz', 'frinta', 'frintx', 'frinti']:
        for sh in ['2s', '4s', '2d']:
            for rd in [0, 1]:
                add('float-round-integral', f'{op} v{rd}.{sh}, v1.{sh}')
    # Both input aliases matter: helpers must snapshot sources before clearing vd.
    for op in ['sshl', 'ushl', 'smax', 'umax', 'smin', 'umin', 'addp', 'smaxp', 'umaxp', 'sminp', 'uminp']:
        for sh in shapes:
            if sh == '2d' and op not in ('sshl', 'ushl', 'addp'):
                continue
            for rd in [0, 1, 2]:
                add('integer-three-same', f'{op} v{rd}.{sh}, v1.{sh}, v2.{sh}')
        if op in ('sshl', 'ushl'):
            add('integer-three-same', f'{op} d1, d1, d2')
    for op in ['cmgt', 'cmge', 'cmle', 'cmlt']:
        for sh in shapes:
            add('compare-zero', f'{op} v1.{sh}, v1.{sh}, #0')
        add('compare-zero', f'{op} d1, d1, #0')
    for op in ['saddlp', 'uaddlp', 'sadalp', 'uadalp']:
        for src, dst in [('8b', '4h'), ('16b', '8h'), ('4h', '2s'), ('8h', '4s'), ('2s', '1d'), ('4s', '2d')]:
            add('pairwise-long', f'{op} v1.{dst}, v1.{src}')
    for op in ['addv', 'saddlv', 'uaddlv', 'smaxv', 'umaxv', 'sminv', 'uminv']:
        for sh in shapes[:-1]:
            if sh == '2s':
                continue
            letter = sh[-1]
            dst = {'b': 'h', 'h': 's', 's': 'd'}[letter] if op.endswith('addlv') else letter
            add('integer-reduction', f'{op} {dst}1, v1.{sh}')
    add('integer-reduction', 'addp d1, v1.2d')
    for op in ['fmaxv', 'fminv', 'fmaxnmv', 'fminnmv']:
        add('float-reduction', f'{op} s1, v1.4s')
    for op in ['fcvtl', 'fcvtl2']:
        for dst, src in [('4s', '8h' if op.endswith('2') else '4h'), ('2d', '4s' if op.endswith('2') else '2s')]:
            add('float-conversion', f'{op} v1.{dst}, v1.{src}')
    for op in ['fcvtn', 'fcvtn2']:
        for dst, src in [('8h' if op.endswith('2') else '4h', '4s'), ('4s' if op.endswith('2') else '2s', '2d')]:
            add('float-conversion', f'{op} v1.{dst}, v1.{src}')
    for op in ['ssra', 'usra', 'srshr', 'urshr', 'srsra', 'ursra', 'sri', 'sli']:
        for sh in shapes:
            bits = {'b': 8, 'h': 16, 's': 32, 'd': 64}[sh[-1]]
            amounts = [0, bits - 1] if op == 'sli' else [1, bits // 2, bits]
            for shift in amounts:
                add('immediate-shift', f'{op} v1.{sh}, v1.{sh}, #{shift}')
                if sh == '2d':
                    add('immediate-shift', f'{op} d1, d1, #{shift}')
    for op in ['shrn', 'rshrn', 'shrn2', 'rshrn2']:
        for low, high, src, bits in [('8b', '16b', '8h', 8), ('4h', '8h', '4s', 16), ('2s', '4s', '2d', 32)]:
            for shift in [1, bits]:
                dst = high if op.endswith('2') else low
                add('narrow-shift', f'{op} v1.{dst}, v1.{src}, #{shift}')
    for poly in ['', 'c']:
        for size in ['b', 'h', 'w', 'x']:
            src = 'x' if size == 'x' else 'w'
            add('crc32', f'crc32{poly}{size} w1, w1, {src}2')
    for width in ['w', 'x']:
        for load in ['ldxp', 'ldaxp']:
            add('paired-exclusive', f'{load} {width}4, {width}5, [x1]')
            for store in ['stxp', 'stlxp']:
                ld = f'{load} {width}4, {width}5, [x1]'
                st = f'{store} w3, {width}6, {width}7, [x1]'
                add('paired-exclusive', ld, st)                  # succeeds
                add('paired-exclusive', st)                      # no monitor
                add('paired-exclusive', ld, 'clrex', st)          # cleared monitor
                add('paired-exclusive', ld, 'str x8, [x1]', st)   # low value changed
                if width == 'x':
                    add('paired-exclusive', ld, 'str x8, [x1, #8]', st)  # high value changed
                add('paired-exclusive', ld, 'add x1, x1, #16', st)  # wrong address
                add('paired-exclusive', ld, 'add x1, x1, #16', st, 'sub x1, x1, #16', st)
                add('paired-exclusive', ld, st, st)              # monitor consumed
    for op in ['frsqrte', 'frecpe']:
        for args in ['s1, s1', 'd1, d1', 'v1.2s, v1.2s', 'v1.4s, v1.4s', 'v1.2d, v1.2d']:
            add('reciprocal-estimate', f'{op} {args}')
    # Odyssey-only CPU families are retained and tested explicitly as well.
    for op in ['sqadd', 'uqadd', 'sqsub', 'uqsub']:
        for sh in shapes:
            add('saturating-arithmetic', f'{op} v1.{sh}, v1.{sh}, v2.{sh}')
    for op in ['smull', 'umull', 'smlal', 'umlal', 'smlsl', 'umlsl']:
        for upper in ['', '2']:
            for dst, low, high in [('8h', '8b', '16b'), ('4s', '4h', '8h'), ('2d', '2s', '4s')]:
                src = high if upper else low
                add('long-multiply', f'{op}{upper} v1.{dst}, v1.{src}, v2.{src}')
            for dst, src, elem in [('4s', '8h' if upper else '4h', 'h'), ('2d', '4s' if upper else '2s', 's')]:
                add('long-multiply', f'{op}{upper} v1.{dst}, v1.{src}, v2.{elem}[1]')
    for op in ['mul', 'mla', 'mls']:
        for sh in ['4h', '8h', '2s', '4s']:
            add('multiply-element', f'{op} v1.{sh}, v1.{sh}, v2.{sh[-1]}[1]')
            if op == 'mla':
                add('multiply-element', f'{op} v1.{sh}, v1.{sh}, v2.{sh}')
    for op in ['sqshrn', 'sqrshrn', 'uqshrn', 'uqrshrn', 'sqshrun', 'sqrshrun']:
        for low, high, src, bits in [('8b', '16b', '8h', 8), ('4h', '8h', '4s', 16), ('2s', '4s', '2d', 32)]:
            for shift in [1, bits]:
                add('saturating-narrow', f'{op} v1.{low}, v1.{src}, #{shift}')
                add('saturating-narrow', f'{op}2 v1.{high}, v1.{src}, #{shift}')
                scalar = {'b': 'h', 'h': 's', 's': 'd'}[low[-1]]
                add('saturating-narrow', f'{op} {low[-1]}1, {scalar}1, #{shift}')
    for count in [2, 4]:
        for elem, last in [('b', 15), ('h', 7), ('s', 3), ('d', 1)]:
            regs = ', '.join(f'v{i}.{elem}' for i in range(count))
            for op in ['ld', 'st']:
                for lane in [0, last]:
                    for post in ['', f', #{count * (1 << ["b", "h", "s", "d"].index(elem))}', ', x2']:
                        add('lane-memory', f'{op}{count} {{{regs}}}[{lane}], [x1]{post}')
        for sh in shapes:
            regs = ', '.join(f'v{i}.{sh}' for i in range(count))
            for op in ['ld', 'st']:
                add('interleaved-memory', f'{op}{count} {{{regs}}}, [x1]')
    add('cache-maintenance', 'dc civac, x1')
    add('cache-maintenance', 'mrs x0, ctr_el0')
    add('arithmetic-control', 'add x0, x1, #1')
    for cc in ['eq', 'ne', 'ge', 'lt']:
        add('conditional-branch', f'b.{cc} .+8')
    add('undefined-trap', '.word 0xe7ffdefe')
    return out
