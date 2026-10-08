#include "sim.h"
#include <string.h>

void Sim_Init(Sim* s, SimAudioPacket* pkt, const int16_t* pcm, uint64_t frames, uint32_t batching, uint64_t tsBase)
{
	memset(s, 0, sizeof(*s));
	s->pkt = pkt;
	s->pcm = pcm;
	s->frames = frames;
	s->batching = batching > SIM_MAX_BATCH ? SIM_MAX_BATCH : batching;
	s->tsBase = tsBase;
}

uint64_t Sim_TsOfFrame(const Sim* s, uint64_t frame)
{
	return s->tsBase + (frame * 1000000u + 24000u) / 48000u;
}

NextAudioResult Sim_Step(Sim* s, uint32_t newFrames)
{
	uint8_t* data = s->pkt->data;
	const uint32_t cap = sizeof(s->pkt->data);
	uint32_t off = NextAudio_BeginPacket(data, cap);
	if (newFrames == 0)
		newFrames = (1 + s->batching) * 1024;

	// A real grc capture never exceeds the buffer; clamp like the sysmodule's capture would refuse to
	uint32_t maxFrames = (cap - off) / 4;
	uint32_t n = newFrames > maxFrames ? maxFrames : newFrames;
	for (uint32_t i = 0; i < n; i++)
	{
		memcpy(data + off + 4 * i, &s->pcm[2 * (s->pos % s->frames)], 4);
		s->pos++;
	}
	uint64_t ts = Sim_TsOfFrame(s, s->captured);
	s->captured += n;

	// Pass the unclamped size: the encoder must clamp it itself
	NextAudioResult r = NextAudio_Encode(data, cap, newFrames * 4, ts);
	s->pkt->h.Magic = NEXT_PACKET_MAGIC;
	s->pkt->h.DataSize = r.payloadSize;
	s->pkt->h.Timestamp = r.timestamp;
	s->pkt->h.MetaData = NEXT_META_AUDIO | NEXT_META_DATA;
	s->pkt->h.ReplaySlot = r.replaySlot;
	if (r.hasExtHeader)
		NextExt_PatchEncodeUs(data, r.encodeUs);
	return r;
}
