#pragma once
// Audio encoders for the NSDVR extension: PCM48 passthrough, PCM24 (63-tap half-band decimator),
// IMA ADPCM and Opus (libopus, fixed point, CELT only). Portable C, no heap, no platform calls.
//
// In-place contract (this is what keeps the memory cost down to one small headroom area):
//   The capture buffer `buf` (capacity `cap` bytes) is laid out as
//     [0, off)              carry samples from the previous packet (Opus) / unused headroom (PCM24, ADPCM)
//     [off, off + newBytes) freshly captured s16le stereo 48 kHz PCM (what grc wrote)
//   where `off` is the value returned by NextAudio_BeginPacket(). NextAudio_Encode() then writes the
//   complete packet payload (ExtAudioHeader + body for compressed codecs) starting at buf[0].
//   Output never overtakes unread input, so no second packet-sized buffer is needed.
//   Opus leaves the tail of the input (< 1 frame) in the buffer; BeginPacket moves it to the front.

#include "next_ext.h"

#ifndef NEXT_WITH_OPUS
#define NEXT_WITH_OPUS 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Headroom the capture buffer needs in front of the grc data: the largest Opus carry
// (959 stereo frames = 3836 bytes, rounded up) which is also >= NEXT_AUDIO_PCM_OFFSET.
#define NEXT_AUDIO_HEADROOM 3840
// Capture offset used by PCM24/ADPCM so their output (header + body) can be written in place
#define NEXT_AUDIO_PCM_OFFSET 32

#define NEXT_AUDIO_RATE 48000
#define NEXT_OPUS_FRAME_20MS 960
#define NEXT_OPUS_FRAME_10MS 480
// CBR frame size at the maximum bitrate: 256 kbps * 20 ms / 8
#define NEXT_OPUS_MAX_FRAME_BYTES 640

// Static memory for libopus. Both values are checked at run time (Opus is disabled, never
// overflowed, if they turn out too small). See host/README.md for how they were measured.
#define NEXT_OPUS_STATE_BYTES 10496
#ifndef NEXT_OPUS_PSEUDOSTACK_BYTES
#define NEXT_OPUS_PSEUDOSTACK_BYTES 43008
#endif

typedef struct {
	_Alignas(16) uint8_t opusState[NEXT_OPUS_STATE_BYTES];
	_Alignas(16) uint8_t pseudoStack[NEXT_OPUS_PSEUDOSTACK_BYTES];
	uint8_t frameOut[NEXT_OPUS_MAX_FRAME_BYTES];
} NextAudioWork;

typedef struct {
	uint32_t payloadSize;  // bytes to send starting at buf[0]; 0 = nothing to send for this capture
	uint64_t timestamp;    // PacketHeader.Timestamp: time of the first sample in the payload (us)
	uint8_t replaySlot;    // 0xE0 | codec
	uint8_t codec;         // codec actually used for this packet
	uint8_t hasExtHeader;  // 1 if buf[0..12) is an ExtAudioHeader (compressed codecs)
	uint8_t opusFailed;    // libopus reported/raised an error; the encoder was reset
	uint16_t samplesPerChannel;
	uint8_t frameCount;
	uint64_t encodeUs;     // time spent encoding (0 without a clock hook); the caller patches the header
} NextAudioResult;

// `work` holds the big Opus buffers; it may be NULL (or too small), then Opus requests fall back to PCM48.
void NextAudio_Init(NextAudioWork* work);
// Optional hooks: `yield` is called between Opus frames (the sysmodule lets the video thread run
// there), `nowUs` is used to measure the encoding time (without the yields). Both may be NULL.
void NextAudio_SetHooks(void (*yield)(void), uint64_t (*nowUs)(void));

// New connection: reset all encoder state and use `cfg` from the first packet on
void NextAudio_Start(const NextAudioConfig* cfg);
// Control message: stored and applied at the next packet boundary (NextAudio_BeginPacket)
void NextAudio_Request(const NextAudioConfig* cfg);
// Applies pending configuration, moves the Opus carry to buf[0] and returns the capture offset
// (bytes in front of the grc data). Always <= NEXT_AUDIO_HEADROOM.
uint32_t NextAudio_BeginPacket(uint8_t* buf, size_t cap);
// The captured audio is not contiguous with the previous packet (capture error): drop the carry
void NextAudio_Discontinuity(void);
// Encodes in place, see the contract at the top. `newBytes` is what grc returned (clamped to the
// buffer, rounded down to whole stereo frames). `captureTs` is the grc timestamp of buf[off].
NextAudioResult NextAudio_Encode(uint8_t* buf, size_t cap, uint32_t newBytes, uint64_t captureTs);

NextAudioConfig NextAudio_Current(void);
bool NextAudio_OpusAvailable(void);

// Statistics / test helpers
typedef struct {
	uint32_t opusErrors;       // opus_encode() < 0 or libopus internal assertion caught
	uint32_t opusFatal;        // celt_fatal() calls (pseudostack overflow, assertion)
	uint32_t opusStateBytes;   // opus_encoder_init(NULL, ...) for the configuration we use
} NextAudioStats;
NextAudioStats NextAudio_GetStats(void);
// Test hook: limit for the pseudostack (bytes), default NEXT_OPUS_PSEUDOSTACK_BYTES
void NextAudio_SetPseudoStackLimit(int bytes);
void NextAudio_ClearPseudoStack(uint8_t pattern);
uint32_t NextAudio_PseudoStackUsed(uint8_t pattern);

// ----------------------------------------------------------------------------- codec primitives
// Exposed for unit tests and for decoding in the host tools.

typedef struct {
	int16_t d[2][128]; // doubled ring buffer per channel
	uint8_t pos;
	uint8_t phase;
} NextHalfband;

void NextHalfband_Reset(NextHalfband* h);
// Decimates interleaved stereo frames (in and out may alias as long as out <= in, see contract).
// Returns the number of output frames written.
uint32_t NextHalfband_Process(NextHalfband* h, const uint8_t* in, uint32_t frames, uint8_t* out);
// Q30 taps: center tap and the 16 non-zero odd taps (m = 1, 3, ..., 31); the filter is symmetric
extern const int32_t NextHalfband_Center;
extern const int32_t NextHalfband_Odd[16];

typedef struct {
	int32_t predictor;
	int32_t index;
} NextAdpcmChannel;

extern const int16_t NextAdpcm_StepTable[89];
extern const int8_t NextAdpcm_IndexTable[8];

uint8_t NextAdpcm_EncodeSample(NextAdpcmChannel* c, int32_t sample);
int16_t NextAdpcm_DecodeSample(NextAdpcmChannel* c, uint8_t code);
// Decodes one ADPCM body (after the 12 byte ExtAudioHeader): 8 byte state + `frames` bytes.
// Writes frames * 2 int16 to out. Returns false if the input is too short.
bool NextAdpcm_DecodePacket(const uint8_t* body, size_t len, uint32_t frames, int16_t* out);

#ifdef __cplusplus
}
#endif
