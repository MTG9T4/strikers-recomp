"""Summarize completed launch evidence; captures still need visual review."""
import json
from pathlib import Path
import re
from collections import Counter

ROOT=Path(__file__).resolve().parents[2]


def analyze(folder):
    result=json.loads((folder/'result.json').read_text())
    text=(folder/'game.log').read_text(errors='replace')
    before=text.split('nvn_vk_frame: 1 draws',1)[0]
    calls=[float(v) for v in re.findall(r'took ([0-9.]+) s in the driver',before)]
    waits=[float(v) for v in re.findall(r'HELD ([0-9.]+)',before)]
    first=next((e['seconds'] for e in result['events'] if 'nvn_vk_frame: 1 draws' in e['line']),None)
    captures=[e for e in result['events'] if e['line'].startswith('capture-frame:')]
    summary=dict(result=str((folder/'result.json').relative_to(ROOT)),host_sha256=result['host_sha256'],
        valid_timing=result['valid_timing'],exit=result['exit'],wall_seconds=result['seconds'],
        first_present_seconds=first,capture_checkpoints=captures,checkpoint_times_are_upper_bounds=True,
        cache_before=result['cache_before'],reported_handover_wait_seconds=round(sum(waits)/1000,4),
        reported_slow_driver_calls_before_present=len(calls),max_reported_driver_library_seconds=max(calls,default=0),
        driver_call_times_overlap_do_not_sum=True,
        worker_count=re.findall(r'; (\d+) worker threads;',text),
        raised_idle_threads=re.findall(r'^nn::os: thread .*raised from IDLE.*$',text,re.M))
    if (folder/'thread-samples.jsonl').exists():
        samples=[json.loads(l) for l in (folder/'thread-samples.jsonl').read_text().splitlines()]
        host_pid=Counter(r['pid'] for r in samples).most_common(1)[0][0]
        samples=[r for r in samples if r['pid']==host_pid and (first is None or r['seconds']<=first)]
        threads=[t for r in samples for t in r.get('threads',[])]
        nv=[t for t in threads if t.get('start_module','').lower().startswith('nv')]
        summary['thread_observation']=dict(samples=len(samples),host_pid=host_pid,
            priorities=dict(Counter(t.get('relative_priority') for t in threads)),
            nvidia_priorities=dict(Counter(t.get('relative_priority') for t in nv)),
            idle_observations=sum(t.get('relative_priority')==-15 for t in threads))
    (folder/'analysis.json').write_text(json.dumps(summary,indent=2))
    return summary


if __name__=='__main__':
    index=ROOT/'build/phase8/job21c-serial-results.json'
    summaries={k:analyze((ROOT/p).parent) for k,p in json.loads(index.read_text()).items()}
    (ROOT/'build/phase8/job21c-launch-analysis.json').write_text(json.dumps(summaries,indent=2))
    print(json.dumps(summaries,indent=2))
