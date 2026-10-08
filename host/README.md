# NSDVR extension: macOS host build

**English** | [简体中文](README.zh-CN.md)

This builds **exactly the same** extension code as the sysmodule (`../sysmodule/source/next/*.c`) and the same
libopus tree (`../third_party/opus`, the official 1.6.1 tarball, configured by `../third_party/opus_nx`) with clang
on macOS. Outputs:

| Output | Purpose |
|---|---|
| `build/sysdvr_hostmock` | Simulates SysDVR on a Switch (TCP Bridge protocol 03 + NSDVR extension) for client end-to-end tests |
| `build/sysdvr_hostclient` | Test client: handshake, validates every packet, decodes every audio codec, sends scheduled control messages, summarizes diagnostics |
| `build/next_tests` | Unit and fuzz tests (ASan + UBSan) |
| `build/next_bench` | Encode time, libopus state / pseudostack size, thread stack usage |
| `build/next_vectors` | Generates the shared test vectors (written to `tools/ext_vectors/` in the client repository) |

Protocol: `docs/nsdvr-ext-protocol.md` in the client repository, [NSDVR](https://github.com/onewilk/NSDVR).

The default paths below assume the client repository is cloned next to this one (`../NSDVR`). Override with
`CLIENT_REPO=...`.

## Build and test

```sh
cd NSDVR-server/host
make -j10            # everything
make test            # unit / fuzz tests (needs a 48 kHz WAV, see TEST_WAV in the Makefile)
make bench           # timing and memory measurements
make demo-h264       # build/demo.h264 (10 s, 1280x720@30) with the client's tools/make_demo_h264.swift
make selftest        # end to end: 9 scenarios (ports 29911/29922, a hostmock on the default ports is not disturbed)
make vectors         # regenerate the test vectors
python3 tools/verify_vectors.py <ext_vectors dir> <audio_codec_eval.py>   # needs numpy: re-checks the vectors with the reference implementation
```

### Test audio

`TEST_WAV` can be any 48 kHz 16-bit WAV. The test vectors and the measurements below use
[Raspberrymusic – Aliens](https://commons.wikimedia.org/wiki/File:Raspberrymusic_-_Aliens_(trailer_music;_cinematic_epic_electronic_classical_music).flac)
from Wikimedia Commons (CC BY 3.0), converted to 48 kHz 16-bit stereo and stored as `audio_eval/src/aliens_48k.wav`
in the client repository (that directory is not committed).

## sysdvr_hostmock

```sh
./build/sysdvr_hostmock --h264 build/demo.h264 \
    --wav ../../NSDVR/audio_eval/src/aliens_48k.wav
```

By default it listens on `0.0.0.0:9911` (video) and `0.0.0.0:9922` (audio) and sends the
`SysDVR|6.3|03|XAW00000000000` beacon to UDP 19999 on `255.255.255.255,127.0.0.1` every 2 s. Options:

| Option | Description |
|---|---|
| `--h264 FILE` | Annex-B H.264, looped at `--fps` (default 30); one packet per VCL NAL with the preceding SPS/PPS/SEI merged in (one frame per packet, like grc). Without it, undecodable synthetic frames are sent |
| `--wav FILE` | 48 kHz 16-bit WAV (mono is duplicated to stereo), read in real time in 1024-sample chunks, looped |
| `--listen ADDR` | Listen address (for example `--listen 192.168.1.10` to serve only the LAN interface) |
| `--video-port N` / `--audio-port N` | Ports (default 9911 / 9922) |
| `--stall-every SEC --stall-ms MS` | Every SEC seconds the **video send** blocks for MS ms (default 300 ms). Stalls count in `videoSendBlock*`; the virtual grc keeps producing frames and drops old ones once reading falls more than `--grc-queue` (default 3) frames behind, so grc gaps appear and `gapsAfterSlowSend` goes up |
| `--stall-audio-every SEC --stall-audio-ms MS` | Same for the audio send |
| `--drop-every N` | Drops every Nth non-IDR frame "at the source": gaps without a slow send before them (`gapsAfterSlowSend` does not go up), to tell the two causes of frame loss apart |
| `--grc-queue N` | Frames the virtual grc keeps while reading falls behind |
| `--sndbuf BYTES` | SO_SNDBUF of the client socket (simulates the Switch's smaller send buffer) |
| `--no-opus` | Simulates Opus memory being unavailable: Opus requests fall back to PCM48 (marker 0xE0) |
| `--no-beacon` / `--beacon-targets LIST` / `--serial S` | Beacon control |
| `--once` | Exit after serving one connection per port |
| `--duration SEC` | Exit after SEC seconds |
| `--quiet-diag` | Do not print every diagnostics packet |

hostmock prints the handshake (whether the extension was requested, initial codec parameters), every audio codec
switch (read back from the packets actually sent), every diagnostics packet, and how many frames the virtual grc
dropped when the client disconnects.

### Behavior (same as the sysmodule)

- Handshake: after connecting it sends the 10-byte `SysDVR|03\0`, receives the 16-byte request, validates it with
  the official rules (magic, version, channels, AudioBatching ≤ 5), replies with the 72-byte response (with a set of
  fake memory pools when MemoryDiag was requested), then waits 500 ms before streaming.
- Clients without the extension flag (FeatureFlags bit 2) take the official loop: raw PCM audio, `ReplaySlot = 0xFF`,
  no diagnostics packets.
- With the extension flag it runs `next_session.c` (the same code the sysmodule uses):
  - Audio packets have `ReplaySlot = 0xE0 | codec` and `MetaData = 0x06`; compressed codecs carry the 12-byte
    `ExtAudioHeader` (`encodeUs` is measured).
  - Control messages on the audio socket are read non-blockingly before every packet and take effect from the next
    packet; a codec switch drops the incomplete Opus frame tail (< 20 ms).
  - When diagnostics were requested, the video channel carries one diagnostics packet per second:
    **`MetaData = 0x07`** (type bits 3 + Data bit 0x04), `ReplaySlot = 0xFF`, `Timestamp` = timestamp of the latest
    video frame, 56-byte ExtDiag v1 payload. Client dispatchers should check `(MetaData & 3) == 3` first.
  - grc capture error packets keep the official format (no ExtAudioHeader, `ReplaySlot = 0xFF`).
  - In the diagnostics, `core3IdlePermille` is the idle permille of the Mac's CPU 3 and `sysdvrCpuPermille` is the
    CPU usage of the whole hostmock process.
- Opus: libopus 1.6.1 fixed point, `OPUS_APPLICATION_RESTRICTED_CELT` (CELT only), constant bitrate, stereo 48 kHz;
  every frame is exactly `kbps × frame samples / 384` bytes; encoder delay 120 samples (2.5 ms).

## sysdvr_hostclient

```sh
./build/sysdvr_hostclient --seconds 10 --codec opus --kbps 96 --complexity 5 \
    --ctrl "3:adpcm,5:pcm24,7:opus:64:10:10"      # time:codec[:kbps[:complexity[:frame ms]]]
./build/sysdvr_hostclient --legacy                # official protocol (no extension flag)
./build/sysdvr_hostclient --fuzz-ctrl             # keeps sending garbage / truncated / invalid control messages on the audio socket
./build/sysdvr_hostclient --dump-wav out.wav      # decoded audio (PCM24 upsampled by simple repetition, for listening only)
```

Checks: header magic/size, type bits, consistency of the audio marker and ExtAudioHeader, Opus CBR frame size and
count, each ADPCM packet's state header matching the decoder state at the end of the previous packet, continuous
audio timestamps (more than 25 ms off within one codec is a violation), diagnostics interval, audio not silent.
The `--expect-*` options are for automated assertions (see `tests/selftest.sh`). Exit code 0 = pass.

## Measurements (Apple M4, `make bench`)

See `build/bench.txt`. Highlights (2026-09-30):

- libopus encoder state (RESTRICTED_CELT, stereo, fixed point): 10380 bytes (static buffer 10496).
- Pseudostack (NONTHREADSAFE_PSEUDOSTACK) worst case 41532 bytes (20 ms, complexity ≥ 8; at most 23660 with 10 ms
  frames), configured as 43008 bytes. Measured by binary searching the smallest working limit under libopus's own
  pseudostack overflow check (ENABLE_HARDENING) over 2 frame sizes × 11 complexities × 4 bitrates × 4 signals.
- Encode time (CPU ms per second of audio, AudioBatching 3): PCM24 0.30, ADPCM 0.40,
  Opus 96k/20 ms: c0 1.5, c5 2.7, c10 3.9; 256k/20 ms/c10 4.9.

## Sysmodule memory and stack (devkitA64 GCC 15.2, `../build_nsdvr_docker.sh`)

- Compared with the official 804fd36 (`out/SIZES.txt`): `.text` 67008 → 181760, `.rodata` 1448 → 20464,
  `.eh_frame` 12424 → 13920, `.data` 784 → 792, `.bss` 909288 → 938888. The three page-aligned segments together go
  from 1003520 to 1171456 bytes, **+164 KB of process memory**.
- The `.bss` growth comes from two places: the new `TcpMode` member of the `StaticBuffers` union (Opus state 10496 +
  pseudostack 43008 + frame buffer 640 bytes, sharing memory with RTSP mode; the union grows from 28 KB to 56 KB) and
  3840 bytes of headroom in front of `APkt` (for in-place encoding).
- The audio thread stack is still 0x2000: static analysis with `-fcallgraph-info=su` (`tools/stackcheck.sh` +
  `tools/maxstack.py`) gives a worst call chain of 3824 bytes (NextSession_Audio → … → celt_encode_with_ec →
  quant_all_bands → quant_partition → alg_quant), plus up to 4 levels of quant_partition recursion (240 bytes each),
  about 4.8 KB in total; there are no VLAs or alloca (the pseudostack replaces them).
