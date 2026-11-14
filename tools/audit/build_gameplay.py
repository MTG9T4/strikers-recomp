"""Job09 host build: isolated objects, /Z7 avoids restricted MSVC PDB-server IPC.

Uses the existing build/link rules without editing job10's build_exe.py.
"""
from pathlib import Path
import argparse
import sys
import hashlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/lifter'))
import build_exe as build

original_flags = build.msvc_flags
original_build = build.T.build
object_tag = 'job09-'


def flags(opt, asan=False):
    return ['/Z7' if f == '/Zi' else f for f in original_flags(opt, asan) if f != '/FS']


def location(*parts):
    if parts[:2] == ('obj', 'host'):
        return original_build('obj', 'host', object_tag + parts[2])
    return original_build(*parts)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--opt', action='store_true')
    parser.add_argument('--asan', action='store_true')
    parser.add_argument('--lan-sdk', action='store_true', help='include the SDK zlib strategy table reached by Pia')
    parser.add_argument('--online-main', action='store_true', help='isolated DLL with the original NEX formatter callback entry')
    parser.add_argument('--output', help='alternative .exe basename inside build/bin')
    parser.add_argument('--scale-test', action='store_true', help='audit-only paused-match resolution switching')
    args = parser.parse_args()
    if args.online_main:
        if not args.output:parser.error('--online-main requires an isolated --output executable')
        import subprocess
        subprocess.run([sys.executable,str(ROOT / 'tools/audit/prepare_online_main.py')],check=True)
        build.PRODUCTS['game']='strikers_job17_game.dll'
    if args.scale_test:
        if not args.output:parser.error('--scale-test requires an isolated --output executable')
        object_tag='job16-scale-test-'
        build.MANIFEST['host_defines']=list(build.MANIFEST['host_defines'])+['STRIKERS_PC_SCALE_TEST=1']
    if args.lan_sdk:
        from prepare_lan_sdk import prepare
        prepare()
    # Stable across paths and CRLF checkouts; different networking source builds
    # are listed but cannot join each other. No timestamp/compiler-path salt.
    build.network_fingerprint()
    if args.output:
        if Path(args.output).name != args.output or not args.output.endswith('.exe'):
            parser.error('--output must be an .exe basename')
        build.PRODUCTS['host'] = args.output
    build.msvc_flags = flags
    build.T.build = location
    env = build.msvc_environment()
    raise SystemExit(build.build_host(False, args.opt, env, args.asan))
