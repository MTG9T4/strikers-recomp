"""Sequential, isolated PC graphics acceptance runs. All paths are project-local.

Run with --cases s200,s150,s050,fsr,timing100,nearest,integer. Does not change
desktop modes, use game addresses, or terminate other workers' processes.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from build_runtime import ROOT

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',default='strikers_native.exe')
    parser.add_argument('--cases',default='s200,s150,s050,fsr,timing100')
    parser.add_argument('--wait-pid',type=int)
    parser.add_argument('--prefix',default='job16-v4')
    parser.add_argument('--seconds', type=int, help='override the per-case run duration')
    parser.add_argument('--env', action='append', default=[], help='additional KEY=VALUE audit switch')
    args=parser.parse_args()
    if Path(args.exe).name!=args.exe:parser.error('exe must be a build/bin basename')
    if args.wait_pid:
        kernel=ctypes.WinDLL('kernel32');kernel.OpenProcess.restype=ctypes.c_void_p
        kernel.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_uint32];kernel.CloseHandle.argtypes=[ctypes.c_void_p]
        handle=kernel.OpenProcess(0x100000,False,args.wait_pid)
        if handle:
            print('Waiting for this job\'s previous run',args.wait_pid,flush=True)
            while kernel.WaitForSingleObject(handle,1000)==0x102:pass
            kernel.CloseHandle(handle)
    cases={'s100':(1,'linear',700,True),'s200':(2,'linear',700,True),'s150':(1.5,'linear',240,True),
           's050':(.5,'linear',240,False),'fsr':(.5,'fsr',240,True),
           'timing100':(1,'linear',240,False),'nearest':(.5,'nearest',150,True),'integer':(.5,'integer',150,True),
           'window':(0,'linear',100,True)}
    results=[]
    for case in args.cases.split(','):
        scale,filter_,seconds,validate=cases[case]
        if args.seconds is not None: seconds=args.seconds
        name=args.prefix+'-'+case;out=ROOT/'build/run'/name
        out.mkdir(parents=True,exist_ok=True)
        if (out/'userdata').exists():raise RuntimeError('fresh userdata required: '+str(out))
        scale_text='window' if scale==0 else str(scale)
        (out/'strikers.ini').write_text(f'window=windowed\nwidth=1280\nheight=720\nresolution_scale={scale_text}\nupscale={filter_}\nsharpness=0.2\n')
        env={k:v for k,v in os.environ.items() if not k.startswith('STRIKERS_')}
        env.update(STRIKERS_ROMFS=(ROOT/'build/dump/update/romfs').as_posix(),STRIKERS_USERDATA=(out/'userdata').as_posix(),
                   STRIKERS_CONFIG=(out/'strikers_controllers.ini').as_posix(),STRIKERS_SDL_TEST_SEQ='../../tools/audit/job11_quick_battle.seq',STRIKERS_SDL_TEST_COUNT='1',
                   STRIKERS_KEYBOARD_PLAYER='0',STRIKERS_VOLUME='0',
                   STRIKERS_VK_VALIDATE=str(int(validate)),STRIKERS_VK_GPUTIME='1',STRIKERS_VK_RTLOG='1',
                   STRIKERS_SHOT_DIR=out.as_posix(),STRIKERS_SHOT_RANGES='2700-2700,6000-18000/1000',
                   STRIKERS_VK_SHOT_NATIVE='1',STRIKERS_PC_SHOT='2700,6000,7000,8000,9000,10000,11000,12000,13000,14000,15000,16000,17000,18000',STRIKERS_CLOCKSTAT='1',STRIKERS_SHOW='0')
        for setting in args.env:
            key,value=setting.split('=',1)
            if not key.startswith('STRIKERS_'):parser.error('--env accepts STRIKERS_ switches only')
            env[key]=value
        exe=ROOT/'build/bin'/args.exe
        record=dict(case=case,scale=scale,filter=filter_,validate=validate,exe=args.exe,exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),env={k:v for k,v in env.items() if k.startswith('STRIKERS_')})
        print('Starting',case,seconds,'seconds',flush=True)
        start=time.time()
        with (out/'game.log').open('wb') as log:
            process=subprocess.Popen([str(exe),str(ROOT/'build/dump/update/elf/main.elf'),str(seconds)],cwd=ROOT/'build/bin',env=env,stdout=log,stderr=subprocess.STDOUT)
            record['pid']=process.pid
            (out/'run.json').write_text(json.dumps(record,indent=2)+'\n')
            try:process.wait(timeout=seconds+30)
            except subprocess.TimeoutExpired:process.kill();process.wait();record['killed_for_timeout']=True
        record.update(exit=process.returncode,wall_seconds=time.time()-start)
        text=(out/'game.log').read_text(errors='replace')
        record['validation_messages']=sum(line.startswith('vk-validation:') for line in text.splitlines())
        record['summary']=[line for line in text.splitlines() if line.startswith(('nvn_vk_gpu_play:','nvn_vk: ')) and ('frames presented,' in line or line.startswith('nvn_vk_gpu_play:'))]
        results.append(record);(out/'run.json').write_text(json.dumps(record,indent=2)+'\n')
        print(case,'exit',process.returncode,'validation',record['validation_messages'],record['summary'],flush=True)
        (ROOT/'build/run'/(args.prefix+'-matrix.json')).write_text(json.dumps(results,indent=2)+'\n')
        if process.returncode or record['validation_messages']:return 1
    return 0

if __name__=='__main__':raise SystemExit(main())
