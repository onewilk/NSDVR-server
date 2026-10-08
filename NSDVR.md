# NSDVR experimental server

**English** | [简体中文](NSDVR.zh-CN.md)

This repository is an unofficial fork of [SysDVR](https://github.com/exelix11/SysDVR) by exelix11. The `nsdvr`
branch carries the server (sysmodule) side of the experimental protocol extension used by
[NSDVR](https://github.com/onewilk/NSDVR), a third-party SysDVR client for HarmonyOS.

It is not an official SysDVR release and is not affiliated with the SysDVR project. Please do not report problems with
this build to upstream SysDVR.

> [!WARNING]
> Experimental. So far it has been verified with host builds, unit/fuzz tests and end-to-end tests against a simulated
> Switch (`host/`), and it builds with devkitA64. It has **not been tested on real hardware yet**.

## What it adds

A backward-compatible extension of the TCP Bridge protocol 03. It is enabled only when a client sets FeatureFlags
bit 2 in the handshake; the official handshake ignores unknown feature bits and the Reserved bytes. Clients without
the flag, including the official SysDVR client, take the unchanged official code path.

- **Audio compression, switchable at any time**: PCM 48 kHz, PCM 24 kHz (63-tap half-band decimator), IMA ADPCM and
  Opus (libopus 1.6.1, fixed point, CELT only, constant bitrate). The client switches the codec, Opus bitrate,
  complexity and frame size with 8-byte control messages on the audio socket, without reconnecting.
- **Diagnostics**: one packet per second on the video channel with send blocking time, grc gaps, gaps after slow
  sends, audio encode time, CPU core 3 idle ticks, SysDVR thread ticks and IP_TOS results.
- **IP_TOS 0xA0** (AC_VI) on request.
- **No heap allocations**: the Opus state and a static pseudostack live in the `StaticBuffers` union, shared with RTSP
  mode. libopus hardening failures `longjmp` back to the caller instead of calling `abort()`, so a libopus problem
  degrades the stream instead of crashing the sysmodule.
- **Memory cost**: about +164 KB of process memory compared with the official build (details in
  [host/README.md](host/README.md)).

The screen-off behavior ([SysDVR#402](https://github.com/exelix11/SysDVR/issues/402)) is untouched and left to upstream.

## Protocol

The wire format is specified in `docs/nsdvr-ext-protocol.md` in the
[NSDVR](https://github.com/onewilk/NSDVR) repository. That document is the contract between the two
repositories; shared test vectors live in its `tools/ext_vectors/`.

## Layout

| Path | Contents |
|---|---|
| `sysmodule/source/next/` | Extension code (portable C, shared by the sysmodule and the host build) |
| `sysmodule/source/modes/TCPnext.c` | TCP Bridge integration |
| `third_party/opus/` | Unmodified libopus 1.6.1 release (some test/doc/model directories removed) |
| `third_party/opus_nx/` | libopus configuration and build glue for the sysmodule |
| `host/` | macOS build: simulated Switch (`sysdvr_hostmock`), test client, unit/fuzz tests, benchmarks, vector generator |
| `build_nsdvr_docker.sh` | Builds this branch and the official baseline with Docker |

Branches: `master` is upstream SysDVR at 804fd36, unchanged; `nsdvr` is the extension.

## Building

With Docker (no local devkitPro needed):

```sh
./build_nsdvr_docker.sh
```

It builds this branch and the official baseline (804fd36) with the `devkitpro/devkita64` image and packages both like
`ReleaseSysmodule.sh` does:

- `out/nsdvr/atmosphere/contents/00FF0000A53BB665/` – this branch
- `out/baseline/atmosphere/contents/00FF0000A53BB665/` – the official code, for comparison
- `out/SIZES.txt` – section sizes of both builds

With a local devkitPro installation, `make -C sysmodule` works as in upstream.

## Trying it on a Switch

Only if you are comfortable testing experimental sysmodules:

1. Back up `atmosphere/contents/00FF0000A53BB665/` from your SD card.
2. Copy `out/nsdvr/atmosphere` over it and reboot.
3. Connect with NSDVR over TCP Bridge. Official clients keep working as before.

Check the free memory first: the extension needs about 164 KB more than the official build. To go back, restore the
backup and reboot.

## Testing without a Switch

See [host/README.md](host/README.md): the same extension code is built on macOS with a simulated Switch, a test
client, unit and fuzz tests (ASan/UBSan) and an end-to-end self test.

## License

GPL-2.0, the same as SysDVR (see [LICENSE](LICENSE)). libopus is BSD-3-Clause (`third_party/opus/COPYING`).

All credit for SysDVR itself goes to exelix11 and the SysDVR contributors.
