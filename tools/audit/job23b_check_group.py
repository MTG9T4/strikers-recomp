"""Combine real-game LAN checking with Worker enrollment and outsider evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from test_lan_replay import verify,ROOT


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run',type=Path,required=True)
    p.add_argument('--review',type=Path,required=True)
    p.add_argument('--binaries',type=Path,required=True)
    p.add_argument('--test-host-provenance',type=Path)
    p.add_argument('--invite-evidence',type=Path,help='a passing check.json on the same binaries, when this run missed the Invite button')
    a=p.parse_args();run=a.run.resolve()
    assert run.is_relative_to(ROOT/'build/run')
    rows=json.loads((run/'manifest.json').read_text())
    members=[r for r in rows if r['group_member']]
    outsiders=[r for r in rows if not r['group_member']]
    assert len(members)==2
    expected=json.loads(a.binaries.read_text())['binary_sha256']
    same_host=True
    for row in rows:
        assert row['exit_code']==0 and row['valid_timing'] and not row.get('forced_launcher_close')
        for name,digest in expected.items():
            if name=='strikers_native.exe':
                same_host &= row['binary_sha256'][name]==digest
                assert row['player_host_sha256']==digest
            else:assert row['binary_sha256'][name]==digest,'Changed acceptance binary: '+name
    active={r['node'] for r in json.loads((run/'service-players.json').read_text())['players'] if r['state']=='active' and not r['removed']}
    assert all(r['node'] in active for r in members)
    assert all(r['node'] not in active for r in outsiders)
    pair=run/'members-manifest.json';pair.write_text(json.dumps(members,indent=2))
    report=verify(pair,a.review)
    visual=json.loads(a.review.read_text())
    ui=visual['group_ui']
    assert ui['enrollment'] in ('F1 form','native self-test via group_join')
    assert all(r['enrollment_route']==ui['enrollment'] for r in members)
    if ui['enrollment']=='native self-test via group_join':
        assert a.test_host_provenance,'Native test host provenance is required'
        test=json.loads(a.test_host_provenance.read_text())
        production=json.loads((ROOT/json.loads(a.binaries.read_text())['release_host_provenance']).read_text())
        assert test['local_group_test'] and test['group_self_test']
        assert production['host_sha256']==expected['strikers_native.exe']
        assert all(r['binary_sha256']['strikers_native.exe']==test['host_sha256'] for r in members)
        original={r['source']:r['sha256'] for r in production['sources']}
        variant={r['source']:r['sha256'] for r in test['sources']}
        assert all(variant.get(name)==digest for name,digest in original.items())
        assert set(variant)-set(original)=={'tools/audit/job23b_group_driver.c'}
        assert test['network_fingerprint']==production['network_fingerprint']
        for row in members:
            log=(ROOT/row['log']).read_text(errors='replace')
            assert '[group-selftest] join queued' in log and '[group] enrolled via configured service' in log
        station=next(r for r in members if r['role']=='station')
        assert '[group-selftest] invite received settings_open=0' in (ROOT/station['log']).read_text(errors='replace')
    assert all(ui[k] is True for k in ('friends','hosting_badge'))
    if a.invite_evidence:
        other=json.loads(a.invite_evidence.read_text())
        assert other['status']=='PASS' and other['same_player_host'] and other['invite_seen']
        assert other['binary_sha256']==expected
    else:assert ui['invite'] is True and ui['invite_did_not_steal_controls'] is True
    captures=json.loads((run/'ui-captures.json').read_text())
    required={'host-friends','station-friends','host-hosting-badge','station-room-listed'}
    if not a.invite_evidence:required.add('invite-toast-station')
    if outsiders:required.add('outsider-no-room')
    assert required<={c['label'] for c in captures}
    for capture in captures:
        file=(ROOT/capture['png']).resolve()
        assert file.is_relative_to(run)
        assert hashlib.sha256(file.read_bytes()).hexdigest()==capture['sha256']
    for row in outsiders:
        log=(ROOT/row['log']).read_text(errors='replace')
        assert '[zerotier] state=2 Connected.' in log
        assert row['node'] not in {r['node'] for r in members}
        assert set(row['env']['STRIKERS_ZT_PEERS'].split(','))=={r['node'] for r in members}
        assert len(re.findall(r'scan .*found=0',log))>=3 and not re.search(r'scan .*found=[1-9]|connect result=joined',log)
        assert visual['outsider_no_room'] is True
    report.update(status='PASS' if same_host else 'PASS_LOCAL_HOST_VARIANT',
        same_player_host=same_host,all_other_binaries_match=True,
        service_kind=members[0]['service_kind'],enrollment=ui['enrollment'],
        directory_only=True,outsider_rejected=bool(outsiders),ui_capture_labels=sorted(required),
        invite_seen=not a.invite_evidence,invite_evidence=str(a.invite_evidence or ''),binary_sha256=expected)
    (run/'check.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
