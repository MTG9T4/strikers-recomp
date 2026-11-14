"""Download the build tools into deps/, each checked against a known SHA-256.

Hashes for SDL3, Vulkan-Headers, glslang and imgui are the ones ssbu-recomp's scripts/install.py checked on 2026-10-19.
LLVM 9.0.0 is the official installer from releases.llvm.org (its hash matches the prefix/suffix ssbu-recomp recorded
for the signed file), unpacked with 7-Zip so no administrator prompt is needed.
libopus 1.5.2 is xiph's release tarball; its hash matches xiph's SHA256SUMS.txt (checked 2026-11-04). It is source only:
the runtime build compiles it (no DNN/DRED features, so no model download).
libzt 1.8.10 (ZeroTier's embeddable SDK, built into the game for online play with nothing to install) and its three
submodules are GitHub archives of the exact commits libzt 1.8.10 pins. Licenses checked 2026-11-05: libzt BSL 1.1 with
Change Date 2026-01-01 and ZeroTierOne (a47c3cb) BSL 1.1 with Change Date 2025-01-01, both now Apache-2.0
(GPL-3.0 compatible); lwIP and lwip-contrib BSD-style.
Ghidra comes from NSA's GitHub release, checked against the digest GitHub publishes for the asset. It is pinned to
12.1.4 because rea (the reverse-engineering MCP) accepts only Ghidra 12.1.x.

Usage: python -I scripts/fetch_deps.py [name ...]
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = os.path.join(ROOT, 'deps')
DL = os.path.join(DEPS, '_downloads')

# name -> (url, sha256, folder in deps, file that proves the folder is complete)
TOOLS = {
    'sdl3': ('https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-devel-3.4.18-VC.zip',
             '78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd', 'SDL3', 'include/SDL3/SDL.h'),
    'vulkan-headers': ('https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.329.zip',
                       '0f55079543cfbf023bda65f81bd2d5a1bab581f31e5c33cf9fd781bd754ebaba', 'Vulkan-Headers',
                       'include/vulkan/vulkan.h'),
    'glslang': ('https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip',
                '82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b', 'glslang',
                'include/glslang/Include/glslang_c_interface.h'),
    'imgui': ('https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip',
              'fd37507c8476a6d14cc7c4b352401f31bcbd0f0d995d35390811e968c466f46e', 'imgui', 'imgui.h'),
    'llvm': ('https://releases.llvm.org/9.0.0/LLVM-9.0.0-win64.exe',
             '71ed99a6720945d76b6772584735f2ed472f12719dff184a0b1a6a35047b8863', 'llvm', 'bin/clang.exe'),
    'opus': ('https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz',
             '65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1', 'opus', 'include/opus.h'),
}


# libzt and its submodules: (url, sha256, path inside deps/libzt)
LIBZT = [
    ('https://github.com/zerotier/libzt/archive/a6f6886a8c8c07249f9e5d0cae431848727fe845.zip',
     '3e12160ff8ce9ec835cca6f17db73cf4f8e783ec3aac0423e52900b058b635a4', ''),
    ('https://github.com/zerotier/ZeroTierOne/archive/a47c3cb16f1c16bc9be555fd7f6bcd34e843f001.zip',
     '080984fcc9fd94565721706136f4e1c75b959a57183a89bff23fe67ed22ade84', 'ext/ZeroTierOne'),
    ('https://github.com/joseph-henry/lwip/archive/32708c0a8b140efb545cc35101ee5fdeca6d6489.zip',
     '018afe8ef6e508e557037dcd32acc7ed5620daed29dca74ef6556a9d5e602ea9', 'ext/lwip'),
    ('https://github.com/joseph-henry/lwip-contrib/archive/4fd612c9c72dfcd1db6618bd59c1a17d9f5b55f8.zip',
     'c025ba3d469f79c98e34aaaac491c229f8a8c54b6e3d892d83953b40a12637a1', 'ext/lwip-contrib'),
]

GHIDRA_TAG = 'Ghidra_12.1.4_build'


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def download(url, dst):
    if os.path.isfile(dst):
        return
    print('  downloading', url)
    with urllib.request.urlopen(url) as r, open(dst + '.part', 'wb') as f:
        shutil.copyfileobj(r, f, 1 << 20)
    os.replace(dst + '.part', dst)


def unpack_zip(arc, dst):
    """Unpack a zip or .tar.gz into dst, dropping a single top-level folder if the archive has one."""
    tmp = dst + '.tmp'
    shutil.rmtree(tmp, ignore_errors=True)
    if arc.endswith('.tar.gz'):
        with tarfile.open(arc) as t:
            t.extractall(tmp, filter='data')
    else:
        with zipfile.ZipFile(arc) as z:
            z.extractall(tmp)
    entries = os.listdir(tmp)
    src = os.path.join(tmp, entries[0]) if len(entries) == 1 and os.path.isdir(os.path.join(tmp, entries[0])) else tmp
    shutil.rmtree(dst, ignore_errors=True)
    shutil.move(src, dst)
    shutil.rmtree(tmp, ignore_errors=True)


def find_7z():
    for p in (shutil.which('7z'), r'C:\Program Files\7-Zip\7z.exe'):
        if p and os.path.isfile(p):
            return p
    raise SystemExit('7-Zip not found (winget install 7zip.7zip)')


def fetch_tool(name):
    url, want, folder, proof = TOOLS[name]
    dst = os.path.join(DEPS, folder)
    if os.path.isfile(os.path.join(dst, proof)):
        print('%s: already in deps/%s' % (name, folder))
        return
    arc = os.path.join(DL, url.rsplit('/', 1)[1])
    download(url, arc)
    got = sha256(arc)
    if got != want:
        os.remove(arc)
        raise SystemExit('%s: SHA-256 mismatch (%s), download removed' % (name, got))
    if arc.endswith('.exe'):                       # LLVM's NSIS installer: unpack its files with 7-Zip
        shutil.rmtree(dst, ignore_errors=True)
        subprocess.check_call([find_7z(), 'x', '-y', '-o' + dst, arc], stdout=subprocess.DEVNULL)
        shutil.rmtree(os.path.join(dst, '$PLUGINSDIR'), ignore_errors=True)
    else:
        unpack_zip(arc, dst)
    if not os.path.isfile(os.path.join(dst, proof)):
        raise SystemExit('%s: unpacked but %s is missing' % (name, proof))
    print('%s: ok (sha256 %s) -> deps/%s' % (name, got, folder))


def fetch_libzt():
    dst = os.path.join(DEPS, 'libzt')
    if os.path.isfile(os.path.join(dst, 'ext', 'ZeroTierOne', 'node', 'Node.cpp')) and os.path.isfile(os.path.join(dst, 'include', 'ZeroTierSockets.h')):
        print('libzt: already in deps/libzt')
        return
    shutil.rmtree(dst, ignore_errors=True)
    for url, want, sub in LIBZT:
        arc = os.path.join(DL, 'libzt-' + (sub.replace('/', '_') or 'root') + '-' + url.rsplit('/', 1)[1])
        download(url, arc)
        got = sha256(arc)
        if got != want:
            os.remove(arc)
            raise SystemExit('libzt %s: SHA-256 mismatch (%s), download removed' % (sub or 'root', got))
        target = os.path.join(dst, *sub.split('/')) if sub else dst
        if sub:
            shutil.rmtree(target, ignore_errors=True)       # the root archive has empty submodule folders
        unpack_zip(arc, target)
    print('libzt: 1.8.10 + ZeroTierOne/lwip/lwip-contrib ok -> deps/libzt')


def fetch_ghidra():
    dst = os.path.join(DEPS, 'ghidra')
    if os.path.isfile(os.path.join(dst, 'ghidraRun.bat')):
        print('ghidra: already in deps/ghidra')
        return
    with urllib.request.urlopen('https://api.github.com/repos/NationalSecurityAgency/ghidra/releases/tags/' + GHIDRA_TAG) as r:
        rel = json.load(r)
    asset = next(a for a in rel['assets'] if a['name'].endswith('.zip') and 'PUBLIC' in a['name'])
    digest = (asset.get('digest') or '').removeprefix('sha256:')
    if not digest:
        raise SystemExit('ghidra: GitHub gives no digest for %s; refusing' % asset['name'])
    arc = os.path.join(DL, asset['name'])
    download(asset['browser_download_url'], arc)
    got = sha256(arc)
    if got != digest:
        os.remove(arc)
        raise SystemExit('ghidra: SHA-256 mismatch (%s), download removed' % got)
    unpack_zip(arc, dst)
    print('ghidra: %s ok (sha256 %s) -> deps/ghidra' % (rel['tag_name'], got))


def main(names):
    os.makedirs(DL, exist_ok=True)
    for name in names or list(TOOLS) + ['libzt', 'ghidra']:
        if name == 'ghidra':
            fetch_ghidra()
        elif name == 'libzt':
            fetch_libzt()
        else:
            fetch_tool(name)


if __name__ == '__main__':
    main(sys.argv[1:])
