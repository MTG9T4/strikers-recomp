"""Measure captured WAVs and compare intro screenshots to independently decoded source frames.

Usage: python tools/audit/analyze_media.py wav <file> [--start n --seconds n --out file]
       python tools/audit/analyze_media.py movie <capture-manifest.json>
       python tools/audit/analyze_media.py inventory
"""
import argparse
from collections import Counter
import hashlib
import json
import math
import mmap
from pathlib import Path
import struct
import subprocess
import wave

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]


def levels(p, start, seconds, out):
    with wave.open(str(p), 'rb') as w:
        rate, channels, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
        assert width == 2
        w.setpos(round(start * rate))
        raw = w.readframes(round(seconds * rate) if seconds else w.getnframes())
    if out:
        if out.exists():
            raise FileExistsError(out)
        with wave.open(str(out), 'wb') as w:
            w.setparams((channels, width, rate, 0, 'NONE', 'not compressed')); w.writeframes(raw)
    a = np.frombuffer(raw, dtype='<i2').astype(np.float64)
    peak = float(np.max(np.abs(a))); rms = float(np.sqrt(np.mean(a * a)))
    return dict(source=str(p), output=str(out) if out else None, start_seconds=start,
                seconds=len(a) / channels / rate, rate=rate, channels=channels, peak=peak, rms=rms,
                peak_dbfs=20 * math.log10(peak / 32768) if peak else None,
                rms_dbfs=20 * math.log10(rms / 32768) if rms else None,
                nonzero_samples=int(np.count_nonzero(a)), clipped_samples=int(np.count_nonzero(np.abs(a) >= 32767)),
                pcm_sha256=hashlib.sha256(raw).hexdigest())


def movie(manifest):
    # All source frames, reduced only for matching; no timing assumptions about dropped movie frames.
    size = (128, 72)
    video = ROOT / 'build/dump/update/romfs/Movies/IntroMovie.webm'
    raw = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(video), '-vf',
                                   'scale=128:72:in_color_matrix=bt601:out_range=full',
                                   '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'])
    source = np.frombuffer(raw, dtype=np.uint8).reshape(-1, size[1], size[0], 3)
    rows = []
    for row in json.loads(manifest.read_text()):
        if row['frame'] > 2400:
            continue
        p = ROOT / row['png']
        with Image.open(p) as im:
            assert im.size == (1920, 1080)
            shot = np.asarray(im.convert('RGB').resize(size, Image.Resampling.BILINEAR), dtype=np.float32)
        # Batch the diff so this check also runs on modest hosts.
        errors = np.concatenate([np.mean((source[i:i+64].astype(np.float32) - shot) ** 2, axis=(1, 2, 3))
                                 for i in range(0, len(source), 64)])
        best = int(np.argmin(errors))
        ref = source[best].astype(np.float32)
        rows.append(dict(frame=row['frame'], png=str(p.relative_to(ROOT)), source_frame=best,
                         source_seconds=best * 1001 / 30000, rmse=float(np.sqrt(errors[best])),
                         channel_mean_error=np.mean(shot - ref, axis=(0, 1)).tolist(),
                         source_size=[1600, 900], capture_size=[1920, 1080], x_scale=1.2, y_scale=1.2))
    return rows


def inventory():
    rows = []
    for p in sorted((ROOT / 'build/dump/update/romfs/wwiseaudio').glob('*')):
        with p.open('rb') as f:
            with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
                assert m[:4] == b'AKPK'
                # PCK header's four section sizes: language, bank, streamed media, external source.
                language, bank, media, external = struct.unpack_from('<4I', m, 12)
                table = 28 + language + bank
                count = struct.unpack_from('<I', m, table)[0]
                assert media == 4 + count * 20
                entries, codecs = [], Counter()
                for i in range(count):
                    ident, block, size, pos, lang = struct.unpack_from('<5I', m, table + 4 + i * 20)
                    off = block * pos
                    assert off + size <= len(m)
                    assert m[off:off+4] == b'RIFF' and m[off+8:off+16] == b'WAVEfmt '
                    tag, ch, rate = struct.unpack_from('<HHI', m, off + 20)
                    codecs[f'0x{tag:04x}/{ch}ch/{rate}Hz'] += 1
                    entries.append(dict(id=ident, offset=off, size=size, codec=f'0x{tag:04x}', channels=ch, rate=rate))
                rows.append(dict(path=str(p.relative_to(ROOT)), streamed_entries=count, codecs=dict(codecs), entries=entries))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('mode', choices=['wav', 'movie', 'inventory']); ap.add_argument('file', type=Path, nargs='?')
    ap.add_argument('--start', type=float, default=0); ap.add_argument('--seconds', type=float)
    ap.add_argument('--out', type=Path)
    args = ap.parse_args()
    result = levels(args.file, args.start, args.seconds, args.out) if args.mode == 'wav' else movie(args.file) if args.mode == 'movie' else inventory()
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
