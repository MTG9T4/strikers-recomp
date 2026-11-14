"""Summarize route refusals, validation, captures and pipeline holds.

Usage: python tools/audit/material_run_summary.py build/run/job12-final.log ...
The JSON goes to stdout so callers choose a project-local evidence destination.
"""
import hashlib
import json
from pathlib import Path
import re
import sys


def summarize(path):
    raw=path.read_bytes();text=raw.decode(errors='replace').replace('\r\n','\n')
    holds=[]
    pattern=r'nvn_vk_perf: frames (\d+)\.\.(\d+).*?holds so far: (\d+), ([\d.]+) ms, longest ([\d.]+) ms'
    for lo,hi,n,total,longest in re.findall(pattern,text):
        holds.append(dict(first=int(lo),last=int(hi),count=int(n),total_ms=float(total),longest_ms=float(longest)))
    return dict(log=str(path),sha256=hashlib.sha256(raw).hexdigest(),
                validation_enabled='validation layer on (VK_LAYER_KHRONOS_validation)' in text,
                validation_messages=re.findall(r'^vk-validation:.*$',text,re.M),
                refusals=sorted(set(re.findall(r'^.*(?:machine code not translated|glslang rejected|dispatch left out).*$',text,re.M))),
                frame_counters=[int(x) for x in re.findall(r'^nvn_vk: frame (\d+):',text,re.M)],
                captures=[int(x) for x in re.findall(r'^capture-frame: (\d+)$',text,re.M)],
                holds=holds,
                final_hold_summary=re.findall(r'^nvn_vk_pipelines: holds.*$',text,re.M)[-1:],
                skipped_draws=re.findall(r'^nvn_vk_skip:.*$',text,re.M)[-1:],
                driver_cache=re.findall(r'^nvn_vk: pipeline cache.*$',text,re.M),
                shutdown_audio_summary=bool(re.search(r'^nn::audio: 48000 Hz,',text,re.M)))


if __name__=='__main__':
    print(json.dumps([summarize(Path(p)) for p in sys.argv[1:]],indent=2))
