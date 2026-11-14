"""Record Claude's visual review of the two final production-host group runs, for job23b_check_group.py."""
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
RUNS={
    'job24-final-relay-01':dict(
        host=(13411,13571,31301,32681),station=(13411,13571,31270,32650),invite=True,
        ui={'host-friends':'host/f1-hosting.png','host-hosting-badge':'host/f1-hosting.png','station-friends':'station/f1-joined.png',
            'invite-toast-station':'station/toast-26.png','station-room-listed':'station/userdata/last-game-frame-5901.png'}),
    'job24-final-direct-01':dict(
        host=(13411,13571,30625,32005),station=(13411,13571,30679,32059),invite=False,
        ui={'host-friends':'host/f1-hosting.png','host-hosting-badge':'host/f1-hosting.png','station-friends':'station/f1-joined.png',
            'station-room-listed':'station/userdata/last-game-frame-5901.png','outsider-no-room':'outsider/userdata/last-game-frame-8301.png'}),
}

for name,run in RUNS.items():
    folder=ROOT/'build/run'/name
    review={role:dict(score='0-0',movement_frames=list(run[role][:2]),results_frame=run[role][2],returned_frame=run[role][3],returned_to='main menu')
            for role in ('host','station')}
    review['group_ui']=dict(enrollment='F1 form',friends=True,hosting_badge=True,invite=run['invite'],invite_did_not_steal_controls=run['invite'])
    if 'outsider-no-room' in run['ui']:review['outsider_no_room']=True
    (folder/'review.json').write_text(json.dumps(review,indent=2))
    captures=[dict(label=label,png=str((folder/path).relative_to(ROOT)),sha256=hashlib.sha256((folder/path).read_bytes()).hexdigest())
              for label,path in run['ui'].items()]
    (folder/'ui-captures.json').write_text(json.dumps(captures,indent=2))
