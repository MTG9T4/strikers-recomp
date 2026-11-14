"""Kill only an owned release launcher; prove its real game survives and exits cleanly."""
import ctypes as C
from ctypes import wintypes as W
import argparse
import json
import re
import shutil
import subprocess
import time
from pathlib import Path

from job21b_launch import ROOT, strikers_processes, process_snapshot, close_owned_window
from common import clean_environment, sha256


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name',default='job24-launcher-survival')
    args=parser.parse_args()
    if Path(args.name).name!=args.name:parser.error('Invalid evidence name')
    setup = Path(json.loads((ROOT/'build/phase8/job24-setup-result.json').read_text())['install'])
    out = ROOT/'build/phase8'/args.name
    out.mkdir(exist_ok=False)
    install = out/'bin'; install.mkdir()
    for src in setup.iterdir():
        if src.suffix.lower() in ('.exe', '.dll'):
            shutil.copyfile(src, install/src.name)
    shutil.copyfile(ROOT/'tools/release/strikers.ini', install/'strikers.ini')
    quiet = None
    while quiet is None or time.monotonic()-quiet < 120:
        if strikers_processes(): quiet = None
        elif quiet is None: quiet = time.monotonic()
        time.sleep(2)
    elf = setup/'build/dump/update/elf/main.elf'
    romfs = setup/'build/dump/update/romfs'
    seq = out/'launcher.seq'
    seq.write_text(f'duration 240\nelf {elf}\nromfs {romfs}\nclick\nwait done\nexpect success\nexit\n')
    env = clean_environment(out/'sandbox', ROOT/'build/phase8/portable2/toolchain/llvm')
    env.update(STRIKERS_NETWORK='offline', STRIKERS_VOLUME='0', STRIKERS_SHOW='0',
               STRIKERS_VK_FRAMELOG='1-60000', STRIKERS_FSLOG='1')
    k = C.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD]; k.OpenProcess.restype=W.HANDLE
    k.WaitForSingleObject.argtypes=[W.HANDLE,W.DWORD]
    k.GetExitCodeProcess.argtypes=[W.HANDLE,C.POINTER(W.DWORD)]
    k.TerminateProcess.argtypes=[W.HANDLE,W.UINT]
    k.CloseHandle.argtypes=[W.HANDLE]
    launcher = subprocess.Popen([str(install/'StrikersRecomp.exe'),'--self-test',str(seq)], cwd=install, env=env)
    host = None; pid = None; observations=[]; contamination=[]; start=time.monotonic()
    log = install/'userdata/last-game.log'
    def observe():
        nonlocal contamination
        active = strikers_processes()
        contamination = [p for p in active if p['pid'] != pid]
        if contamination: raise RuntimeError('External game detected; discard this attempt')
        text=log.read_text(errors='replace') if log.exists() else ''
        frame=max(map(int,re.findall(r'nvn_vk_frame: (\d+) draws',text)),default=0)
        observations.append(dict(seconds=round(time.monotonic()-start,3),frame=frame,game_alive=k.WaitForSingleObject(host,0)==258))
        return frame
    try:
        while time.monotonic()-start < 120:
            children=[p for p in process_snapshot() if p['parent']==launcher.pid and p['name'].lower()=='strikers_native.exe']
            if len(children)==1:
                pid=children[0]['pid']; host=k.OpenProcess(0x100000|0x1000,False,pid)
                if not host: raise C.WinError(C.get_last_error())
                break
            if launcher.poll() is not None: raise RuntimeError('Launcher exited before game creation')
            time.sleep(.2)
        if not host: raise RuntimeError('No owned game child')
        while observe()<300:
            if launcher.poll() is not None: raise RuntimeError('Launcher exited before termination test')
            if time.monotonic()-start>150: raise RuntimeError('No gameplay frames')
            time.sleep(.5)
        before=observations[-1]['frame']
        if not k.TerminateProcess(W.HANDLE(launcher._handle),0xE0464645): raise C.WinError(C.get_last_error())
        launcher.wait(10)
        deadline=time.monotonic()+15
        while time.monotonic()<deadline:
            observe()
            assert k.WaitForSingleObject(host,0)==258,'Game died when the launcher terminated'
            time.sleep(.5)
        after=observe()
        assert after>before+100,'Game stayed alive but stopped presenting frames'
        # Existing helper posts WM_CLOSE and intentionally returns None.
        # Verify success through the retained process handle and clean log.
        close_owned_window(pid)
        assert k.WaitForSingleObject(host,60000)==0,'Game did not exit normally after WM_CLOSE'
        code=W.DWORD(); assert k.GetExitCodeProcess(host,C.byref(code))
        assert code.value==0,hex(code.value)
        text=log.read_text(errors='replace')
        assert re.search(r'shutdown clean:.*0 Vulkan call failures, 0 validation messages',text)
        assert '0 calls of operations that are not implemented' in text
        assert not re.search(r'GUEST TRAP|HOST EXCEPTION|UNTRANSLATED INSTRUCTION|unhandled exception|VUID-',text,re.I)
        proof=dict(status='PASS',launcher_pid=launcher.pid,game_pid=pid,launcher_exit=launcher.returncode,
                   requested_launcher_exit='0xE0464645',game_exit=code.value,frame_before=before,frame_after=after,
                   survival_seconds=15,observations=observations,contamination=contamination,
                   binary_sha256={p.name:sha256(p) for p in install.iterdir() if p.suffix.lower() in ('.exe','.dll')})
        shutil.copyfile(log,out/'game.log')
        (out/'result.json').write_text(json.dumps(proof,indent=2))
        print(json.dumps({k:v for k,v in proof.items() if k not in ('observations','binary_sha256')},indent=2))
    except BaseException as error:
        (out/'result.json').write_text(json.dumps(dict(status='CONTAMINATED' if contamination else 'FAILED',
            error=str(error),observations=observations,contamination=contamination),indent=2))
        raise
    finally:
        if host:
            if k.WaitForSingleObject(host,0)==258: close_owned_window(pid)
            k.CloseHandle(host)
        if launcher.poll() is None:
            close_owned_window(launcher.pid)
            try: launcher.wait(10)
            except subprocess.TimeoutExpired: launcher.terminate(); launcher.wait()


if __name__=='__main__': main()
