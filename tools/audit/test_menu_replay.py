"""Check a completed menu replay's movie delivery and runtime diagnostics.

Screens still require visual review; this checks the runtime evidence, not OCR.
Usage: python tools/audit/test_menu_replay.py build/run/job11-sdl04.log
"""
import argparse
import json
from pathlib import Path
import re


def assert_no_unimplemented_imports(log):
    # Another runtime thread can print between the header and its blank line.
    # hostrt.c emits one numeric row for every reached unimplemented import.
    section=re.search(r'unimplemented imports reached:\n(.*?)\nrun-time host functions:',log,re.S)
    assert section,'Import report missing or incomplete'
    assert not re.search(r'(?m)^\s+\d+\s+\S+',section[1]),'Reached unimplemented imports: '+section[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    args = p.parse_args()
    log = args.log.read_text(errors='replace')
    errors = re.findall(r'^vk-validation:.*$|^.*(?:GUEST TRAP|UNTRANSLATED INSTRUCTION|ERROR: AddressSanitizer|'
                        r'unhandled exception|VUID-).*$' , log, re.M | re.I)
    assert not errors, '\n'.join(errors[:10])
    assert_no_unimplemented_imports(log)
    assert '0 calls of operations that are not implemented' in log, 'NVN call summary absent/nonzero'
    transition = re.search(r'opened memory.webm: 618547 bytes(.*?)Extractor destroyed', log, re.S)
    assert transition, 'main-menu transition was not reached'
    counts = re.search(r'destroyed: (\d+) input buffers, (\d+) output buffers, (\d+) copied', transition[1])
    assert counts and int(counts[2]) == 31 and int(counts[3]) > 0, 'short movie did not deliver a picture'
    frames = [int(n) for n in re.findall(r'^capture-frame: (\d+)$', log, re.M)]
    assert len(frames) >= 2, 'visual review evidence missing'
    assert re.search(r'^display: \d+ frames presented', log, re.M), 'run has not shut down normally'
    print(json.dumps({'log': str(args.log), 'main_menu_movie_outputs': int(counts[2]),
                      'main_menu_movie_copies': int(counts[3]), 'captures': frames,
                      'runtime_diagnostics': 'PASS'}, indent=2))


if __name__ == '__main__':
    main()
