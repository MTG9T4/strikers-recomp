"""Regression tests for the shipped Opus DLL, SDK decode, AudioOut WAV, and movie surface ABI.

Compiler scratch lives in the system temporary directory. Never rebuild junctioned inputs.
"""
import ctypes as C
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
import wave

from build_runtime import ROOT, msvc_environment


def main():
    env = msvc_environment()
    tmp = Path(tempfile.gettempdir()) / ('strikers-media-test-' + uuid.uuid4().hex)
    tmp.mkdir()
    loaded = []
    log = []
    try:
        def build(name, sources, libs):
            cl = str(Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe')
            cmd = [cl, '/nologo', '/LD', '/MD', '/O2', '/Gy', '/std:c11', '/DWIN32_LEAN_AND_MEAN',
                   '/D_CRT_SECURE_NO_WARNINGS']
            cmd += ['/I' + str(ROOT / p) for p in ('runtime/core', 'runtime/services', 'runtime/gfx', 'build/gen')]
            cmd += [str(ROOT / p) for p in sources]
            cmd += ['/link', '/OPT:REF', '/OUT:' + str(tmp / (name + '.dll')), *libs]
            r = subprocess.run(cmd, cwd=tmp, env=env, text=True, capture_output=True)
            log.append(r.stdout + r.stderr)
            if r.returncode:
                raise RuntimeError(log[-1])
            dll = C.CDLL(str(tmp / (name + '.dll')))
            loaded.append(dll)
            return dll

        audio = build('audio', ['tools/audit/job10_audio_contracts.c'],
                      ['winmm.lib', 'ole32.lib', 'uuid.lib'])
        movie = build('movie', ['tools/audit/job10_movie_contracts.c', 'runtime/gfx/nvn_movie.c'],
                      ['mfplat.lib', 'mfreadwrite.lib', 'mfuuid.lib', 'ole32.lib', 'shlwapi.lib'])
        assert movie.movie_test() == 0, 'MF format-3 texture/format-4 bounds regression'
        log.append('Movie: direct native texture, unchanged handle storage, red colour, small capacity and short source PASS')
        black = movie.black_video_test()
        assert black == 0, f'no-decoder video track as black pictures: {black:#x}'
        log.append('Movie: a video track without a decoder plays black pictures at the frame rate, seeks, and ends PASS')
        for enabled, limited, frames in ((0, 0, 0), (1, 1, 48000), (1, 0, 1536)):
            path = tmp / f'capture-{enabled}-{limited}.wav'
            assert audio.wav_test(os.fsencode(path), limited, enabled) == 0
            if not enabled:
                assert not path.exists()
                continue
            with wave.open(str(path), 'rb') as w:
                assert (w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()) == (2, 2, 48000, frames)
                assert w.readframes(1) == struct.pack('<hh', 1000, -1000)
            assert path.stat().st_size == 44 + frames * 4
        log.append('WAV: disabled, sample-exact cap, partial shutdown header, stereo gain PASS')
        # Audit 03 raised Wwise's audio thread here; since audit 04 the nn::os priority mapping does it (os_threads.c
        # host_priority, tested in job06_contracts.c), so appending never changes the producer's priority.
        LOWEST, BELOW, NORMAL = -2, -1, 0
        cases = [((LOWEST, 1, 0), LOWEST), ((BELOW, 1, 0), BELOW), ((NORMAL, 1, 0), NORMAL), ((LOWEST, 0, 0), LOWEST)]
        for args, want in cases:
            got = audio.producer_priority_test(*args)
            assert got == want, f'producer priority {args}: {got}, expected {want}'
        log.append('AudioOut producer priority: left as the nn::os mapping set it (audit 04) PASS')
        opus_path = ROOT / 'build/bin/libopus-0.dll'
        opus = C.CDLL(str(opus_path)); loaded.append(opus)
        opus.opus_encoder_create.argtypes = [C.c_int, C.c_int, C.c_int, C.POINTER(C.c_int)]
        opus.opus_encoder_create.restype = C.c_void_p
        opus.opus_encoder_destroy.argtypes = [C.c_void_p]
        opus.opus_encode.argtypes = [C.c_void_p, C.POINTER(C.c_int16), C.c_int, C.c_void_p, C.c_int]
        audio.sdk_decode.argtypes = [C.c_char_p, C.c_void_p, C.c_int, C.c_void_p, C.c_int,
                                     C.POINTER(C.c_uint64), C.POINTER(C.c_int), C.c_int]
        for channels in (1, 2):
            err = C.c_int()
            enc = opus.opus_encoder_create(48000, channels, 2049, C.byref(err))
            assert enc and err.value == 0
            try:
                pcm = (C.c_int16 * (960 * channels))(*[int(12000 * math.sin(2 * math.pi * 440 * (i // channels) / 48000))
                                                     for i in range(960 * channels)])
                packet = C.create_string_buffer(4000)
                n = opus.opus_encode(enc, pcm, 960, packet, len(packet))
                assert n > 0
                wire = struct.pack('>II', n, 0) + packet.raw[:n]
                out = (C.c_int16 * (960 * channels + 8))()
                for i in range(960 * channels, len(out)): out[i] = 0x1234
                used, frames = C.c_uint64(), C.c_int()
                assert audio.sdk_decode(os.fsencode(opus_path), wire, len(wire), out, 960 * channels * 2,
                                        C.byref(used), C.byref(frames), channels) == 0
                assert used.value == len(wire) and frames.value == 960
                peak = max(abs(x) for x in out[:960 * channels]); assert peak > 1000
                assert all(x == 0x1234 for x in out[960 * channels:])
                assert audio.sdk_decode(os.fsencode(opus_path), wire, 7, out, 960 * channels * 2,
                                        C.byref(used), C.byref(frames), channels) != 0
                log.append(f'Opus SDK: {channels}ch, {n} packet bytes -> {frames.value} frames, peak {peak}; guard and truncated header PASS')
            finally:
                opus.opus_encoder_destroy(enc)
        # Ensure source-list selection excludes all optional DNN and ISA groups.
        sys.path.insert(0, str(ROOT / 'tools/lifter'))
        import build_exe
        sources = build_exe.opus_sources(str(ROOT / 'deps/opus'))
        assert len(sources) == len(set(sources)) == 137
        assert all(Path(p).is_file() for p in sources)
        assert not any('/dnn/' in p.replace('\\', '/') or '/fixed/' in p.replace('\\', '/') for p in sources)
        log.append('Opus release source-list selection: 137 unique portable float files PASS')
        print('\n'.join(log))
    finally:
        (ROOT / 'build/run/job10-media-contracts.log').write_text('\n'.join(log) + '\n')
        for dll in reversed(loaded): C.windll.kernel32.FreeLibrary(C.c_void_p(dll._handle))
        if tmp.resolve().parent != Path(tempfile.gettempdir()).resolve():
            raise RuntimeError('scratch cleanup escaped temp')
        shutil.rmtree(tmp)


if __name__ == '__main__':
    main()
