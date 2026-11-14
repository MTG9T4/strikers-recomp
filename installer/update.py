"""In-game updates: check for a newer release and install it in place. Run by the launcher and by StrikersSetup.exe --update.

  python -I -X utf8 -B installer/update.py check   one JSON line: {"available": true, "version": ..., "minutes": ...}
  python -I -X utf8 -B installer/update.py apply   @phase8 progress lines, @error on failure

The release list (release/latest.json) comes from the group service named in strikers.ini and is signed with the
organizer's Ed25519 key; the public half is below. Every file is checked against its SHA-256 from that list before
it is used. Nothing is sent but the requests themselves. Game files, saves, settings, the shader cache and the
player's own build stay as they are. When an update changes what the game is built from, the game is rebuilt here
from the game files already in build/dump: the player picks nothing again.
"""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(ROOT / 'tools/release'))

PUBLIC_KEY = 'ac0d39ad8a2a33f2fdee3bbfec14d54b24735c476ab8e2dbd6c57fa68ccfcc18'
WORK = ROOT / 'installer/update'
PENDING = WORK / 'pending.json'
INSTALLED = ROOT / 'installer/installed-release.json'
KEEP = {'strikers.ini'}                     # the player's settings; F1 rewrites it
NEVER = ('userdata/', 'build/', 'cache/', 'installer/sandbox/', 'installer/update/', 'toolchain/python/')
# What the player's game DLLs are built from (build_player.py, build_player_sdk.py and the steps before them).
BUILD_INPUTS = ('tools/lifter/', 'tools/extract/', 'scripts/', 'tools/target.py', 'toolchain/llvm/', 'runtime/core/game_shim_eh.c',
                'tools/release/build_player.py', 'tools/release/build_player_sdk.py', 'tools/release/player_crt.c',
                'tools/release/sdk_player_shim.c', 'tools/release/sdk_player_min.h', 'tools/release/include/', 'tools/release/defs/')
WARM_INPUTS = ('strikers_native.exe', 'installer/shader-list.json', 'installer/warmup.py')
BUILD_STEPS = [('functions', ['tools/extract/functions.py', '--no-review']), ('imports', ['tools/extract/imports.py']),
               ('lift', ['tools/lifter/recomp.py', 'full']), ('tables', ['tools/lifter/gen_tables.py']),
               ('sdk', ['tools/lifter/sdklift.py', 'gen'])]


def progress(percent, text):
    print(f'@phase8 {percent}|{text}', flush=True)


def version_tuple(text):
    return tuple(int(x) for x in re.findall(r'\d+', str(text))[:4])


def sha256(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def service():
    """The base URL: strikers.ini's group_service. STRIKERS_UPDATE_BASE is for tests on this PC only."""
    test = os.environ.get('STRIKERS_UPDATE_BASE', '')
    if test:
        if not re.fullmatch(r'http://127\.0\.0\.1:\d{2,5}', test):
            raise ValueError('STRIKERS_UPDATE_BASE takes a local test address only')
        return test
    for line in (ROOT / 'strikers.ini').read_text(encoding='utf-8', errors='replace').splitlines():
        m = re.match(r'\s*group_service\s*=\s*(\S+)', line)
        if m and re.fullmatch(r'https://[A-Za-z0-9.-]+', m[1].rstrip('/')):
            return m[1].rstrip('/')
    raise ValueError('No update service is set in strikers.ini')


AGENT = 'StrikersRecomp-updater'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
SERVER_AUTH = '1.3.6.1.5.5.7.3.1'


class CertificateRefused(ValueError):
    """This way of checking the site's certificate refused it; another way may not."""


def fetch_curl(curl, url, timeout, limit):
    result = subprocess.run([str(curl), '-sS', '-L', '--fail', '--proto', '=https,http', '--max-time', str(timeout),
                             '--max-filesize', str(limit), '-A', AGENT, url],
                            capture_output=True, timeout=timeout + 10, creationflags=NO_WINDOW)
    if result.returncode:
        raise ValueError('The update service could not be reached (curl ' + str(result.returncode) + ')')
    return result.stdout


def windows_roots():
    """Python's own TLS check, trusting only what Windows trusts as a root for web servers. ssl.create_default_context()
    also trusts Windows' store of intermediates ('CA'), which on many PCs still holds an old 'ISRG Root X2' signed by
    ISRG Root X1 that expired in September 2025. OpenSSL takes that stored copy over the current one the server sends
    and refuses the site as expired (seen from November 2026, when the service's certificate moved to that chain)."""
    import ssl
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)          # checks the certificate and the host name
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    roots = 0
    for der, encoding, trust in (ssl.enum_certificates('ROOT') if hasattr(ssl, 'enum_certificates') else []):
        if encoding == 'x509_asn' and (trust is True or SERVER_AUTH in trust):
            try:
                context.load_verify_locations(cadata=der)
                roots += 1
            except ssl.SSLError:
                pass
    if not roots:
        context.load_default_certs()
    return context


def fetch_python(url, timeout, limit):
    import ssl
    import urllib.error
    request = urllib.request.Request(url, headers={'User-Agent': AGENT})
    try:
        with urllib.request.urlopen(request, timeout=timeout, context=windows_roots()) as response:
            return response.read(limit + 1)
    except urllib.error.URLError as error:
        if isinstance(error.reason, ssl.SSLCertVerificationError):
            raise CertificateRefused('The update service\'s certificate was refused (' + str(error.reason.verify_message) + ')')
        raise


def fetch_powershell(url, timeout, limit):
    """Windows' own certificate check (it fetches a missing root from Windows Update), for Windows without curl.
    The address goes in through the environment, never through the command line."""
    import tempfile
    handle, out = tempfile.mkstemp(prefix='strikers-update-')
    os.close(handle)
    script = ("$ProgressPreference='SilentlyContinue';$ErrorActionPreference='Stop';"
              "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;"
              "Invoke-WebRequest -UseBasicParsing -Uri $env:STRIKERS_FETCH_URL -OutFile $env:STRIKERS_FETCH_OUT"
              " -UserAgent $env:STRIKERS_FETCH_AGENT -TimeoutSec " + str(int(timeout)))
    shell = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
    try:
        result = subprocess.run([str(shell), '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', script],
                                env=dict(os.environ, STRIKERS_FETCH_URL=url, STRIKERS_FETCH_OUT=out, STRIKERS_FETCH_AGENT=AGENT),
                                capture_output=True, timeout=timeout + 30, creationflags=NO_WINDOW)
        if result.returncode:
            raise ValueError('The update service could not be reached (PowerShell ' + str(result.returncode) + ')')
        with open(out, 'rb') as f:
            return f.read(limit + 1)
    finally:
        try:
            os.unlink(out)
        except OSError:
            pass


def fetch(url, timeout, limit):
    """Windows' own curl (Schannel, the system's certificates): Python's own TLS check fails behind some antivirus
    web filters. Without curl (Windows 10 before 1803): Python against Windows' roots, then PowerShell when that
    refuses the certificate."""
    curl = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/curl.exe'
    if curl.is_file():
        data = fetch_curl(curl, url, timeout, limit)
    else:
        try:
            data = fetch_python(url, timeout, limit)
        except CertificateRefused:
            data = fetch_powershell(url, timeout, limit)
    if len(data) > limit:
        raise ValueError('An update file is larger than its list says')
    return data


def latest(timeout):
    """The signed release list, or an exception."""
    from Crypto.Signature import eddsa
    base = service()
    data = fetch(base + '/release/latest.json', timeout, 4 << 20)
    signature = bytes.fromhex(fetch(base + '/release/latest.sig', timeout, 1024).decode().strip())
    key = eddsa.import_public_key(bytes.fromhex(PUBLIC_KEY))
    eddsa.new(key, 'rfc8032').verify(data, signature)        # raises ValueError when it is not the organizer's
    release = json.loads(data)
    if release.get('schema') != 1 or not isinstance(release.get('files'), dict):
        raise ValueError('Unknown release list')
    return base, release


def installed_version():
    for path, key in ((INSTALLED, 'version'), (ROOT / 'release-manifest.json', 'version')):
        try:
            value = json.loads(path.read_text()).get(key)
            if value:
                return value
        except (OSError, ValueError):
            pass
    return '0'


def safe(name):
    p = PurePosixPath(name)
    if p.is_absolute() or '..' in p.parts or '\\' in name or ':' in name or not name:
        raise ValueError('Unsafe path in the release list: ' + name)
    return name


def plan(release):
    """What changes: files to fetch, files to remove, and whether to rebuild or warm up."""
    local = json.loads((ROOT / 'release-manifest.json').read_text()).get('files', {})
    fetch_list, remove = [], []
    for name, meta in sorted(release['files'].items()):
        safe(name)
        if name in KEEP:
            continue
        if name.startswith(NEVER):
            if name.startswith('toolchain/python/') and (ROOT / name).is_file() and sha256(ROOT / name) != meta['sha256']:
                raise ValueError('This update replaces the build tools; extract the new release into a new folder instead')
            continue
        target = ROOT / name
        if not target.is_file() or sha256(target) != meta['sha256']:
            fetch_list.append(name)
    for name in sorted(local):
        if name not in release['files'] and name not in KEEP and not name.startswith(NEVER):
            safe(name)
            if (ROOT / name).is_file():
                remove.append(name)
    changed = fetch_list + remove
    rebuild = any(n.startswith(BUILD_INPUTS) for n in changed)
    warm = rebuild or any(n in WARM_INPUTS or n.startswith('runtime/gfx/') for n in changed)
    return dict(version=release['version'], fetch=fetch_list, remove=remove, rebuild=rebuild, warm=warm,
                bytes=sum(release['files'][n]['size'] for n in fetch_list))


def quick_plan(release):
    """plan() without reading the installed files: the check runs while the game starts, and hashing every file then took
    longer than the launcher waits. The installed release list says what is there; apply() still checks every file."""
    local = json.loads((ROOT / 'release-manifest.json').read_text()).get('files', {})
    changed = [n for n, m in release['files'].items() if n not in KEEP and not n.startswith(NEVER) and local.get(n) != m['sha256']]
    changed += [n for n in local if n not in release['files'] and n not in KEEP and not n.startswith(NEVER)]
    rebuild = any(n.startswith(BUILD_INPUTS) for n in changed)
    return dict(fetch=changed, remove=[], rebuild=rebuild,
                warm=rebuild or any(n in WARM_INPUTS or n.startswith('runtime/gfx/') for n in changed),
                bytes=sum(release['files'][n]['size'] for n in changed if n in release['files']))


def minutes(p):
    return max(1, round(p['bytes'] / (4 << 20) / 60 + (6 if p['rebuild'] else 0) + (2 if p['warm'] else 0)))


def cleanup():
    """Program files replaced by the last update; a running exe can be renamed but not deleted."""
    for path in ROOT.glob('**/*.old-update'):
        if path.relative_to(ROOT).parts[0] in ('userdata', 'build', 'cache'):
            continue
        try:
            path.unlink()
        except OSError:
            pass


def wrong_cpu():
    """build/cpu-level.txt names a level this PC lacks: the install was moved from a newer PC."""
    try:
        built = (ROOT / 'build/cpu-level.txt').read_text().strip()
    except OSError:
        return False
    from player_setup import cpu_level
    order = ['base', 'v2', 'v3']
    return built in order and order.index(built) > order.index(cpu_level())


def check():
    try:
        cleanup()
        if PENDING.exists() or wrong_cpu():      # the game cannot run until it is finished (or rebuilt for this CPU)
            version = json.loads(PENDING.read_text())['version'] if PENDING.exists() else installed_version()
            print(json.dumps(dict(available=True, version=version, minutes=8, pending=True)))
            return
        _, release = latest(4)
        if version_tuple(release['version']) <= version_tuple(installed_version()):
            print(json.dumps(dict(available=False, version=installed_version())))
            return
        p = quick_plan(release)
        print(json.dumps(dict(available=True, version=release['version'],
                              minutes=minutes(p), notes=str(release.get('notes', ''))[:300])))
    except Exception as error:
        print(json.dumps(dict(available=False, error=type(error).__name__)))


def download(base, release, names, start, span):
    WORK.mkdir(parents=True, exist_ok=True)
    total = sum(release['files'][n]['size'] for n in names) or 1
    done = 0
    for name in names:
        meta = release['files'][name]
        staged = WORK / meta['sha256']
        if not (staged.is_file() and sha256(staged) == meta['sha256']):
            part = staged.with_suffix('.part')
            with part.open('wb') as out:
                for piece in meta.get('parts') or [meta['sha256']]:
                    if not re.fullmatch(r'[0-9a-f]{64}(\.\d+)?', piece):
                        raise ValueError('Unsafe file name in the release list')
                    out.write(fetch(f'{base}/release/files/{piece}', 120, meta['size']))
            if sha256(part) != meta['sha256'] or part.stat().st_size != meta['size']:
                part.unlink()
                raise ValueError('A downloaded file did not pass its safety check; try the update again')
            part.replace(staged)
        done += meta['size']
        progress(start + span * done // total, f'Downloading the update ({done >> 20} of {total >> 20} MB)…')


def swap(release, p):
    """Every file moved into place, or none: the old ones are renamed first and put back on a failure."""
    moved = []                                   # (target, its old copy or None)
    try:
        for name in p['fetch'] + p['remove']:
            target = ROOT / name
            old = target.with_name(target.name + '.old-update')
            if target.exists():
                if old.exists():
                    old.unlink()
                target.rename(old)
            moved.append((target, old if old.exists() else None))
            if name in p['fetch']:
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(WORK / release['files'][name]['sha256'], target)
    except Exception:
        for target, old in reversed(moved):
            if target.exists():
                target.unlink()
            if old is not None:
                old.rename(target)
        raise


def build(env, report):
    """The player's game DLLs again, from the game files already unpacked by setup."""
    work = ROOT / 'build'
    sys.path.insert(0, str(ROOT / 'installer'))
    from player_setup import compile_jobs
    steps = BUILD_STEPS + [('compile', ['tools/release/build_player.py', '--llvm', str(ROOT / 'toolchain/llvm'),
                                        '--output', str(work / 'player-dll'), '--jobs', str(compile_jobs()), '--link-existing']),
                           ('sdk compile', ['tools/release/build_player_sdk.py', '--llvm', str(ROOT / 'toolchain/llvm'),
                                            '--output', str(work / 'player-sdk')])]
    if not (work / 'dump/update/elf/main.elf').is_file():
        raise ValueError('This copy has no game files to rebuild from; extract the new release into a new folder')
    if (work / 'player-sdk').exists():
        shutil.rmtree(work / 'player-sdk')
    for i, (name, command) in enumerate(steps):
        report(i, len(steps), name)
        with (work / ('update-' + name.replace(' ', '-') + '.log')).open('w', encoding='utf-8') as log:
            child = subprocess.Popen([sys.executable, '-I', '-X', 'utf8', '-B', *command], cwd=ROOT, env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                                     errors='replace', creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            for line in child.stdout:
                log.write(line)
            if child.wait():
                raise subprocess.CalledProcessError(child.returncode, command)
    shutil.copyfile(work / 'player-dll/strikers_game.dll', ROOT / 'strikers_game.dll')
    shutil.copyfile(work / 'player-sdk/strikers_sdk.dll', ROOT / 'strikers_sdk.dll')


def game_art():
    """Copies installed before 1.0.11 had the icon and start-up picture built into the programs; the project no longer ships
    any artwork. Read them once from the player's own files (installer/game_art.py) while those are still where setup found
    them, and give the shortcut the icon. Cosmetic: nothing here stops an update."""
    art = ROOT / 'build/game-art'
    if (art / 'icon.ico').is_file():
        return
    try:
        selection = json.loads((ROOT / 'build/player-state.json').read_text())['selection']
        sys.path.insert(0, str(ROOT / 'installer'))
        import game_art as maker
        from shortcut import create_shortcut
        read = None
        if 'exefs' in selection:                 # the folders the player extracted (installer/game_files.py)
            read = lambda: maker.icon_in_folders([selection[k]['path'] for k in ('exefs', 'romfs')])
        if read is None:
            return
        maker.make(read(), art)
        link = ROOT / 'Strikers Recomp.lnk'
        if link.exists():
            create_shortcut(link, ROOT / 'StrikersRecomp.exe', ROOT, art / 'icon.ico')
    except (Exception, SystemExit) as error:
        print('game art: skipped (%s)' % type(error).__name__, flush=True)


def apply():
    from common import clean_environment
    import msvcrt
    (ROOT / 'build').mkdir(exist_ok=True)
    with (ROOT / 'build/setup.lock').open('a+b') as lock:
        lock.seek(0); lock.write(b'0'); lock.flush(); lock.seek(0)
        msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        cleanup()
        if not PENDING.exists() and wrong_cpu():  # moved from a newer PC: the same version, built again for this CPU
            WORK.mkdir(parents=True, exist_ok=True)
            PENDING.write_text(json.dumps(dict(version=installed_version(), fetch=[], remove=[], rebuild=True, warm=False)))
        if PENDING.exists():                     # an update whose files are in place; its rebuild or warm-up did not finish
            p = json.loads(PENDING.read_text())
            progress(40, f'Finishing the update to {p["version"]}…')
        else:
            progress(2, 'Checking for the update…')
            base, release = latest(20)
            if version_tuple(release['version']) <= version_tuple(installed_version()):
                progress(100, 'This copy is up to date.')
                return
            p = plan(release)
            download(base, release, p['fetch'], 5, 30)
            progress(36, 'Installing the update…')
            WORK.mkdir(parents=True, exist_ok=True)
            PENDING.write_text(json.dumps(p))
            try:
                swap(release, p)
            except Exception:
                PENDING.unlink()
                raise
            INSTALLED.write_text(json.dumps(dict(version=release['version'], files=release['files']), indent=1))
        env = clean_environment(ROOT / 'installer/sandbox', ROOT / 'toolchain/llvm')
        if p['rebuild']:
            build(env, lambda i, n, name: progress(40 + 50 * i // n, f'Rebuilding your PC game ({i + 1} of {n}) — roughly {max(1, 6 - i)} minutes remaining (estimate)'))
        if p['warm']:
            import warmup
            warmup.run(ROOT, {k: v for k, v in env.items() if not k.startswith('STRIKERS_')},
                       lambda m: progress(92, f'Preparing your graphics — roughly {m} minutes remaining (estimate)'))
        game_art()
        PENDING.unlink()
        shutil.rmtree(WORK, ignore_errors=True)
        progress(100, f'Updated to {p["version"]}.')


if __name__ == '__main__':
    if sys.argv[1:] == ['check']:
        check()
    elif sys.argv[1:] == ['apply']:
        try:
            apply()
        except BaseException as error:
            print('Update failure type: ' + type(error).__name__ + ': ' + str(error)[:300], flush=True)
            if PENDING.exists():
                message = 'The update is installed but not finished; open the game again to finish it.'
            elif isinstance(error, ValueError):
                message = str(error).rstrip('.; ') + '.'
            else:
                message = 'The update could not finish; your game is unchanged. Try again later.'
            print('@error ' + message, flush=True)
            raise SystemExit(1)
    else:
        raise SystemExit('usage: update.py check|apply')
