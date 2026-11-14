"""Differential SDK call-binding regression against ELF weak/strong resolution.

The instruction difftest explicitly excludes calls leaving a function. This
case compares sdklift's call target with the real modules' symbol binding and
visibility instead. Run before/after changes to sdklift's binding rules.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/lifter'))
import sdklift as lift


def visible_functions(module, binding):
    result = {}
    for i, (name, kind, bind, section, value, size) in enumerate(module.symbols()):
        other = module.d[0x1000 + module.dt[6] + i * 24 + 5]
        if section and kind == 2 and bind == binding and other & 3 == 0:
            result[name] = value
    return result


def main():
    game = lift.F.Module(lift.T.elf('main'))
    strong = visible_functions(game, 1)
    weak = visible_functions(lift.MOD, 2)
    overlaps = sorted(strong.keys() & weak.keys())
    assert '_ZdlPv' in overlaps
    bad = []
    for name in overlaps:
        env = lift.Env(lift.Rules(), [])
        actual = env.call(lift.SDK_BASE + weak[name])
        expected = actual == f'call_indirect(c, 0x{0x7100000000 + strong[name]:x}ULL);'
        print(f'{name}: main strong +0x{strong[name]:x}, SDK weak +0x{weak[name]:x}: {actual}: {"PASS" if expected else "FAIL"}')
        if not expected:
            bad.append(name)
    assert not bad, 'SDK weak definitions incorrectly bound locally: ' + ', '.join(bad)


if __name__ == '__main__':
    main()
