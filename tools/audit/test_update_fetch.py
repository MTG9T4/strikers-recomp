"""The updater's three ways to fetch (installer/update.py fetch): curl, Python against Windows' roots, PowerShell.

  .venv/Scripts/python -I tools/audit/test_update_fetch.py           local checks (a test server on 127.0.0.1)
  .venv/Scripts/python -I tools/audit/test_update_fetch.py --live https://<group service>    also the live service, read only

--live fetches release/latest.json and latest.sig each way, checks they match and carry the organizer's signature,
and says whether this PC's Windows keeps the expired 'ISRG Root X2' copy that makes Python's default check refuse
the site (the case the Windows-roots check is for).
"""
import http.server
import os
from pathlib import Path
import ssl
import sys
import threading
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'installer'))
import update  # noqa: E402

fails = 0


def check(ok, what):
    global fails
    print(('ok   ' if ok else 'FAIL ') + what, flush=True)
    fails += 0 if ok else 1


def raises(kind, call):
    try:
        call()
    except kind:
        return True
    except Exception as error:
        print('     raised', type(error).__name__, error)
    return False


BODY = b'strikers update test ' * 50


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = BODY if self.path.split('?')[0] == '/file' else b''
        self.send_response(200 if body else 404)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def local():
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_port}'
    try:
        context = update.windows_roots()
        check(context.verify_mode == ssl.CERT_REQUIRED and context.check_hostname, 'Windows roots: certificates and host names are checked')
        check(len(context.get_ca_certs()) > 0, f'Windows roots: {len(context.get_ca_certs())} trusted roots loaded')
        check(len(context.get_ca_certs()) <= len(ssl.enum_certificates('ROOT')), 'Windows roots: nothing from the intermediates store')
        check(update.fetch_python(base + '/file', 5, 4096) == BODY, 'Python: fetches a file')
        check(raises(urllib.error.HTTPError, lambda: update.fetch_python(base + '/missing', 5, 4096)), 'Python: a missing file is an error')
        check(len(update.fetch_python(base + '/file', 5, 100)) == 101, 'Python: reads at most one byte past the limit')
        check(update.fetch_powershell(base + '/file', 10, 4096) == BODY, 'PowerShell: fetches a file')
        check(raises(ValueError, lambda: update.fetch_powershell(base + '/missing', 10, 4096)), 'PowerShell: a missing file is an error')
        before = set(Path(os.environ.get('TEMP', '.')).glob('strikers-update-*'))
        update.fetch_powershell(base + '/file', 10, 4096)
        check(set(Path(os.environ.get('TEMP', '.')).glob('strikers-update-*')) == before, 'PowerShell: leaves no file behind')
        odd = base + "/file?x=$(exit)';exit;'"            # run as script, this would end PowerShell before fetching
        check(update.fetch_powershell(odd, 10, 4096) == BODY, 'PowerShell: the address is data, not script')

        # fetch(): which way runs
        calls = []
        saved = (update.fetch_curl, update.fetch_python, update.fetch_powershell, os.environ.get('SystemRoot'))
        def fake(name, result):
            def run(*args):
                calls.append(name)
                if isinstance(result, Exception):
                    raise result
                return result
            return run
        try:
            update.fetch_curl, update.fetch_python, update.fetch_powershell = fake('curl', b'c'), fake('python', b'p'), fake('ps', b's')
            calls.clear(); check(update.fetch('u', 1, 10) == b'c' and calls == ['curl'], 'fetch: curl when Windows has it')
            os.environ['SystemRoot'] = str(ROOT / 'build/no-such-windows')
            calls.clear(); check(update.fetch('u', 1, 10) == b'p' and calls == ['python'], 'fetch: Python without curl')
            update.fetch_python = fake('python', update.CertificateRefused('expired'))
            calls.clear(); check(update.fetch('u', 1, 10) == b's' and calls == ['python', 'ps'], 'fetch: PowerShell when Python refuses the certificate')
            update.fetch_python = fake('python', urllib.error.URLError(ConnectionRefusedError()))
            calls.clear(); check(raises(urllib.error.URLError, lambda: update.fetch('u', 1, 10)) and calls == ['python'], 'fetch: no PowerShell when the service is down')
            update.fetch_python = fake('python', b'x' * 11)
            check(raises(ValueError, lambda: update.fetch('u', 1, 10)), 'fetch: more than the limit is refused')
        finally:
            update.fetch_curl, update.fetch_python, update.fetch_powershell, root = saved
            if root is None:
                os.environ.pop('SystemRoot', None)
            else:
                os.environ['SystemRoot'] = root
    finally:
        server.shutdown()


def live(base):
    from Crypto.Signature import eddsa
    assert base.startswith('https://') and base.count('/') == 2, 'give the service as https://host'
    print('live service:', base.split('//')[1].split('.')[0] + '...')
    try:
        urllib.request.urlopen(urllib.request.Request(base + '/release/latest.sig', headers={'User-Agent': update.AGENT}), timeout=15).read()
        print('     note: Python\'s default check accepts the service on this PC')
    except urllib.error.URLError as error:
        print('     note: Python\'s default check refuses it here:', getattr(error.reason, 'verify_message', error.reason))
    curl = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/curl.exe'
    ways = {'curl': lambda u, t, n: update.fetch_curl(curl, u, t, n), 'python': update.fetch_python, 'powershell': update.fetch_powershell}
    got = {}
    for name, way in ways.items():
        try:
            got[name] = (way(base + '/release/latest.json', 30, 4 << 20), way(base + '/release/latest.sig', 30, 1024))
            check(True, f'live: {name} fetches the release list')
        except Exception as error:
            check(False, f'live: {name}: {type(error).__name__}: {error}')
    check(len({v for v in got.values()}) == 1, 'live: all ways get the same bytes')
    key = eddsa.import_public_key(bytes.fromhex(update.PUBLIC_KEY))
    for name, (data, sig) in got.items():
        check(raises(Exception, lambda: eddsa.new(key, 'rfc8032').verify(data, bytes.fromhex(sig.decode().strip()))) is False,
              f'live: {name}: the organizer\'s signature')


if __name__ == '__main__':
    local()
    if '--live' in sys.argv:
        live(sys.argv[sys.argv.index('--live') + 1].rstrip('/'))
    print(('FAIL' if fails else 'PASS') + f': {fails} failed')
    sys.exit(1 if fails else 0)
