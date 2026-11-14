"""runtime/gfx/smaa/smaa_src.h from runtime/gfx/smaa/SMAA.hlsl: the shader text as a C array for smaa_present.inc.

  python tools/release/embed_smaa.py           write it
  python tools/release/embed_smaa.py --check   exit 1 when it is not what SMAA.hlsl gives

Checked in, so neither host build (and nothing under tools/lifter, which the in-game updater treats as a reason to rebuild a
player's game) has to generate it.
"""
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
SRC, OUT = ROOT / 'runtime/gfx/smaa/SMAA.hlsl', ROOT / 'runtime/gfx/smaa/smaa_src.h'


def text():
    data = SRC.read_bytes() + b'\0'
    rows = '\n'.join(','.join(str(v) for v in data[i:i + 64]) + ',' for i in range(0, len(data), 64))
    return ('/* Generated from SMAA.hlsl (iryoku/smaa 71c806a, MIT) by tools/release/embed_smaa.py; do not edit. */\n'
            'static const char smaa_src[] = {\n' + rows + '\n};\n')


if __name__ == '__main__':
    if '--check' in sys.argv:
        sys.exit(0 if OUT.exists() and OUT.read_text() == text() else 1)
    OUT.write_text(text(), newline='\n')
    print(OUT, OUT.stat().st_size)
