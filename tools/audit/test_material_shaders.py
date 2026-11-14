"""Real #3117 translation, SPIR-V validation and Vulkan execution regression.

Pass the captured s03117_0.mxd. Defaults to the local job12 fixture directory.
No game dump is checked into Git. All products stay in this worktree's build/.
"""
import ctypes
from pathlib import Path
import struct
import subprocess
import sys
from build_runtime import ROOT, msvc_environment
from material_shaders import build_probe


def main():
    fixture = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build/run/job12-shaders/s03117_0.mxd'
    dst = ROOT / 'build/run/job12-compute'
    dst.mkdir(parents=True, exist_ok=True)
    captured = dst / fixture.name
    captured.write_bytes(fixture.read_bytes())
    subprocess.run([str(build_probe()), str(captured), '5'], check=True)
    raw = captured.read_bytes()
    ctl_size = struct.unpack_from('<I', raw, 12)[0]
    ctl = raw[20:20+ctl_size]
    csize, coff = struct.unpack_from('<II', ctl, 0x6fc)
    constants = dst / '3117.c1'
    constants.write_bytes(raw[20+ctl_size+coff:20+ctl_size+coff+csize])
    src = Path(str(captured) + '.glsl').read_bytes()
    compiler = ctypes.CDLL(str(ROOT / 'build/bin/strikers_glslang.dll'))
    words = ctypes.POINTER(ctypes.c_uint32)()
    count, log = ctypes.c_size_t(), ctypes.create_string_buffer(16384)
    compile_ = compiler.strikers_glsl_to_spirv
    compile_.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(ctypes.POINTER(ctypes.c_uint32)),
                         ctypes.POINTER(ctypes.c_size_t), ctypes.c_char_p, ctypes.c_size_t]
    assert compile_(5, src, ctypes.byref(words), ctypes.byref(count), log, len(log)), log.value.decode()
    spv = dst / '3117.spv'
    spv.write_bytes(ctypes.string_at(words, count.value * 4))
    compiler.strikers_spirv_free(words)
    sdk = sorted(Path('C:/VulkanSDK').glob('*/Bin/spirv-val.exe'))[-1].parents[1]
    subprocess.run([str(sdk / 'Bin/spirv-val.exe'), '--target-env', 'vulkan1.3', str(spv)], check=True)
    # Keep the prior real-game refusal regressions, using the same compiler and
    # validator. Their snapshots may be supplied alongside #3117.
    for serial in (2695,3119,3158,3159,3160,3161):
        source=fixture.parent/f's{serial:05}_0.mxd'
        if not source.exists():
            continue
        copy=dst/source.name
        copy.write_bytes(source.read_bytes())
        subprocess.run([str(build_probe()),str(copy),'5'],check=True)
        data=copy.read_bytes();ctl_len=struct.unpack_from('<I',data,12)[0]
        stage=5 if struct.unpack_from('<I',data,20+0x714)[0]==5 else [-1,0,3,4,2,1][(struct.unpack_from('<I',data,20+ctl_len+0x30)[0]>>10)&15]
        assert compile_(stage,Path(str(copy)+'.glsl').read_bytes(),ctypes.byref(words),ctypes.byref(count),log,len(log)),log.value.decode()
        other=dst/(source.name+'.spv');other.write_bytes(ctypes.string_at(words,count.value*4));compiler.strikers_spirv_free(words)
        subprocess.run([str(sdk/'Bin/spirv-val.exe'),'--target-env','vulkan1.3',str(other)],check=True)
        print(source.name,'SPIR-V PASS')
    env = msvc_environment()
    cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
    obj = ROOT / 'build/obj/host/job12-probe'
    exe = obj / 'material_compute_test.exe'
    subprocess.run([str(cl), '/nologo', '/MD', '/O2', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS',
                    '/I' + str(sdk / 'Include'), '/I' + str(ROOT / 'runtime/gfx'), str(ROOT / 'tools/audit/material_compute_test.c'),
                    '/Fe' + str(exe), '/link', str(sdk / 'Lib/vulkan-1.lib')], cwd=obj, env=env, check=True)
    subprocess.run([str(exe), str(spv), str(constants), str(captured)], check=True)
    print('real #3117 translation, spirv-val and GPU semantics PASS')


if __name__ == '__main__':
    main()
