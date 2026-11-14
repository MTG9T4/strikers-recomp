"""Project-authored release utilities. GPL-3.0-or-later."""
import hashlib
import os
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def sha256(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def within(root, path):
    root, path = Path(root).resolve(), Path(path).resolve()
    if path == root or root not in path.parents:
        raise ValueError('Output must be inside its designated folder')
    # resolve follows existing junctions/symlinks before the containment check.
    return path


def clean_environment(folder, llvm):
    """An allowlisted child environment; never inherits developer search paths."""
    folder = Path(folder).resolve()
    tmp = folder / 'temp'
    tmp.mkdir(parents=True, exist_ok=True)
    win = Path(os.environ.get('SystemRoot', r'C:\Windows'))
    return dict(SystemRoot=str(win), WINDIR=str(win), COMSPEC=str(win / 'System32/cmd.exe'),
                PATH=os.pathsep.join((str(Path(llvm).resolve() / 'bin'), str(win / 'System32'))),
                TEMP=str(tmp), TMP=str(tmp), USERPROFILE=str(folder), HOME=str(folder),
                APPDATA=str(folder / 'profile/roaming'), LOCALAPPDATA=str(folder / 'profile/local'),
                PYTHONNOUSERSITE='1', PYTHONDONTWRITEBYTECODE='1', LLVM_ROOT=str(Path(llvm).resolve()))


def run(command, env, cwd, log=None):
    result = subprocess.run([str(x) for x in command], cwd=cwd, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace',
                            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if log:
        Path(log).write_text(result.stdout, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(f'{Path(command[0]).name} failed ({result.returncode}): {result.stdout[-2500:]}')
    return result.stdout


def safe_unzip(archive, destination):
    destination = Path(destination).resolve()
    with zipfile.ZipFile(archive) as z:
        for entry in z.infolist():
            if '\\' in entry.filename or ':' in entry.filename or entry.filename.startswith('/'):
                raise ValueError('Unsafe archive path')
            within(destination, destination / entry.filename)
            if (entry.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError('Archive symlinks are forbidden')
        z.extractall(destination)
