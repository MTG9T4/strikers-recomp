"""Run the pinned, offline Worker unit suite. workerd acceptance is separate."""
import subprocess
import sys
from build_runtime import ROOT

if __name__=='__main__':
    raise SystemExit(subprocess.call(['node','--test','--test-isolation=none','test/worker.test.mjs' if '--workerd' in sys.argv else 'test/unit.test.mjs'],cwd=ROOT/'cloudflare/group'))
