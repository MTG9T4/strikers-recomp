"""After a crash the release launcher asks to send the crash report; Yes sends it (a local receiver on 127.0.0.1 stands in
for the report service), No sends nothing. The real launcher (build/phase8/apps/StrikersRecomp.exe) watches a small program
that crashes. STRIKERS_REPORT_TEST_ANSWER answers the question for the test, and the launcher writes what it would have
shown to <userdata>\\report-result.txt."""
import http.server
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import urllib.parse
import uuid
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_runtime import ROOT, msvc_environment

received = []


class Receiver(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        received.append((self.path, {k.lower(): urllib.parse.unquote(v) for k, v in self.headers.items()}, body))
        data = b'{"id":"R-CRASH2"}'
        self.send_response(200); self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(data)))
        self.end_headers(); self.wfile.write(data)

    def log_message(self, *a):
        pass


def main():
    out = ROOT / 'build/phase8/crash-tests' / ('report-' + uuid.uuid4().hex)
    out.mkdir(parents=True)
    launcher = ROOT / 'build/phase8/apps/StrikersRecomp.exe'
    if not launcher.exists():
        subprocess.run([sys.executable, '-I', str(ROOT / 'tools/release/build_apps.py')], check=True)
    source = out / 'crash.cpp'
    # The log carries the account's own profile path and the PC name, as a real log can: both must be gone from what is sent.
    source.write_text(r'''#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
int main() { setvbuf(stdout,0,_IONBF,0);
printf("system: Strikers Recomp 9.9.9 | game code for v3 | Windows 11 build 26200 | Test CPU, 8 logical processors | 16 GB memory | folder %s\\Games\n", getenv("USERPROFILE"));
printf("nvn_vk: using device 0: Test GPU 9000 (loader Vulkan 1.4.0); extensions\n");
printf("pc name %s here\n", getenv("COMPUTERNAME"));
RaiseException(0xE0420042, EXCEPTION_NONCONTINUABLE, 0, 0); return 90; }
''')
    env = msvc_environment()
    cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
    subprocess.run([str(cl), '/nologo', '/MT', str(source), '/Fo' + str(out / 'crash.obj'), '/Fe' + str(out / 'crash.exe')],
                   cwd=out, env=env, check=True, capture_output=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Receiver)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        run_env = {**os.environ, 'STRIKERS_REPORT_URL': f'http://127.0.0.1:{server.server_address[1]}', 'STRIKERS_REPORT_TEST_ANSWER': 'yes'}
        user = out / 'userdata'
        p = subprocess.run([str(launcher), '--crash-test', str(out / 'crash.exe'), str(user)], cwd=out, env=run_env, timeout=120, capture_output=True)
        assert p.returncode != 0
        result = (user / 'report-result.txt').read_text(encoding='utf-8')
        assert result.startswith('Sent. Report ID R-CRASH2'), result
        assert len(received) == 1
        path, headers, body = received[0]
        assert path == '/v1/report' and headers['content-type'] == 'application/zip'
        assert headers['x-strikers-kind'] == 'crash' and headers['x-strikers-version'] == '9.9.9' and headers['x-strikers-gpu'] == 'Test GPU 9000'
        assert headers['x-strikers-cpu'] == 'Test CPU' and headers['x-strikers-memory'] == '16 GB'
        z = zipfile.ZipFile(io.BytesIO(body))
        names = [n.split('/', 1)[1] for n in z.namelist() if '/' in n and not n.endswith('/')]
        assert 'last-game.log' in names and 'report.json' in names, names
        assert any(n.endswith('.dmp') for n in names) and any(n.endswith('.txt') for n in names), names
        dmp = next(n for n in z.namelist() if n.endswith('.dmp'))
        assert z.read(dmp)[:4] == b'MDMP'
        assert json.loads(z.read(next(n for n in z.namelist() if n.endswith('report.json'))))['kind'] == 'crash'
        account = Path(os.environ['USERPROFILE']).name.encode()
        pc = os.environ.get('COMPUTERNAME', '').encode()
        for n in z.namelist():
            if n.endswith(('.log', '.txt', '.ini', '.json')):
                data = z.read(n)
                assert b'\\Users\\' + account not in data and (len(pc) < 4 or pc not in data), n
        log = z.read(next(n for n in z.namelist() if n.endswith('last-game.log')))
        assert b'\\Users\\<user>\\Games' in log and (len(pc) < 4 or b'pc name <pc> here' in log), log[:400]
        assert (user / 'crashes').exists() and list((user / 'crashes').glob('*.dmp')), 'the local copy stays'
        assert list((user / 'reports').glob('crash-*.zip')), 'the zip is kept in userdata\\reports'

        received.clear()
        user2 = out / 'userdata-no'
        p = subprocess.run([str(launcher), '--crash-test', str(out / 'crash.exe'), str(user2)], cwd=out,
                           env={**run_env, 'STRIKERS_REPORT_TEST_ANSWER': 'no'}, timeout=120, capture_output=True)
        assert p.returncode != 0 and not received, 'No must send nothing'
        assert (user2 / 'report-result.txt').read_text(encoding='utf-8') == 'not asked to send'
        assert not (user2 / 'reports').exists()
        print('PASS: Yes sends the crash zip (minidump, crash text, log, report.json; account and PC names taken out of the text '
              'files; version, GPU, CPU and memory read from the log); the local copies stay. No sends nothing.')
    finally:
        server.shutdown()


if __name__ == '__main__':
    main()
