#pragma once
// Test-only performance logger, built with `make PERFLOG=1` (never part of a release build).
//
// Once per second it appends one CSV line to /config/sysdvr/perf/perf_<unix time>.csv on the SD card:
//   - busy permille of all four CPU cores, measured like Status Monitor does it (idle tick count read
//     by a thread pinned to each core),
//   - CPU time of SysDVR's own threads (video, audio, IPC/main) and of the logger itself,
//   - bytes sent by SysDVR, the foreground title, the CPU clock and the free system memory pool,
//   - which audio path is streaming (official PCM or the extension's encoding) and its encode time.
// Lines are buffered and written every few seconds, so the SD card is touched rarely. The logger
// keeps running while nothing is streaming, which gives the "game only" baseline.

#include <switch.h>

#if PERFLOG

// Starts the logger threads. sd may be NULL (no SD card): then nothing is started.
void PerfLog_Start(FsFileSystem* sd);
// Called by SocketSendAll for every successful bsdSend
void PerfLog_AddSent(u32 bytes);

// Thread handles of the streaming threads (core.c), INVALID_HANDLE when not running
Handle PerfLog_VideoThreadHandle(void);
Handle PerfLog_AudioThreadHandle(void);

// Implemented by the NSDVR extension; the weak defaults in perflog.c describe the official build.
// Returns and clears the encode time (us) and the number of extension audio packets sent since the
// last call (the official build never sends any)
void PerfLog_TakeAudioEncode(u32* encodeUs, u32* packets);
// Active extension audio encoding, written like the client's tags: pcm, 24k, adpcm, opus/96/5/20
void PerfLog_AudioTag(char* out, u32 cap);

#else

static inline void PerfLog_AddSent(u32 bytes) { (void)bytes; }

#endif
