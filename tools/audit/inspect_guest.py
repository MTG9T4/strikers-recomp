"""Read-only guest-memory inspection of a probe process started by this job.

inspect_guest.py PID ADDRESS [SIZE]
inspect_guest.py PID --vtables FUNCTION
No writes, injection, suspension, or process setting changes.
"""
import ctypes
from ctypes import wintypes as w
import struct
import sys


class Reader:
    def __init__(self, pid):
        self.k = ctypes.WinDLL('kernel32', use_last_error=True)
        self.k.OpenProcess.restype = w.HANDLE
        self.k.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        self.k.CloseHandle.argtypes = [w.HANDLE]
        self.h = self.k.OpenProcess(0x1010, False, pid)
        if not self.h:
            raise ctypes.WinError(ctypes.get_last_error())

    def read(self, addr, size):
        b = ctypes.create_string_buffer(size)
        n = ctypes.c_size_t()
        if not self.k.ReadProcessMemory(self.h, addr, b, size, ctypes.byref(n)):
            raise ctypes.WinError(ctypes.get_last_error())
        return b.raw[:n.value]

    def close(self):
        self.k.CloseHandle(self.h)


def main():
    r = Reader(int(sys.argv[1]))
    try:
        if sys.argv[2] == '--vtables':
            start = 0x7100000000 + 11558912
            data = r.read(start, 0x1556000 - 11558912)  # rodata, relocated data and bss of this main.elf
            needle = struct.pack('<Q', int(sys.argv[3], 0))
            for i in range(0, len(data) - 64, 8):
                if data[i:i+8] == needle:
                    print(hex(start+i), [hex(v) for v in struct.unpack('<12Q', data[i-32:i+64])])
        else:
            addr = int(sys.argv[2], 0)
            data = r.read(addr, int(sys.argv[3], 0) if len(sys.argv) > 3 else 256)
            for i in range(0, len(data), 32):
                b = data[i:i+32]
                print(f'{addr+i:012x}:', ' '.join(b[j:j+8][::-1].hex() for j in range(0,len(b),8)),
                      ''.join(chr(c) if 32 <= c < 127 else '.' for c in b))
    finally:
        r.close()


if __name__ == '__main__':
    main()
