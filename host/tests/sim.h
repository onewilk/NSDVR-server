#pragma once
// Socket-free replica of the audio session loop (same buffer layout as the sysmodule's APkt)
#include "next_audio.h"

#define SIM_ABUF 0x1000
#define SIM_MAX_BATCH 5

typedef struct __attribute__((packed)) {
	NextPacketHeader h;
	uint8_t data[SIM_ABUF * (1 + SIM_MAX_BATCH) + NEXT_AUDIO_HEADROOM];
} SimAudioPacket;

typedef struct {
	SimAudioPacket* pkt;
	const int16_t* pcm;
	uint64_t frames;   // source length
	uint64_t pos;      // next source frame
	uint32_t batching;
	uint64_t tsBase;
	uint64_t captured; // total frames captured so far
} Sim;

void Sim_Init(Sim* s, SimAudioPacket* pkt, const int16_t* pcm, uint64_t frames, uint32_t batching, uint64_t tsBase);
// One loop iteration: BeginPacket, "grc" capture of newFrames (0 = (1 + batching) * 1024), Encode.
// Fills the packet header like the session does. The source loops.
NextAudioResult Sim_Step(Sim* s, uint32_t newFrames);
uint64_t Sim_TsOfFrame(const Sim* s, uint64_t frame);
