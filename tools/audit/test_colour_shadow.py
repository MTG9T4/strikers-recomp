"""Execute a real Gear shader's translated TLD4S against its captured colour map.

Full captured #596 also compiles and passes spirv-val. The executed instruction
is extracted unchanged from its translation; reference/coordinates are supplied
by the test. All nine NVN comparison modes are checked against a CPU texel oracle.
Requires local job13 Gear and indexed ball resource captures (not committed).
"""
import ctypes
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import numpy as np
from PIL import Image
from build_runtime import ROOT, msvc_environment
from material_shaders import build_probe, build_texture_probe


def main():
    gear=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT/'build/run/job13-gear-resources'
    bundle=Path(sys.argv[2]).resolve() if len(sys.argv)>2 else ROOT/'build/run/job13-index-resources'
    stem=sys.argv[3] if len(sys.argv)>3 else 'f2850_d387'
    out=ROOT/'build/run/job13-colour-shadow-test';out.mkdir(exist_ok=True)
    env=os.environ.copy();env['MX_SINT_TEX']='64'
    shader=out/'gear.bin';shader.write_bytes((gear/'prog_596_1.bin').read_bytes())
    subprocess.run([str(build_probe()),str(shader),'1'],env=env,check=True)
    full=Path(str(shader)+'.glsl').read_text()
    assert 'sampler2DShadow mx_t6_' not in full and 'mx_cc4(' in full
    sdk=sorted(Path('C:/VulkanSDK').glob('*/Bin/spirv-val.exe'))[-1].parents[1]
    compiler=ctypes.CDLL(str(ROOT/'build/bin/strikers_glslang.dll'));fn=compiler.strikers_glsl_to_spirv
    fn.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.POINTER(ctypes.POINTER(ctypes.c_uint32)),ctypes.POINTER(ctypes.c_size_t),ctypes.c_char_p,ctypes.c_size_t]
    def compile_(src,path):
        p=ctypes.POINTER(ctypes.c_uint32)();n=ctypes.c_size_t();log=ctypes.create_string_buffer(16384)
        assert fn(1,src.encode(),ctypes.byref(p),ctypes.byref(n),log,len(log)),log.value.decode()
        path.write_bytes(ctypes.string_at(p,n.value*4));compiler.strikers_spirv_free(p)
        subprocess.run([str(sdk/'Bin/spirv-val.exe'),'--target-env','vulkan1.3',str(path)],check=True)
    compile_(full,out/'full-gear.spv')
    stmt=next(line for line in full.splitlines() if line.startswith('{ vec4 tv = mx_cc4(') and 'packHalf2x16' in line)
    ref=int(re.search(r'mx_cc4\(F\(r(\d+)\)',stmt)[1])
    xy=[int(v) for v in re.search(r'vec2\(F\(r(\d+)\), F\(r(\d+)\)\)',stmt).groups()]
    dest=[int(v) for v in re.findall(r'r(\d+) = packHalf2x16',stmt)]
    helpers='\n'.join(line for line in full.splitlines() if line.startswith(('float mx_cc(', 'vec4 mx_cc4(')))
    source='#version 460\n#define F uintBitsToFloat\nlayout(location=0)out vec4 frag0;\nlayout(std430,set=0,binding=2)readonly buffer CB2{uint c2[];};\nlayout(set=1,binding=6)uniform sampler2D mx_t6_sampler2D;\n'+helpers+'\nvoid main(){\n'
    source+='uint '+','.join(f'r{k}=0u' for k in range(256))+';\n'
    source+=f'r{xy[0]}=floatBitsToUint((float(int(gl_FragCoord.x)%32)+0.25)/32.0);r{xy[1]}=floatBitsToUint((float(int(gl_FragCoord.y)%32)+0.25)/32.0);\n'
    source+=f'float refs[7]=float[7](-0.1,0,0.25,0.5,0.75,1,1.1);r{ref}=floatBitsToUint(refs[int(gl_FragCoord.x)%7]);\n'+stmt+f'\nfrag0=vec4(unpackHalf2x16(r{dest[0]}),unpackHalf2x16(r{dest[1]}));}}\n'
    (out/'instruction.glsl').write_text(source);compile_(source,out/'instruction.spv')
    # Build the shared replay harness and preserve its original mesh coverage.
    runenv=os.environ.copy()
    for k in ('MATERIAL_DEPTH_FILE','MATERIAL_DEPTH_ALWAYS','MATERIAL_DARK_SHADOW','MATERIAL_REAL_SHADOW','MX_SINT_TEX'):
        runenv.pop(k,None)
    subprocess.run([sys.executable,str(ROOT/'tools/audit/probe_material_fragment.py'),'--folder',str(bundle),'--stem',stem,'--out',str(out),'--runtime-spv'],env=runenv,check=True)
    original=np.frombuffer((out/'ball.rgba32f').read_bytes(),'<f4').reshape(1080,1920,4)
    covered=original[:,:,3]!=0
    assert covered.any()
    prefix=gear/'shadow-mismatch-596-s1-t6'
    subprocess.run([str(build_texture_probe()),str(prefix)],check=True)
    rgba=np.asarray(Image.open(str(prefix)+'_l0.png'))
    assert rgba.shape==(32,32,4)
    values=rgba[:,:,0].astype('f4')/255
    values=np.where(values<=0.04045,values/12.92,((values+0.055)/1.055)**2.4)
    yy,xx=np.nonzero(covered);x0=np.maximum(xx%32-1,0);x1=xx%32;y0=np.maximum(yy%32-1,0);y1=yy%32
    texels=np.stack([values[y1,x0],values[y1,x1],values[y0,x1],values[y0,x0]],axis=1)
    refs=np.array([-.1,0,.25,.5,.75,1,1.1],dtype='f4')[xx%7,None]
    expected=[texels,np.zeros_like(texels),refs<texels,refs==texels,refs<=texels,refs>texels,refs!=texels,refs>=texels,np.ones_like(texels)]
    runenv.update(MATERIAL_FRAME=stem.split('_')[0][1:],MATERIAL_VERTEX_FETCH='1',MATERIAL_CULL='1',MATERIAL_OVERRIDE_T6=prefix.as_posix(),MATERIAL_COLOUR_SHADOW_MASK='64',MATERIAL_C2_FILE=(out/'c2.bin').as_posix())
    records=[]
    for mode in range(9):
        c2=bytearray(1152);struct.pack_into('<I',c2,262*4,mode);(out/'c2.bin').write_bytes(c2)
        pixels=out/f'mode{mode}.rgba32f'
        subprocess.run([str(ROOT/'build/obj/host/job12-probe/material_fragment_probe.exe'),str(out/'vertex.spv'),str(out/'instruction.spv'),str(bundle/(stem+'_vb0.bin')),str(bundle),str(pixels),str(bundle/stem)],env=runenv,check=True)
        actual=np.frombuffer(pixels.read_bytes(),'<f4').reshape(1080,1920,4)[covered]
        want=expected[mode].astype('f2').astype('f4');bad=int(np.count_nonzero(actual!=want))
        records.append(dict(mode=mode,fragments=int(covered.sum()),mismatches=bad))
        assert bad==0,records[-1]
    (out/'results.json').write_text(json.dumps(records,indent=2)+'\n')
    print('real Gear TLD4S, captured checker, all nine compare modes: PASS')


if __name__=='__main__':main()
