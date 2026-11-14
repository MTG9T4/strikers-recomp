"""Install the two pinned binary wheels into embedded Python, without pip."""
import hashlib
import json
import os
from pathlib import Path
import sys
import urllib.request
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/release'))
from common import safe_unzip, sha256


class OfficialRedirectsOnly(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,req,fp,code,msg,headers,newurl):
        url=urlsplit(newurl)
        if url.scheme!='https' or url.hostname not in ('www.python.org','files.pythonhosted.org'):
            raise ValueError('Tool download redirected outside its official source')
        return super().redirect_request(req,fp,code,msg,headers,newurl)


def fetch(entry, cache):
    url = entry['url']
    if not url.startswith(('https://www.python.org/ftp/python/', 'https://files.pythonhosted.org/packages/')):
        raise ValueError('Unapproved download source')
    path = cache / url.rsplit('/', 1)[1]
    if not path.exists():
        if os.environ.get('STRIKERS_OFFLINE')=='1':
            raise ValueError('A required cached build helper is missing; restore it and retry')
        part = path.with_suffix(path.suffix+'.part')
        with urllib.request.build_opener(OfficialRedirectsOnly()).open(url, timeout=60) as src, part.open('wb') as dst:
            for block in iter(lambda: src.read(1024*1024), b''):
                dst.write(block)
        part.replace(path)
    if sha256(path) != entry['sha256']:
        raise ValueError('A tool download did not pass its safety check; remove it and retry')
    return path


def main():
    lock = json.loads((ROOT / 'installer/toolchain.lock.json').read_text())
    cache = ROOT / 'installer/cache'
    cache.mkdir(exist_ok=True)
    site = ROOT / 'toolchain/python/Lib/site-packages'
    site.mkdir(parents=True, exist_ok=True)
    for entry in lock['wheels']:
        safe_unzip(fetch(entry, cache), site)
    pth = ROOT / 'toolchain/python/python313._pth'
    # Explicit paths work with -I; no user site or registry search, no pip.
    pth.write_text('python313.zip\n.\nLib/site-packages\n')
    # This invocation started before its ._pth was rewritten. The next isolated
    # interpreter consumes the final path list; record the pinned bootstrap now.
    proof=dict(interpreter=sys.executable,version=sys.version,
        wheels={entry['url'].rsplit('/',1)[-1]:entry['sha256'] for entry in lock['wheels']},
        site_packages=str(site),python_archive_sha256=lock['python']['sha256'])
    (ROOT/'toolchain/bootstrap-proof.json').write_text(json.dumps(proof,indent=2))
    print('Build helpers ready: embedded Python '+sys.version.split()[0], flush=True)


if __name__ == '__main__':
    try:main()
    except (ValueError,OSError) as error:
        print('@error '+str(error).rstrip('.')+'.',flush=True)
        raise SystemExit(1)
