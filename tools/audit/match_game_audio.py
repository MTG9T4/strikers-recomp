"""Match AudioOut to streamed PTADPCM media, using the supplied game's own decode table.

Read-only inputs. Prints numeric Wwise IDs, alignment and normalized correlation;
does not infer missing bank/event names. Codec identification reference:
https://github.com/vgmstream/vgmstream/blob/master/src/meta/wwise.c (0x8311).
"""
import argparse
import json
from pathlib import Path
import struct
import wave

import numpy as np

ROOT = Path(__file__).resolve().parents[2]


def decode(data, channels, blocksize, table):
    samples = 2 + (blocksize - 5) * 2
    n = len(data) // (blocksize * channels)
    blocks = np.frombuffer(data[:n * blocksize * channels], dtype=np.uint8).reshape(n, channels, blocksize)
    h2 = (blocks[:, :, 0].astype(np.int32) | blocks[:, :, 1].astype(np.int32) << 8)
    h1 = (blocks[:, :, 2].astype(np.int32) | blocks[:, :, 3].astype(np.int32) << 8)
    h2 = np.where(h2 >= 32768, h2 - 65536, h2)
    h1 = np.where(h1 >= 32768, h1 - 65536, h1)
    index = blocks[:, :, 4].astype(np.int32)
    assert np.max(index) <= 12
    out = np.empty((n, samples, channels), np.int16)
    out[:, 0, :] = h2; out[:, 1, :] = h1
    for i in range(samples - 2):
        nibble = (blocks[:, :, 5 + i // 2] >> (4 * (i % 2))) & 15
        step = table[index, nibble, 0]
        index = table[index, nibble, 1]
        value = np.clip(step + 2 * h1 - h2, -32768, 32767)
        out[:, i + 2, :] = value
        h2, h1 = h1, value
    return out.reshape(-1, channels)


def downmix(a):
    a = a[:, 0].astype(np.float64)
    return a[:len(a)//8 * 8].reshape(-1, 8).mean(axis=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('wav', type=Path); ap.add_argument('--start', type=float, default=100)
    ap.add_argument('--seconds', type=float, default=3)
    args = ap.parse_args()
    # First row of the exact table from main's read-only image. Resolve by content,
    # then use all table entries from the local ELF; no downloaded decoder/data.
    image = (ROOT / 'build/dump/update/elf/main.elf').read_bytes()
    first = [-14, 2, -10, 2, -7, 1, -5, 1, -3, 0, -2, 0, -1, 0, 0, 0]
    pos = image.index(struct.pack('<16i', *first))
    table = np.frombuffer(image[pos:pos+16*16*2*4], dtype='<i4').reshape(16, 16, 2)
    with wave.open(str(args.wav), 'rb') as w:
        assert w.getframerate() == 48000 and w.getsampwidth() == 2
        w.setpos(round(args.start * 48000))
        target = downmix(np.frombuffer(w.readframes(round(args.seconds * 48000)), '<i2').reshape(-1, w.getnchannels()))
    target -= target.mean()
    energy = np.dot(target, target)
    result = []
    for pack in json.loads((ROOT / 'build/run/job10-audio-inventory.json').read_text()):
        with (ROOT / pack['path']).open('rb') as f:
            for e in pack['entries']:
                if e['rate'] != 48000 or e['channels'] != 2 or e['size'] < 48000 * 4:
                    continue
                f.seek(e['offset']); wem = f.read(e['size'])
                cur = 12; data = None; align = 0
                while cur + 8 <= len(wem):
                    tag, size = struct.unpack_from('<4sI', wem, cur)
                    if tag == b'fmt ': align = struct.unpack_from('<H', wem, cur + 8 + 12)[0]
                    if tag == b'data': data = wem[cur+8:cur+8+size]
                    cur += 8 + size + (size & 1)
                if not data or not align: continue
                source = downmix(decode(data, 2, align // 2, table))
                n = len(target)
                if len(source) < n: continue
                size = 1 << (len(source) + n - 1).bit_length()
                dot = np.fft.irfft(np.fft.rfft(source, size) * np.fft.rfft(target[::-1], size), size)[n-1:len(source)]
                sums = np.concatenate(([0.], np.cumsum(source)))
                squares = np.concatenate(([0.], np.cumsum(source * source)))
                var = np.maximum(squares[n:] - squares[:-n] - (sums[n:] - sums[:-n])**2 / n, 1.)
                corr = dot / np.sqrt(var * energy)
                best = int(np.argmax(corr))
                result.append(dict(pack=pack['path'], id=e['id'], correlation=float(corr[best]),
                                   source_seconds=best / 6000, source_duration=len(source) / 6000))
    print(json.dumps(dict(wav=str(args.wav), capture_start=args.start, seconds=args.seconds,
                         table_main_offset=hex(pos - 0x1000), matches=sorted(result, key=lambda e: -e['correlation'])[:10]), indent=2))


if __name__ == '__main__':
    main()
