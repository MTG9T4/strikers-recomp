"""Opt-in problem reports, the game's side (runtime/core/report_send.c): redaction, private settings, the summary read from
a log, and the upload to a local receiver on 127.0.0.1 (plain http is allowed to 127.0.0.1 only). No game, no network beyond loopback. Evidence goes under --out in build/run."""
import argparse
import ctypes
import getpass
import http.server
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import urllib.parse
import uuid
from build_runtime import ROOT, msvc_environment

HARNESS = r'''
#include "report_send.c"
__declspec(dllexport) int t_redact(const char *p, unsigned max) { return report_redact_file(p, max); }
__declspec(dllexport) int t_settings(const char *a, const char *b) { return report_copy_settings(a, b); }
__declspec(dllexport) void t_info(const char *log, char *out, unsigned cap) {
    ReportInfo i; memset(&i, 0, sizeof i); report_info_from_log(log, &i);
    snprintf(out, cap, "%s|%s|%s|%s|%s", i.version, i.gpu, i.cpu, i.os, i.memory);
}
__declspec(dllexport) int t_send(const char *url, const char *zip, const char *desc, char *id, char *msg) {
    ReportInfo i; memset(&i, 0, sizeof i);
    snprintf(i.version, sizeof i.version, "0.2.0"); snprintf(i.kind, sizeof i.kind, "menu");
    snprintf(i.gpu, sizeof i.gpu, "AMD Radeon Graphics"); snprintf(i.cpu, sizeof i.cpu, "AMD Ryzen Z1 Extreme");
    snprintf(i.os, sizeof i.os, "Windows 11 build 26200"); snprintf(i.memory, sizeof i.memory, "13 GB");
    snprintf(i.description, sizeof i.description, "%s", desc);
    return report_send(url, zip, &i, id, 64, msg, 400);
}
__declspec(dllexport) void t_url(const char *ini, char *out) { report_url(ini, out, 512); }
'''

received = []


class Receiver(http.server.BaseHTTPRequestHandler):
    answer = (200, {'id': 'R-7K2Q9F'})

    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        received.append({'path': self.path, 'type': self.headers['Content-Type'], 'body': body,
                         'headers': {k: urllib.parse.unquote(v) for k, v in self.headers.items() if k.lower().startswith('x-strikers-')}})
        code, payload = Receiver.answer
        data = json.dumps(payload).encode()
        self.send_response(code); self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(data)))
        self.end_headers(); self.wfile.write(data)

    def log_message(self, *a):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/run/report-send')
    args = parser.parse_args()
    out = args.out.resolve()
    assert out.is_relative_to(ROOT / 'build/run') and not out.exists(), 'use an unused build/run path'
    out.mkdir(parents=True)
    temp = Path(tempfile.gettempdir()) / ('strikers-report-' + uuid.uuid4().hex)
    temp.mkdir()
    result = {}
    server = None
    try:
        (temp / 't.c').write_text(HARNESS)
        env = msvc_environment()
        cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
        dll = temp / 'report.dll'
        build = subprocess.run([str(cl), '/nologo', '/LD', '/MD', '/O2', '/W3', '/we4013', '/D_CRT_SECURE_NO_WARNINGS',
                                '/DWIN32_LEAN_AND_MEAN', '/I' + str(ROOT / 'runtime/core'),
                                str(temp / 't.c'), '/link', '/OUT:' + str(dll)], env=env, cwd=temp, capture_output=True, text=True)
        (out / 'build.log').write_text(build.stdout + build.stderr)
        assert build.returncode == 0, build.stdout + build.stderr
        lib = ctypes.CDLL(str(dll))
        b = lambda s: str(s).encode('mbcs')

        account, pc = getpass.getuser(), os.environ.get('COMPUTERNAME', '')
        profile = Path(os.environ['USERPROFILE']).name
        text = (f'folder C:\\Users\\{account}\\Games\\strikers\n'
                f'path c:/users/{account.lower()}/Downloads/x.log\n'
                f'json "C:\\\\Users\\\\{account}\\\\AppData"\n'
                f'end C:\\Users\\{profile}\n'
                f'longer C:\\Users\\{account}extra\\keep\n'
                f'public C:\\Users\\Public\\x\n'
                f'pc {pc} and {pc}SUFFIX\n')
        log = temp / 'redact.log'; log.write_bytes(text.encode())
        assert lib.t_redact(b(log), 0) == 1
        red = log.read_bytes().decode()
        (out / 'redacted.txt').write_text(red.replace(account, 'ACCOUNT').replace(pc, 'PC') if account else red)
        assert f'\\Users\\{account}\\' not in red and f'/users/{account.lower()}/' not in red.lower(), red
        assert 'C:\\Users\\<user>\\Games' in red and '/Users/<user>/' in red.replace('/users/', '/Users/') and 'C:\\\\Users\\\\<user>\\\\AppData' in red
        assert f'C:\\Users\\{account}extra' in red, 'a longer folder name is someone else: kept'
        assert 'C:\\Users\\Public' in red
        if len(pc) >= 4:
            assert f'pc <pc> and {pc}SUFFIX' in red, red
        result['redaction'] = 'passed: account in either slash style, any case, JSON-escaped and at the end; PC name alone; Public and longer names kept'

        big = temp / 'big.log'; big.write_bytes(b'H' * 300000 + b'M' * 400000 + b'T' * 300000)
        assert lib.t_redact(b(big), 400000) == 1
        data = big.read_bytes()
        assert data.startswith(b'H' * 100000) and data.endswith(b'T' * 300000) and b'left out of the report' in data and len(data) < 420000
        result['size_cap'] = 'passed: a 1 MB log cut to its first quarter and last three quarters of 400 KB around a marker'

        ini = temp / 'strikers.ini'
        ini.write_bytes(b'[Network]\r\nmode = internet\r\nroom_key = secret words\r\ngroup_code = ABCD-2345\r\ngroup_service = https://g.example\r\nzerotier_network =\r\nroom_key =\r\n')
        copied = temp / 'copy.ini'
        assert lib.t_settings(b(ini), b(copied)) == 1
        c = copied.read_bytes().decode()
        assert 'secret words' not in c and 'ABCD-2345' not in c and 'g.example' not in c and 'mode = internet' in c
        assert c.count('(removed from the report)') == 3 and 'room_key =\r\n' in c
        result['private_settings'] = 'passed: room_key, group_code and group_service replaced; empty values and other keys kept'

        sample = temp / 'last-game.log'
        sample.write_text('Local SDK bridge\nsystem: Strikers Recomp 0.1.1 | game code for v3 | Windows 11 build 26200 | AMD Ryzen Z1 Extreme   , 16 logical processors | 13 GB memory | folder C:\\x\n'
                          'nvn_vk: device 0: AMD Radeon Graphics (integrated GPU)\nnvn_vk: using device 0: AMD Radeon Graphics (loader Vulkan 1.4.341); extensions\n')
        buf = ctypes.create_string_buffer(600)
        lib.t_info(b(sample), buf, 600)
        assert buf.value.decode() == '0.1.1|AMD Radeon Graphics|AMD Ryzen Z1 Extreme|Windows 11 build 26200|13 GB', buf.value
        sample.write_text('system: Strikers Recomp (no release list) | game code for (no cpu-level.txt) | Windows 11 build 26300 | Test CPU, 32 logical processors | 64 GB memory | folder C:\\x\n')
        lib.t_info(b(sample), buf, 600)
        assert buf.value.decode().startswith('dev|'), buf.value
        result['summary_from_log'] = 'passed: version, GPU, CPU, Windows and memory read from a real log\'s lines; a dev build is "dev"'

        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Receiver)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        url = f'http://127.0.0.1:{server.server_address[1]}'
        zipfile = temp / 'r.zip'; zipfile.write_bytes(b'PK\x03\x04' + os.urandom(200000))
        ident, msg = ctypes.create_string_buffer(64), ctypes.create_string_buffer(400)
        desc = 'Black screen after the intro — "é" 100% & more\nsecond line'
        assert lib.t_send(b(url + '/'), b(zipfile), desc.encode('utf-8'), ident, msg) == 1, msg.value
        assert ident.value == b'R-7K2Q9F'
        r = received[-1]
        assert r['path'] == '/v1/report' and r['type'] == 'application/zip' and r['body'] == zipfile.read_bytes()
        h = {k.lower(): v for k, v in r['headers'].items()}
        assert h['x-strikers-version'] == '0.2.0' and h['x-strikers-kind'] == 'menu' and h['x-strikers-gpu'] == 'AMD Radeon Graphics'
        assert h['x-strikers-description'] == desc, h['x-strikers-description']
        result['upload'] = 'passed: POST /v1/report, the zip byte for byte, percent-encoded UTF-8 headers decoded exactly'

        Receiver.answer = (429, {'error': 'Too many reports. Try again in a minute.'})
        assert lib.t_send(b(url), b(zipfile), b'', ident, msg) == 0 and msg.value == b'Too many reports. Try again in a minute.'
        Receiver.answer = (500, {})
        assert lib.t_send(b(url), b(zipfile), b'', ident, msg) == 0 and b'answered 500' in msg.value
        Receiver.answer = (200, {'nothing': 1})
        assert lib.t_send(b(url), b(zipfile), b'', ident, msg) == 0 and b'does not understand' in msg.value
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0)); dead = s.getsockname()[1]
        assert lib.t_send(b(f'http://127.0.0.1:{dead}'), b(zipfile), b'', ident, msg) == 0 and b"Couldn't reach" in msg.value
        assert lib.t_send(b('http://example.com'), b(zipfile), b'', ident, msg) == 0 and b'https' in msg.value
        huge = temp / 'huge.zip'; huge.write_bytes(b'PK\x03\x04' + bytes(10 * 1024 * 1024))
        n = len(received)
        assert lib.t_send(b(url), b(huge), b'', ident, msg) == 0 and b'too big' in msg.value and len(received) == n
        result['failures'] = 'passed: service error text shown as is, other statuses, a bad answer, no service, http to a public host and a zip over 10 MB'

        url_out = ctypes.create_string_buffer(512)
        off = temp / 'off.ini'; off.write_text('[Network]\nreport_url = off\n')
        lib.t_url(b(off), url_out); assert url_out.value == b''
        own = temp / 'own.ini'; own.write_text('[Network]\nreport_url = https://reports.example/\n')
        lib.t_url(b(own), url_out); assert url_out.value == b'https://reports.example/'
        if 'STRIKERS_REPORT_URL' not in os.environ:
            blank = temp / 'blank.ini'; blank.write_text('[Network]\nmode = internet\n')
            lib.t_url(b(blank), url_out); assert url_out.value == b'https://reports.mtg9t4.fun', url_out.value
        result['settings'] = 'passed: the built-in address; report_url = off turns sending off; report_url names another service'
        result['status'] = 'PASS'
    finally:
        if server:
            server.shutdown()
        shutil.rmtree(temp, ignore_errors=True)
        (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
