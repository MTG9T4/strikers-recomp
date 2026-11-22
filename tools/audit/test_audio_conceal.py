"""Gap concealment of the AudioOut output (runtime/audio/nnaudio.c), simulated in virtual time.

    python -I tools/audit/test_audio_conceal.py [--seconds 30] [--wav-dir DIR]

Builds tools/audit/audio_conceal_sim.c (the production nnaudio.c against a simulated waveOut device and a simulated
game whose audio thread mixes slower than real time, as on a Ryzen Z1 Extreme at 56-58 fps) and runs it with the
concealment off (STRIKERS_AUDIO_CONCEAL=0, the output before 2026-11-21) and on (the default). Checks that the device's
gaps and the clicks at their edges are replaced by filled gaps, that latency stays under 120 ms, and that a game at
full speed is left alone. --wav-dir keeps what the simulated device played, to listen to.
Compiler scratch lives in the system temporary directory.
"""
import argparse
import ctypes as C
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_runtime import ROOT, msvc_environment

FIELDS = ['seconds', 'dry_ms', 'fill_ms', 'faded', 'lat_p50', 'lat_p99', 'lat_max_ms', 'src_d2_max', 'out_d2_max',
          'underruns', 'underrun_ms', 'starved', 'appends', 'fills', 'clicks', 'hard_dry_runs', 'dry_runs']


class Result(C.Structure):
    _fields_ = [(f, C.c_double) for f in FIELDS]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--seconds', type=float, default=30.0)
    p.add_argument('--wav-dir', type=Path)
    a = p.parse_args()
    env = msvc_environment()
    tmp = Path(tempfile.gettempdir()) / ('strikers-conceal-test-' + uuid.uuid4().hex)
    tmp.mkdir()
    dll = None
    try:
        cl = str(Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe')
        cmd = [cl, '/nologo', '/LD', '/MD', '/O2', '/std:c11', '/DWIN32_LEAN_AND_MEAN', '/D_CRT_SECURE_NO_WARNINGS']
        cmd += ['/I' + str(ROOT / q) for q in ('runtime/core', 'runtime/services', 'runtime/gfx', 'build/host-gen', 'build/gen')]
        cmd += [str(ROOT / 'tools/audit/audio_conceal_sim.c'), '/link', '/OUT:' + str(tmp / 'conceal.dll'),
                'winmm.lib', 'ole32.lib', 'uuid.lib']
        r = subprocess.run(cmd, cwd=tmp, env=env, text=True, capture_output=True)
        if r.returncode:
            raise RuntimeError(r.stdout + r.stderr)
        dll = C.CDLL(str(tmp / 'conceal.dll'))
        dll.conceal_sim.argtypes = [C.c_int, C.c_double, C.c_double, C.c_double, C.c_int, C.c_int, C.c_char_p,
                                    C.POINTER(Result)]

        dll.conceal_sim_period.argtypes = [C.c_uint]

        def run(name, mode, speed, delay_ms, conceal_ms, stalls, period=480):
            res = Result()
            wav = b''
            if a.wav_dir:
                a.wav_dir.mkdir(parents=True, exist_ok=True)
                wav = os.fsencode(a.wav_dir / f'{name}.wav')
            dll.conceal_sim_period(period)
            assert dll.conceal_sim(mode, speed, delay_ms, a.seconds, conceal_ms, stalls, wav, C.byref(res)) == 0
            return {f: getattr(res, f) for f in FIELDS}

        # (name, mode, speed, delay_ms, STRIKERS_AUDIO_CONCEAL (0 off, -1 default), stalls[, engine period]);
        # mode 0 = paced by game speed, 1 = paced by released buffers (see audio_conceal_sim.c)
        cases = [('93pct-off', 0, 0.93, 0, 0, 1), ('93pct-on', 0, 0.93, 0, -1, 1),
                 ('96pct-off', 0, 0.96, 0, 0, 0), ('96pct-on', 0, 0.96, 0, -1, 0),
                 ('100pct-off', 0, 1.0, 0, 0, 0), ('100pct-on', 0, 1.0, 0, -1, 0),
                 ('busy-off', 1, 0, 9.0, 0, 1), ('busy-on', 1, 0, 9.0, -1, 1),
                 ('idle-off', 1, 0, 1.0, 0, 0), ('idle-on', 1, 0, 1.0, -1, 0),
                 ('93pct-20ms-off', 0, 0.93, 0, 0, 1, 960), ('93pct-20ms-on', 0, 0.93, 0, -1, 1, 960)]
        rows = {c[0]: run(*c) for c in cases}
        print(f'{"case":14} {"dry ms":>8} {"hard cuts":>9} {"clicks":>6} {"underruns":>9} {"starved":>7} '
              f'{"fills":>5} {"fill ms":>8} {"faded":>5} {"lat p50/p99/max ms":>18}')
        for name, r in rows.items():
            print(f'{name:14} {r["dry_ms"]:8.1f} {r["hard_dry_runs"]:9.0f} {r["clicks"]:6.0f} '
                  f'{r["underruns"]:9.0f} {r["starved"]:7.0f} {r["fills"]:5.0f} {r["fill_ms"]:8.1f} {r["faded"]:5.0f} '
                  f'{r["lat_p50"]:8.0f}/{r["lat_p99"]:.0f}/{r["lat_max_ms"]:.1f}')
        ms = a.seconds * 1000
        for name in ('93pct', 'busy'):
            off, on = rows[name + '-off'], rows[name + '-on']
            # before: the device runs dry, with a click at the edges of the gaps
            assert off['dry_ms'] > 0.03 * ms and off['clicks'] > 100, (name, off)
            # after: the gaps are filled without a click; silence only inside the 120 and 180 ms stalls, faded
            assert on['clicks'] == 0 and on['hard_dry_runs'] == 0 and on['dry_ms'] < 400, (name, on)
            assert on['fill_ms'] < 1.1 * off['dry_ms'], (name, on, 'more filled than the old output missed: a spiral')
            assert on['lat_p99'] < 120 and on['lat_max_ms'] < 120, (name, on)
        r = rows['96pct-on']
        assert rows['96pct-off']['clicks'] > 0 and r['clicks'] == 0 and r['dry_ms'] == 0 and r['lat_max_ms'] < 120, r
        for name in ('100pct', 'idle'):                      # a game on time is left alone: same output, same latency
            off, on = rows[name + '-off'], rows[name + '-on']
            assert off['dry_ms'] == 0 and on['dry_ms'] == 0 and on['fills'] == 0 and on['clicks'] == 0, (name, on)
            assert abs(on['lat_p50'] - off['lat_p50']) <= 1 and on['lat_max_ms'] <= off['lat_max_ms'] + 1, (name, on)
        # a 20 ms engine period: the low-water mark rises after the first late fills, then the gaps are filled
        off, on = rows['93pct-20ms-off'], rows['93pct-20ms-on']
        assert on['clicks'] <= 20 and on['dry_ms'] < 0.1 * off['dry_ms'] and on['lat_max_ms'] < 120, on
        print('Gap concealment: a game at 93-96% speed or with a busy audio thread plays without gaps or clicks '
              '(silence only inside stalls of 120 ms or more, faded), latency under 120 ms, a game on time is left '
              'alone, longer engine periods adapt PASS')
    finally:
        if dll is not None:
            C.windll.kernel32.FreeLibrary(C.c_void_p(dll._handle))
        if tmp.resolve().parent != Path(tempfile.gettempdir()).resolve():
            raise RuntimeError('scratch cleanup escaped temp')
        shutil.rmtree(tmp)


if __name__ == '__main__':
    main()
