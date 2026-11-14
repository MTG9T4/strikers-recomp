"""SMAA and HDR at present (smaa_present.inc, hdr_present.inc): a hidden boot of a test copy with a fresh profile to the
Futbot welcome screen (job27_one_match.seq's first steps), saving at the same frames the game's own picture
(STRIKERS_SHOT_EVERY: before SMAA) and the window's (STRIKERS_PC_SHOT: after SMAA / HDR, and the PC overlay).

  python tools/audit/job48_present_test.py "<install glob under build/phase8>" <name> "<strikers.ini lines; separated>" <frame>[,<frame>...] [env=value ...]

The game runs hidden (STRIKERS_SHOW=0), so no window comes to the front.
"""
import json, os, re, shutil, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/audit'))
from job47_fast_forward import game


def main():
    install = next((ROOT / 'build/phase8').glob(sys.argv[1]))
    name, ini, frames = sys.argv[2], sys.argv[3], sys.argv[4]
    extra = dict(a.split('=', 1) for a in sys.argv[5:])
    last = max(int(f) for f in frames.split(','))
    assert game(install) is None, 'the test copy is already running'
    out = install / 'present-test' / name
    shutil.rmtree(out, ignore_errors=True)
    (out / 'userdata').mkdir(parents=True)
    (out / 'strikers.ini').write_text('window = windowed\nwidth = 1280\nheight = 720\nresolution_scale = 1\nupscale = linear\n'
                                      'start_screen = off\n' + ini.replace(';', '\n') + '\n[Network]\nmode = offline\n')
    seq = [l for l in (ROOT / 'tools/audit/job27_one_match.seq').read_text().splitlines() if not l.startswith('wait')]
    (out / 'replay.seq').write_text('\n'.join(l for l in seq if not re.match(r'\d+\+', l) or int(l.split('+')[0]) < 1600) + '\n')
    if os.environ.get('PRESENT_SEED'):                           # a saved profile and a script of its own (PRESENT_SEQ), e.g. to a match
        (out / 'userdata/save').mkdir(parents=True)
        shutil.copyfile(os.environ['PRESENT_SEED'], out / 'userdata/save/strkrs.save')
        shutil.copyfile(os.environ['PRESENT_SEQ'], out / 'replay.seq')
    env = {k: v for k, v in os.environ.items() if not k.startswith('STRIKERS_')}
    env.update(STRIKERS_USERDATA=str(out / 'userdata'), STRIKERS_PC_CONFIG=str(out / 'strikers.ini'),
               STRIKERS_CONFIG=str(out / 'userdata/controllers.ini'), STRIKERS_ROMFS=str(install / 'build/dump/update/romfs'),
               STRIKERS_VK_CACHE='cache/vk',
               STRIKERS_SDL_TEST_SEQ=str(out / 'replay.seq'), STRIKERS_KEYBOARD_PLAYER='0', STRIKERS_VOLUME='0',
               STRIKERS_NETWORK='offline', STRIKERS_SHOW='0', STRIKERS_SHOT_RANGES=','.join(f'{f}-{f}' for f in frames.split(',')),
               STRIKERS_PC_SHOT=frames, STRIKERS_SHOT_DIR=str(out), _NO_DEBUG_HEAP='1')
    seconds = extra.pop('SECONDS', None)                          # SECONDS=<n>: the game ends by itself after n seconds (its summaries are logged)
    env.update(extra)
    log = out / 'game.log'
    t0 = time.time()
    with log.open('wb') as f:
        p = subprocess.Popen([str(install / 'strikers_native.exe'), str(install / 'build/dump/update/elf/main.elf'), seconds or '600'],
                             cwd=install, env=env, stdout=f, stderr=subprocess.STDOUT, creationflags=0x08000000)
    while time.time() - t0 < 600 and p.poll() is None:
        time.sleep(1)
        if not seconds and ((out / f'pc_{last}.bmp').exists() or (out / f'pc_{last}.raw').exists()):
            time.sleep(3)
            break
    if p.poll() is None:
        p.kill()
        p.wait(30)
    text = log.read_text(errors='replace')
    lines = [l for l in text.splitlines() if re.search(r'pc-smaa|pc-hdr|HDR|pc-capture|shot: frame|glslang rejected|could not be made|validation|astc-gpu', l)]
    files = sorted(x.name for x in out.iterdir() if x.suffix in ('.bmp', '.raw'))
    result = dict(name=name, seconds=round(time.time() - t0), files=files, lines=lines[:20])
    (out / 'report.json').write_text(json.dumps(result, indent=1))
    print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
