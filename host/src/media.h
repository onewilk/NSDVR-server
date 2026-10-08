#pragma once
// WAV / H.264 Annex-B helpers for the host tools (heap allowed here, this never runs on the Switch)
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	int16_t* samples; // interleaved stereo
	uint32_t frames;
	uint32_t rate;
} WavData;

// Loads a 16-bit PCM WAV; mono files are duplicated to stereo. Returns false with a message in err.
bool Wav_Load(const char* path, WavData* out, char* err, size_t errLen);
bool Wav_Write(const char* path, const int16_t* stereo, uint32_t frames, uint32_t rate);
void Wav_Free(WavData* w);
// Synthetic stereo test signal (440 Hz left, 660 Hz right, slow level changes), `frames` long
void Wav_Synthetic(WavData* out, uint32_t frames);

typedef struct {
	const uint8_t* data;
	uint32_t size;
	bool idr;
} H264Frame;

typedef struct {
	uint8_t* blob;
	H264Frame* frames;
	uint32_t count;
	uint32_t idrCount;
	uint8_t spsPps[256]; // first SPS + PPS found in the file (with start codes), for injection
	uint32_t spsPpsSize;
} H264File;

// One packet per VCL NAL; preceding non-VCL NALs (SPS/PPS/SEI) are merged into it, AUDs dropped.
// This is what grc produces (one NAL per packet) and matches tools/mock_sysdvr.py.
bool H264_Load(const char* path, H264File* out, char* err, size_t errLen);
// Non-decodable synthetic frames (protocol tests only): an "IDR" every 30 frames
void H264_Synthetic(H264File* out);
void H264_Free(H264File* f);

uint32_t Crc32(const uint8_t* data, size_t len);
