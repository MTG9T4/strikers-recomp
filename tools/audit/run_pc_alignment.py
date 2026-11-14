"""Pause one actual match through HID and compare the same frozen view at each scale.

Requires the isolated --scale-test audit host. That build replaces host images
at frame boundaries, then lets ordinary draw commands rerender them. No guest
memory/state or clock edits. The default host has no resolution-switch message.
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
    args=ap.parse_args();out=args.out.resolve();out.relative_to(ROOT/'build/run');out.mkdir(exist_ok=False)
    seq=(ROOT/'tools/audit/job11_quick_battle.seq').read_text().split('wait open:')[0]
    (out/'pause.seq').write_text(seq+'\n6100+8:!\n6400+8:*\n')
    (out/'strikers.ini').write_text('window=windowed\nwidth=1280\nheight=720\nresolution_scale=1\nupscale=linear\n')
    env={k:v for k,v in os.environ.items() if not k.startswith('STRIKERS_')}
    env.update(STRIKERS_ROMFS=(ROOT/'build/dump/update/romfs').as_posix(),STRIKERS_USERDATA=(out/'userdata').as_posix(),
               STRIKERS_CONFIG=(out/'strikers_controllers.ini').as_posix(),STRIKERS_SDL_TEST_SEQ=(out/'pause.seq').as_posix(),
               STRIKERS_KEYBOARD_PLAYER='0',STRIKERS_VK_SCALE='1.000001',STRIKERS_ENABLE_RESOLUTION_SCALING='1',
               STRIKERS_VK_VALIDATE='1',STRIKERS_VOLUME='0',STRIKERS_SHOW='0',STRIKERS_PC_SHOT='6400',
               STRIKERS_SHOT_DIR=out.as_posix(),STRIKERS_SHOT_RANGES='6400-6400',STRIKERS_VK_SHOT_NATIVE='1',
               STRIKERS_VK_FRAMELOG='6350-7200')
    exe=ROOT/'build/bin/strikers_job16b_scale_test.exe';captures=[]
    with (out/'game.log').open('wb') as log:
        p=subprocess.Popen([str(exe),str(ROOT/'build/dump/update/elf/main.elf'),'440'],cwd=ROOT/'build/bin',env=env,stdout=log,stderr=subprocess.STDOUT)
        metadata=dict(pid=p.pid,exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),env={k:v for k,v in env.items() if k.startswith('STRIKERS_')},captures=captures)
        def action(name,*values):
            subprocess.run([sys.executable,str(ROOT/'tools/audit/pc_window_probe.py'),str(p.pid),name,*map(str,values)],check=True,capture_output=True)
        def capture(label):
            prior=set(out.glob('pc_*.bmp'));action('capture');deadline=time.monotonic()+15
            while time.monotonic()<deadline:
                for source in set(out.glob('pc_*.bmp'))-prior:
                    try:
                        with Image.open(source) as image:image.load();image.save(out/(label+'.png'))
                        captures.append(dict(label=label,source=source.name));print(captures[-1],flush=True);return
                    except (OSError,ValueError):pass
                if p.poll() is not None:raise RuntimeError('game exited')
                time.sleep(.1)
            raise TimeoutError(label)
        try:
            deadline=time.monotonic()+350
            while not (out/'pc_6400.bmp').exists():
                if p.poll() is not None or time.monotonic()>deadline:raise RuntimeError('paused-match frame not reached')
                time.sleep(.2)
            time.sleep(1);capture('paused-100-initial')
            for scale in (150,200,50,100):
                action('scale-test',scale)
                # Several ordinary frames must redraw after copying old image contents.
                time.sleep(2)
                capture('paused-'+str(scale))
            action('close');p.wait(timeout=25)
        finally:
            if p.poll() is None:p.kill();p.wait()
            metadata['exit']=p.returncode;(out/'run.json').write_text(json.dumps(metadata,indent=2)+'\n')
    text=(out/'game.log').read_text(errors='replace')
    assert p.returncode==0,p.returncode
    assert not any(line.startswith('vk-validation:') for line in text.splitlines())
    assert '0 Vulkan call failures, 0 validation messages' in text
    print('Same paused match view captured at native, 1.5x, 2x and 0.5x; review images and draw counts.')


if __name__=='__main__':main()
