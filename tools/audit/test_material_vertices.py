"""Run real depth/colour vertex shaders on the same captured ball inputs.

Arguments: material capture directory, frame_draw stem (e.g. f3300_d399).
Requires a .vattr/.vstream and *_vb*/sb*/s0_c3.bin resource bundle.
"""
import ctypes
import argparse
from pathlib import Path
import re
import struct
import subprocess
import sys
from build_runtime import ROOT, msvc_environment
from material_shaders import build_probe


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder',type=Path);parser.add_argument('stem')
    parser.add_argument('--shaders',default='66,90')
    parser.add_argument('--fixtures',type=Path,default=ROOT/'build/run/job12-shaders')
    parser.add_argument('--out',type=Path,default=ROOT/'build/run/job12-vertex-test')
    args=parser.parse_args()
    folder,stem=args.folder.resolve(),args.stem
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    attr = struct.unpack('<32I', (folder / (stem + '.vattr')).read_bytes())
    streams = struct.unpack('<64I', (folder / (stem + '.vstream')).read_bytes())
    count = (folder / (stem + '_vb0.bin')).stat().st_size // streams[0]
    data = bytearray(count * 128 * 4)
    for a, desc in enumerate(attr):
        fmt, stream, offset = desc & 255, desc >> 8 & 31, desc >> 14
        if not fmt:
            continue
        p = folder / f'{stem}_vb{stream}.bin'
        if not p.exists():
            continue
        raw = p.read_bytes()
        comps, kind = (fmt-1)//12+1, (fmt-1)%12
        sizes = [1,1,1,1,2,2,2,2,2,4,4,4]
        codes = ['B','b','B','b','e','H','h','H','h','f','I','i']
        assert 1 <= fmt <= 48, (a, fmt)
        for i in range(count):
            index = i // streams[2*stream+1] if streams[2*stream+1] else i
            start = index*streams[2*stream]+offset
            if start+comps*sizes[kind] <= len(raw):
                struct.pack_into('<4f', data, i*512+a*16, *([0.,0.,0.,1.]))
                for ch in range(comps):
                    value=struct.unpack_from('<'+codes[kind],raw,start+ch*sizes[kind])[0]
                    if kind in (2,3,7,8,10,11):
                        struct.pack_into('<I',data,i*512+a*16+ch*4,int(value)&0xffffffff)
                    else:
                        if kind in (0,5):value/=255 if kind==0 else 65535
                        if kind in (1,6):value=max(value/(127 if kind==1 else 32767),-1.)
                        struct.pack_into('<f',data,i*512+a*16+ch*4,float(value))
    inp = out / (stem + '.inputs')
    inp.write_bytes(data)
    compiler = ctypes.CDLL(str(ROOT / 'build/bin/strikers_glslang.dll'))
    compile_ = compiler.strikers_glsl_to_spirv
    compile_.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(ctypes.POINTER(ctypes.c_uint32)), ctypes.POINTER(ctypes.c_size_t), ctypes.c_char_p, ctypes.c_size_t]
    spvs = []
    fixtures = args.fixtures.resolve()
    for serial in map(int,args.shaders.split(',')):
        p = out / f's{serial:05}_0.mxd'
        p.write_bytes((fixtures / p.name).read_bytes())
        subprocess.run([str(build_probe()), str(p), '0'], check=True)
        src = Path(str(p) + '.glsl').read_text()
        src = re.sub(r'layout\(push_constant\).*?\};', 'const float mx_point_size = 1.0;', src)
        src = re.sub(r'out gl_PerVertex \{.*?\};', 'vec4 mx_position; float mx_pointsize;', src, flags=re.S)
        src = src.replace('gl_Position','mx_position').replace('gl_PointSize','mx_pointsize')
        src = re.sub(r'layout\(location = (\d+)\) in vec4 in_attr\d+;', lambda m: '#define in_attr'+m[1]+' uintBitsToFloat(uvec4(mx_input[gl_GlobalInvocationID.x * 128u + '+str(4*int(m[1]))+'u],mx_input[gl_GlobalInvocationID.x * 128u + '+str(4*int(m[1])+1)+'u],mx_input[gl_GlobalInvocationID.x * 128u + '+str(4*int(m[1])+2)+'u],mx_input[gl_GlobalInvocationID.x * 128u + '+str(4*int(m[1])+3)+'u]))', src)
        src = re.sub(r'layout\(location = \d+\) out vec4 (\w+);', r'vec4 \1;', src)
        src = src.replace('void main()', 'void mx_vertex()')
        keep = ''.join('for(uint k=0u;k<4u;k++)mx_output[132u*gl_GlobalInvocationID.x+'+str(4+4*int(n))+'u+k]=floatBitsToUint(out_attr'+n+'[k]);' for n in re.findall(r'vec4 out_attr(\d+);',src))
        src += '\nlayout(local_size_x=64) in;\nlayout(std430,set=0,binding=0) readonly buffer Input { uint mx_input[]; };\nlayout(std430,set=0,binding=1) buffer Output { uint mx_output[]; };\nvoid main(){if(gl_GlobalInvocationID.x >= '+str(count)+'u)return; mx_vertex(); for(uint k=0u;k<4u;k++)mx_output[132u*gl_GlobalInvocationID.x+k]=floatBitsToUint(mx_position[k]);'+keep+'}\n'
        # The input block must be declared before mx_vertex uses its macro.
        decl='layout(std430,set=0,binding=0) readonly buffer Input { uint mx_input[]; };'
        src=src.replace(decl,'').replace('void mx_vertex()', decl+'\nvoid mx_vertex()')
        src=src.replace('gl_InstanceIndex','0').replace('gl_BaseInstance','0').replace('gl_BaseVertex','0').replace('gl_VertexIndex','int(gl_GlobalInvocationID.x)')
        Path(str(p)+'.compute.glsl').write_text(src)
        words=ctypes.POINTER(ctypes.c_uint32)(); count_=ctypes.c_size_t();log=ctypes.create_string_buffer(16384)
        assert compile_(5,src.encode(),ctypes.byref(words),ctypes.byref(count_),log,len(log)),log.value.decode()
        spv=Path(str(p)+'.spv');spv.write_bytes(ctypes.string_at(words,count_.value*4));compiler.strikers_spirv_free(words);spvs.append(spv)
    env=msvc_environment();cl=Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe';sdk=sorted(Path('C:/VulkanSDK').glob('*/Bin/spirv-val.exe'))[-1].parents[1];obj=ROOT/'build/obj/host/job12-probe';exe=obj/'material_vertex_test.exe'
    subprocess.run([str(cl),'/nologo','/MD','/O2','/std:c11','/D_CRT_SECURE_NO_WARNINGS','/I'+str(sdk/'Include'),'/I'+str(ROOT/'runtime/gfx'),str(ROOT/'tools/audit/material_vertex_test.c'),'/Fe'+str(exe),'/link',str(sdk/'Lib/vulkan-1.lib')],cwd=obj,env=env,check=True)
    return subprocess.call([str(exe),*map(str,spvs),str(inp),str(folder/(stem+'_s0_c3.bin')),str(folder/(stem+'_sb2.bin')),str(out/stem)])


if __name__=='__main__':
    raise SystemExit(main())
