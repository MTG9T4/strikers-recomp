"""Queue only the next two serial gates after both cold timings finish."""
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[2]
index=ROOT/'build/phase8/job21c-serial-results.json'
while True:
    if index.exists():
        try:ready='developer_cold' in json.loads(index.read_text())
        except ValueError:ready=False
        if ready:break
    time.sleep(5)
for script,args,label in [('job21c_setup.py',[],'setup'),('job21c_serial.py',['prepared'],'prepared')]:
    with (ROOT/f'build/phase8/job21c-{label}-driver.log').open('wb') as log:
        result=subprocess.call([sys.executable,'-B',str(ROOT/'tools/audit'/script),*args],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
    if result:raise RuntimeError('Gate failed; inspect job21c-'+label+'-driver.log')
