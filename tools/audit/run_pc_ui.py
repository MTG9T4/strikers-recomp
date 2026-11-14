"""Exercise only this probe's native game window; preserve screenshots and settings.

No desktop settings, guest addresses, or input to other applications. PNGs are
lossless copies of the renderer's opt-in final swapchain captures.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from PIL import Image
from build_runtime import ROOT


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--scale',type=float,default=.5)
    args=ap.parse_args()
    out=args.out.resolve();out.relative_to(ROOT/'build/run')
    out.mkdir(exist_ok=False)
    (out/'strikers.ini').write_text(f'window=windowed\nwidth=1280\nheight=1024\nresolution_scale={args.scale}\nupscale=fsr\nsharpness=0.2\nframe_cap=30\nvsync=off\nvolume=55\n')
    env={k:v for k,v in os.environ.items() if not k.startswith('STRIKERS_')}
    env.update(STRIKERS_ROMFS=(ROOT/'build/dump/update/romfs').as_posix(),
               STRIKERS_USERDATA=(out/'userdata').as_posix(),STRIKERS_CONFIG=(out/'strikers_controllers.ini').as_posix(),
               STRIKERS_PC_SHOT='0',STRIKERS_SHOT_DIR=out.as_posix(),STRIKERS_VK_VALIDATE='1',
               STRIKERS_SHOW='0',STRIKERS_KEYBOARD_PLAYER='0',STRIKERS_VSYNC_LOCK='0',STRIKERS_CLOCKSTAT='1')
    exe=ROOT/'build/bin/strikers_native.exe';records=[]
    with (out/'game.log').open('wb') as log:
        p=subprocess.Popen([str(exe),str(ROOT/'build/dump/update/elf/main.elf'),'240'],cwd=ROOT/'build/bin',env=env,stdout=log,stderr=subprocess.STDOUT)
        metadata=dict(pid=p.pid,exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),env={k:v for k,v in env.items() if k.startswith('STRIKERS_')},actions=records)
        def action(name,*values):
            r=subprocess.run([sys.executable,str(ROOT/'tools/audit/pc_window_probe.py'),str(p.pid),name,*map(str,values)],capture_output=True,text=True,check=True)
            state=json.loads(r.stdout);records.append(state);print(state,flush=True);return state
        def capture(label):
            prior=set(out.glob('pc_*.bmp'));action('capture')
            end=time.monotonic()+15
            while time.monotonic()<end:
                for path in set(out.glob('pc_*.bmp'))-prior:
                    try:
                        with Image.open(path) as image:
                            image.load();image.save(out/(label+'.png'))
                        records.append(dict(capture=label,source=path.name));return
                    except (OSError,ValueError):pass
                if p.poll() is not None:raise RuntimeError('game exited during '+label)
                time.sleep(.1)
            raise TimeoutError('no swapchain capture for '+label)
        try:
            deadline=time.monotonic()+80
            while 'pc-fsr: AMD FSR' not in (out/'game.log').read_text(errors='replace'):
                if p.poll() is not None or time.monotonic()>deadline:raise RuntimeError('FSR startup failed')
                time.sleep(.2)
            action('resize',1280,1024);action('f1');time.sleep(1)
            capture('overlay-display')
            action('click',180,133);time.sleep(.3);capture('overlay-graphics')
            action('click',320,133);time.sleep(.3);capture('overlay-controls')
            action('click',380,397);time.sleep(.4);action('key','0x58');time.sleep(.4)
            capture('overlay-conflict')
            action('click',100,915);time.sleep(.3)
            config=(out/'strikers.ini').read_text()
            assert 'key.A = 0x5A, 0x58, 0x00' in config,config
            assert 'volume = 55' in config
            action('f1');time.sleep(.4);capture('letterbox-fsr')
            action('alt-enter');time.sleep(1);capture('borderless')
            action('alt-enter');time.sleep(1)
            action('mode',2);action('focus');time.sleep(1);capture('exclusive-or-fallback')
            assert action('minimize')['minimized'];time.sleep(.5)
            action('restore');time.sleep(1);capture('restored')
            action('mode',0);time.sleep(.5)
            action('resize',960,540);time.sleep(.5);capture('windowed-resized')
            action('close');p.wait(timeout=20)
            assert p.returncode==0,p.returncode
        finally:
            if p.poll() is None:p.kill();p.wait()
            metadata['exit']=p.returncode
            (out/'run.json').write_text(json.dumps(metadata,indent=2)+'\n')
    text=(out/'game.log').read_text(errors='replace')
    assert not any(line.startswith('vk-validation:') for line in text.splitlines())
    assert '0 Vulkan call failures, 0 validation messages' in text
    print('Native overlay, binding save, FSR, resize, display mode, minimise/restore: PASS')


if __name__=='__main__':main()
