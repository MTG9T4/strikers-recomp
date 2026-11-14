"""Prepare this PC's local graphics caches with the player's own built game.

Uses the production host and ordinary HID replay in a disposable app-local
profile. No cached shader or pipeline bytes are included in release payloads.
"""
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import re
import shutil
import subprocess
import time


def close_owned_window(pid):
    user=C.WinDLL('user32',use_last_error=True)
    callback=C.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
    user.GetWindowThreadProcessId.argtypes=[W.HWND,C.POINTER(W.DWORD)]
    user.PostMessageW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM]
    user.EnumWindows.argtypes=[callback,W.LPARAM]
    @callback
    def visit(hwnd,_):
        owner=W.DWORD();user.GetWindowThreadProcessId(hwnd,C.byref(owner))
        if owner.value==pid:user.PostMessageW(hwnd,0x10,0,0)  # normal WM_CLOSE, cache flush on exit
        return True
    user.EnumWindows(visit,0)


def prepare(root,environment,progress):
    root=Path(root).resolve()
    folder=root/'installer/graphics';folder.mkdir(parents=True,exist_ok=True)
    cache=root/'cache/vk';cache.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(root/'strikers.ini',folder/'strikers.ini')
    # The same normal first-run prompts as a player; only this scratch profile
    # receives the setup replay's save data. The player's userdata is untouched.
    sequence=folder/'prepare.seq'
    sequence.write_text('wait movie\n300+12:Z\n800+12:Z\n1000+12:Z\n1350+12:Z\n1550+12:d\n1650+12:Z\n2000+8:*\n')
    env={k:v for k,v in environment.items() if not k.startswith('STRIKERS_')}
    env.update(STRIKERS_VK_CACHE='cache/vk',STRIKERS_VK_FRAMELOG='1-3600',
        STRIKERS_USERDATA=str(folder/'userdata'),STRIKERS_CONFIG=str(folder/'controllers.ini'),
        STRIKERS_PC_CONFIG=str(folder/'strikers.ini'),STRIKERS_ROMFS=str(root/'build/dump/update/romfs'),
        STRIKERS_REC=str(folder/'input-last.rec'),
        STRIKERS_SDL_TEST_SEQ=str(sequence),STRIKERS_NETWORK='offline',STRIKERS_SHOW='0',STRIKERS_VOLUME='0')
    log=folder/'prepare.log'
    start=time.monotonic();frame=shaders=0;closing=None;first_present=None
    estimated_minutes=3  # audit 07: without the debug heap, a cold launch reaches the menu in about 80 s on the reference PC.
    progress(94,f'Preparing graphics, about {estimated_minutes} minutes (estimate)…')
    with log.open('wb') as output,log.open('r',encoding='utf-8',errors='replace') as observation:
        process=subprocess.Popen([str(root/'strikers_native.exe'),str(root/'build/dump/update/elf/main.elf')],
            cwd=root,env=env,stdout=output,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            last_update=0
            while process.poll() is None:
                now=time.monotonic()
                for line in observation:
                    if 'nvn_vk_gpl: stage library' in line:shaders+=1
                    match=re.search(r'nvn_vk_frame: (\d+) draws ',line)
                    if match:
                        frame=max(frame,int(match[1]))
                        if first_present is None:first_present=now-start
                if frame>=3600 and closing is None:
                    close_owned_window(process.pid);closing=now
                if closing is not None and now-closing>60:
                    raise ValueError('Graphics preparation could not close cleanly; retry setup')
                if now-start>3600:
                    raise ValueError('Graphics preparation took longer than an hour; see installer/graphics/prepare.log')
                if now-last_update>=5:
                    elapsed_minutes=int((now-start)/60)
                    timing=(f'about {max(1,estimated_minutes-elapsed_minutes)} minutes (estimate)' if elapsed_minutes<estimated_minutes
                            else f'{elapsed_minutes} minutes elapsed; taking longer than estimated')
                    progress(94+min(3,frame*4//3600),f'Preparing graphics, {timing} — {shaders} shaders prepared…')
                    last_update=now
                time.sleep(.25)
            if process.returncode or frame<3600:
                raise ValueError('Graphics preparation did not finish; see installer/graphics/prepare.log')
        finally:
            if process.poll() is None:process.terminate()
            process.wait()
    if not (cache/'pipes.bin').is_file():
        raise ValueError('The graphics cache was not saved; retry setup')
    proof=dict(seconds=time.monotonic()-start,estimated_minutes=estimated_minutes,first_present_seconds=first_present,frames=frame,
        shaders_reported=shaders,exit=process.returncode,cache='cache/vk',profile='installer/graphics/userdata')
    proof['cache_artifacts']={}
    for name in ('spv','stage','nv'):
        files=[p for p in (cache/name).rglob('*') if p.is_file()]
        proof['cache_artifacts'][name]=dict(files=len(files),bytes=sum(p.stat().st_size for p in files))
    (folder/'result.json').write_text(json.dumps(proof,indent=2))
    return proof
