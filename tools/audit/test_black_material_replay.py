"""Indexed captured ball replay, real shadow/depth, checked against live R11 RGB.

Usage: test_black_material_replay.py CAPTURE_DIRECTORY COLOUR_DRAW_STEM
Requires a job13 bundle including .draw, .indices, depth-aspect copies and the
actual runtime SPIR-V. Captures remain local. This validates the captured draw;
it does not assert that a different historical black frame has been repaired.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import numpy as np
from build_runtime import ROOT


def r11_decode(raw):
    channels=[]
    for shift,bits in ((0,6),(11,6),(22,5)):
        value=(raw>>shift)&((1<<(bits+5))-1)
        exp=(value>>bits).astype('i4');mant=value&((1<<bits)-1)
        frac=mant.astype('f8')/(1<<bits)
        channels.append(np.where(exp==0,np.ldexp(frac,-14),np.ldexp(1+frac,exp-15)))
    return np.stack(channels,axis=-1)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder',type=Path);parser.add_argument('stem')
    parser.add_argument('--vs',type=int,default=90);parser.add_argument('--fs',type=int,default=164)
    parser.add_argument('--out',type=Path,default=ROOT/'build/run/job13-black-replay-test')
    args=parser.parse_args();folder=args.folder.resolve();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    stem=args.stem;draw=struct.unpack('<8I',(folder/(stem+'.draw')).read_bytes())
    assert draw[0]==4 and draw[7] and draw[3] in (1,2)
    indices=np.frombuffer((folder/(stem+'.indices')).read_bytes(),'<u2' if draw[3]==1 else '<u4')
    assert len(indices)==draw[2]
    frame=stem.split('_')[0][1:]
    env={k:v for k,v in os.environ.items() if not k.startswith(('MATERIAL_','MX_'))}
    env.update(MATERIAL_FRAME=frame,MATERIAL_CULL='1',MATERIAL_VERTEX_FETCH='1',MATERIAL_REAL_SHADOW='1',MATERIAL_FS_SERIAL=str(args.fs),MATERIAL_DEPTH_FILE=(folder/(stem+'.depth')).as_posix())
    subprocess.run([sys.executable,str(ROOT/'tools/audit/probe_material_fragment.py'),'--folder',str(folder),'--stem',stem,'--out',str(out),'--runtime-spv','--vs',str(args.vs),'--fs',str(args.fs)],env=env,check=True)
    # The driver changes only SPIR-V descriptor numbers, retains original vertex
    # formats/streams and uses the captured index count, base vertex and instance.
    exe=ROOT/'build/obj/host/job12-probe/material_fragment_probe.exe'
    cases={}
    for name,shadow,depth,d32 in [('all-lit',False,False,False),('real-shadow',True,False,False),('real-shadow-depth',True,True,False),('d32-shadow-depth',True,True,True)]:
        e=env.copy()
        if not shadow:e.pop('MATERIAL_REAL_SHADOW')
        if not depth:e.pop('MATERIAL_DEPTH_FILE')
        if d32:e['MATERIAL_SHADOW_D32']='1'
        target=out/(name+'.rgba32f')
        subprocess.run([str(exe),str(out/'vertex.spv'),str(out/'fragment.spv'),str(folder/(stem+'_vb0.bin')),str(folder),str(target),str(folder/stem)],env=e,check=True)
        cases[name]=np.frombuffer(target.read_bytes(),'<f4').reshape(1080,1920,4)
    a=cases['real-shadow-depth'];covered=a[:,:,3]!=0
    assert covered.any(),'draw is fully occluded; use a capture with visible ball fragments'
    assert np.isfinite(a[covered]).all()
    d32=cases['d32-shadow-depth'];assert np.array_equal(a,d32),'D24/D32 shadow comparison differs'
    live_path=folder/(f'{stem}_vs{args.vs}_fs{args.fs}.r11')
    live=r11_decode(np.frombuffer(live_path.read_bytes(),'<u4').reshape(1080,1920))
    expected=np.maximum(a[covered,:3],0).astype('f8');observed=live[covered]
    # One quantization step at each R11/R10 exponent. Allow the adjacent rounding
    # choice between floating test RT and the original packed attachment.
    ex=np.maximum(np.floor(np.log2(np.maximum(expected,2.0**-14))),-14)
    step=np.exp2(ex-np.array([6,6,5]))
    bad=np.abs(observed-expected)>step*1.01+1e-7
    summary={'stem':stem,'indices':len(indices),'maximum_index':int(indices.max()),
             'coverage':{k:int(np.count_nonzero(v[:,:,3])) for k,v in cases.items()},
             'live_colour_component_mismatches':int(bad.sum()),'d24_d32_shadow':'bit identical'}
    (out/'results.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))
    assert not bad.any(),'captured live colour disagrees with the replay'


if __name__=='__main__':main()
