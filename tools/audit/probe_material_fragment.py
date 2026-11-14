"""Isolate captured ball shading; optional real shadow and depth via MATERIAL_*.

Requires job12-resources/f7500_d324 bundle, decoded texture levels, and
test_material_vertices.py outputs. Output is diagnostic, not a regression oracle.
"""
import ctypes
import argparse
import os
from pathlib import Path
import re
import subprocess
import struct
from build_runtime import ROOT, msvc_environment
from material_shaders import build_probe

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--folder',type=Path,default=ROOT/'build/run/job12-resources')
    parser.add_argument('--stem',default='f7500_d324')
    parser.add_argument('--out',type=Path,default=ROOT/'build/run/job12-fragment')
    parser.add_argument('--real-vertex',action='store_true')
    parser.add_argument('--depth-output',action='store_true')
    parser.add_argument('--vertex-fetch',action='store_true',help='original Vulkan vertex fetch instead of unpacked storage')
    parser.add_argument('--runtime-spv',action='store_true',help='exact captured runtime SPIR-V, with descriptor bindings renumbered only')
    parser.add_argument('--vs',type=int,default=90)
    parser.add_argument('--fs',type=int,default=164)
    args=parser.parse_args()
    if args.runtime_spv:args.vertex_fetch=True
    folder=args.folder.resolve();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    os.environ['MATERIAL_FRAME']=args.stem.split('_')[0][1:]
    os.environ['MATERIAL_FS_SERIAL']=str(args.fs)
    p=out/'ball.bin';p.write_bytes((folder/f'prog_{args.fs}_1.bin').read_bytes())
    env=os.environ.copy();env['MX_INT_TEX']='2048'
    state=folder/(args.stem+'_stage1.txt')
    if state.exists():
        key=re.search(r'key ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+)',state.read_text())
        if key:
            env['MX_INT_TEX']=str(int(key[3],16));env['MX_SINT_TEX']=str(int(key[4],16))
    subprocess.run([str(build_probe()),str(p),'1'],env=env,check=True)
    fs=Path(str(p)+'.glsl').read_text()
    if args.depth_output:
        fs=fs.replace('frag0 = vec4(F(r0), F(r1), F(r2), F(r3));','frag0 = vec4(gl_FragCoord.z, 0, 0, 1);')
    vs='#version 460\nlayout(std430,set=0,binding=0)readonly buffer Vert{vec4 data[];};\n'
    attrs=re.findall(r'layout\(location = (\d+)\) in vec4 in_attr\d+;',fs)
    for a in attrs:vs+=f'layout(location={a})out vec4 in_attr{a};\n'
    vs+='void main(){gl_Position=data[33*gl_VertexIndex];'
    for a in attrs:vs+=f'in_attr{a}=data[33*gl_VertexIndex+{int(a)+1}];'
    vs+='}\n'
    if args.real_vertex or args.vertex_fetch:
        vp=out/'vertex.bin';vp.write_bytes((folder/f'prog_{args.vs}_0.bin').read_bytes())
        attr=struct.unpack('<32I',(folder/(args.stem+'.vattr')).read_bytes())
        ints=sints=0
        for i,a in enumerate(attr):
            f=a&255;k=(f-1)%12
            if f and k in (2,3,7,8,10,11):
                ints|=1<<i
                if k in (3,8,11):sints|=1<<i
        ve=os.environ.copy();ve['MX_INT_ATTRS']=str(ints);ve['MX_SINT_ATTRS']=str(sints)
        subprocess.run([str(build_probe()),str(vp),'0'],env=ve,check=True)
        vs=Path(str(vp)+'.glsl').read_text()
        vs=vs.replace('binding = 3)', 'binding = 7)').replace('binding = 2)', 'binding = 8)')
        if not args.vertex_fetch:
            vs=re.sub(r'layout\(location = (\d+)\) in vec4 in_attr\d+;',lambda m:f'#define in_attr{m[1]} data[32*gl_VertexIndex+{m[1]}]',vs)
            vs=re.sub(r'layout\(location = (\d+)\) in ([ui])vec4 in_attr\d+;',lambda m:f'#define in_attr{m[1]} '+('floatBitsToUint' if m[2]=='u' else 'floatBitsToInt')+f'(data[32*gl_VertexIndex+{m[1]}])',vs)
            vs=vs.replace('void main()', 'layout(std430,set=0,binding=0) readonly buffer Vert {vec4 data[];};\nvoid main()')
        else:os.environ['MATERIAL_VERTEX_FETCH']='1'
    compiler=ctypes.CDLL(str(ROOT/'build/bin/strikers_glslang.dll'));fn=compiler.strikers_glsl_to_spirv
    fn.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.POINTER(ctypes.POINTER(ctypes.c_uint32)),ctypes.POINTER(ctypes.c_size_t),ctypes.c_char_p,ctypes.c_size_t]
    for stage,src,name in [(0,vs,'vertex'),(1,fs,'fragment')]:
        words=ctypes.POINTER(ctypes.c_uint32)();count=ctypes.c_size_t();log=ctypes.create_string_buffer(16384)
        assert fn(stage,src.encode(),ctypes.byref(words),ctypes.byref(count),log,len(log)),log.value.decode()
        (out/(name+'.spv')).write_bytes(ctypes.string_at(words,count.value*4));compiler.strikers_spirv_free(words)
    if args.runtime_spv:
        for serial,stage,name in [(args.vs,0,'vertex'),(args.fs,1,'fragment')]:
            words=list(struct.unpack('<'+'I'*((folder/f'prog_{serial}_{stage}.spv').stat().st_size//4),(folder/f'prog_{serial}_{stage}.spv').read_bytes()))
            at=5
            while at<len(words):
                n,op=words[at]>>16,words[at]&65535
                if op==71 and words[at+2]==33:
                    b=words[at+3]
                    words[at+3]=({3:7,22:8}.get(b,b) if stage==0 else b-40 if 40<=b<80 else b)
                at+=n
            (out/(name+'.spv')).write_bytes(struct.pack('<'+'I'*len(words),*words))
    env=msvc_environment();cl=Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe';sdk=sorted(Path('C:/VulkanSDK').glob('*/Bin/spirv-val.exe'))[-1].parents[1];obj=ROOT/'build/obj/host/job12-probe';exe=obj/'material_fragment_probe.exe'
    texobj=obj/'tex_decode.obj'
    if not texobj.exists() or texobj.stat().st_mtime<(ROOT/'runtime/gfx/tex_decode.c').stat().st_mtime:
        subprocess.run([str(cl),'/nologo','/MD','/O2','/c',str(ROOT/'runtime/gfx/tex_decode.c')],cwd=obj,env=env,check=True)
    subprocess.run([str(cl),'/nologo','/MD','/O2','/std:c11','/D_CRT_SECURE_NO_WARNINGS','/I'+str(sdk/'Include'),'/I'+str(ROOT/'runtime/gfx'),str(ROOT/'tools/audit/material_fragment_probe.c'),str(texobj),'/Fe'+str(exe),'/link',str(sdk/'Lib/vulkan-1.lib')],cwd=obj,env=env,check=True)
    verts=f'{args.stem}.inputs' if args.real_vertex else f'{args.stem}-1.bin'
    vertfile=folder/(args.stem+'_vb0.bin') if args.vertex_fetch else ROOT/f'build/run/job12-vertex-test/{verts}'
    subprocess.run([str(exe),str(out/'vertex.spv'),str(out/'fragment.spv'),str(vertfile),str(folder),str(out/'ball.rgba32f'),str(Path(os.environ.get('MATERIAL_UNIFORMS',str(folder/args.stem))).resolve())],check=True)

if __name__=='__main__':main()
