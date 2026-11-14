"""Difftest the real ball shader's quad instructions against Maxwell lane rules.

Render 256 isolated, single-pixel triangles. Their uncovered neighbours must
participate as helper invocations in the captured XOR-1/XOR-2 SHFL sequence.
The independent oracle is the gradient of the linear varying (x,y):
(-dy(x), -dy(y), -dx(x), -dx(y)) = (0,-1,-1,0).
"""
import ctypes
from pathlib import Path
import struct
import subprocess
from build_runtime import ROOT, msvc_environment
from material_shaders import build_probe


def main():
    out=ROOT/'build/run/job12-quad-test';out.mkdir(parents=True,exist_ok=True)
    raw=(ROOT/'build/run/job12-shaders/s00164_0.mxd').read_bytes();ctl_size=struct.unpack_from('<I',raw,12)[0];start=20+ctl_size+0x30
    code=raw[start:start+0x50+30*8]+struct.pack('<Q',0xe30000000007000f)
    fragment=out/'ball_quad.bin';fragment.write_bytes(code)
    subprocess.run([str(build_probe()),str(fragment),'1'],check=True)
    vs=b'''#version 460
layout(location=10) out vec4 uv;
void main(){int t=gl_VertexIndex/3;vec2 centre=vec2(t%16,t/16)*4.0+1.5;vec2 offsets[3]=vec2[3](vec2(-.25,-.25),vec2(.5,-.25),vec2(-.25,.5));vec2 p=centre+offsets[gl_VertexIndex%3];uv=vec4(p,0,1);gl_Position=vec4(p/32.0-1.0,0,1);}
'''
    dll=ctypes.CDLL(str(ROOT/'build/bin/strikers_glslang.dll'));compile_=dll.strikers_glsl_to_spirv
    compile_.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.POINTER(ctypes.POINTER(ctypes.c_uint32)),ctypes.POINTER(ctypes.c_size_t),ctypes.c_char_p,ctypes.c_size_t]
    paths=[]
    for stage,src in [(0,vs),(1,Path(str(fragment)+'.glsl').read_bytes())]:
        words=ctypes.POINTER(ctypes.c_uint32)();n=ctypes.c_size_t();log=ctypes.create_string_buffer(16384)
        assert compile_(stage,src,ctypes.byref(words),ctypes.byref(n),log,len(log)),log.value.decode()
        p=out/f'{stage}.spv';p.write_bytes(ctypes.string_at(words,n.value*4));dll.strikers_spirv_free(words);paths.append(p)
    env=msvc_environment();cl=Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe';sdk=sorted(Path('C:/VulkanSDK').glob('*/Bin/spirv-val.exe'))[-1].parents[1]
    for p in paths:subprocess.run([str(sdk/'Bin/spirv-val.exe'),'--target-env','vulkan1.3',str(p)],check=True)
    obj=ROOT/'build/obj/host/job12-probe';exe=obj/'material_quad_test.exe'
    subprocess.run([str(cl),'/nologo','/MD','/O2','/std:c11','/D_CRT_SECURE_NO_WARNINGS','/I'+str(sdk/'Include'),'/I'+str(ROOT/'runtime/gfx'),str(ROOT/'tools/audit/material_quad_test.c'),'/Fe'+str(exe),'/link',str(sdk/'Lib/vulkan-1.lib')],cwd=obj,env=env,check=True)
    return subprocess.call([str(exe),*map(str,paths)])


if __name__=='__main__':raise SystemExit(main())
