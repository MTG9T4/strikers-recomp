"""Compare raster depth from real captured ball depth and colour vertex code.

Uses the f7500_d324 resource bundle and vertex test's unpacked attributes.
Attribute storage replaces vertex fetch only; position instructions are intact.
"""
import ctypes
from pathlib import Path
import re
import subprocess
import numpy as np
from build_runtime import ROOT, msvc_environment

def main():
    out=ROOT/'build/run/job12-raster';out.mkdir(exist_ok=True)
    compiler=ctypes.CDLL(str(ROOT/'build/bin/strikers_glslang.dll'));fn=compiler.strikers_glsl_to_spirv
    fn.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.POINTER(ctypes.POINTER(ctypes.c_uint32)),ctypes.POINTER(ctypes.c_size_t),ctypes.c_char_p,ctypes.c_size_t]
    def compile_(stage,src,name):
        p=out/(name+'.spv');p.with_suffix('.glsl').write_text(src)
        words=ctypes.POINTER(ctypes.c_uint32)();count=ctypes.c_size_t();log=ctypes.create_string_buffer(16384)
        assert fn(stage,src.encode(),ctypes.byref(words),ctypes.byref(count),log,len(log)),log.value.decode()
        p.write_bytes(ctypes.string_at(words,count.value*4));compiler.strikers_spirv_free(words);return p
    env=msvc_environment();cl=Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe';sdk=sorted(Path('C:/VulkanSDK').glob('*/Bin/spirv-val.exe'))[-1].parents[1];obj=ROOT/'build/obj/host/job12-probe';exe=obj/'material_raster_test.exe'
    subprocess.run([str(cl),'/nologo','/MD','/O2','/std:c11','/D_CRT_SECURE_NO_WARNINGS','/I'+str(sdk/'Include'),'/I'+str(ROOT/'runtime/gfx'),str(ROOT/'tools/audit/material_raster_test.c'),str(ROOT/'runtime/gfx/tex_decode.c'),'/Fe'+str(exe),'/link',str(sdk/'Lib/vulkan-1.lib')],cwd=obj,env=env,check=True)
    outputs=[]
    for serial in (66,90):
        src=(ROOT/f'build/run/job12-vertex-test/s{serial:05}_0.mxd.glsl').read_text()
        src=src.replace('void main()', 'layout(std430,set=0,binding=0) readonly buffer Input {vec4 data[];};\nvoid main()')
        src=re.sub(r'layout\(location = (\d+)\) in vec4 in_attr\d+;',lambda m:f'#define in_attr{m[1]} data[32*gl_VertexIndex+{m[1]}]',src)
        fs='#version 460\n'
        attrs=re.findall(r'layout\(location = (\d+)\) out vec4 out_attr\d+;',src)
        for a in attrs:fs+=f'layout(location={a})in vec4 out_attr{a};\n'
        fs+='layout(location=0)out vec4 color;void main(){color=vec4(gl_FragCoord.z,0,0,1);'
        for a in attrs:fs+=f'color.g+=dot(out_attr{a},vec4(1));'
        fs+='}'
        vs=compile_(0,src,f'vs{serial}');frag=compile_(1,fs,f'fs{serial}');output=out/f'depth{serial}.bin'
        folder=ROOT/'build/run/job12-resources';stem='f7500_d324'
        subprocess.run([str(exe),str(vs),str(frag),str(ROOT/f'build/run/job12-vertex-test/{stem}.inputs'),str(folder/f'{stem}_s0_c3.bin'),str(folder/f'{stem}_sb2.bin'),str(output)],check=True)
        outputs.append(np.frombuffer(output.read_bytes(),'<u4').reshape(-1,4))
    a,b=outputs;m=(a[:,3]!=0)|(b[:,3]!=0);bad=np.count_nonzero(a[m,0]!=b[m,0]);print('Raster depth:',m.sum(),'covered pixels,',bad,'bitwise depth/coverage mismatches')
    return int(bad!=0)

if __name__=='__main__':raise SystemExit(main())
