#include <string.h>
#include <stdatomic.h>
#include "next_diag.h"

typedef struct {
	uint64_t windowStartUs;
	NextCpuSample cpuStart;

	uint64_t lastTs;
	bool haveLastTs;
	uint64_t lastSendUs; // duration of the most recent video send, survives window changes

	uint32_t framesSent;
	uint32_t grcGaps;
	uint64_t sendBlockTotalUs;
	uint64_t sendBlockMaxUs;
	uint32_t sendsOver20ms;
	uint32_t gapsAfterSlowSend;
} VideoWindow;

static VideoWindow V;

static atomic_uint AudioPackets;
static atomic_uint AudioEncodeUs;
static atomic_uint AudioSendUs;
static atomic_uint TosFlags;

static uint32_t Permille(uint64_t num, uint64_t den)
{
	if (den == 0)
		return NEXT_DIAG_UNAVAILABLE;
	uint64_t p = (num * 1000u + den / 2) / den;
	return next_sat32(p);
}

void NextDiag_Begin(uint64_t nowUs, const NextCpuSample* cpu)
{
	memset(&V, 0, sizeof(V));
	V.windowStartUs = nowUs;
	if (cpu)
		V.cpuStart = *cpu;

	atomic_store(&AudioPackets, 0);
	atomic_store(&AudioEncodeUs, 0);
	atomic_store(&AudioSendUs, 0);
	atomic_fetch_and(&TosFlags, ~1u);
}

void NextDiag_OnVideoFrame(uint64_t ts)
{
	if (V.haveLastTs && ts > V.lastTs)
	{
		uint64_t delta = ts - V.lastTs;
		if (delta > NEXT_VIDEO_GAP_US)
		{
			V.grcGaps++;
			// Was the send that happened between the two captures slow?
			if (V.lastSendUs > NEXT_SLOW_SEND_US)
				V.gapsAfterSlowSend++;
		}
	}
	// ts <= lastTs means the grc timeline restarted (e.g. a new game): just resync
	V.lastTs = ts;
	V.haveLastTs = true;
}

void NextDiag_OnVideoSent(uint64_t sendUs)
{
	V.framesSent++;
	V.sendBlockTotalUs += sendUs;
	if (sendUs > V.sendBlockMaxUs)
		V.sendBlockMaxUs = sendUs;
	if (sendUs > NEXT_SLOW_SEND_US)
		V.sendsOver20ms++;
	V.lastSendUs = sendUs;
}

void NextDiag_OnAudioPacket(uint64_t encodeUs, uint64_t sendUs)
{
	atomic_fetch_add(&AudioPackets, 1u);
	atomic_fetch_add(&AudioEncodeUs, next_sat32(encodeUs));
	atomic_fetch_add(&AudioSendUs, next_sat32(sendUs));
}

void NextDiag_SetTos(int bit, bool ok)
{
	unsigned mask = 1u << (bit & 1);
	if (ok)
		atomic_fetch_or(&TosFlags, mask);
	else
		atomic_fetch_and(&TosFlags, ~mask);
}

bool NextDiag_Due(uint64_t nowUs)
{
	return nowUs >= V.windowStartUs && nowUs - V.windowStartUs >= NEXT_DIAG_PERIOD_US;
}

void NextDiag_Collect(uint64_t nowUs, const NextCpuSample* cpu, NextDiagReport* out)
{
	memset(out, 0, sizeof(*out));
	uint64_t elapsed = nowUs >= V.windowStartUs ? nowUs - V.windowStartUs : 0;
	out->intervalMs = next_sat32((elapsed + 500) / 1000);
	out->videoFramesSent = V.framesSent;
	out->videoGrcGaps = V.grcGaps;
	out->videoSendBlockTotalUs = next_sat32(V.sendBlockTotalUs);
	out->videoSendBlockMaxUs = next_sat32(V.sendBlockMaxUs);
	out->videoSendsOver20ms = V.sendsOver20ms;
	out->gapsAfterSlowSend = V.gapsAfterSlowSend;
	out->audioPackets = atomic_exchange(&AudioPackets, 0);
	out->audioEncodeTotalUs = atomic_exchange(&AudioEncodeUs, 0);
	out->audioSendBlockTotalUs = atomic_exchange(&AudioSendUs, 0);
	out->tosFlags = atomic_load(&TosFlags);

	out->core3IdlePermille = NEXT_DIAG_UNAVAILABLE;
	out->sysdvrCpuPermille = NEXT_DIAG_UNAVAILABLE;
	if (cpu)
	{
		const NextCpuSample* a = &V.cpuStart;
		if (cpu->idleValid && a->idleValid && cpu->idleTicks >= a->idleTicks && cpu->idleTotalTicks > a->idleTotalTicks)
		{
			uint32_t p = Permille(cpu->idleTicks - a->idleTicks, cpu->idleTotalTicks - a->idleTotalTicks);
			out->core3IdlePermille = p > 1000 ? 1000 : p;
		}
		if (cpu->busyValid && a->busyValid && cpu->busyTicks >= a->busyTicks && cpu->wallTicks > a->wallTicks)
			out->sysdvrCpuPermille = Permille(cpu->busyTicks - a->busyTicks, cpu->wallTicks - a->wallTicks);
		V.cpuStart = *cpu;
	}

	// New window; lastTs / lastSendUs carry over so gaps across the window edge are still attributed
	V.windowStartUs = nowUs;
	V.framesSent = 0;
	V.grcGaps = 0;
	V.sendBlockTotalUs = 0;
	V.sendBlockMaxUs = 0;
	V.sendsOver20ms = 0;
	V.gapsAfterSlowSend = 0;
}
