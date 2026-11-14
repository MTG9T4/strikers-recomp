"""WAV levels and QPC/device-position drift for a selected presented-frame range."""
import argparse
import json
from pathlib import Path
import re
import wave
import numpy as np


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('wav', type=Path)
    p.add_argument('timeline', type=Path)
    p.add_argument('--frames', default='6500,18000')
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    text = a.timeline.read_text()
    freq = int(re.search(r'# QPC (\d+)', text)[1])
    rows = np.array([list(map(int, l.split()[1:])) for l in text.splitlines() if l.startswith('P ')], dtype=np.int64)
    lo, hi = map(int, a.frames.split(','))
    rows = rows[(rows[:, 0] >= lo) & (rows[:, 0] <= hi) & (rows[:, 2] >= 0)]
    assert len(rows) > 10
    with wave.open(str(a.wav)) as f:
        rate, channels, frames = f.getframerate(), f.getnchannels(), f.getnframes()
        assert f.getsampwidth() == 2 and channels == 2
        start, end = int(rows[0, 2]), int(rows[-1, 2])
        assert 0 <= start < end <= frames
        f.setpos(start)
        pcm = np.frombuffer(f.readframes(end - start), dtype='<i2').reshape(-1, 2).astype(np.float64)
    rms = np.sqrt(np.mean(pcm ** 2, axis=0))
    peak = np.max(np.abs(pcm), axis=0)
    elapsed = (rows[:, 1] - rows[0, 1]) / freq
    sample_elapsed = (rows[:, 2] - rows[0, 2]) / rate
    drift_ms = (sample_elapsed - elapsed) * 1000
    queue_ms = (rows[:, 3] - rows[:, 2]) / rate * 1000
    # Compare the same device cursor with the same QPC used by presents.
    result = dict(wav=str(a.wav), timeline=str(a.timeline), sample_rate=rate, channels=channels,
                  total_seconds=frames / rate, presented_frames=[int(rows[0, 0]), int(rows[-1, 0])],
                  device_sample_range=[start, end], duration_seconds=float(elapsed[-1]),
                  measured_fps=float((rows[-1, 0] - rows[0, 0]) / elapsed[-1]),
                  rms_dbfs=(20 * np.log10(np.maximum(rms, 1) / 32768)).tolist(),
                  peak_dbfs=(20 * np.log10(np.maximum(peak, 1) / 32768)).tolist(),
                  clipped_samples=int(np.sum((pcm >= 32767) | (pcm <= -32768))),
                  zero_sample_fraction=float(np.mean(pcm == 0)),
                  device_clock_drift_end_ms=float(drift_ms[-1]),
                  device_clock_drift_p01_p99_ms=np.percentile(drift_ms, [1, 99]).tolist(),
                  submitted_minus_played_p01_p50_p99_ms=np.percentile(queue_ms, [1, 50, 99]).tolist(),
                  goal_sync='not inferred from overall clock slope; annotate goal frame and sound sample separately')
    a.out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
