# Changelog

**English** | [简体中文](CHANGELOG.zh-CN.md)

Changes to the NSDVR experimental server (the `nsdvr` branch), which is based on SysDVR 6.3 (upstream commit
804fd36); upstream changes are in SysDVR's own releases. A `nsdvr-v<version>` tag publishes the sysmodule built by CI as
a GitHub pre-release, with the matching section below as its release notes. No build has been published yet.

## 1.1.0 — 2026-10-11

### Fixed

- An extension audio session now ends as soon as its client closes the connection, or when a video session ends.
  Before, it ended only after sending had been blocked for 10 s. At Opus 64 kbps (about 8 KB/s) a client that went
  away without its FIN or RST reaching the console kept the session alive for 40–80 s, and the audio port refused the
  client's next connection all that time.

### Added

- Test-only performance logger, `make PERFLOG=1` (normal builds do not include it): once per second it writes the load
  of every core, SysDVR's thread CPU, bytes sent, the audio encoding, encode time, clocks and free memory to
  `/config/sysdvr/perf/` on the SD card. See [PERFLOG.md](PERFLOG.md).
- [Audio compression test report](reports/2026-10-11-audio-compression/README.md), the first measurements on a real
  console (firmware 22.5.0): low-complexity Opus 64k costs about 1.2 points of core 3 and sharply reduces dropped
  frames when the Wi-Fi link can carry only about 5 to 6.5 Mbps.

## 1.0.0 — 2026-10-08

First public version of the extension (source only).

### Added

- A backward-compatible extension of TCP Bridge protocol 03, enabled only when the client sets FeatureFlags bit 2.
  Clients without the flag, including the official SysDVR client, take the unchanged official code path.
- Audio codecs PCM 48 kHz, PCM 24 kHz (63-tap half-band decimator), IMA ADPCM and Opus (libopus 1.6.1, fixed point,
  CELT only, constant bitrate), switchable at any time with 8-byte control messages on the audio socket, without
  reconnecting. Each packet reports how long the console spent encoding it.
- One diagnostics packet per second on the video channel: send blocking time, grc gaps, gaps after slow sends, audio
  encode time, core 3 idle ticks, SysDVR thread ticks and IP_TOS results.
- IP_TOS 0xA0 (AC_VI) on request.
- No heap allocations: the Opus state and a static pseudostack share the `StaticBuffers` union with RTSP mode, and
  libopus hardening failures return an error instead of aborting the sysmodule. About 164 KB more memory than the
  official build.
- `host/`: the extension built for macOS with a simulated console, a test client, unit and fuzz tests and end-to-end
  scenarios; CI builds the sysmodule (with a size comparison against the official build) and runs them.
