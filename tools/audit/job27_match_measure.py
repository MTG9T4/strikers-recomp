"""Play one hidden Quick Battle on a fresh profile with an existing cache, and report the stalls."""
import json,os,re,shutil,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'installer'))
import warmup
install=next((ROOT/'build/phase8').glob(sys.argv[1]));cache=sys.argv[2];name=sys.argv[3]
work=install/'build/warmup';shutil.rmtree(work/'userdata',ignore_errors=True);(work/'userdata').mkdir(parents=True)
env={k:v for k,v in os.environ.items() if not k.startswith('STRIKERS_')}
for extra in sys.argv[4:]:
    k,_,v=extra.partition('=')
    env[k if v else 'STRIKERS_VK_HOLDLOG']=v or '1'
state={}
import time as _t
_t0=_t.monotonic();state['events']=[]
def done(line):
    if 'StadiumGlobal/Persistent.data' in line and not any(e[0]=='stadium' for e in state['events']):state['events'].append(('stadium',round(_t.monotonic()-_t0,1)))
    m=re.match(r'input wait: satisfied at frame (\d+)',line)
    if m:state['fence']=int(m[1]);state['events'].append(('match end',round(_t.monotonic()-_t0,1),int(m[1])))
    m=re.match(r'nvn_vk: totals at frame (\d+)',line)
    return bool(m and 'fence' in state and int(m[1])>=state['fence']+2400)
r=warmup.start(install,work,env,cache,name,done,int(os.environ.get('MEASURE_LIMIT','800')),lambda s:None,ROOT/'tools/audit'/os.environ.get('MEASURE_ROUTE','job27_one_match.seq'))
log=(work/(name+'.log')).read_text(encoding='utf-8',errors='replace')
holds=re.findall(r'holds of the render thread at a draw: (\d+), (\d+) ms in all, longest ([\d.]+) ms \(stage made at the draw (\d+) / (\d+) ms, stage waited for (\d+) / (\d+) ms, pipeline made at the draw (\d+) / (\d+) ms, pipeline waited for (\d+) / (\d+) ms',log)
ahead=re.findall(r'pipelines made ahead: (\d+) of (\d+) listed',log)
spikes=[float(x) for x in re.findall(r'nvn_perf: frames \d+\.\.\d+ frame time ms:.*p99 ([\d.]+) max',log)]
print(json.dumps(dict(r,holds=holds[-1] if holds else None,ahead=ahead,windows=len(spikes),windows_p99_over_60=sum(x>60 for x in spikes),fence=state.get('fence'),events=state['events']),indent=1),flush=True)
shutil.copyfile(work/(name+'.log'),ROOT/'build/phase8'/(name+'.log'))
