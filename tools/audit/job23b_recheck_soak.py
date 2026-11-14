"""Reassess preserved Job 23b evidence using the corrected elapsed-time window.

Never modifies the original result, raw samples, runtime log or binaries.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re

from run_player_soak import ROOT,growth_report,match_fences
from test_menu_replay import assert_no_unimplemented_imports


def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run',type=Path,required=True)
    p.add_argument('--binaries',type=Path,required=True)
    a=p.parse_args();run=a.run.resolve()
    assert run.is_relative_to(ROOT/'build/phase8')
    original=json.loads((run/'result.json').read_text())
    expected=json.loads(a.binaries.read_text())['binary_sha256']
    assert original['exit']==0 and original['valid_timing'] and not original['contamination']
    assert not original.get('forced_timeout') and original['wall_seconds']>=7200
    seconds=original['seconds_requested'];assert seconds==7200
    for name,value in original['binary_sha256'].items():
        assert value==expected[name]==digest(run/'bin'/name),name
    rows=[{k:float(v) for k,v in row.items()} for row in csv.DictReader((run/'resources.csv').open())]
    assert len(rows)==original['samples'] and rows[0]['seconds']<5
    assert all(0<b['seconds']-a['seconds']<65 for a,b in zip(rows,rows[1:]))
    in_window=[r for r in rows if 0<=r['seconds']<seconds]
    assert len(in_window)>=120 and in_window[-1]['seconds']>=seconds-60
    census=[json.loads(line) for line in (run/'handle-types.jsonl').read_text().splitlines()]
    assert len(census)==len(rows)
    assert all(c['created_filetime']==original['census_created_filetime'] for c in census)
    checks=[json.loads(line) for line in (run/'external-game-checks.jsonl').read_text().splitlines()]
    assert checks and all(not c['external_game_paths'] and c['runtime_error_markers']==0 for c in checks)
    log=(run/'game.log').read_text(errors='replace')
    assert_no_unimplemented_imports(log)
    assert '0 calls of operations that are not implemented' in log
    assert not re.search(r'GUEST TRAP|HOST EXCEPTION|UNTRANSLATED INSTRUCTION|unhandled exception|VUID-|^vk-validation:',log,re.I|re.M)
    assert re.search(r'shutdown clean:.*0 Vulkan call failures, 0 validation messages',log)
    completed=match_fences((run/'replay.seq').read_text(),log)
    assert completed==original['match_end_fences'] and completed>=10
    report,passed=growth_report(rows,seconds,True)
    report.update(status='PASS' if passed else 'NOT_PASSED',wall_seconds=original['wall_seconds'],
        raw_samples=len(rows),samples_within_requested_run=len(in_window),match_end_fences=completed,
        host_sha256=original['host_sha256'],binary_sha256=original['binary_sha256'],
        runtime_checks='PASS',valid_timing=True,original_status=original['status'],
        reason='Correct elapsed second-half window; exclude post-duration teardown sample. Thresholds and WebM allowance unchanged.',
        evidence_sha256={name:digest(run/name) for name in ('result.json','resources.csv','game.log','handle-types.jsonl','external-game-checks.jsonl')},
        evaluator_sha256=digest(ROOT/'tools/audit/run_player_soak.py'),
        original_window_diagnostics={k:original[k] for k in ('last_half_slope_per_hour','last_half_handle_slopes_per_hour')})
    (run/'growth-review.json').write_text(json.dumps(report,indent=2))
    print(json.dumps({k:report[k] for k in ('status','raw_samples','samples_within_requested_run','match_end_fences','growth_window','last_half_slope_per_hour','last_half_handle_slopes_per_hour')},indent=2))
    return 0 if passed else 1


if __name__=='__main__':raise SystemExit(main())
