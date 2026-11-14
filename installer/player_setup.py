"""Local-only player pipeline. Invoked by the setup GUI's embedded Python."""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time
sys.path.insert(0,str(Path(__file__).resolve().parent))
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/release'))
from common import clean_environment, sha256, within
import game_files

# Setup builds the game from the ExeFS and RomFS folders the player extracted from their own Switch (game_files.py).
PACKAGES=False
FOLDERS=True                    # this run's mode, for the failure message
SHORTCUT='Strikers Recomp.lnk'


def progress(percent,text):
    print(f'@phase8 {percent}|{text}',flush=True)


def compile_jobs():
    """Half the processors, 4 GB of memory each, between 4 and 8: a busier PC trips build_player's time limit."""
    class Memory(ctypes.Structure):
        _fields_=[('length',ctypes.c_ulong),('load',ctypes.c_ulong),('total',ctypes.c_ulonglong),('rest',ctypes.c_ulonglong*6)]
    memory=Memory();memory.length=ctypes.sizeof(memory)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(memory))
    return max(4,min(8,(os.cpu_count() or 8)//2,memory.total>>32))


def cpu_level():
    """The instruction set the game code is built for (build_player.py CPU_FLAGS), from what Windows reports.
    STRIKERS_CPU_LEVEL=base|v2|v3 overrides it for tests."""
    forced=os.environ.get('STRIKERS_CPU_LEVEL')
    if forced in ('base','v2','v3'):return forced
    present=ctypes.windll.kernel32.IsProcessorFeaturePresent
    if present(40):return 'v3'          # PF_AVX2_INSTRUCTIONS_AVAILABLE
    if present(38):return 'v2'          # PF_SSE4_2_INSTRUCTIONS_AVAILABLE
    return 'base'


def verify_payload(root):
    manifest=json.loads((root/'release-manifest.json').read_text())
    if manifest.get('game_free_host') is not True:
        raise ValueError('This host has not passed the game-free release check')
    for name,digest in manifest['files'].items():
        # The organizer's strikers.ini replaces this one, and F1 rewrites it. It is settings, not a program file.
        if name!='strikers.ini' and sha256(within(root,root/name))!=digest:
            raise ValueError('A setup file is missing or damaged; extract a fresh copy')


def main():
    total_start=time.monotonic()
    timings=[]
    global FOLDERS
    request=json.loads((ROOT/'installer/request.json').read_text(encoding='utf-8-sig'))
    folders=FOLDERS='exefs' in request
    copy=True
    if folders:
        exefs,romfs=game_files.locate(request['exefs'],request['romfs'])
        game_files.readonly((exefs,romfs),ROOT)
        request=dict(exefs=str(exefs),romfs=str(romfs))
    elif not PACKAGES:
        raise ValueError('Choose the ExeFS and RomFS folders you extracted from your own Switch')
    verify_payload(ROOT)
    # Refuse junctions in the install tree: all generated writes must stay here.
    if any(p.is_symlink() or (hasattr(p,'is_junction') and p.is_junction()) for p in [ROOT,*ROOT.rglob('*')]):
        raise ValueError('Choose an installation folder without symbolic links or junctions')
    work=ROOT/'build'
    work.mkdir(exist_ok=True)
    import msvcrt
    with (work/'setup.lock').open('a+b') as lock:
        lock.seek(0);lock.write(b'0');lock.flush();lock.seek(0)
        msvcrt.locking(lock.fileno(),msvcrt.LK_NBLCK,1)
        env=clean_environment(ROOT/'installer/sandbox',ROOT/'toolchain/llvm')
        os.environ.clear()
        os.environ.update(env)
        progress(4,'Checking game identity, version and file integrity…')
        checked=time.monotonic()
        if folders:
            identities=[game_files.check(exefs,romfs)]
            main_file=exefs/'main'
            selection=dict(exefs=dict(path=str(exefs),size=main_file.stat().st_size,modified=main_file.stat().st_mtime_ns),
                           romfs=dict(path=str(romfs),files=identities[0]['romfs_files']))
        timings.append(dict(step='validation',seconds=time.monotonic()-checked))
        def save_timings():
            (work/'setup-timings.json').write_text(json.dumps(dict(interpreter=sys.executable,
                version=sys.version,steps=timings,total_seconds=time.monotonic()-total_start),indent=2))
        save_timings()
        (work/'player-identity.json').write_text(json.dumps(identities,indent=2))
        state_path=work/'player-state.json'
        if state_path.exists():
            state=json.loads(state_path.read_text())
            if state['selection']!=selection:
                raise ValueError('These files differ from the previous build; extract setup into a new folder')
        else:
            if any(work.glob('dump/update/exefs/*')):
                raise ValueError('An unrecognized build exists; extract setup into a new folder')
            state=dict(selection=selection,done=[])
        def save_state():
            pending=state_path.with_suffix('.part')
            pending.write_text(json.dumps(state,indent=2));pending.replace(state_path)
        save_state()
        if folders:
            commands=[('dump',10,['installer/game_files.py','copy',str(exefs),str(romfs),str(work/'dump/update')]),
                      ('art',27,['installer/game_art.py','--folders',str(exefs),str(romfs),str(work/'game-art')]),
                      ('identify',28,['scripts/identify.py'])]
        commands+=[('functions',30,['tools/extract/functions.py','--no-review']),('imports',34,['tools/extract/imports.py']),
                  ('lift',38,['tools/lifter/recomp.py','full']),('tables',52,['tools/lifter/gen_tables.py']),
                  ('sdk',56,['tools/lifter/sdklift.py','gen']),
                  ('compile',60,['tools/release/build_player.py','--llvm',str(ROOT/'toolchain/llvm'),
                                 '--output',str(work/'player-dll'),'--jobs',str(compile_jobs())]),
                  ('sdk compile',90,['tools/release/build_player_sdk.py','--llvm',str(ROOT/'toolchain/llvm'),
                                    '--output',str(work/'player-sdk')])]
        start=time.monotonic()
        labels={'dump':'Reading your game files','art':"Reading your game's icon",'identify':'Checking your game version',
                'functions':'Preparing the game','imports':'Preparing the game',
                'lift':'Converting the game for your PC','tables':'Preparing game data',
                'sdk':'Preparing game support','compile':'Building your PC game',
                'sdk compile':'Building game support'}
        for name,percent,command in commands:
            elapsed=time.monotonic()-start
            estimate=max(1,int(elapsed*(100-percent)/max(percent-4,1)/60))
            if name=='compile':estimate=8
            progress(percent,f'{labels[name]} — roughly {estimate} minutes remaining (estimate)')
            # A step is marked complete only after its child exits successfully.
            # Inputs are fully validated again on every retry. Never mix dumps.
            if name in state['done']:
                print(name+': verified previous successful step; resuming',flush=True)
                continue
            if name=='compile' and (work/'player-dll').exists():command=command+['--link-existing']
            step_start=time.monotonic()
            with (work/(name.replace(' ','-')+'.log')).open('w',encoding='utf-8') as step_log:
                child=subprocess.Popen([sys.executable,'-I','-X','utf8','-B',*command],cwd=ROOT,env=env,
                    stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace',
                    creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                for line in child.stdout:
                    step_log.write(line);step_log.flush()
                    if line.startswith('@phase8 '):print(line.rstrip(),flush=True)
                if child.wait():raise subprocess.CalledProcessError(child.returncode,command)
            timings.append(dict(step=name,seconds=time.monotonic()-step_start));save_timings()
            state['done'].append(name);save_state()
        # Do not claim success with a host that still embeds game code.
        verify_payload(ROOT)
        import shutil
        shutil.copyfile(work/'player-dll/strikers_game.dll',ROOT/'strikers_game.dll')
        shutil.copyfile(work/'player-sdk/strikers_sdk.dll',ROOT/'strikers_sdk.dll')
        # Portable shortcut stays inside the install folder. The player can copy
        # it to Desktop; setup never writes outside this tree.
        progress(91,'Creating your game shortcut…')
        step_start=time.monotonic()
        from shortcut import create_shortcut
        # The icon is the game's own (installer/game_art.py, from the player's files); none ships with this project.
        icon=work/'game-art/icon.ico'
        create_shortcut(ROOT/SHORTCUT,ROOT/'StrikersRecomp.exe',ROOT,icon if icon.is_file() else None)
        timings.append(dict(step='shortcut',seconds=time.monotonic()-step_start));save_timings()
        (work/'setup-complete.json').write_text(json.dumps(dict(seconds=time.monotonic()-start)))
        (ROOT/'installer/request.json').unlink()
        (ROOT/'installer/packages.txt').unlink(missing_ok=True)
        # The game is complete from here on; the warm-up only saves the player's first-match stutter.
        import warmup
        def warmup_progress(minutes):
            progress(93,f'Preparing your graphics — roughly {minutes} minutes remaining (estimate)')
        step_start=time.monotonic()
        game_env={k:v for k,v in env.items() if not k.startswith('STRIKERS_')}
        print('warm-up: '+json.dumps(warmup.run(ROOT,game_env,warmup_progress)),flush=True)
        timings.append(dict(step='warm-up',seconds=time.monotonic()-step_start));save_timings()
        progress(100,'Ready — open Strikers Recomp to play.')


if __name__=='__main__':
    try:main()
    except BaseException as error:
        # Never dump key values or package bytes in a traceback. The child step
        # and exception type remain diagnostic; the UI gets one plain sentence.
        if isinstance(error,ValueError):message=str(error).rstrip('.; ')+'.'
        elif isinstance(error,FileNotFoundError):message='A selected folder is missing; choose your ExeFS and RomFS folders again.'
        elif isinstance(error,(KeyError,AssertionError,SystemExit)):message='Your game files could not be read; extract them again from your own Switch.'
        elif isinstance(error,subprocess.CalledProcessError):message='A build step failed; keep this folder and retry setup.'
        else:message='Setup could not finish; see installer/setup.log for the failed step.'
        print('Setup failure type: '+type(error).__name__,flush=True)
        print('@error '+message,flush=True)
        raise SystemExit(1)
