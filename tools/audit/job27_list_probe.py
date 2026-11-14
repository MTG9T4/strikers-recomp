"""Run setup's warm-up on an existing install, on a cache of its own, using a chosen shader list."""
import json,os,shutil,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'installer'))
import warmup
install=next((ROOT/'build/phase8').glob(sys.argv[1]));cache=sys.argv[2]
shutil.copyfile(sys.argv[3],install/'installer/shader-list.json')
shutil.rmtree(install/'build/warmup',ignore_errors=True);shutil.rmtree(install/cache,ignore_errors=True)
env={k:v for k,v in os.environ.items() if not k.startswith('STRIKERS_')}
print(json.dumps(warmup.run(install,env,lambda m:print('about',m,'minutes left',flush=True),cache,keep=True),indent=2),flush=True)
