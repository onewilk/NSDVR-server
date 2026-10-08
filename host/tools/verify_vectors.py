#!/usr/bin/env python3
"""Checks the NSDVR extension test vectors against the reference implementation
(NSDVR/tools/audio_codec_eval.py, needs numpy) and re-parses every stream.

usage: verify_vectors.py <ext_vectors dir> <path to audio_codec_eval.py>
"""
import importlib.util
import os
import struct
import sys

import numpy as np


def load_ref(path):
    spec = importlib.util.spec_from_file_location('audio_codec_eval', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def raw(path):
    return np.fromfile(path, dtype='<i2').reshape(-1, 2)


def packets(path):
    data = open(path, 'rb').read()
    pos = 0
    while pos < len(data):
        magic, size, ts, meta, slot = struct.unpack_from('<IIQBB', data, pos)
        assert magic == 0xCCCCCCCC, f'{path}: bad magic at {pos}'
        yield pos, ts, meta, slot, data[pos + 18:pos + 18 + size]
        pos += 18 + size
    assert pos == len(data), f'{path}: trailing bytes'


def adpcm_decode(body, frames, ref):
    out = np.empty((frames, 2), dtype=np.int16)
    for c in range(2):
        pred, idx = struct.unpack_from('<hB', body, 4 * c)
        for i in range(frames):
            code = (body[8 + i] >> (4 * c)) & 15
            step = ref.STEPS[idx]
            delta = step >> 3
            if code & 4: delta += step
            if code & 2: delta += step >> 1
            if code & 1: delta += step >> 2
            pred = pred - delta if code & 8 else pred + delta
            pred = max(-32768, min(32767, pred))
            idx = max(0, min(88, idx + ref.INDEX_ADJ[code & 7]))
            out[i, c] = pred
    return out


def main():
    d, refpath = sys.argv[1], sys.argv[2]
    ref = load_ref(refpath)
    x = raw(os.path.join(d, 'input_48k_s16le_stereo.raw')).astype(np.float64)
    ok = True

    # PCM24 against down24(): streaming output k == reference output k - 15 (filter delay 31 input samples)
    exp24 = raw(os.path.join(d, 'pcm24.expected_24k_s16le_stereo.raw')).astype(np.int64)
    r24 = ref.down24(x).astype(np.int64)
    n = len(exp24) - 15
    diff = np.abs(exp24[15:15 + n] - r24[:n])
    print(f'PCM24 vs audio_codec_eval.down24: {diff.size} samples, max |diff| {diff.max()} LSB, {np.count_nonzero(diff)} differ')
    ok &= diff.max() <= 1 and np.count_nonzero(diff) * 1000 < diff.size

    # ADPCM against adpcm() (bit exact)
    expad = raw(os.path.join(d, 'adpcm.expected_48k_s16le_stereo.raw')).astype(np.int64)
    rad = ref.adpcm(x).astype(np.int64)
    same = np.array_equal(expad, rad[:len(expad)])
    print(f'ADPCM vs audio_codec_eval.adpcm: {"bit exact" if same else "MISMATCH"} ({len(expad)} frames)')
    ok &= same

    # Re-parse every stream
    for name in ['pcm48', 'pcm24', 'adpcm', 'opus_96k_20ms_c5', 'opus_64k_10ms_c0', 'opus_256k_20ms_c10', 'switch']:
        dec, next_ts, count, frames_total = [], None, 0, 0
        prev_codec = None
        for pos, ts, meta, slot, body in packets(os.path.join(d, f'{name}.stream.bin')):
            count += 1
            assert meta == 0x06 and 0xE0 <= slot <= 0xE3, f'{name}: meta {meta:#x} slot {slot:#x}'
            codec = slot & 3
            if codec == 0:
                spc, rate = len(body) // 4, 48000
                dec.append(np.frombuffer(body, dtype='<i2').reshape(-1, 2))
            else:
                ver, hc, cf, fc, kbps, spc, enc_us = struct.unpack_from('<BBBBHHI', body, 0)
                assert ver == 1 and hc == codec, f'{name}: header'
                b = body[12:]
                if codec == 1:
                    assert len(b) == 4 * spc and kbps == 768
                    rate = 24000
                    dec.append(np.frombuffer(b, dtype='<i2').reshape(-1, 2))
                elif codec == 2:
                    assert len(b) == 8 + spc and kbps == 384
                    rate = 48000
                    dec.append(adpcm_decode(b, spc, ref))
                else:
                    fs = 480 if cf >> 4 == 1 else 960
                    assert spc == fc * fs, f'{name}: opus spc'
                    p = 0
                    for _ in range(fc):
                        (ln,) = struct.unpack_from('<H', b, p)
                        assert ln == kbps * fs // 384, f'{name}: CBR frame length {ln}'
                        p += 2 + ln
                    assert p == len(b), f'{name}: opus body'
                    rate = 48000
            if next_ts is not None and codec == prev_codec:
                assert abs(ts - next_ts) <= 1, f'{name}: timestamp {ts} expected {next_ts}'
            next_ts = ts + spc * 1000000 // rate
            prev_codec = codec
            frames_total += spc
        if name in ('pcm48', 'pcm24', 'adpcm'):
            got = np.concatenate(dec)
            suffix = 'expected_24k_s16le_stereo.raw' if name == 'pcm24' else 'expected_48k_s16le_stereo.raw'
            exp = raw(os.path.join(d, f'{name}.{suffix}'))
            same = np.array_equal(got, exp)
            ok &= same
            print(f'{name}: {count} packets, python decode == expected file: {same}')
        else:
            print(f'{name}: {count} packets, {frames_total} samples/channel, structure and timestamps OK')

    print('RESULT:', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
