"""Real captured material: a mode-0 warmed library reused for a mode-1 draw.

Replays the same indexed draw, shadow map and real LEQUAL depth attachment with
native GPL. The stale static convention must reject the draw; dynamic draw-time
mode must equal the correct static pipeline. Also checks dynamic mode 0 against
native static mode 0 without replacing any shader instruction.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import numpy as np
from PIL import Image
from build_runtime import ROOT


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('folder',type=Path);p.add_argument('stem')
    p.add_argument('--vs',type=int,default=90);p.add_argument('--fs',type=int,default=164)
    p.add_argument('--out',type=Path,required=True)
    args=p.parse_args();folder=args.folder.resolve();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    env={k:v for k,v in os.environ.items() if not k.startswith(('MATERIAL_','MX_'))}
    env.update(MATERIAL_FRAME=args.stem.split('_')[0][1:],MATERIAL_REAL_SHADOW='1',MATERIAL_CULL='1',MATERIAL_VERTEX_FETCH='1',MATERIAL_FS_SERIAL=str(args.fs),MATERIAL_GPL='1')
    before=folder/(args.stem+'_before.r11')
    if before.exists():env['MATERIAL_BACKGROUND_FILE']=before.as_posix()
    env['MATERIAL_DEPTH_FILE']=(folder/(args.stem+'.depth')).as_posix()
    subprocess.run([sys.executable,str(ROOT/'tools/audit/probe_material_fragment.py'),'--folder',str(folder),'--stem',args.stem,'--vs',str(args.vs),'--fs',str(args.fs),'--runtime-spv','--out',str(out)],env=env,check=True)
    cases={}
    for name,warm,draw,dynamic,depth in [('native-mode1',1,1,False,True),('stale-mode0',0,1,False,True),('fixed-mode1',0,1,True,True),('native-mode0',0,0,False,False),('fixed-mode0',1,0,True,False)]:
        e=env.copy();e['MATERIAL_CLIP_WARM']=str(warm);e['MATERIAL_CLIP_DRAW']=str(draw)
        if dynamic:e['MATERIAL_CLIP_DYNAMIC']='1'
        if not depth:e.pop('MATERIAL_DEPTH_FILE')
        target=out/(name+'.rgba32f')
        subprocess.run([str(ROOT/'build/obj/host/job12-probe/material_fragment_probe.exe'),str(out/'vertex.spv'),str(out/'fragment.spv'),str(folder/(args.stem+'_vb0.bin')),str(folder),str(target),str(folder/args.stem)],env=e,check=True)
        cases[name]=np.frombuffer(target.read_bytes(),'<f4').reshape(1080,1920,4)
    coverage={k:int(np.count_nonzero(v[:,:,3])) for k,v in cases.items()}
    assert coverage['native-mode1']>0,coverage
    assert coverage['stale-mode0']==0,coverage
    assert np.array_equal(cases['native-mode1'],cases['fixed-mode1'],equal_nan=True),'dynamic mode 1 differs from native'
    assert np.array_equal(cases['native-mode0'],cases['fixed-mode0'],equal_nan=True),'dynamic mode 0 differs from native'
    summary={'stem':args.stem,'vertex':args.vs,'fragment':args.fs,'native_GPL':True,'real_shadow':True,'real_LEQUAL_depth':True,'coverage':coverage,'both_dynamic_modes':'bit identical to native static control','captured_background':before.exists()}
    (out/'results.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))
    mask=cases['native-mode1'][:,:,3]!=0;yy,xx=np.nonzero(mask)
    box=(max(0,int(xx.min())-20),max(0,int(yy.min())-20),min(1920,int(xx.max())+21),min(1080,int(yy.max())+21))
    for name in ('stale-mode0','fixed-mode1'):
        rgb=np.nan_to_num(np.maximum(cases[name][:,:,:3],0),nan=0,posinf=1)
        image=Image.fromarray((np.clip(rgb,0,1)**(1/2.2)*255).astype('u1'))
        image.save(out/(name+'.png'))
        crop=image.crop(box);crop.resize((crop.width*4,crop.height*4),Image.Resampling.NEAREST).save(out/(name+'-crop.png'))


if __name__=='__main__':main()
