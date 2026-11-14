"""Build this PC's shaders before the first match, from the player's own game files.

installer/shader-list.json names the shader variants and pipeline states the game uses, as hashes and
render state. It is written into the cache folder, then the game runs hidden twice: the first start
hands over the game's programs and builds what the list names, the second makes the listed pipelines
and saves them. Nothing is played. A warm-up that fails costs time, never the install.
It uses the player's own strikers.ini, because scaled rendering uses other fragment shaders.
"""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0,str(Path(__file__).resolve().parent))
from prepare_graphics import close_owned_window

LIST='shader-list.json'
HANDOVER_FRAME=3000 # by here the game has handed over its start-up programs
LIMIT=600           # seconds a start may run; the game exits by itself at this mark
GRACE=60
ESTIMATE=3          # minutes on the reference PC


def write_list(root,cache):
    """Adds the listed names to the cache; what the PC already has stays."""
    text=(root/'installer'/LIST).read_bytes();data=json.loads(text)
    folder=root/cache;stage=folder/'stage'
    stage.mkdir(parents=True,exist_ok=True)
    mark=folder/'shader-list.applied';digest=hashlib.sha256(text).hexdigest()
    if mark.is_file() and mark.read_text().strip()==digest:return dict(already=True)
    for name,records in data['vars'].items():
        with (stage/(name+'.var')).open('ab') as out:out.write(bytes.fromhex(records))
    with (folder/'pipes.bin').open('ab') as out:out.write(bytes.fromhex(''.join(data['pipes'])))
    with (folder/'gspipes.bin').open('ab') as out:out.write(bytes.fromhex(''.join(data['gspipes'])))
    mark.write_text(digest)
    return dict(variants=len(data['vars']),pipelines=len(data['pipes']),geometry_pipelines=len(data['gspipes']))


def start(root,work,env,cache,name,done,limit,report,route=None):
    """One hidden game process, closed normally once done(line) is true, so it saves its caches."""
    (work/'none.seq').write_text('# no input\n')
    shutil.copyfile(root/'strikers.ini',work/'strikers.ini')
    child_env=dict(env,STRIKERS_ROMFS=str(root/'build/dump/update/romfs'),STRIKERS_USERDATA=str(work/'userdata'),
        STRIKERS_CONFIG=str(work/'controllers.ini'),STRIKERS_PC_CONFIG=str(work/'strikers.ini'),STRIKERS_SDL_TEST_SEQ=str(route or work/'none.seq'),
        STRIKERS_KEYBOARD_PLAYER='0',STRIKERS_VOLUME='0',STRIKERS_SHOW='0',STRIKERS_NETWORK='offline',
        STRIKERS_TRACE=str(work/'boot_trace.txt'),STRIKERS_VK_CACHE=cache)
    started=time.monotonic();reached=[]
    child=subprocess.Popen([str(root/'strikers_native.exe'),str(root/'build/dump/update/elf/main.elf'),str(limit)],cwd=root,env=child_env,
        stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace',
        creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    def read():
        with (work/(name+'.log')).open('w',encoding='utf-8') as log:
            for line in child.stdout:
                log.write(line)
                if not reached and done(line):reached.append(time.monotonic())
    reader=threading.Thread(target=read,daemon=True);reader.start()
    closing=None;told=0
    while child.poll() is None:
        now=time.monotonic()
        if reached and closing is None:
            close_owned_window(child.pid);closing=now
        if now-started>limit+GRACE or (closing is not None and now-closing>=GRACE):
            child.kill();break
        if now-told>=15:
            report(now-started);told=now
        time.sleep(.5)
    code=child.wait();reader.join(10)
    return dict(name=name,seconds=round(time.monotonic()-started),exit=code,reached=bool(reached))


def run(root,env,report,cache='cache/vk',limit=LIMIT,keep=False):
    """report(minutes_left). Never raises: the player has a working game either way."""
    root=Path(root);work=root/'build/warmup';result={}
    begun=time.monotonic()
    tell=lambda _:report(max(1,ESTIMATE-int((time.monotonic()-begun)/60)))
    try:
        (work/'userdata').mkdir(parents=True,exist_ok=True)
        result['listed']=write_list(root,cache)
        frame=lambda line:(m:=re.match(r'nvn_vk: totals at frame (\d+)',line)) is not None and int(m[1])>=HANDOVER_FRAME
        result['build']=start(root,work,env,cache,'build',frame,limit,tell)
        result['pipelines']=start(root,work,env,cache,'pipelines',lambda line:line.startswith('nvn_vk: pipelines made ahead'),limit,tell)
    except Exception as error:
        result['error']=type(error).__name__
    try:
        (work/'warmup.json').write_text(json.dumps(result,indent=2))
        if not keep:shutil.rmtree(work/'userdata',ignore_errors=True)
    except OSError:pass
    return result
