"""Kill a child in each production commit phase, then remount in a new process."""
from pathlib import Path
import argparse
import json
import os
import re
import subprocess
import time
from build_runtime import ROOT, msvc_environment


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, default=ROOT / 'build/run/job14-save-crash')
    p.add_argument('--legacy-move', action='store_true', help='control: compile the old move-and-consume apply operation into the probe only')
    p.add_argument('--no-posix-rename', action='store_true', help='control (audit 03): job 14 apply, MoveFileEx only, compiled into the probe only')
    a = p.parse_args()
    out = a.out.resolve()
    assert out.is_relative_to(ROOT / 'build/run') and not out.exists()
    out.mkdir()
    env = msvc_environment()
    cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
    exe = out / 'fs_probe.exe'
    source = ROOT / 'tools/audit/job14_fs_probe.c'
    if a.no_posix_rename:
        control = (ROOT / 'runtime/fs/nnfs.c').read_text()
        control, n = re.subn(r'int renamed = ok && rename_replace\(f, b\);', 'int renamed = 0;', control)
        assert n == 1
        control_path = out / 'nnfs_no_posix_rename.c'
        control_path.write_text(control)
        source = out / 'probe_no_posix_rename.c'
        source.write_text((ROOT / 'tools/audit/job14_fs_probe.c').read_text().replace('#include "../../runtime/fs/nnfs.c"', '#include "' + control_path.as_posix() + '"'))
    if a.legacy_move:
        legacy = (ROOT / 'runtime/fs/nnfs.c').read_text()
        legacy, n = re.subn(r'if \(exists\(a\)\) \{.*?\} else if \(!exists\(b\)\) \{ fclose\(j\); return -2; \}',
                            'if (exists(a)) { make_parents(b); if (!MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { fclose(j); return -2; } }',
                            legacy, count=1, flags=re.S)
        assert n == 1
        legacy_path = out / 'nnfs_legacy_control.c'
        legacy_path.write_text(legacy)
        fixture = source.read_text().replace('#include "../../runtime/fs/nnfs.c"', '#include "' + legacy_path.as_posix() + '"')
        source = out / 'probe_legacy.c'
        source.write_text(fixture)
    stub = out / 'probe_stubs.c'              # save_import.c's window (none in the probe)
    stub.write_text('void *display_hwnd(void) { return 0; }\n')
    with (out / 'build.log').open('w') as log:
        subprocess.run([str(cl), '/nologo', '/MD', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS',
                        '/I' + str(ROOT / 'runtime/core'), '/I' + str(ROOT / 'build/gen'),
                        str(source), str(ROOT / 'runtime/fs/save_import.c'), str(ROOT / 'runtime/fs/romfs_pkg.c'), str(ROOT / 'runtime/fs/mods.c'),
                        str(stub), '/Fe' + str(exe), 'advapi32.lib', 'user32.lib', 'comdlg32.lib'],
                       cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    subprocess.run([str(exe), 'selftest'], env=env, check=True)
    results = []
    # Audit 03: Commit while the game holds the committed file open for reading (MoveFileEx refused it: ERROR_ACCESS_DENIED).
    root = out / 'reader-open'
    (root / 'save').mkdir(parents=True)
    (root / 'save/strkrs.save').write_bytes(b'OLD SAVE')
    code = subprocess.run([str(exe), root.as_posix(), 'readopen'], env=env).returncode
    if a.legacy_move or a.no_posix_rename:
        assert code == 14, f'control: expected the commit to fail with a reader open, exit {code}'
        subprocess.run([str(exe), root.as_posix(), 'recover'], env=env, check=True)
    else:
        assert code == 0, f'commit with an open reader: exit {code}'
        assert (root / 'save/strkrs.save').read_bytes() == b'NEW SAVE'
        assert not (root / 'save.commit').exists() and not list(root.glob('save.work.*/*'))
    results.append(dict(phase='reader-open', exit=code, committed=(root / 'save/strkrs.save').read_bytes().decode()))
    root = out / 'flush-failure'
    (root / 'save/nested').mkdir(parents=True)
    (root / 'save/strkrs.save').write_bytes(b'OLD SAVE')
    subprocess.run([str(exe), root.as_posix(), 'flushfail'], env=env, check=True)
    assert (root / 'save/strkrs.save').read_bytes() == b'OLD SAVE'
    assert not (root / 'save.commit').exists()
    subprocess.run([str(exe), root.as_posix(), 'recover'], env=env, check=True)
    results.append(dict(phase='flush-failure', old_save='preserved', decision_marker='absent'))
    for phase in ('prepared', 'decided', 'applied'):
        root = out / phase
        save = root / 'save'
        (save / 'nested').mkdir(parents=True)
        (save / 'strkrs.save').write_bytes(b'OLD SAVE')
        (save / 'strkrs.save.apply').write_bytes(b'GAME OWNED FILE')
        (save / 'nested/old.bin').write_bytes(b'OLD FILE')
        logfile = out / (phase + '.log')
        with logfile.open('w') as log:
            child = subprocess.Popen([str(exe), root.as_posix(), 'write'], env=dict(env, STRIKERS_FS_TEST_PAUSE=phase),
                                     stdout=log, stderr=subprocess.STDOUT)
            try:
                until = time.monotonic() + 15
                while 'fs-test: paused ' + phase not in logfile.read_text():
                    assert child.poll() is None, logfile.read_text()
                    assert time.monotonic() < until, 'crash boundary timeout'
                    time.sleep(.02)
                before = (save / 'strkrs.save').read_bytes()
            finally:
                child.kill()
                child.wait()
        subprocess.run([str(exe), root.as_posix(), 'recover'], env=env, check=True)
        value = (save / 'strkrs.save').read_bytes()
        expected = b'OLD SAVE' if phase == 'prepared' else b'NEW SAVE'
        assert value == expected
        nested = sorted(f.name for f in (save / 'nested').iterdir())
        if a.legacy_move and phase == 'applied':
            assert nested == [], 'legacy control did not reproduce the lost child file'
        else:
            assert nested == (['old.bin'] if phase == 'prepared' else ['new.bin']), nested
        assert (save / 'strkrs.save.apply').read_bytes() == b'GAME OWNED FILE'
        assert not list(root.glob('save.commit')) and not list(root.glob('save.work.*'))
        # Mount the result a second time, just as a newer game run would do.
        subprocess.run([str(exe), root.as_posix(), 'recover'], env=env, check=True)
        assert (save / 'strkrs.save').read_bytes() == expected
        results.append(dict(phase=phase, legacy_lost_child=a.legacy_move and phase == 'applied', killed_pid=child.pid, before_kill=before.decode(),
                            recovered=value.decode(), nested=nested, second_mount='PASS'))
    (out / 'results.json').write_text(json.dumps(results, indent=2))
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
