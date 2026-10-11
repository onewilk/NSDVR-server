# Audio compression test, 2026-10-11

[中文](README.zh-CN.md)

What the NSDVR audio encodings cost on the console, and whether they reduce dropped frames. Raw data is in
[`data/`](data/); the summary was posted to [SysDVR #402](https://github.com/exelix11/SysDVR/issues/402#issuecomment-6106541472).

## Summary

- **CPU.** While racing in Mario Kart 8 Deluxe, PCM streaming puts core 3 at 59.3%. 24 kHz PCM and IMA ADPCM cost
  nothing extra (−0.4 points), Opus 64 kbps / complexity 0 adds 1.2 points, Opus 96 kbps / complexity 5 adds 1.7,
  Opus 128 kbps / complexity 10 / 10 ms frames adds 4.6. Cores 0–2 (the game's) never change.
- **Good network: nothing to fix.** With the phone's 5 GHz hotspot or a router nearby, actual gameplay drops almost no
  frames with any codec (0–0.7%; Tears of the Kingdom 0.06%).
- **Marginal network: Opus helps a lot.** Dropped frames follow how far the stream's bitrate exceeds what the Wi-Fi link
  can carry. Video alone needs about 5 Mbps; PCM brings it to 6.5, 24 kHz to 5.75, Opus 64k to 5.07. With the link
  around 5.3–5.7 Mbps, PCM dropped 17–23% of frames while Opus dropped 0–9%. Below ~5 Mbps nothing helps.
- **Mario Kart 8's mode-select menu** fully loads core 3 for about 0.5 s every 30 s, and SysDVR drops about a second of
  video each time. It happens without streaming and with every other sysmodule disabled, and not while racing or in
  other games. It is not caused by SysDVR or by the network.
- **Recommendation:** keep Opus 64 kbps / complexity 0 as the compressed option (optionally 96 kbps / complexity 5 for
  quality, 24 kHz PCM as a no-CPU fallback); drop ADPCM and high-complexity Opus.

## Setup

| | |
|---|---|
| Console | Switch, firmware 22.5.0, Atmosphère. CPU 1020 MHz (sys-clk); its auto CPU boost reacts to the menu spikes, see below |
| Client | NSDVR debug build on a HUAWEI Mate 90 Pro Max, TCP Bridge, smoothing "low latency", network optimization on |
| Network | phone's 5 GHz Wi-Fi Direct hotspot (channel 36), or a 5 GHz router; "weak" = console moved away from the router |
| Games | Mario Kart 8 Deluxe (mode-select menu, and Grand Prix with smart steering + auto-accelerate), Tears of the Kingdom |
| Builds | [`perflog`](../../PERFLOG.md) = upstream master + logger, `nsdvr` + logger (`make PERFLOG=1`), plain `nsdvr` |

### Method

- The test-only logger ([PERFLOG.md](../../PERFLOG.md)) writes one line per second on the console: busy share of all four
  cores (idle tick count read by a thread pinned to each core, like Status Monitor), SysDVR's thread CPU, bytes sent,
  the audio encoding in use, clocks and free memory. Its own CPU time is logged too (0.007% on average).
- The client writes one line per second to `stats.csv`, including the extension's diagnostics (grc gaps, send blocking).
- The client's automatic rotation switches the encoding every minute; both logs record the encoding, which is also used
  to align the console and phone clocks ([`tools/perf_report.py`](https://github.com/onewilk/NSDVR/blob/main/tools/perf_report.py)).
- The first 3 seconds after every switch, seconds within ±1 s of a core 3 spike, and (while racing) loading screens are
  left out.

## Results

### CPU on a static menu screen (Mario Kart 8, core 3 busy %, spikes excluded)

| | Core 3 | Audio thread |
|---|---|---|
| Not streaming | 54.0 | – |
| Upstream: video only | 59.1 | – |
| Upstream: video + PCM | 62.4 | 0.1 |
| Extension: PCM | 63.7 | 0.1 |
| Extension: 24 kHz | 63.4 | 0.3 |
| Extension: ADPCM | 63.1 | 0.6 |
| Extension: Opus 64k c0 | 64.2 | 2.1 |
| Extension: Opus 96k c5 | 65.6 | 3.7 |
| Extension: Opus 128k c10 10 ms | 67.3 | 5.5 |

Video costs about 5 points of core 3 and PCM audio about 3. The two upstream rows come from a different boot than the
rest; the extension's PCM path being 1.3 points higher is either its diagnostics or boot-to-boot noise.

### CPU while racing (Mario Kart 8, two rotations, about 100 s per encoding)

| | Core 3 | vs PCM | Audio thread | Encode ms/s | Sent kbps |
|---|---|---|---|---|---|
| Not streaming | 47.7 | | | | 0 |
| PCM | 59.3 | | 0.1 | 0.0 | 6486 |
| 24 kHz | 59.0 | −0.4 | 0.3 | 3.8 | 5772 |
| ADPCM | 58.9 | −0.4 | 0.6 | 8.2 | 5385 |
| Opus 64k c0 | 60.5 | +1.2 | 2.1 | 37.0 | 5066 |
| Opus 96k c5 | 61.1 | +1.7 | 3.7 | 58.8 | 5107 |
| Opus 128k c10 10 ms | 63.9 | +4.6 | 5.9 | 93.3 | 5090 |

95% confidence intervals are within ±0.4 points. Cores 0–2: about 50 / 37 / 30% for every encoding. The
"not streaming" row has only ~30 s of racing.

### Dropped frames with a good connection

| Run | Duration | Dropped frames |
|---|---|---|
| Mario Kart 8, racing, router, all encodings | 12 min | 0–0.7% per encoding |
| Mario Kart 8, racing, router, PCM | 4 min | 1 frame |
| Tears of the Kingdom, hotspot, PCM | 4.6 min | 5 frames (0.06%) |

### Mario Kart 8 mode-select menu: a core 3 spike every 30 s

- Every 30 s, core 3 goes to ~100% for about 0.5 s (sys-clk then boosts the CPU to 1785 MHz, which is a reaction, not
  the cause). During the spike SysDVR's sends stall for 225–690 ms and grc drops frames.
- In the first run, all 19 send stalls longer than 200 ms and 577 of 584 dropped frames were within ±1 s of a spike.
- Still there without streaming, with all other sysmodules disabled (SaltyNX, sys-clk, fan control, emuiibo,
  MissionControl, sys-KeyX, NxThemes; only sys-patch and nx-ovlloader kept), without the logger (plain `nsdvr`: a stall
  every 30 s, 26–31 frames each, 92% of all drops), and on a router instead of the hotspot.
- Not there on the home menu, while racing, or in Tears of the Kingdom.

### Marginal network (console far from the router, PCM / 24 kHz / Opus 64k rotating every minute)

Dropped frames per one-minute segment, in order:

| Run | PCM | 24 kHz | Opus 64k |
|---|---|---|---|
| 1 | 0.1, 2.3, 0, **63.2**, 17.9 | **27.8**, 0, 2.2, **48.0**, 0.9 | 0, 0, 0, 5.8, 0.6 |
| 2 | 12.9, 17.4, 23.5, 23.0, 23.4 | 1.3, 2.0, 12.9, 37.4 | 0.1, 2.3, 9.4, **62.4** |
| Pooled | 16.0% / 21.3% | 15.8% / 14.3% | **1.3%** / 18.6% |

What the console actually sent in run 2 (logger), against what each encoding needs:

| | Needs | Sent (share of need) | Dropped |
|---|---|---|---|
| PCM | ~6.5 Mbps | 5.3–5.7 Mbps (81–87%) | 17–23% |
| 24 kHz | ~5.75 Mbps | 5.7 → 5.1 Mbps (99% → 89%) | 1–2% → 13% |
| Opus 64k | ~5.07 Mbps | 5.1 → 4.6 Mbps (100% → 90%) | 0–2% → 9% |

The 62% Opus segment in run 2 is a minute in which the link collapsed to 1.4–2.5 Mbps (with a reconnect); every
encoding fails there. Against the segments right before and after it, Opus was lower in 7 of 9 comparisons (1 tie,
1 higher: the collapse).

## Conclusions

1. The extension's encodings are cheap on core 3; low-complexity Opus costs about one point and never touches the game's
   cores.
2. With a good link nothing is dropped and compression changes nothing. When the link is between about 5 and 6.5 Mbps,
   Opus 64k (1.4 Mbps less than PCM) is the difference between smooth and broken; 24 kHz only saves 0.75 Mbps and helps
   less. Below 5 Mbps the video alone does not fit.
3. TCP itself showed no problem: whenever the link could carry the stream (Opus at 98–100% of its need), almost
   nothing was dropped (0–2%), and with a good link there were no send stalls during gameplay. The drops seen came from link capacity or
   from console CPU (the menu spike), neither of which a different transport fixes. A UDP mode is therefore not
   justified by this data.
4. The client's network early warning only sees the phone's own Wi-Fi link (it reported "normal" throughout the
   marginal runs, where the console's link was the weak one).

## Limitations

- One console, one phone, two games; the menu spike was only seen in Mario Kart 8.
- The marginal-network runs depend on a link whose capacity changed during the test; the conclusion rests on the
  consistent ordering and the send-rate evidence, not on the pooled percentages.
- Listening quality was not part of this test (see the earlier offline evaluation for ADPCM's noise on chiptune music).

## Data

| File | Run |
|---|---|
| `perf_1791693561.csv` | extension + logger, MK8 menu, all six encodings, hotspot |
| `perf_1791695134.csv` | upstream + logger, MK8 menu: not streaming / video + PCM / video only |
| `perf_1791696042.csv` | extension + logger, MK8 menu, not streaming, all other sysmodules disabled |
| `perf_1791698333.csv` | extension + logger, MK8 racing, six encodings × 2, router |
| `perf_1791699432.csv` | extension + logger, marginal network run 1 |
| `perf_1791701133.csv` | extension + logger, marginal network run 2 |
| `client_stats.csv` | the phone's per-second stats for every session that day (UTC session times: 04:49–04:51 menu, 05:08 upstream, 05:32 plain build control, 05:39 Tears of the Kingdom, 05:47 router menu → racing, 06:00 racing, 06:19 marginal 1, 06:46 marginal 2) |

Reproduce a table, e.g. the racing run:

```
python3 tools/perf_report.py nsdvr=data/perf_1791698333.csv --stats data/client_stats.csv
```

(`tools/perf_report.py` is in the client repository.)
