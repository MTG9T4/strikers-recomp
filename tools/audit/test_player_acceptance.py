"""Verify exact-binary offline matches using save fences and reviewed captures."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from run_player_soak import match_fences
from test_menu_replay import assert_no_unimplemented_imports

ROOT=Path(__file__).resolve().parents[2]


def verify(run,binaries,review,matches):
    result=json.loads((run/'result.json').read_text())
    expected=json.loads(binaries.read_text())['binary_sha256']
    assert result['exit']==0 and result['valid_timing'] and result['status']=='COMPLETED'
    assert all(result['binary_sha256'].get(name)==digest for name,digest in expected.items()),'Binary set changed'
    log=(run/'game.log').read_text(errors='replace')
    script=(run/'replay.seq').read_text()
    assert match_fences(script,log)>=matches and result['match_end_fences']>=matches,'Too few completed matches'
    assert_no_unimplemented_imports(log)
    assert not re.search(r'GUEST TRAP|HOST EXCEPTION|UNTRANSLATED INSTRUCTION|unhandled exception|ERROR: AddressSanitizer|^vk-validation:|VUID-',log,re.I|re.M)
    assert '0 calls of operations that are not implemented' in log
    assert re.search(r'shutdown clean:.*0 Vulkan call failures, 0 validation messages',log)
    steps=[s.strip() for s in script.splitlines() if s.strip() and not s.lstrip().startswith('#')]
    blocks={i+1 for i,s in enumerate(steps) if s=='wait open:StrkrsSave:/strkrs.save'}
    fences=[int(frame) for frame,block in re.findall(r'^input wait: satisfied at frame (\d+); block (\d+) begins',log,re.M) if int(block) in blocks]
    captures={r['frame']:r for r in json.loads((run/'game-captures.json').read_text())}
    visual=json.loads(review.read_text())['matches']
    assert len(visual)>=matches
    evidence=[]
    for i,row in enumerate(visual[:matches]):
        frames=[row['full_time_frame'],row['results_frame'],row['returned_frame']]
        assert fences[i]<frames[0]<frames[1]<frames[2]
        assert i+1==len(fences) or frames[2]<fences[i+1]
        assert row['returned_to']=='main menu' and re.fullmatch(r'\d+-\d+',row['score'])
        for frame in frames:
            capture=captures[frame]
            assert not capture['spatially_uniform']
            assert hashlib.sha256((ROOT/capture['png']).read_bytes()).hexdigest()==capture['png_sha256']
        evidence.append(dict(match=i+1,save_frame=fences[i],**row))
    return dict(status='PASS',matches=evidence,binary_sha256=expected,runtime_diagnostics='PASS',valid_timing=True)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run',type=Path,required=True)
    p.add_argument('--binaries',type=Path,required=True)
    p.add_argument('--review',type=Path,required=True)
    p.add_argument('--matches',type=int,required=True)
    p.add_argument('--output',type=Path)
    a=p.parse_args()
    report=verify(a.run,a.binaries,a.review,a.matches)
    if a.output:a.output.write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
