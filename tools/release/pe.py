"""Small read-only PE32+ inspector, also used to reject embedded guest payloads."""
from pathlib import Path
import struct


class PE:
    def __init__(self, path, allow_pe32=False):
        self.data = path if isinstance(path,bytes) else Path(path).read_bytes()
        d = self.data
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        if d[:2] != b'MZ' or d[pe:pe+4] != b'PE\0\0':
            raise ValueError('Not a PE image')
        count, optional_size = struct.unpack_from('<H12xH', d, pe+6)
        self.optional = pe+24
        magic=struct.unpack_from('<H', d, self.optional)[0]
        self.is64=magic==0x20b
        if not self.is64 and not (allow_pe32 and magic==0x10b):
            raise ValueError('Only PE32+ x64 images are supported')
        self.sections = []
        start = self.optional + optional_size
        for i in range(count):
            h = start + i*40
            virtual_size, rva, size, offset = struct.unpack_from('<IIII', d, h+8)
            self.sections.append((rva, max(size, virtual_size), offset))

    def offset(self, rva):
        for base, size, file_offset in self.sections:
            if base <= rva < base+size:
                return file_offset+rva-base
        raise ValueError('PE RVA out of bounds')

    def string(self, rva):
        off = self.offset(rva)
        return self.data[off:self.data.index(b'\0', off)].decode('ascii')

    def directory(self, index):
        return struct.unpack_from('<II', self.data, self.optional+(112 if self.is64 else 96)+index*8)

    def exports(self):
        rva, _ = self.directory(0)
        if not rva:
            return []
        off = self.offset(rva)
        count, names = struct.unpack_from('<I4xI', self.data, off+24)
        return sorted(self.string(struct.unpack_from('<I', self.data, self.offset(names)+i*4)[0]) for i in range(count))

    def imports(self):
        rva, _ = self.directory(1)
        if not rva:
            return {}
        off, result = self.offset(rva), {}
        while any(self.data[off:off+20]):
            thunk, _, _, name, iat = struct.unpack_from('<IIIII', self.data, off)
            symbols, pos = [], self.offset(thunk or iat)
            while (value := struct.unpack_from('<Q' if self.is64 else '<I', self.data, pos)[0]):
                symbols.append('#'+str(value & 0xffff) if value >> (63 if self.is64 else 31) else self.string(value+2))
                pos += 8 if self.is64 else 4
            result[self.string(name)] = symbols
            off += 20
        return result
