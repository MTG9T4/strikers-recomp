"""Every regression test of the project in one command (audit 03).

    .venv/Scripts/python -I tools/audit/run_all_tests.py [--exe strikers_native.exe] [--log GAME_LOG ...] [--only NAME ...]

Runs the tests one after the other (the material harnesses share one probe executable), each with a time limit, writes
each one's output to build/run/<prefix>-<name>.txt and prints a table; the exit code is the number of failures.

Fixtures (captured stages and material bundles, not in Git) are taken from this checkout's build/run; a worktree that
lacks one gets a copy from the main checkout's build/run (read there, never written). The pad self-test runs the given
executable from build/bin with STRIKERS_PAD_SELFTEST=1 (SDL virtual devices only). Completed game logs given with --log
are checked by test_menu_replay.py (Quick Battle and other menu routes) or, for a Training match log, by
test_gameplay_replay.py --completed-match; nothing here starts the game itself.
"""
import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AUDIT = ROOT / 'tools/audit'
PY = [sys.executable]


def main_checkout():
    """The main checkout's root (this one, unless this is a worktree)."""
    try:
        common = subprocess.run(['git', 'rev-parse', '--path-format=absolute', '--git-common-dir'], cwd=ROOT,
                                capture_output=True, text=True, check=True).stdout.strip()
        return Path(common).parent
    except (OSError, subprocess.CalledProcessError):
        return ROOT


def fixture(rel):
    """build/run/<rel> of this checkout; copied from the main checkout when only it has it. None if neither has it."""
    mine = ROOT / 'build/run' / rel
    if mine.exists():
        return mine
    theirs = main_checkout() / 'build/run' / rel
    if not theirs.exists():
        return None
    if theirs.is_dir():
        shutil.copytree(theirs, mine)
    else:
        mine.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(theirs, mine)
    return mine


def tests(a, prefix):
    """(name, command or None when a fixture is missing, timeout seconds, text that must appear in the output or None)."""
    t = []
    def add(name, cmd, timeout=900, must=None, needs=()):
        t.append((name, None if any(n is None for n in needs) else cmd, timeout, must))
    for name in ('test_input_script', 'test_movie_queue', 'test_media_contracts', 'test_runtime_contracts',
                 'test_sdk_interposition', 'test_gfx_contracts', 'test_spv_files'):
        add(name, PY + [str(AUDIT / (name + '.py'))])
    add('test_nex', PY + [str(AUDIT / 'test_nex.py'), '--out', str(ROOT / f'build/run/{prefix}-nex')], timeout=120)
    add('test_zerotier', PY + [str(AUDIT / 'test_zerotier.py')], timeout=120)
    add('test_group_service', PY + [str(AUDIT / 'test_group_service.py')], timeout=120)
    add('test_group_worker', PY + [str(AUDIT / 'test_group_worker.py')], timeout=120)
    add('test_group_workerd', PY + [str(AUDIT / 'test_group_worker.py'), '--workerd'], timeout=120)
    add('test_zt_group_policy', PY + [str(AUDIT / 'test_zt_group_policy.py')], timeout=120)
    add('test_zt_group_two', PY + [str(AUDIT / 'test_zerotier.py'), '--group-live', '2', '--out', str(ROOT / f'build/run/{prefix}-group-two')], timeout=420, must='"status": "PASS"')
    add('test_zt_group_three', PY + [str(AUDIT / 'test_zerotier.py'), '--group-live', '3', '--out', str(ROOT / f'build/run/{prefix}-group-three')], timeout=420, must='"status": "PASS"')
    add('test_player_crash', PY + [str(AUDIT / 'test_player_crash.py')], timeout=120)
    add('test_player_process', PY + [str(AUDIT / 'test_player_process.py')], timeout=60)
    add('test_player_soak', PY + [str(AUDIT / 'test_player_soak.py')], timeout=60)
    add('test_group_json_fuzz', PY + [str(AUDIT / 'test_group_json_fuzz.py'), '--out', str(ROOT / f'build/run/{prefix}-group-json-fuzz')], timeout=600, must='PASS')   # audit 06
    add('test_pc_features', PY + [str(AUDIT / 'test_pc_features.py'), '--out', str(ROOT / f'build/run/{prefix}-pc-features')], timeout=120)
    add('test_online_services', PY + [str(AUDIT / 'test_online_services.py'), '--out', str(ROOT / f'build/run/{prefix}-online-services')], timeout=180)
    stages = [fixture(f'job08-stages/s{s:05}_0.mxd') for s in (2695, 3119, 3158, 3159, 3160, 3161)]
    add('test_gameplay_shaders', PY + [str(AUDIT / 'test_gameplay_shaders.py')] + [str(s) for s in stages if s], needs=stages)
    mxd = fixture('job08-stages/s03117_0.mxd')
    add('test_material_shaders', PY + [str(AUDIT / 'test_material_shaders.py'), str(mxd)], must='PASS', needs=[mxd])
    ball, qb = fixture('job13-final-ball-resources'), fixture('job13-final-qb-resources')
    add('test_variant_key', PY + [str(AUDIT / 'test_variant_key.py')], needs=[ball, qb])
    if ball:
        add('clip_ball', PY + [str(AUDIT / 'test_material_clip_mode.py'), str(ball), 'f4000_d312', '--out', str(ROOT / f'build/run/{prefix}-clip-ball')])
        add('black_ball', PY + [str(AUDIT / 'test_black_material_replay.py'), str(ball), 'f4000_d312', '--out', str(ROOT / f'build/run/{prefix}-black-ball')])
    if qb:
        add('clip_banana', PY + [str(AUDIT / 'test_material_clip_mode.py'), str(qb / 'fs593-v1236'), 'f6494_d417', '--vs', '90', '--fs', '593',
                                 '--out', str(ROOT / f'build/run/{prefix}-clip-banana')])
        add('clip_box', PY + [str(AUDIT / 'test_material_clip_mode.py'), str(qb / 'fs2834-v1648'), 'f5306_d400', '--vs', '2190', '--fs', '2834',
                              '--out', str(ROOT / f'build/run/{prefix}-clip-box')])
    gear, index = fixture('job13-gear-resources'), fixture('job13-index-resources')
    add('test_colour_shadow', PY + [str(AUDIT / 'test_colour_shadow.py'), str(gear), str(index)], must='PASS', needs=[gear, index])
    add('test_save_crash', PY + [str(AUDIT / 'test_save_crash.py'), '--out', str(ROOT / f'build/run/{prefix}-save-crash')])
    add('test_save_import', PY + [str(AUDIT / 'test_save_import.py')])
    add('test_cheats', PY + [str(AUDIT / 'test_cheats.py')])
    add('test_update_fetch', PY + [str(AUDIT / 'test_update_fetch.py')])
    add('test_texrepl', PY + [str(AUDIT / 'test_texrepl.py')])
    add('test_lan', PY + [str(AUDIT / 'test_lan.py'), '--out', str(ROOT / f'build/run/{prefix}-lan')], timeout=180,
        must='"status": "PASS"')   # audit 04; listed once (audit 06)
    exe = ROOT / 'build/bin' / a.exe
    add('pad_selftest', [str(exe)], timeout=300, must='PAD SELFTEST PASSED', needs=[exe if exe.exists() else None])
    for log in a.log:
        log = Path(log).resolve()
        text = log.read_text(errors='replace') if log.exists() else ''
        if 'job09_training_match' in text or 'training' in log.name:
            add('replay_' + log.stem, PY + [str(AUDIT / 'test_gameplay_replay.py'), '--log', str(log), '--completed-match'])
        else:
            add('replay_' + log.stem, PY + [str(AUDIT / 'test_menu_replay.py'), str(log)])
    return t


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--exe', default='strikers_native.exe', help='executable in build/bin for the pad self-test')
    p.add_argument('--log', action='append', default=[], help='completed game log to check (repeatable)')
    p.add_argument('--only', action='append', default=[], help='run only tests whose name contains this (repeatable)')
    p.add_argument('--prefix', default='tests-' + time.strftime('%Y%m%d-%H%M%S'), help='name prefix of the outputs in build/run')
    a = p.parse_args()
    results = []
    for name, cmd, timeout, must in tests(a, a.prefix):
        if a.only and not any(o in name for o in a.only):
            continue
        out = ROOT / 'build/run' / f'{a.prefix}-{name}.txt'
        if cmd is None:
            results.append((name, 'SKIP', 0.0, 'fixture or executable missing'))
            print(f'{name:28} SKIP (fixture or executable missing)', flush=True)
            continue
        env = None
        if name == 'pad_selftest':
            import os
            env = dict(os.environ, STRIKERS_PAD_SELFTEST='1')
        t0 = time.time()
        try:
            r = subprocess.run(cmd, cwd=ROOT if name != 'pad_selftest' else ROOT / 'build/bin', env=env,
                               capture_output=True, text=True, errors='replace', timeout=timeout)
            text, code = r.stdout + r.stderr, r.returncode
        except subprocess.TimeoutExpired as e:
            text, code = (e.stdout or '') if isinstance(e.stdout, str) else '', 'timeout'
        dt = time.time() - t0
        out.write_text(text)
        ok = code == 0 and (must is None or must in text)
        why = '' if ok else f'exit {code}' + ('' if must is None or must in text else f', "{must}" missing')
        results.append((name, 'PASS' if ok else 'FAIL', dt, why))
        print(f'{name:28} {"PASS" if ok else "FAIL"} {dt:6.1f} s  {why}  -> {out.relative_to(ROOT).as_posix()}', flush=True)
    failed = sum(1 for r in results if r[1] == 'FAIL')
    skipped = sum(1 for r in results if r[1] == 'SKIP')
    print(f'\n{len(results) - failed - skipped} passed, {failed} failed, {skipped} skipped')
    return failed


if __name__ == '__main__':
    sys.exit(main())
