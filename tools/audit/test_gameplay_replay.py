"""Opt-in real-game regression: fresh launch -> Training Match -> Vulkan shutdown.

Requires the extracted game, a built host, and the already installed Khronos
validation layer. Uses a new isolated userdata directory; never resets a save.
This checks SDK destructor binding and both graphics-pipeline cache lifetimes.
Run: python tools/audit/test_gameplay_replay.py --name job09-validation14
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def verify_log(logpath, completed=False):
    log = logpath.read_text(errors='replace')
    assert 'validation layer on (VK_LAYER_KHRONOS_validation)' in log
    geometry = re.search(r'images:.*draws with a geometry stage: (\d+) \((\d+) pipelines(?: made at a draw, (\d+) made ahead from gspipes.bin)?\)', log)
    assert geometry and int(geometry[1]) > 0 and int(geometry[2]) + int(geometry[3] or 0) > 0, 'geometry draw/cache evidence missing'
    shutdown = re.search(r'shutdown clean:.*\((\d+) frames presented, 0 Vulkan call failures, 0 validation messages\)', log)
    assert shutdown and int(shutdown[1]) >= 4200, 'replay did not reach gameplay and shut down cleanly'
    # The startup banner says messages are printed as "vk-validation:"; only
    # lines beginning with that prefix are actual layer diagnostics.
    assert not re.search(r'^vk-validation:|ERROR: AddressSanitizer|unhandled exception|guest trap', log, re.I | re.M)
    assert '0 calls of operations that are not implemented' in log
    if completed:
        assert re.search(r'^input wait: satisfied at frame \d+;', log, re.M), 'full-time wait did not complete'
        assert len(re.findall(r'^capture-frame:', log, re.M)) >= 7, 'post-match review evidence missing'
    print('Training Match, SDK destructor, geometry-cache shutdown: PASS')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--name')
    group.add_argument('--log', type=Path, help='verify an existing completed run without launching again')
    parser.add_argument('--completed-match', action='store_true')
    parser.add_argument('--exe', default='strikers_native.exe')
    parser.add_argument('--seconds', type=int, default=180)
    args = parser.parse_args()
    if args.log:
        verify_log(args.log, args.completed_match)
        return
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
        parser.error('--name must be a simple evidence basename')
    userdata = ROOT / 'build/run' / (args.name + '-userdata')
    logpath = ROOT / 'build/run' / (args.name + '.log')
    if userdata.exists() or logpath.exists():
        parser.error('evidence/userdata already exists; choose a new --name')
    command = [sys.executable, '-I', str(ROOT / 'tools/audit/boot_probe.py'),
               'run', args.name, '--normal', '--exe', args.exe,
               '--seconds', str(args.seconds)]
    for setting in [f'STRIKERS_USERDATA={userdata.as_posix()}',
                    'STRIKERS_SEQ=../../tools/audit/job09_training_match.seq',
                    'STRIKERS_CAPTURE_AT=2100,2400,3300,4200',
                    'STRIKERS_VK_VALIDATE=1', 'STRIKERS_FATAL_DIAG=1']:
        command += ['--env', setting]
    run = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, check=True)
    print(run.stdout, end='')
    assert re.search(r'^exit 0;', run.stdout, re.M), 'host did not exit normally'
    verify_log(logpath, args.completed_match)


if __name__ == '__main__':
    main()
