"""Regression: translate real formerly refused packages and compile Vulkan GLSL.

Uses snapshots under build/run/job08-stages by default, or paths on the command line.
All compiler products and snapshot copies are in the system temp directory.
"""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
from build_runtime import ROOT, msvc_environment


def main():
    env = msvc_environment()
    cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
    tmp = Path(tempfile.gettempdir()) / ('strikers-shaders-' + uuid.uuid4().hex)
    tmp.mkdir()
    dll = None
    try:
        exe = tmp / 'probe.exe'
        subprocess.run([str(cl), '/nologo', '/MD', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS',
                        '/I' + str(ROOT / 'runtime/gfx'), str(ROOT / 'tools/audit/job09_shader_probe.c'),
                        str(ROOT / 'runtime/gfx/mx_translate.c'), '/Fe' + str(exe)],
                       cwd=tmp, env=env, check=True)
        paths = [Path(p).resolve() for p in sys.argv[1:]] or [
            ROOT / f'build/run/job08-stages/s{serial:05}_0.mxd' for serial in (2695, 3119, 3158, 3159, 3160, 3161)]
        copies = []
        for p in paths:
            dst = tmp / p.name
            shutil.copy2(p, dst)
            copies.append(dst)
        subprocess.run([str(exe), *map(str, copies)], cwd=tmp, env=env, check=True)
        dll = ctypes.CDLL(str(ROOT / 'build/bin/strikers_glslang.dll'))
        compile_ = dll.strikers_glsl_to_spirv
        compile_.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(ctypes.POINTER(ctypes.c_uint32)),
                             ctypes.POINTER(ctypes.c_size_t), ctypes.c_char_p, ctypes.c_size_t]
        def check(stage, src):
            words = ctypes.POINTER(ctypes.c_uint32)()
            count, log = ctypes.c_size_t(), ctypes.create_string_buffer(8192)
            ok = compile_(stage, src, ctypes.byref(words), ctypes.byref(count), log, len(log))
            assert ok and words[0] == 0x07230203, log.value.decode()
            validators = sorted(Path('C:/VulkanSDK').glob('*/Bin/spirv-val.exe'))
            validator = shutil.which('spirv-val') or (str(validators[-1]) if validators else None)
            if validator:
                spv = tmp / 'test.spv'
                spv.write_bytes(ctypes.string_at(words, count.value * 4))
                subprocess.run([validator, '--target-env', 'vulkan1.3', str(spv)], check=True)
            dll.strikers_spirv_free(words)
        for p in copies:
            raw = p.read_bytes()
            ctl_len = struct.unpack_from('<I', raw, 12)[0]
            stage = 5 if struct.unpack_from('<I', raw, 20+0x714)[0] == 5 else [-1,0,3,4,2,1][
                (struct.unpack_from('<I', raw, 20+ctl_len+0x30)[0] >> 10) & 15]
            check(stage, Path(str(p) + '.glsl').read_bytes())
            print(p.name, 'SPIR-V PASS')
        check(2, b'#version 460\nlayout(points) in; layout(points,max_vertices=1) out; void main(){gl_Position=gl_in[0].gl_Position;EmitVertex();EndPrimitive();}')
        print('geometry stage SPIR-V PASS')
    finally:
        if dll:
            ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(dll._handle))
        if tmp.resolve().parent != Path(tempfile.gettempdir()).resolve():
            raise RuntimeError('scratch cleanup escaped temp directory')
        shutil.rmtree(tmp)


if __name__ == '__main__':
    main()
