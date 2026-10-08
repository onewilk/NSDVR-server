#include <string.h>
#include "next_audio.h"

#if NEXT_WITH_OPUS
#include <setjmp.h>
#include "opus.h"
// libopus pseudostack pointers (celt/stack_alloc.h, defined in celt.c with NONTHREADSAFE_PSEUDOSTACK)
extern char* global_stack;
extern char* scratch_ptr;
// third_party/opus_nx/opus_nx_helpers.c
int next_opus_encoder_size_restricted_celt(int channels);
#endif

// ----------------------------------------------------------------------------- tables

// 63-tap half-band low-pass (cutoff fs/4), identical to HALFBAND_63 in tools/audio_codec_eval.py:
// sinc(0.5 n) * kaiser(63, beta = 8), normalised to unity DC gain, quantised to Q30.
// Taps at even offsets from the center are exactly zero; the filter is symmetric.
const int32_t NextHalfband_Center = 536870877;
const int32_t NextHalfband_Odd[16] = {
	340454404, -109996806, 61980721, -40253917, 27528255, -19117651, 13223757, -8993887,
	5952446, -3795048, 2304152, -1312194, 685162, -315208, 117077, -25786,
};

// Standard IMA/DVI ADPCM tables (same as STEPS / INDEX_ADJ in tools/audio_codec_eval.py)
const int16_t NextAdpcm_StepTable[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
	97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
	724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
	4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
	18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
const int8_t NextAdpcm_IndexTable[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

// ----------------------------------------------------------------------------- helpers

static inline int16_t rd_s16(const uint8_t* p) { int16_t v; memcpy(&v, p, 2); return v; }
static inline void wr_s16(uint8_t* p, int16_t v) { memcpy(p, &v, 2); }

static inline int16_t clamp16(int64_t v)
{
	return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

static inline uint64_t FramesToUs(uint64_t frames)
{
	return (frames * 1000000u + NEXT_AUDIO_RATE / 2) / NEXT_AUDIO_RATE;
}

// ----------------------------------------------------------------------------- PCM24

void NextHalfband_Reset(NextHalfband* h)
{
	memset(h, 0, sizeof(*h));
}

uint32_t NextHalfband_Process(NextHalfband* h, const uint8_t* in, uint32_t frames, uint8_t* out)
{
	unsigned pos = h->pos & 63u;
	unsigned phase = h->phase & 1u;
	uint32_t produced = 0;

	for (uint32_t i = 0; i < frames; i++)
	{
		// Read the input frame first: in-place callers rely on it (output never passes the read point)
		int16_t l = rd_s16(in + 4 * i);
		int16_t r = rd_s16(in + 4 * i + 2);
		h->d[0][pos] = h->d[0][pos + 64] = l;
		h->d[1][pos] = h->d[1][pos + 64] = r;

		if (phase)
		{
			// Newest sample is at d[pos + 64], the 63-tap window is d[pos + 2 .. pos + 64], center d[pos + 33]
			const int16_t* cl = &h->d[0][pos + 33];
			const int16_t* cr = &h->d[1][pos + 33];
			int64_t al = (int64_t)NextHalfband_Center * cl[0];
			int64_t ar = (int64_t)NextHalfband_Center * cr[0];
			for (int m = 0; m < 16; m++)
			{
				const int k = 2 * m + 1;
				al += (int64_t)NextHalfband_Odd[m] * (cl[-k] + cl[k]);
				ar += (int64_t)NextHalfband_Odd[m] * (cr[-k] + cr[k]);
			}
			wr_s16(out + 4 * produced, clamp16((al + (1 << 29)) >> 30));
			wr_s16(out + 4 * produced + 2, clamp16((ar + (1 << 29)) >> 30));
			produced++;
		}

		phase ^= 1u;
		pos = (pos + 1) & 63u;
	}

	h->pos = (uint8_t)pos;
	h->phase = (uint8_t)phase;
	return produced;
}

// ----------------------------------------------------------------------------- ADPCM

uint8_t NextAdpcm_EncodeSample(NextAdpcmChannel* c, int32_t sample)
{
	if (c->index < 0 || c->index > 88)
		c->index = 0;

	int32_t step = NextAdpcm_StepTable[c->index];
	int32_t diff = sample - c->predictor;
	uint8_t code = 0;
	if (diff < 0)
	{
		code = 8;
		diff = -diff;
	}

	int32_t delta = step >> 3;
	if (diff >= step)
	{
		code |= 4;
		diff -= step;
		delta += step;
	}
	if (diff >= (step >> 1))
	{
		code |= 2;
		diff -= step >> 1;
		delta += step >> 1;
	}
	if (diff >= (step >> 2))
	{
		code |= 1;
		delta += step >> 2;
	}

	int32_t p = (code & 8) ? c->predictor - delta : c->predictor + delta;
	c->predictor = p < -32768 ? -32768 : (p > 32767 ? 32767 : p);

	int32_t idx = c->index + NextAdpcm_IndexTable[code & 7];
	c->index = idx < 0 ? 0 : (idx > 88 ? 88 : idx);
	return code;
}

int16_t NextAdpcm_DecodeSample(NextAdpcmChannel* c, uint8_t code)
{
	if (c->index < 0 || c->index > 88)
		c->index = 0;

	int32_t step = NextAdpcm_StepTable[c->index];
	int32_t delta = step >> 3;
	if (code & 4)
		delta += step;
	if (code & 2)
		delta += step >> 1;
	if (code & 1)
		delta += step >> 2;

	int32_t p = (code & 8) ? c->predictor - delta : c->predictor + delta;
	c->predictor = p < -32768 ? -32768 : (p > 32767 ? 32767 : p);

	int32_t idx = c->index + NextAdpcm_IndexTable[code & 7];
	c->index = idx < 0 ? 0 : (idx > 88 ? 88 : idx);
	return (int16_t)c->predictor;
}

bool NextAdpcm_DecodePacket(const uint8_t* body, size_t len, uint32_t frames, int16_t* out)
{
	if (!body || len < NEXT_ADPCM_STATE_SIZE + (size_t)frames)
		return false;

	NextAdpcmChannel ch[2];
	for (int c = 0; c < 2; c++)
	{
		ch[c].predictor = rd_s16(body + 4 * c);
		ch[c].index = body[4 * c + 2];
	}

	const uint8_t* data = body + NEXT_ADPCM_STATE_SIZE;
	for (uint32_t i = 0; i < frames; i++)
	{
		out[2 * i] = NextAdpcm_DecodeSample(&ch[0], data[i] & 0x0F);
		out[2 * i + 1] = NextAdpcm_DecodeSample(&ch[1], data[i] >> 4);
	}
	return true;
}

// ----------------------------------------------------------------------------- encoder state

typedef struct {
	NextAudioConfig cur;
	NextAudioConfig pending;
	bool hasPending;
	bool opusReady;
	uint32_t offset;   // capture offset handed out by BeginPacket
	uint32_t carryPos; // Opus: where the unencoded tail sits after Encode (bytes)
	uint32_t carryLen; // bytes, always a multiple of 4 and < one Opus frame
	NextHalfband hb;
	NextAdpcmChannel adpcm[2];
} EncoderState;

static EncoderState S;
static NextAudioWork* W;
static bool OpusAvail;
static void (*YieldHook)(void);
static uint64_t (*ClockHook)(void);
static NextAudioStats Stats;

#if NEXT_WITH_OPUS
// Referenced by GLOBAL_STACK_SIZE in third_party/opus_nx/next_opus_config.h
int next_opus_pseudostack_limit = NEXT_OPUS_PSEUDOSTACK_BYTES;

void* opus_alloc(size_t size) { (void)size; return NULL; }
void* opus_realloc(void* ptr, size_t size) { (void)ptr; (void)size; return NULL; }
void opus_free(void* ptr) { (void)ptr; }
void* opus_alloc_scratch(size_t size)
{
	(void)size;
	return W ? (void*)W->pseudoStack : NULL;
}

// libopus is built with ENABLE_HARDENING: pseudostack overflows and internal assertions end up in
// celt_fatal(). Instead of abort() (which would crash the sysmodule and force a console reboot) we
// jump back to the guarded call site, reset libopus and carry on without Opus for that packet.
static jmp_buf OpusJmp;
static volatile int OpusGuard;

void celt_fatal(const char* str, const char* file, int line)
{
	(void)str; (void)file; (void)line;
	Stats.opusFatal++;
	if (OpusGuard)
	{
		OpusGuard = 0;
		longjmp(OpusJmp, 1);
	}
	// Not reachable: every libopus call below is guarded. Never return (the prototype is noreturn).
	for (;;) { }
}

static void ResetPseudoStack(void)
{
	global_stack = scratch_ptr = W ? (char*)W->pseudoStack : NULL;
}

static bool OpusSetup(const NextAudioConfig* c)
{
	S.opusReady = false;
	if (!OpusAvail)
		return false;

	if (setjmp(OpusJmp))
	{
		ResetPseudoStack();
		Stats.opusErrors++;
		return false;
	}

	OpusGuard = 1;
	OpusEncoder* e = (OpusEncoder*)W->opusState;
	int r = opus_encoder_init(e, NEXT_AUDIO_RATE, 2, OPUS_APPLICATION_RESTRICTED_CELT);
	if (r == OPUS_OK) r = opus_encoder_ctl(e, OPUS_SET_VBR(0));
	if (r == OPUS_OK) r = opus_encoder_ctl(e, OPUS_SET_BITRATE((opus_int32)c->opusKbps * 1000));
	if (r == OPUS_OK) r = opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(c->complexity));
	if (r == OPUS_OK) r = opus_encoder_ctl(e, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
	if (r == OPUS_OK) r = opus_encoder_ctl(e, OPUS_SET_LSB_DEPTH(16));
	OpusGuard = 0;

	if (r != OPUS_OK)
	{
		Stats.opusErrors++;
		return false;
	}

	S.opusReady = true;
	return true;
}

// Bitrate / complexity changes: opus_encoder_ctl on the live encoder, no re-init
static bool OpusApplyParams(const NextAudioConfig* c)
{
	if (!S.opusReady)
		return OpusSetup(c);

	if (setjmp(OpusJmp))
	{
		ResetPseudoStack();
		Stats.opusErrors++;
		S.opusReady = false;
		return false;
	}

	OpusGuard = 1;
	OpusEncoder* e = (OpusEncoder*)W->opusState;
	int r = opus_encoder_ctl(e, OPUS_SET_BITRATE((opus_int32)c->opusKbps * 1000));
	if (r == OPUS_OK) r = opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(c->complexity));
	OpusGuard = 0;

	if (r != OPUS_OK)
	{
		Stats.opusErrors++;
		return OpusSetup(c);
	}
	return true;
}

static int OpusEncodeFrame(const uint8_t* pcm, int frameSize, uint8_t* out, int maxBytes)
{
	if (setjmp(OpusJmp))
	{
		ResetPseudoStack();
		return OPUS_INTERNAL_ERROR;
	}

	OpusGuard = 1;
	int r = opus_encode((OpusEncoder*)W->opusState, (const opus_int16*)(const void*)pcm, frameSize, out, maxBytes);
	OpusGuard = 0;
	return r;
}
#else
static bool OpusSetup(const NextAudioConfig* c) { (void)c; return false; }
static bool OpusApplyParams(const NextAudioConfig* c) { (void)c; return false; }
#endif

static NextAudioConfig ClampConfig(const NextAudioConfig* in)
{
	unsigned kbps = in->opusKbps;
	uint8_t kbpsHalf = (uint8_t)((kbps > 510 ? 510 : kbps) / 2);
	if (kbpsHalf == 0)
		kbpsHalf = 1; // 0 would mean "default", map tiny values to the minimum instead
	return NextExt_AudioConfigFromWire(in->codec, kbpsHalf, (uint8_t)((in->complexity & 0x0F) | ((in->frameCode & 0x0F) << 4)));
}

static void ResetCodecState(void)
{
	S.carryPos = S.carryLen = 0;
	NextHalfband_Reset(&S.hb);
	memset(S.adpcm, 0, sizeof(S.adpcm));

	if (S.cur.codec == NextCodec_OPUS && !OpusSetup(&S.cur))
		S.cur.codec = NextCodec_PCM48; // Opus unavailable: fall back to plain PCM (visible in the slot marker)
}

static void ApplyConfig(const NextAudioConfig* requested, bool forceReset)
{
	NextAudioConfig c = ClampConfig(requested);
	if (c.codec == NextCodec_OPUS && !OpusAvail)
		c.codec = NextCodec_PCM48;

	bool codecChanged = forceReset || c.codec != S.cur.codec;
	bool paramsChanged = c.opusKbps != S.cur.opusKbps || c.complexity != S.cur.complexity;
	S.cur = c;

	if (codecChanged)
		ResetCodecState();
	else if (c.codec == NextCodec_OPUS && (paramsChanged || !S.opusReady))
	{
		if (!OpusApplyParams(&S.cur))
		{
			S.cur.codec = NextCodec_PCM48;
			ResetCodecState();
		}
	}
	// A frame length change needs nothing: the frame size is an opus_encode() argument
}

// ----------------------------------------------------------------------------- public API

void NextAudio_Init(NextAudioWork* work)
{
	memset(&S, 0, sizeof(S));
	memset(&Stats, 0, sizeof(Stats));
	S.cur = NextExt_DefaultAudioConfig();
	W = work;
	OpusAvail = false;

#if NEXT_WITH_OPUS
	if (W)
	{
		ResetPseudoStack();
		int need = next_opus_encoder_size_restricted_celt(2);
		Stats.opusStateBytes = need > 0 ? (uint32_t)need : 0;
		OpusAvail = need > 0 && need <= NEXT_OPUS_STATE_BYTES && next_opus_pseudostack_limit <= NEXT_OPUS_PSEUDOSTACK_BYTES;
	}
#endif
}

void NextAudio_SetHooks(void (*yield)(void), uint64_t (*nowUs)(void))
{
	YieldHook = yield;
	ClockHook = nowUs;
}

static inline uint64_t Now(void)
{
	return ClockHook ? ClockHook() : 0;
}

void NextAudio_Start(const NextAudioConfig* cfg)
{
	NextAudioConfig c = cfg ? *cfg : NextExt_DefaultAudioConfig();
	S.hasPending = false;
	S.offset = 0;
	ApplyConfig(&c, true);
}

void NextAudio_Request(const NextAudioConfig* cfg)
{
	if (!cfg)
		return;
	S.pending = *cfg;
	S.hasPending = true;
}

void NextAudio_Discontinuity(void)
{
	S.carryPos = S.carryLen = 0;
}

uint32_t NextAudio_BeginPacket(uint8_t* buf, size_t cap)
{
	if (S.hasPending)
	{
		S.hasPending = false;
		ApplyConfig(&S.pending, false);
	}

	uint32_t off = 0;
	switch (S.cur.codec)
	{
	case NextCodec_OPUS:
		if (buf && S.carryLen > 0 && S.carryLen <= NEXT_AUDIO_HEADROOM && (size_t)S.carryPos + S.carryLen <= cap)
		{
			if (S.carryPos)
				memmove(buf, buf + S.carryPos, S.carryLen);
			off = S.carryLen;
		}
		S.carryPos = 0;
		S.carryLen = off;
		break;
	case NextCodec_PCM24:
	case NextCodec_ADPCM:
		off = NEXT_AUDIO_PCM_OFFSET;
		break;
	default:
		off = 0;
		break;
	}

	// Only reachable with a buffer smaller than the headroom (never in the sysmodule / host mock):
	// Encode() then refuses to produce anything because the offset is not the one it needs.
	if ((size_t)off > cap)
		off = (uint32_t)cap;

	S.offset = off;
	return off;
}

static void FillHeader(uint8_t* buf, NextAudioResult* res, uint8_t cf, uint16_t kbps)
{
	NextExtAudioHeader h;
	h.codec = res->codec;
	h.complexityFrame = cf;
	h.frameCount = res->frameCount;
	h.bitrateKbps = kbps;
	h.samplesPerChannel = res->samplesPerChannel;
	h.encodeUs = 0; // patched by the caller once the encoding time is known
	NextExt_WriteAudioHeader(buf, &h);
	res->hasExtHeader = 1;
}

NextAudioResult NextAudio_Encode(uint8_t* buf, size_t cap, uint32_t newBytes, uint64_t captureTs)
{
	NextAudioResult res;
	memset(&res, 0, sizeof(res));
	res.codec = S.cur.codec;
	res.replaySlot = NEXT_AUDIO_SLOT(S.cur.codec);
	res.timestamp = captureTs;

	const uint32_t off = S.offset;
	S.offset = 0;
	if (!buf || (size_t)off > cap)
	{
		NextAudio_Discontinuity();
		return res;
	}

	// In-place encoding is only safe with the offset BeginPacket asked for
	const uint32_t needOff = S.cur.codec == NextCodec_PCM24 || S.cur.codec == NextCodec_ADPCM ? NEXT_AUDIO_PCM_OFFSET : 0;
	if (S.cur.codec != NextCodec_OPUS && off != needOff)
	{
		NextAudio_Discontinuity();
		return res;
	}

	if ((size_t)newBytes > cap - off)
		newBytes = (uint32_t)(cap - off);
	newBytes &= ~3u;
	uint32_t frames = newBytes / 4;
	// samplesPerChannel is 16 bits on the wire
	if (frames > 0xFFFF)
		frames = 0xFFFF;

	const uint64_t t0 = Now();
	switch (S.cur.codec)
	{
	case NextCodec_PCM24:
	{
		uint32_t produced = NextHalfband_Process(&S.hb, buf + off, frames, buf + NEXT_EXT_AUDIO_HEADER_SIZE);
		res.samplesPerChannel = (uint16_t)produced;
		res.frameCount = 1;
		FillHeader(buf, &res, 0, 768);
		res.payloadSize = NEXT_EXT_AUDIO_HEADER_SIZE + produced * 4;
		break;
	}
	case NextCodec_ADPCM:
	{
		uint8_t* state = buf + NEXT_EXT_AUDIO_HEADER_SIZE;
		for (int c = 0; c < 2; c++)
		{
			wr_s16(state + 4 * c, (int16_t)S.adpcm[c].predictor);
			state[4 * c + 2] = (uint8_t)S.adpcm[c].index;
			state[4 * c + 3] = 0;
		}

		const uint8_t* in = buf + off;
		uint8_t* out = state + NEXT_ADPCM_STATE_SIZE;
		for (uint32_t i = 0; i < frames; i++)
		{
			int16_t l = rd_s16(in + 4 * i);
			int16_t r = rd_s16(in + 4 * i + 2);
			uint8_t lo = NextAdpcm_EncodeSample(&S.adpcm[0], l);
			uint8_t hi = NextAdpcm_EncodeSample(&S.adpcm[1], r);
			out[i] = (uint8_t)(lo | (hi << 4));
		}

		res.samplesPerChannel = (uint16_t)frames;
		res.frameCount = 1;
		FillHeader(buf, &res, 0, 384);
		res.payloadSize = NEXT_EXT_AUDIO_HEADER_SIZE + NEXT_ADPCM_STATE_SIZE + frames;
		break;
	}
#if NEXT_WITH_OPUS
	case NextCodec_OPUS:
	{
		const uint32_t frameSize = S.cur.frameCode == NextOpusFrame_10ms ? NEXT_OPUS_FRAME_10MS : NEXT_OPUS_FRAME_20MS;
		const uint32_t frameBytes = frameSize * 4;
		const uint32_t total = off + frames * 4;
		uint32_t count = total / frameBytes;
		if (count > 255)
			count = 255;

		uint64_t carryUs = FramesToUs(off / 4);
		res.timestamp = captureTs > carryUs ? captureTs - carryUs : 0;

		if (!S.opusReady)
		{
			// Only reachable if libopus could not be re-initialised after an error: degrade to PCM48 for
			// this and all following packets. buf[0, total) is contiguous PCM (carry + new capture).
			S.cur.codec = NextCodec_PCM48;
			S.carryPos = S.carryLen = 0;
			uint32_t pcmFrames = total / 4 > 0xFFFF ? 0xFFFF : total / 4;
			res.codec = NextCodec_PCM48;
			res.replaySlot = NEXT_AUDIO_SLOT(NextCodec_PCM48);
			res.samplesPerChannel = (uint16_t)pcmFrames;
			res.payloadSize = pcmFrames * 4;
			return res;
		}

		if (count == 0)
		{
			// Not enough audio for one frame yet: keep everything (it is < one frame) for the next packet
			S.carryPos = 0;
			S.carryLen = total <= NEXT_AUDIO_HEADROOM ? total : 0;
			return res;
		}

		uint32_t cbr = (uint32_t)S.cur.opusKbps * frameSize / 384; // kbps * 1000 * frameSize / 48000 / 8
		if (cbr > NEXT_OPUS_MAX_FRAME_BYTES)
			cbr = NEXT_OPUS_MAX_FRAME_BYTES;

		uint32_t outPos = NEXT_EXT_AUDIO_HEADER_SIZE;
		uint32_t done = 0;
		uint64_t opusUs = 0;
		for (uint32_t k = 0; k < count; k++)
		{
			if (k && YieldHook)
				YieldHook();

			uint64_t f0 = Now();
			int r = OpusEncodeFrame(buf + (size_t)k * frameBytes, (int)frameSize, W->frameOut, (int)cbr);
			opusUs += Now() - f0;
			if (r <= 0 || (uint32_t)r > cbr)
				break;
			// The input of frame k has been consumed; never let the output reach frame k + 1
			if (outPos + 2 + (uint32_t)r > (k + 1) * frameBytes)
				break;

			next_wr16(buf + outPos, (uint16_t)r);
			memcpy(buf + outPos + 2, W->frameOut, (size_t)r);
			outPos += 2 + (uint32_t)r;
			done++;
		}

		if (done < count)
		{
			// libopus failed: reset it, drop what could not be encoded and resync on the next capture
			Stats.opusErrors++;
			res.opusFailed = 1;
			S.carryPos = S.carryLen = 0;
			if (!OpusSetup(&S.cur))
				S.cur.codec = NextCodec_PCM48;
			if (done == 0)
				return res;
			count = done;
		}
		else
		{
			S.carryPos = count * frameBytes;
			S.carryLen = total - count * frameBytes;
		}

		res.encodeUs = opusUs;
		res.frameCount = (uint8_t)count;
		res.samplesPerChannel = (uint16_t)(count * frameSize);
		FillHeader(buf, &res, (uint8_t)(S.cur.complexity | (S.cur.frameCode << 4)), S.cur.opusKbps);
		res.payloadSize = outPos;
		break;
	}
#endif
	default:
		// PCM48: the capture offset is 0, the payload is the captured PCM as is
		res.codec = NextCodec_PCM48;
		res.replaySlot = NEXT_AUDIO_SLOT(NextCodec_PCM48);
		res.samplesPerChannel = (uint16_t)frames;
		res.payloadSize = off == 0 ? frames * 4 : 0;
		break;
	}

	if (res.codec != NextCodec_OPUS)
		res.encodeUs = Now() - t0;
	return res;
}

NextAudioConfig NextAudio_Current(void)
{
	return S.cur;
}

bool NextAudio_OpusAvailable(void)
{
	return OpusAvail;
}

NextAudioStats NextAudio_GetStats(void)
{
	return Stats;
}

void NextAudio_SetPseudoStackLimit(int bytes)
{
#if NEXT_WITH_OPUS
	if (bytes > 0 && bytes <= NEXT_OPUS_PSEUDOSTACK_BYTES)
		next_opus_pseudostack_limit = bytes;
#else
	(void)bytes;
#endif
}

void NextAudio_ClearPseudoStack(uint8_t pattern)
{
	if (W)
		memset(W->pseudoStack, pattern, sizeof(W->pseudoStack));
}

uint32_t NextAudio_PseudoStackUsed(uint8_t pattern)
{
	if (!W)
		return 0;
	uint32_t n = sizeof(W->pseudoStack);
	while (n > 0 && W->pseudoStack[n - 1] == pattern)
		n--;
	return n;
}
