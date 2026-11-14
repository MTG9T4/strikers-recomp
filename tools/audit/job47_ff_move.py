"""Fast-forward in play, measured in the game itself: a fresh profile follows job11_quick_battle.seq into the Futbot MOVE
lesson (as it does on this build), then holds the left stick right for 120 frames. Pictures before and after the hold
(the game's own, STRIKERS_SHOT_RANGES) show how far Mario ran. With fast-forward n x (STRIKERS_FF_TEST for the whole run),
each frame carries n refreshes of game time, so he should run about n times as far in the same frames.

  python tools/audit/job47_ff_move.py "<install glob under build/phase8>" <name> [<first second>,<seconds> | -]

Hidden (STRIKERS_SHOW=0): no window comes to the front.
"""
import json, os, re, shutil, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/audit'))
from job47_fast_forward import game
from run_player_soak import sequence

HOLD_FROM, HOLD = 12000, 120


def main():
    install = next((ROOT / 'build/phase8').glob(sys.argv[1]))
    name = sys.argv[2]
    ff = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] != '-' else ''
    assert game(install) is None, 'the test copy is already running'
    out = install / 'ff-move' / name
    shutil.rmtree(out, ignore_errors=True)
    (out / 'userdata').mkdir(parents=True)
    (out / 'strikers.ini').write_text('window = windowed\nwidth = 1280\nheight = 720\nresolution_scale = 1\nupscale = linear\n'
                                      'start_screen = off\nfast_forward = 4\n' + os.environ.get('FF_EXTRA_INI', '').replace(';', '\n') +
                                      '\n[Network]\nmode = offline\n')     # FF_EXTRA_INI: more settings lines, ; separated
    menus = sequence(1).split('wait open:')[0]
    (out / 'replay.seq').write_text(menus + f'\n# the MOVE lesson: run right\n{HOLD_FROM}+{HOLD}:r\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith('STRIKERS_')}
    env.update(STRIKERS_USERDATA=str(out / 'userdata'), STRIKERS_PC_CONFIG=str(out / 'strikers.ini'),
               STRIKERS_CONFIG=str(out / 'userdata/controllers.ini'), STRIKERS_ROMFS=str(install / 'build/dump/update/romfs'),
               STRIKERS_VK_CACHE='cache/vk',
               STRIKERS_SDL_TEST_SEQ=str(out / 'replay.seq'), STRIKERS_KEYBOARD_PLAYER='0', STRIKERS_VOLUME='0',
               STRIKERS_NETWORK='offline', STRIKERS_SHOW='0', STRIKERS_SHOT_DIR=str(out), _NO_DEBUG_HEAP='1',
               STRIKERS_SHOT_RANGES=f'{HOLD_FROM}-{HOLD_FROM + HOLD + 120}/30', STRIKERS_CLOCKSTAT='1')
    if ff:
        env['STRIKERS_FF_TEST'] = ff
    for kv in os.environ.get('FF_EXTRA_ENV', '').split(';'):     # e.g. STRIKERS_WIDE_TEST=0.75 (other audits reuse this run)
        if '=' in kv: env[kv.split('=', 1)[0]] = kv.split('=', 1)[1]
    log = out / 'game.log'
    t0 = time.time()
    last = out / f'frame_{HOLD_FROM + HOLD + 120}.bmp'
    with log.open('wb') as f:
        p = subprocess.Popen([str(install / 'strikers_native.exe'), str(install / 'build/dump/update/elf/main.elf'), '900'],
                             cwd=install, env=env, stdout=f, stderr=subprocess.STDOUT, creationflags=0x08000000)
    while time.time() - t0 < 900 and p.poll() is None and not last.exists():
        time.sleep(1)
    time.sleep(2)
    if p.poll() is None:
        p.kill()
        p.wait(30)
    text = log.read_text(errors='replace')
    signals = re.findall(r'one refresh clock; (\d+) vsync signals', text)
    # Mario's place: the red player marker above him (the field area only); real seconds a frame from the clock line around the hold
    from PIL import Image
    xs = {}
    for s in sorted(out.glob('frame_*.bmp'), key=lambda q: int(q.stem[6:])):
        im = Image.open(s).convert('RGB').resize((640, 360)); px = im.load(); pts = []
        for y in range(60, 300):
            for x in range(40, 600):
                r, g, b = px[x, y]
                if r > 200 and g < 60 and b < 60: pts.append(x)
        if len(pts) > 6: xs[int(s.stem[6:])] = sum(pts) / len(pts)
    per = None
    for m in re.finditer(r'clock: frames (\d+)-(\d+) real ([\d.]+) s', text):
        a, b, sec = int(m[1]), int(m[2]), float(m[3])
        if a <= HOLD_FROM < b: per = sec / (b - a)
    run = sorted(f for f in xs if HOLD_FROM <= f <= HOLD_FROM + 60)
    speed = (xs[run[-1]] - xs[run[0]]) / ((run[-1] - run[0]) * per) if per and len(run) > 1 and run[-1] > run[0] else None
    result = dict(name=name, ff=ff, seconds=round(time.time() - t0), seconds_a_frame=per, marker_x=xs, px_per_real_second=speed,
                  ff_lines=[l for l in text.splitlines() if 'fast-forward' in l][:4])
    (out / 'report.json').write_text(json.dumps(result, indent=1))
    print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
