#pragma once
// Server side diagnostics for the NSDVR extension (ExtDiag v1, one packet per second on the video
// channel). The video thread owns the window; the audio thread only adds to atomic counters.

#include "next_ext.h"

#ifdef __cplusplus
extern "C" {
#endif

// grc records at 30 fps: a gap is an interval longer than 1.5 frame intervals
#define NEXT_VIDEO_FRAME_US 33333u
#define NEXT_VIDEO_GAP_US (NEXT_VIDEO_FRAME_US * 3u / 2u)
#define NEXT_SLOW_SEND_US 20000u
#define NEXT_DIAG_PERIOD_US 1000000u

// CPU counters sampled by the platform at the window edges. Units are free as long as each
// ratio uses one unit: sysdvrCpuPermille = d(busy) / d(wall), core3IdlePermille = d(idle) / d(idleTotal)
typedef struct {
	uint64_t wallTicks;
	uint64_t busyTicks;
	uint64_t idleTicks;
	uint64_t idleTotalTicks;
	uint8_t busyValid;
	uint8_t idleValid;
} NextCpuSample;

// Video session start: clears the video counters and the audio counters, keeps the audio TOS bit
void NextDiag_Begin(uint64_t nowUs, const NextCpuSample* cpu);
// Called for every successfully captured video packet (not for error packets), before sending it
void NextDiag_OnVideoFrame(uint64_t grcTimestamp);
// Called after every video packet was sent, with the time SocketSendAll blocked
void NextDiag_OnVideoSent(uint64_t sendUs);
// Audio thread, after each audio packet
void NextDiag_OnAudioPacket(uint64_t encodeUs, uint64_t sendUs);
// bit 0 = video socket, bit 1 = audio socket
void NextDiag_SetTos(int bit, bool ok);
bool NextDiag_Due(uint64_t nowUs);
// Fills a report for the window that just ended and starts a new one
void NextDiag_Collect(uint64_t nowUs, const NextCpuSample* cpu, NextDiagReport* out);

#ifdef __cplusplus
}
#endif
