"""Compare the actual DLL exports and main guest tables against a developer DLL.

Loads each DLL in a separate process to avoid cross-image symbol/address effects.
No guest function is executed. The table digest excludes relocated native pointers.
"""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pe import PE


def inspect(path):
    class Fn(C.Structure):
        _fields_ = [('addr', C.c_uint64), ('fn', C.c_void_p)]
    class Sym(C.Structure):
        _fields_ = [('name', C.c_char_p), ('value', C.c_uint64), ('defined', C.c_int)]
    class Tables(C.Structure):
        _fields_ = [('functions', C.POINTER(Fn)), ('count', C.POINTER(C.c_int)),
                    ('symbols', C.POINTER(Sym)), ('symbol_count', C.POINTER(C.c_int))]
    dll = C.CDLL(str(Path(path).resolve()))
    host, tables = (C.c_void_p*6)(), Tables()
    dll.game_bind.argtypes = [C.c_void_p, C.POINTER(Tables)]
    dll.game_bind(C.byref(host), C.byref(tables))
    h = hashlib.sha256()
    for i in range(tables.count[0]):
        h.update(tables.functions[i].addr.to_bytes(8, 'little'))
    for i in range(tables.symbol_count[0]):
        s = tables.symbols[i]
        h.update((s.name or b'')+b'\0'+s.value.to_bytes(8, 'little')+s.defined.to_bytes(4, 'little'))
    return dict(functions=tables.count[0], symbols=tables.symbol_count[0], table_sha256=h.hexdigest(),
                exports=PE(path).exports(), imports=PE(path).imports())


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('player', type=Path)
    p.add_argument('developer', type=Path, nargs='?')
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    if a.developer is None:
        print(json.dumps(inspect(a.player)))
    else:
        records = [json.loads(subprocess.check_output([sys.executable, '-I', __file__, str(x)]))
                   for x in (a.player, a.developer)]
        result = dict(player=records[0], developer=records[1])
        result['equal'] = all(records[0][k] == records[1][k] for k in ('functions','symbols','table_sha256','exports'))
        if a.output:
            a.output.write_text(json.dumps(result, indent=2))
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if result['equal'] else 1)
