#pragma once
// Test-only performance logger, built with `make PERFLOG=1` (never part of a release build).
//
// Once per second it appends one CSV line to /config/sysdvr/perf/perf_<unix time>.csv on the SD card:
//   - busy permille of all four CPU cores, measured like Status Monitor does it (idle tick count read
//     by a thread pinned to each core),
//   - CPU time of SysDVR's own threads (video, audio, IPC/main) and of the logger itself,
//   - bytes sent by SysDVR, the foreground title, the CPU clock and the free system memory pool,
//   - with the NSDVR extension: the active audio codec and its encode time.
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

// Implemented by the NSDVR extension. Weak defaults in perflog.c describe the official build:
// codec -1 (official PCM) and no encode time.
int PerfLog_AudioCodec(void);
// Returns and clears the encode time (us) and audio packet count accumulated since the last call
void PerfLog_TakeAudioEncode(u32* encodeUs, u32* packets);

#else

static inline void PerfLog_AddSent(u32 bytes) { (void)bytes; }

#endif
