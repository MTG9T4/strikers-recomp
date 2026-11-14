"""Audit 02: which bits of a stage's texture key change its translation.

Since job 13, exec (OP_TEXTURES) sets a unit's tex_sint bit for every colour texture; the translator reads that bit only
on integer units (signedness) and on units the code samples with a depth comparison (colour-comparison lowering).
nvn_vk_pipelines.c therefore compares key[3] under stage_smask = tex_units & (integer units | comparison units). This
test translates real captured stages with the production translator and checks that rule:
  1. every bit outside the comparison units set in sint_tex leaves the GLSL byte for byte as the zero key makes it;
  2. on a comparison unit the bit gives another translation (the colour lowering, no sampler*Shadow on that unit);
  3. on an integer unit the bit gives another translation (signed sampler).

    python -I tools/audit/test_variant_key.py [stage.bin ...]   (names prog_<serial>_<stage>.bin; default: job 13 fixtures)
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_runtime import ROOT
from material_shaders import build_probe


def translate(exe, src, stage, out, int_tex=0, sint_tex=0):
    env = os.environ.copy()
    for k in ('MX_INT_TEX', 'MX_SINT_TEX', 'MX_INT_ATTRS', 'MX_SINT_ATTRS'):
        env.pop(k, None)
    env['MX_INT_TEX'] = hex(int_tex)
    env['MX_SINT_TEX'] = hex(sint_tex)
    dst = out / f'{src.stem}-i{int_tex:x}-s{sint_tex:x}.bin'
    shutil.copy2(src, dst)
    r = subprocess.run([str(exe), str(dst), str(stage)], capture_output=True, text=True, env=env)
    m = re.search(r'textures ([0-9a-f]+)', r.stdout)
    glsl = Path(str(dst) + '.glsl')
    return (glsl.read_text() if r.returncode == 0 and glsl.exists() else None), (int(m[1], 16) if m else 0), r.stdout.strip()


def main():
    out = ROOT / 'build/run/a02-variant-key'
    out.mkdir(parents=True, exist_ok=True)
    paths = [Path(p).resolve() for p in sys.argv[1:]] or sorted({p.name: p for p in
             list((ROOT / 'build/run').glob('job13-final-*-resources/prog_*.bin')) +
             list((ROOT / 'build/run').glob('job13-final-*-resources/*/prog_*.bin'))}.values())
    gear = ROOT / 'build/run/job13-gear-resources/prog_596_1.bin'
    if not sys.argv[1:] and gear.exists():
        paths.append(gear)                         # read only: the Gear colour-comparison stage of job 13 (main checkout)
    exe = build_probe()
    bad = checked = skipped = 0
    for p in paths:
        stage = int(p.stem.rsplit('_', 1)[1])
        zero, tex, line = translate(exe, p, stage, out)
        if zero is None:
            print(f'{p.name}: not translated, skipped ({line})'); skipped += 1; continue
        shadow = 0
        for u in re.findall(r'uniform sampler\w*Shadow mx_t(\d+)_', zero):
            shadow |= 1 << int(u)
        others = 0xffffffff & ~shadow
        same, _, _ = translate(exe, p, stage, out, sint_tex=others)
        ok1 = same == zero
        ok2 = True
        for u in range(32):
            if shadow >> u & 1:
                colour, _, _ = translate(exe, p, stage, out, sint_tex=1 << u)
                ok2 &= colour is not None and colour != zero and f'Shadow mx_t{u}_' not in colour and 'mx_cc' in colour
        ok3 = True
        plain = tex & ~shadow
        if plain:
            u = (plain & -plain).bit_length() - 1
            unsigned_, _, _ = translate(exe, p, stage, out, int_tex=1 << u)
            signed_, _, _ = translate(exe, p, stage, out, int_tex=1 << u, sint_tex=1 << u)
            ok3 = unsigned_ is not None and signed_ is not None and unsigned_ != signed_
        checked += 1
        status = 'PASS' if ok1 and ok2 and ok3 else 'FAIL'
        bad += status == 'FAIL'
        print(f'{p.name}: stage {stage}, sampled units {tex:#x}, comparison units {shadow:#x}: '
              f'other units ignored {ok1}, comparison units kept {ok2}, integer signedness kept {ok3} -> {status}')
    print(f'{checked} stages checked, {bad} failed, {skipped} not translated (skipped)')
    return 1 if bad or not checked else 0


if __name__ == '__main__':
    raise SystemExit(main())
