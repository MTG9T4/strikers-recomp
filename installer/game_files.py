"""The game's files from folders the player extracted from their own Switch. GPL-3.0-or-later.

  python game_files.py check <exefs folder> <romfs folder>
  python game_files.py copy <exefs folder> <romfs folder> <dump folder>

This project reads no game package and no console key, and decrypts nothing. The player extracts, with a tool on their
own console (nxdumptool, for example):
  - the ExeFS of the 1.3.2 update: main, main.npdm, rtld, sdk, subsdk0;
  - the game's RomFS with the 1.3.2 update applied (Characters, FrontEnd, Movies, wwiseaudio, global.data, ...).
A folder that holds exefs/ and romfs/ folders may be chosen for either.

check: the ExeFS is this game's 1.3.2 program (the program id in main.npdm, the build id of main, the segment hashes of
every module) and the RomFS is the 1.3.2 data (its file count and size). copy: both into <dump>/exefs and <dump>/romfs
(build/dump/update), each file hard-linked when the folders are on the same drive (no second 3 GB), copied otherwise.
The player's folders are only read.
"""
import os
from pathlib import Path, PurePosixPath
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/extract'))

PROGRAM_ID = 0x010019401051C000
MAIN_BUILD_ID = '2e90f0815b1f92a1cbf013b3f4d358f41d729f4e'     # main of update v393216 (1.3.2)
EXEFS = ('main', 'main.npdm', 'rtld', 'sdk', 'subsdk0')
ROMFS_FILES, ROMFS_BYTES = 1810, 2901352124                     # the base RomFS with update v393216 applied
PROTECTED = {'installer', 'build', 'toolchain', 'userdata'}      # setup writes there


def progress(percent, text):
    print(f'@phase8 {percent}|{text}', flush=True)


def locate(exefs, romfs):
    """The two folders, also when the player chose a folder that holds exefs/ and romfs/."""
    exefs, romfs = Path(exefs).resolve(strict=True), Path(romfs).resolve(strict=True)
    if not (exefs / 'main.npdm').is_file() and (exefs / 'exefs/main.npdm').is_file():
        exefs = exefs / 'exefs'
    if not (romfs / 'global.data').is_file() and (romfs / 'romfs').is_dir():
        romfs = romfs / 'romfs'
    if not exefs.is_dir() or not romfs.is_dir():
        raise ValueError('Choose the ExeFS and RomFS folders you extracted from your own Switch')
    return exefs, romfs


def readonly(folders, root):
    """Refuse folders that setup's own output could overwrite or that would copy the installation into itself."""
    root = Path(root).resolve()
    for folder in folders:
        if folder == root or folder in root.parents:
            raise ValueError('Choose extracted folders outside this setup folder')
        if root in folder.parents and folder.relative_to(root).parts[0].lower() in PROTECTED:
            raise ValueError('Choose your extracted folders outside the installer, build, toolchain and userdata folders')


def safe_name(relative):
    parts = PurePosixPath(relative).parts
    if not parts or '..' in parts or any(any(ord(c) < 32 or c in ':<>"|?*' for c in p) for p in parts):
        raise ValueError('The RomFS folder contains an unsafe file name')
    return relative


def romfs_files(romfs):
    """(relative name, full path, size) of every file, in a fixed order."""
    found = []
    for folder, dirs, files in os.walk(romfs):
        dirs.sort()
        if any(Path(folder, d).is_symlink() for d in dirs):
            raise ValueError('The RomFS folder contains a link; extract it again as plain files')
        for name in sorted(files):
            path = Path(folder, name)
            if path.is_symlink():
                raise ValueError('The RomFS folder contains a link; extract it again as plain files')
            found.append((safe_name(path.relative_to(romfs).as_posix()), path, path.stat().st_size))
    return found


def check(exefs, romfs):
    import nso
    missing = [n for n in EXEFS if not (exefs / n).is_file()]
    if missing:
        raise ValueError('The ExeFS folder has no ' + ', '.join(missing) + '; extract the 1.3.2 update\'s ExeFS again')
    try:
        program = nso.npdm_program_id((exefs / 'main.npdm').read_bytes())
    except Exception:
        raise ValueError('The ExeFS folder\'s main.npdm could not be read; extract it again') from None
    if program != PROGRAM_ID:
        raise ValueError('The ExeFS folder is for a different game')
    builds = {}
    for module in ('main', 'rtld', 'sdk', 'subsdk0'):
        try:
            info = nso.nso_info((exefs / module).read_bytes())
        except Exception:
            raise ValueError('The ExeFS file %s is damaged; extract it again' % module) from None
        if not all(s['sha_ok'] for s in info['segments']):
            raise ValueError('The ExeFS file %s is damaged; extract it again' % module)
        builds[module] = info['build_id'].rstrip('0') or '0'
    if builds['main'] != MAIN_BUILD_ID:
        raise ValueError('The ExeFS folder is not from update 1.3.2 (v393216); extract the ExeFS of that update')
    files = romfs_files(romfs)
    size = sum(s for _, _, s in files)
    if (len(files), size) != (ROMFS_FILES, ROMFS_BYTES):
        raise ValueError('The RomFS folder is not the game\'s data with update 1.3.2 applied (%d files, %d MB); '
                         'extract the RomFS again with the update' % (len(files), size >> 20))
    return dict(title_id=f'{PROGRAM_ID:016X}', version=393216, main_build_id=builds['main'], romfs_files=len(files))


def place(source, target):
    """A hard link when the drive allows it, else a copy. An existing target is replaced."""
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        target.unlink()
    try:
        os.link(source, target)
    except OSError:
        shutil.copyfile(source, target)


def copy(exefs, romfs, dump):
    dump = Path(dump)
    for name in EXEFS:
        place(exefs / name, dump / 'exefs' / name)
    files = romfs_files(romfs)
    total, done, shown = sum(s for _, _, s in files) or 1, 0, -1
    out = dump / 'romfs'
    for relative, path, size in files:
        place(path, out / relative)
        done += size
        if done * 20 // total != shown:
            shown = done * 20 // total
            progress(10 + shown * 17 // 20, f'Reading your game files ({done >> 20} of {total >> 20} MB)')
    # Nothing else may stay in the dump's RomFS: the game reads it as the whole RomFS.
    keep = {out / r for r, _, _ in files}
    for path in sorted(out.rglob('*'), reverse=True):
        if path.is_file() and path not in keep:
            path.unlink()


def main():
    args = sys.argv[1:]
    if not ((args[:1] == ['check'] and len(args) == 3) or (args[:1] == ['copy'] and len(args) == 4)):
        raise SystemExit(__doc__)
    exefs, romfs = locate(args[1], args[2])
    identity = check(exefs, romfs)
    print('game files: %s, main %s, %d RomFS files' % (identity['title_id'], identity['main_build_id'][:16],
                                                       identity['romfs_files']), flush=True)
    if args[0] == 'copy':
        copy(exefs, romfs, args[3])
        print('game files: copied to ' + os.path.basename(args[3].rstrip('\\/')), flush=True)


if __name__ == '__main__':
    main()
