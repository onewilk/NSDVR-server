# Performance logger (test builds only)

A test-only build option of the sysmodule (`make PERFLOG=1`) that records console-side CPU usage while
streaming, so codec comparisons don't depend on reading an overlay by hand. It is never part of a release.

## What it records

Once per second, one CSV line in `/config/sysdvr/perf/perf_<unix time>.csv` on the SD card (a new file per
boot, written every 5 s, capped at 48 MB):

| Column | Meaning |
|---|---|
| `unix`, `uptime_s` | console clock (network clock if synced) and seconds since boot |
| `title_id` | foreground game (0 = home menu), refreshed every 5 s |
| `c0`–`c3` | busy permille of each CPU core: idle tick count read by a thread pinned to that core, the way Status Monitor measures it |
| `sysdvr_cpu` | CPU time of SysDVR's video, audio and main threads, permille of one core |
| `video_cpu`, `audio_cpu`, `main_cpu` | the same per thread |
| `log_cpu` | the logger's own threads (to show it doesn't skew the numbers) |
| `send_kbps` | bytes SysDVR sent, all connections |
| `audio` | `-` no audio stream, `official` official PCM path, or the extension's encoding: `pcm`, `24k`, `adpcm`, `opus/<kbps>/<complexity>/<frame ms>` |
| `enc_us`, `audio_pkts` | extension only: encode time and packets in that second |
| `cpu_mhz`, `gpu_mhz` | current clocks (overclocking tools change them; compare like with like) |
| `sys_free_kb` | free memory in the system pool |

It keeps running while nothing is streaming, which gives the game-only baseline.

## Builds

`./build_perflog_docker.sh` builds two variants with the devkitpro/devkita64 image:

- `out/perflog-official`: branch `perflog` = official SysDVR (upstream master) + logger
- `out/perflog-nsdvr`: branch `nsdvr-perflog` = NSDVR extension + logger

Both carry the same logger, so its small overhead cancels out when they are compared. The logger adds
about 18 KB of code and 29 KB of static memory, and uses an NPDM that allows cores 0–3 (the sampler
threads for cores 0–2 wake once per second).

## Reading the logs

`tools/perf_report.py` in the client repository (onewilk/NSDVR) groups the seconds by build, game and
audio path, and joins the client's `stats.csv` for frame loss, grc gaps and send blocking:

```
python3 tools/perf_report.py official=perf_A.csv nsdvr=perf_B.csv --stats stats.csv
```
