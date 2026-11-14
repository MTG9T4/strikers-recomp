"""Create a private test CA/leaf with existing OpenSSL; never imports a store.

Run from the project root: python -m server.nex.generate_pki --out build/online/pki
Only the public ca.der belongs in runtime configuration. Never distribute keys.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[2]


def generate(out, names, days=2, signer=None):
    out = Path(out).resolve()
    if not out.is_relative_to(ROOT / 'build') or out.is_relative_to(ROOT / 'build/dump'):
        raise ValueError('test PKI must live under build, outside build/dump')
    if out.exists():
        raise ValueError('use a new PKI directory; existing keys are never overwritten')
    openssl = shutil.which('openssl')
    if not openssl:
        raise RuntimeError('OpenSSL is required; no automatic install is performed')
    out.mkdir(parents=True)
    def run(*args):
        r = subprocess.run([openssl, *map(str, args)], capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stderr)
    # Normal mkdir inherits the managed Windows temp permissions; Python 3.13's
    # restrictive TemporaryDirectory ACL prevents the compiler/OpenSSL access.
    scratch = Path(tempfile.gettempdir()).resolve() / ('strikers-pki-' + uuid.uuid4().hex)
    scratch.mkdir()
    td = str(scratch)
    try:
        ext = Path(td) / 'leaf.cnf'
        ext.write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n'
                       'extendedKeyUsage=serverAuth\nsubjectAltName=' + ','.join('DNS:' + n for n in names) + '\n')
        if signer is None:
            run('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-sha256', '-days', '3',
                '-subj', '/CN=Strikers localhost development CA', '-addext', 'basicConstraints=critical,CA:TRUE',
                '-addext', 'keyUsage=critical,keyCertSign,cRLSign', '-keyout', out / 'ca.key', '-out', out / 'ca.pem')
        signer = Path(signer) if signer else out
        run('req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=' + names[0],
            '-keyout', out / 'server.key', '-out', Path(td) / 'leaf.csr')
        validity = ['-not_before', '20200101000000Z', '-not_after', '20200102000000Z'] if days < 0 else ['-days', str(days)]
        run('x509', '-req', '-in', Path(td) / 'leaf.csr', '-CA', signer / 'ca.pem', '-CAkey', signer / 'ca.key',
            '-set_serial', '1', *validity, '-sha256', '-extfile', ext, '-out', out / 'server.pem')
        run('x509', '-in', signer / 'ca.pem', '-outform', 'DER', '-out', out / 'ca.der')
    finally:
        if scratch.resolve().parent != Path(tempfile.gettempdir()).resolve():
            raise RuntimeError('scratch cleanup escaped temp')
        shutil.rmtree(scratch)
    for name in ('ca.key', 'server.key'):
        if (out / name).exists():
            (out / name).chmod(0o600)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--name', action='append', default=[])
    args = ap.parse_args()
    names = args.name or ['*.s.n.srv.nintendo.net', '*.n.n.srv.nintendo.net',
                          '*.znc.srv.nintendo.net', '*.lp1.p.srv.nintendo.net']
    print(generate(args.out, names))


if __name__ == '__main__':
    main()
