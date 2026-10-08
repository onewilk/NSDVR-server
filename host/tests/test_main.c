// Unit + fuzz tests for the NSDVR extension code (built with ASan + UBSan).
//   next_tests <48 kHz stereo wav>
#include "next_ext.h"
#include "next_audio.h"
#include "next_diag.h"
#include "next_session.h"
#include "next_platform.h"
#include "media.h"
#include "ref.h"
#include "sim.h"
#include "opus.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int Checks, Failures;
#define CHECK(cond, ...) do { Checks++; if (!(cond)) { Failures++; printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static WavData Wav;
static NextAudioWork Work;
static SimAudioPacket Pkt __attribute__((aligned(4096)));
static uint8_t DecMem[32768];

static uint32_t Rng = 0x12345678;
static uint32_t Rand(void) { Rng ^= Rng << 13; Rng ^= Rng >> 17; Rng ^= Rng << 5; return Rng; }

// ============================================================================ protocol

static void TestHandshake(void)
{
	printf("[handshake]\n");
	uint8_t req[16] = { 0xAA, 0xAA, 0xAA, 0xAA, '0', '3', 2, 0, 3, 0 };

	NextExtConfig c = NextExt_ParseHandshake(req, 16);
	CHECK(!c.enabled && c.audio.codec == 0, "no flag -> disabled");

	req[9] = 0x04 | 0x02; // ext + memory diag
	req[10] = 3; req[11] = 32; req[12] = 0x1A; req[13] = 3;
	c = NextExt_ParseHandshake(req, 16);
	CHECK(c.enabled && c.audio.codec == 3 && c.audio.opusKbps == 64 && c.audio.complexity == 10 && c.audio.frameCode == 1 && c.diag && c.tos,
		"parse: codec %u kbps %u c %u f %u", c.audio.codec, c.audio.opusKbps, c.audio.complexity, c.audio.frameCode);

	req[10] = 9; req[11] = 0; req[12] = 0x2F; req[13] = 0;
	c = NextExt_ParseHandshake(req, 16);
	CHECK(c.audio.codec == 0 && c.audio.opusKbps == 96 && c.audio.complexity == 10 && c.audio.frameCode == 0 && !c.diag && !c.tos, "clamps 1");
	req[11] = 1;
	CHECK(NextExt_ParseHandshake(req, 16).audio.opusKbps == 16, "kbps floor");
	req[11] = 255;
	CHECK(NextExt_ParseHandshake(req, 16).audio.opusKbps == 256, "kbps ceiling");
	CHECK(!NextExt_ParseHandshake(req, 15).enabled && !NextExt_ParseHandshake(req, 17).enabled && !NextExt_ParseHandshake(NULL, 16).enabled, "length");

	// Official validation mirror
	uint8_t v[16] = { 0xAA, 0xAA, 0xAA, 0xAA, '0', '3', 1, 7, 0, 0x04 };
	CHECK(NextExt_ValidateHandshake(v, 16, 1, "03") == NextHs_Ok, "video ok");
	CHECK(NextExt_ValidateHandshake(v, 16, 2, "03") == NextHs_InvalidChannel, "video on audio port");
	v[6] = 3;
	CHECK(NextExt_ValidateHandshake(v, 16, 1, "03") == NextHs_InvalidChannel, "both on video port");
	CHECK(NextExt_ValidateHandshake(v, 16, 0, "03") == NextHs_Ok, "both on any");
	v[6] = 0;
	CHECK(NextExt_ValidateHandshake(v, 16, 0, "03") == NextHs_InvalidMeta, "no meta");
	v[6] = 2; v[8] = 6;
	CHECK(NextExt_ValidateHandshake(v, 16, 2, "03") == NextHs_InvalidArg, "batching");
	v[8] = 5; v[4] = '0'; v[5] = '2';
	CHECK(NextExt_ValidateHandshake(v, 16, 2, "03") == NextHs_WrongVersion, "version");
	v[0] = 0;
	CHECK(NextExt_ValidateHandshake(v, 16, 2, "03") == NextHs_WrongMagic, "magic");
	CHECK(NextExt_ValidateHandshake(v, 12, 2, "03") == NextHs_InvalidSize, "size");
}

static void TestControlParser(void)
{
	printf("[control messages]\n");
	NextCtrlParser p;
	NextAudioConfig cfg = NextExt_DefaultAudioConfig();
	uint8_t m[8];

	NextCtrl_Init(&p);
	NextCtrl_Build(m, 3, 32, 7, 1);
	CHECK(NextCtrl_Feed(&p, m, 8, &cfg) == 1 && cfg.codec == 3 && cfg.opusKbps == 64 && cfg.complexity == 7 && cfg.frameCode == 1, "single");

	// byte by byte
	NextCtrl_Init(&p);
	NextCtrl_Build(m, 2, 0, 0, 0);
	int found = 0;
	for (int i = 0; i < 8; i++)
		found += NextCtrl_Feed(&p, m + i, 1, &cfg);
	CHECK(found == 1 && cfg.codec == 2, "byte by byte");

	// garbage, partial magics and resync
	uint8_t s[64];
	size_t n = 0;
	const char* junk = "xxSDVSSDSDVxS";
	memcpy(s, junk, strlen(junk)); n = strlen(junk);
	NextCtrl_Build(s + n, 1, 10, 3, 0); n += 8;
	s[n++] = 'S'; s[n++] = 'D';
	NextCtrl_Build(s + n, 3, 128, 15, 5); n += 8;
	NextCtrl_Init(&p);
	found = NextCtrl_Feed(&p, s, n, &cfg);
	CHECK(found == 2 && cfg.codec == 3 && cfg.opusKbps == 256 && cfg.complexity == 10 && cfg.frameCode == 0, "resync: found %d codec %u", found, cfg.codec);

	// invalid codec is ignored, parser stays usable
	NextCtrl_Init(&p);
	NextAudioConfig before = cfg;
	NextCtrl_Build(m, 4, 10, 1, 0);
	CHECK(NextCtrl_Feed(&p, m, 8, &cfg) == 0 && cfg.codec == before.codec && p.invalidMessages == 1, "invalid codec ignored");
	NextCtrl_Build(m, 0, 10, 1, 0);
	CHECK(NextCtrl_Feed(&p, m, 8, &cfg) == 1 && cfg.codec == 0, "valid after invalid");

	// a message split across feeds at every position
	for (int cut = 1; cut < 8; cut++)
	{
		NextCtrl_Init(&p);
		NextCtrl_Build(m, 2, 20, 2, 1);
		cfg.codec = 0;
		int f = NextCtrl_Feed(&p, m, (size_t)cut, &cfg) + NextCtrl_Feed(&p, m + cut, (size_t)(8 - cut), &cfg);
		CHECK(f == 1 && cfg.codec == 2 && cfg.opusKbps == 40, "split at %d", cut);
	}

	// corrupted parser state must not break anything
	p.len = 200;
	CHECK(NextCtrl_Feed(&p, m, 8, &cfg) == 1, "corrupted len recovers");
	CHECK(NextCtrl_Feed(NULL, m, 8, &cfg) == 0 && NextCtrl_Feed(&p, NULL, 8, &cfg) == 0, "null args");
}

static void TestHeaders(void)
{
	printf("[packet headers]\n");
	uint8_t b[NEXT_DIAG_SIZE];
	NextExtAudioHeader h = { 3, 0x15, 4, 96, 3840, 1234567 }, g;
	NextExt_WriteAudioHeader(b, &h);
	CHECK(b[0] == 1 && NextExt_ReadAudioHeader(b, 12, &g) && g.codec == 3 && g.complexityFrame == 0x15 && g.frameCount == 4 &&
		g.bitrateKbps == 96 && g.samplesPerChannel == 3840 && g.encodeUs == 1234567, "audio header roundtrip");
	NextExt_PatchEncodeUs(b, 0x1FFFFFFFFull);
	CHECK(next_rd32(b + 8) == 0xFFFFFFFFu, "encodeUs saturates");
	CHECK(!NextExt_ReadAudioHeader(b, 11, &g), "short header");
	b[0] = 2;
	CHECK(!NextExt_ReadAudioHeader(b, 12, &g), "unknown version");

	NextDiagReport r = { 1000, 30, 2, 5000, 400, 1, 1, 12, 3000, 800, 950, 12, 3 }, q;
	NextExt_WriteDiag(b, &r);
	CHECK(b[0] == 1 && b[1] == 0 && NextExt_ReadDiag(b, NEXT_DIAG_SIZE, &q) && memcmp(&r, &q, sizeof(r)) == 0, "diag roundtrip");
	CHECK(next_rd32(b + 44) == 950 && next_rd32(b + 52) == 3, "diag offsets");
	CHECK(!NextExt_ReadDiag(b, NEXT_DIAG_SIZE - 1, &q), "short diag");
}

static void TestFuzzParsers(void)
{
	printf("[fuzz: parsers]\n");
	NextCtrlParser p;
	NextCtrl_Init(&p);
	NextAudioConfig cfg = NextExt_DefaultAudioConfig();
	uint8_t buf[4096];
	long valid = 0;
	for (int round = 0; round < 1000; round++)
	{
		size_t n = Rand() % sizeof(buf);
		for (size_t i = 0; i < n; i++)
		{
			uint32_t r = Rand();
			// bias towards magic bytes so partial / complete messages actually occur
			buf[i] = (r & 7) == 0 ? "SDVX"[(r >> 3) & 3] : (uint8_t)(r >> 8);
		}
		if (round % 3 == 0 && n > 16)
			NextCtrl_Build(buf + Rand() % (n - 8), (uint8_t)(Rand() % 8), (uint8_t)Rand(), (uint8_t)Rand(), (uint8_t)Rand());
		valid += NextCtrl_Feed(&p, buf, n, &cfg);
		CHECK(cfg.codec < 4 && cfg.opusKbps >= 16 && cfg.opusKbps <= 256 && cfg.complexity <= 10 && cfg.frameCode <= 1 && p.len <= 8,
			"control fuzz invariant");
	}
	printf("  control parser: %ld valid messages found in ~2 MB of random input, %u invalid, %u bytes dropped\n",
		valid, p.invalidMessages, p.droppedBytes);

	for (int round = 0; round < 200000; round++)
	{
		size_t n = Rand() % 40;
		for (size_t i = 0; i < n; i++)
			buf[i] = (uint8_t)Rand();
		if (Rand() & 1) { buf[0] = buf[1] = buf[2] = buf[3] = 0xAA; }
		if (n > 9 && (Rand() & 1)) buf[9] |= 4;
		NextExtConfig c = NextExt_ParseHandshake(buf, n);
		int code = NextExt_ValidateHandshake(buf, n, (int)(Rand() % 3), "03");
		CHECK(c.audio.codec < 4 && c.audio.opusKbps >= 16 && c.audio.opusKbps <= 256 && c.audio.complexity <= 10 && code >= 1 && code <= 7,
			"handshake fuzz invariant");
		NextExtAudioHeader h;
		NextDiagReport d;
		NextExt_ReadAudioHeader(buf, n, &h);
		NextExt_ReadDiag(buf, n, &d);
	}
	printf("  handshake / header readers: 200000 random inputs (0..39 bytes)\n");

	// ADPCM decoder: truncated bodies must be rejected, never read out of bounds
	int16_t out[2 * 512];
	int rejected = 0;
	for (int round = 0; round < 20000; round++)
	{
		size_t n = Rand() % 300;
		uint8_t* body = malloc(n ? n : 1); // exact size so ASan sees any overread
		for (size_t i = 0; i < n; i++)
			body[i] = (uint8_t)Rand();
		uint32_t frames = Rand() % 512;
		if (!NextAdpcm_DecodePacket(body, n, frames, out))
			rejected++;
		free(body);
	}
	printf("  ADPCM packet decoder: 20000 random bodies, %d rejected as truncated\n", rejected);
}

// ============================================================================ codecs

static void TestHalfband(void)
{
	printf("[PCM24 half-band vs audio_codec_eval.py]\n");
	double h[63];
	Ref_LowpassFir(12000, 63, h);
	double maxCoefErr = fabs(NextHalfband_Center / 1073741824.0 - h[31]);
	for (int m = 0; m < 16; m++)
	{
		int k = 2 * m + 1;
		maxCoefErr = fmax(maxCoefErr, fabs(NextHalfband_Odd[m] / 1073741824.0 - h[31 + k]));
		maxCoefErr = fmax(maxCoefErr, fabs(NextHalfband_Odd[m] / 1073741824.0 - h[31 - k]));
	}
	double maxEven = 0;
	for (int k = 2; k <= 30; k += 2)
		maxEven = fmax(maxEven, fmax(fabs(h[31 + k]), fabs(h[31 - k])));
	CHECK(maxCoefErr < 1e-9 && maxEven < 1e-15, "coefficients: max error %.3g, even taps %.3g", maxCoefErr, maxEven);

	const size_t frames = 48000 * 2;
	const int16_t* x = Wav.samples + 48000 * 20 * 2; // 2 s starting at 20 s
	int16_t* ref = malloc((frames / 2 + 1) * 4);
	Ref_Down24(x, frames, ref);

	// Streaming, arbitrary chunk sizes (including odd ones and zero)
	NextHalfband hb;
	NextHalfband_Reset(&hb);
	uint8_t* out = malloc(frames * 2 + 64);
	uint32_t produced = 0;
	size_t pos = 0;
	while (pos < frames)
	{
		size_t n = Rand() % 3000;
		if (n > frames - pos)
			n = frames - pos;
		produced += NextHalfband_Process(&hb, (const uint8_t*)(x + 2 * pos), (uint32_t)n, out + produced * 4);
		pos += n;
	}
	CHECK(produced == frames / 2, "produced %u", produced);

	// Streaming output k is the reference output k - 15 (31 input samples of filter delay)
	int maxDiff = 0;
	long diffs = 0, compared = 0;
	for (uint32_t k = 15; k < produced; k++)
		for (int c = 0; c < 2; c++)
		{
			int16_t a;
			memcpy(&a, out + 4 * k + 2 * c, 2);
			int d = abs(a - ref[2 * (k - 15) + c]);
			if (d > maxDiff) maxDiff = d;
			diffs += d != 0;
			compared++;
		}
	printf("  %ld samples compared: max |diff| = %d LSB, %ld differ (%.4f%%)\n", compared, maxDiff, diffs, 100.0 * diffs / compared);
	CHECK(maxDiff <= 1 && diffs * 1000 < compared, "PCM24 matches reference");
	free(out);
	free(ref);
}

static void TestAdpcm(void)
{
	printf("[IMA ADPCM vs audio_codec_eval.py]\n");
	const size_t frames = 48000 * 3;
	const int16_t* x = Wav.samples + 48000 * 30 * 2;
	int16_t* refRecon = malloc(frames * 4);
	uint8_t* refCodes[2] = { malloc(frames), malloc(frames) };
	for (int c = 0; c < 2; c++)
		Ref_AdpcmChannel(x + c, frames, 2, refRecon + c, refCodes[c]);

	NextAdpcmChannel ch[2] = { { 0, 0 }, { 0, 0 } };
	long mismatch = 0;
	for (size_t i = 0; i < frames; i++)
		for (int c = 0; c < 2; c++)
		{
			uint8_t code = NextAdpcm_EncodeSample(&ch[c], x[2 * i + c]);
			mismatch += code != refCodes[c][i] || ch[c].predictor != refRecon[2 * i + c];
		}
	CHECK(mismatch == 0, "encoder bit exact with reference (%ld mismatches)", mismatch);

	// Through the packet pipeline (batching 3), decoding every packet from its own header state
	NextAudio_Init(&Work);
	NextAudioConfig cfg = NextExt_DefaultAudioConfig();
	cfg.codec = NextCodec_ADPCM;
	NextAudio_Start(&cfg);
	Sim sim;
	Sim_Init(&sim, &Pkt, x, frames, 3, 1000000);
	int16_t* dec = malloc(frames * 4 + 65536);
	size_t decoded = 0;
	bool headerOk = true;
	while (decoded + 4096 <= frames)
	{
		NextAudioResult r = Sim_Step(&sim, 0);
		NextExtAudioHeader h;
		headerOk &= r.codec == NextCodec_ADPCM && r.replaySlot == 0xE2 && Pkt.h.DataSize == 12 + 8 + 4096 &&
			NextExt_ReadAudioHeader(Pkt.data, Pkt.h.DataSize, &h) && h.samplesPerChannel == 4096 && h.bitrateKbps == 384;
		headerOk &= Pkt.h.Timestamp == Sim_TsOfFrame(&sim, decoded);
		NextAdpcm_DecodePacket(Pkt.data + 12, Pkt.h.DataSize - 12, 4096, dec + 2 * decoded);
		decoded += 4096;
	}
	CHECK(headerOk, "packet headers / timestamps");
	CHECK(memcmp(dec, refRecon, decoded * 4) == 0, "packet decode == reference reconstruction (%zu frames)", decoded);
	free(dec);
	free(refRecon);
	free(refCodes[0]);
	free(refCodes[1]);
}

static int BestLag(const int16_t* ref, const int16_t* dec, size_t n, int maxLag)
{
	int best = 0;
	double bestC = -1e300;
	for (int lag = 0; lag <= maxLag; lag++)
	{
		double c = 0;
		for (size_t i = 0; i + lag < n; i += 7)
			c += (double)ref[2 * i] * dec[2 * (i + lag)] + (double)ref[2 * i + 1] * dec[2 * (i + lag) + 1];
		if (c > bestC) { bestC = c; best = lag; }
	}
	return best;
}

static double Snr(const int16_t* ref, const int16_t* dec, size_t n, int lag)
{
	double s = 0, e = 0;
	for (size_t i = 0; i + lag < n; i++)
		for (int c = 0; c < 2; c++)
		{
			double a = ref[2 * i + c], b = dec[2 * (i + lag) + c];
			s += a * a;
			e += (a - b) * (a - b);
		}
	return 10 * log10(s / (e + 1e-9));
}

// Runs `packets` iterations of the pipeline with cfg and decodes everything. Returns decoded frames.
static size_t OpusRun(const NextAudioConfig* cfg, const int16_t* x, size_t srcFrames, int batching, int packets,
	int16_t* dec, size_t decCap, bool* consistent)
{
	OpusDecoder* d = (OpusDecoder*)DecMem;
	opus_decoder_init(d, 48000, 2);
	NextAudio_Start(cfg);
	Sim sim;
	Sim_Init(&sim, &Pkt, x, srcFrames, (uint32_t)batching, 5000000);
	size_t out = 0;
	uint64_t encoded = 0;
	*consistent = true;
	for (int p = 0; p < packets; p++)
	{
		NextAudioResult r = Sim_Step(&sim, 0);
		if (r.payloadSize == 0)
			continue;
		NextExtAudioHeader h;
		if (!NextExt_ReadAudioHeader(Pkt.data, Pkt.h.DataSize, &h) || h.codec != 3 || r.replaySlot != 0xE3)
		{
			*consistent = false;
			break;
		}
		uint32_t fs = (h.complexityFrame >> 4) ? 480 : 960;
		*consistent &= h.samplesPerChannel == h.frameCount * fs && h.bitrateKbps == cfg->opusKbps;
		// first sample of the first frame (+-1 us: the carry duration and the capture time are rounded separately)
		*consistent &= llabs((long long)Pkt.h.Timestamp - (long long)Sim_TsOfFrame(&sim, encoded)) <= 1;
		uint32_t pos = 12;
		for (int f = 0; f < h.frameCount; f++)
		{
			uint32_t len = next_rd16(Pkt.data + pos);
			*consistent &= len == (uint32_t)cfg->opusKbps * fs / 384; // CBR
			int n = opus_decode(d, Pkt.data + pos + 2, (opus_int32)len, dec + 2 * out, (int)((decCap - out) > 5760 ? 5760 : decCap - out), 0);
			*consistent &= n == (int)fs;
			if (n > 0) out += (size_t)n;
			pos += 2 + len;
		}
		*consistent &= pos == Pkt.h.DataSize;
		encoded += h.samplesPerChannel;
	}
	// Every captured sample is either encoded or waiting in the carry (< 1 frame)
	uint32_t fs = cfg->frameCode ? 480 : 960;
	*consistent &= sim.captured >= encoded && sim.captured - encoded < fs;
	return out;
}

static void TestOpus(void)
{
	printf("[Opus]\n");
	NextAudio_Init(&Work);
	NextAudioStats st = NextAudio_GetStats();
	CHECK(NextAudio_OpusAvailable() && st.opusStateBytes > 0 && st.opusStateBytes <= NEXT_OPUS_STATE_BYTES,
		"state size %u <= %u", st.opusStateBytes, NEXT_OPUS_STATE_BYTES);
	printf("  encoder state (RESTRICTED_CELT, stereo, fixed point): %u bytes; opus_encoder_get_size(2) = %d\n",
		st.opusStateBytes, opus_encoder_get_size(2));

	const size_t srcFrames = 48000 * 10;
	const int16_t* x = Wav.samples + 48000 * 40 * 2;
	size_t decCap = srcFrames + 48000;
	int16_t* dec = malloc(decCap * 4);

	struct { int kbps, cx, frame, batching; double minSnr; } cases[] = {
		{ 96, 5, 0, 3, 12 }, { 64, 0, 1, 0, 8 }, { 256, 10, 0, 5, 18 }, { 16, 10, 1, 1, 1 }, { 128, 8, 1, 2, 12 },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		NextAudioConfig cfg = NextExt_AudioConfigFromWire(3, (uint8_t)(cases[i].kbps / 2), (uint8_t)(cases[i].cx | (cases[i].frame << 4)));
		bool ok;
		size_t n = OpusRun(&cfg, x, srcFrames, cases[i].batching, 60, dec, decCap, &ok);
		int lag = BestLag(x, dec, n, 600);
		double snr = Snr(x, dec, n > 48000 ? n : 0, lag);
		printf("  %3d kbps c%-2d %s batching %d: %zu frames decoded, delay %d samples, SNR %.1f dB\n", cases[i].kbps, cases[i].cx,
			cases[i].frame ? "10 ms" : "20 ms", cases[i].batching, n, lag, snr);
		CHECK(ok, "packet structure / CBR / timestamps / sample conservation");
		CHECK(snr > cases[i].minSnr, "SNR %.1f", snr);
	}

	// Live parameter changes without re-creating the encoder, frame length switch, codec switches
	NextAudioConfig cfg = NextExt_AudioConfigFromWire(3, 48, 5);
	NextAudio_Start(&cfg);
	Sim sim;
	Sim_Init(&sim, &Pkt, x, srcFrames, 3, 0);
	uint64_t encoded = 0, dropped = 0;
	bool ok = true;
	struct { int at; uint8_t codec, kbpsHalf, cf; } plan[] = {
		{ 5, 3, 16, 0x02 }, { 10, 3, 16, 0x12 }, { 15, 3, 100, 0x0A }, { 20, 2, 0, 0 }, { 25, 3, 48, 0x05 }, { 30, 1, 0, 0 },
		{ 35, 0, 0, 0 }, { 40, 3, 24, 0x10 },
	};
	size_t pi = 0;
	int lastCodec = 3;
	uint64_t carryBefore = 0;
	for (int p = 0; p < 50; p++)
	{
		if (pi < sizeof(plan) / sizeof(plan[0]) && plan[pi].at == p)
		{
			NextAudioConfig c = NextExt_AudioConfigFromWire(plan[pi].codec, plan[pi].kbpsHalf, plan[pi].cf);
			NextAudio_Request(&c);
			pi++;
		}
		NextAudioResult r = Sim_Step(&sim, 0);
		if (r.codec != lastCodec)
		{
			// a codec switch drops the Opus carry (< 1 frame)
			dropped += carryBefore;
			lastCodec = r.codec;
		}
		NextExtAudioHeader h;
		if (r.codec == 3)
		{
			ok &= NextExt_ReadAudioHeader(Pkt.data, Pkt.h.DataSize, &h);
			encoded += h.samplesPerChannel;
			NextAudioConfig cur = NextAudio_Current();
			ok &= h.bitrateKbps == cur.opusKbps && (h.complexityFrame & 15) == cur.complexity && (h.complexityFrame >> 4) == cur.frameCode;
		}
		else if (r.codec == 1)
			encoded += 2 * (uint64_t)r.samplesPerChannel;
		else
			encoded += r.samplesPerChannel;
		carryBefore = sim.captured - encoded - dropped;
	}
	CHECK(ok && NextAudio_GetStats().opusErrors == 0, "live changes consistent");
	CHECK(sim.captured - encoded - dropped < 960, "sample conservation across switches (captured %llu encoded %llu dropped %llu)",
		(unsigned long long)sim.captured, (unsigned long long)encoded, (unsigned long long)dropped);

	// Pseudostack limit too small: libopus hits its hardening check, we recover without crashing
	NextAudio_SetPseudoStackLimit(16384);
	cfg = NextExt_AudioConfigFromWire(3, 48, 10);
	NextAudio_Start(&cfg);
	NextAudioResult r = Sim_Step(&sim, 0);
	NextAudioStats s2 = NextAudio_GetStats();
	printf("  pseudostack limited to 16 KB: codec %u, opusFatal %u, opusErrors %u, payload %u (recovered, no crash)\n",
		r.codec, s2.opusFatal, s2.opusErrors, r.payloadSize);
	CHECK(s2.opusFatal > 0 && r.opusFailed, "overflow detected and handled");
	r = Sim_Step(&sim, 0);
	CHECK(r.codec == NextCodec_PCM48 || r.payloadSize == 0 || r.opusFailed, "degrades after failure");
	NextAudio_SetPseudoStackLimit(NEXT_OPUS_PSEUDOSTACK_BYTES);
	NextAudio_Start(&cfg);
	r = Sim_Step(&sim, 0);
	CHECK(r.codec == 3 && !r.opusFailed && r.payloadSize > 0, "works again with the normal limit");

	// No work memory: Opus requests fall back to PCM48
	NextAudio_Init(NULL);
	NextAudio_Start(&cfg);
	r = Sim_Step(&sim, 0);
	CHECK(r.codec == 0 && r.replaySlot == 0xE0 && r.payloadSize == 4096 * 4, "no Opus memory -> PCM48");
	NextAudio_Init(&Work);
	free(dec);
}

static void TestPipelineFuzz(void)
{
	printf("[fuzz: in-place pipeline]\n");
	NextAudio_Init(&Work);
	NextAudio_Start(NULL);
	OpusDecoder* d = (OpusDecoder*)DecMem;
	opus_decoder_init(d, 48000, 2);
	Sim sim;
	const int16_t* x = Wav.samples;
	Sim_Init(&sim, &Pkt, x, Wav.frames, 3, 7);
	int16_t pcm[5760 * 2];
	long steps = 0, sent = 0, opusFrames = 0;
	bool ok = true;
	const uint32_t cap = sizeof(Pkt.data);
	for (int i = 0; i < 4000; i++)
	{
		uint32_t r = Rand();
		if ((r & 7) == 0)
		{
			NextAudioConfig c = NextExt_AudioConfigFromWire((uint8_t)(Rand() % 5), (uint8_t)Rand(), (uint8_t)Rand());
			NextAudio_Request(&c);
		}
		if ((r & 63) == 1)
			NextAudio_Discontinuity();
		sim.batching = (r >> 8) % 6;
		uint32_t n = 0;
		switch ((r >> 12) % 6)
		{
		case 0: n = 1 + Rand() % 7000; break;                // arbitrary, sometimes more than fits
		case 1: n = 1 + Rand() % 50; break;                  // tiny captures (Opus accumulates)
		case 2: n = cap / 4; break;                          // exactly the whole buffer
		default: n = 0; break;                               // normal (1 + batching) * 1024
		}
		NextAudioResult res = Sim_Step(&sim, n);
		steps++;
		ok &= res.payloadSize <= cap;
		if (!res.payloadSize)
			continue;
		sent++;
		if (res.hasExtHeader)
		{
			NextExtAudioHeader h;
			ok &= NextExt_ReadAudioHeader(Pkt.data, res.payloadSize, &h) && h.codec == res.codec;
			if (h.codec == 1) ok &= res.payloadSize == 12 + 4u * h.samplesPerChannel;
			if (h.codec == 2) ok &= res.payloadSize == 20u + h.samplesPerChannel;
			if (h.codec == 3)
			{
				uint32_t pos = 12;
				for (int f = 0; f < h.frameCount && ok; f++)
				{
					uint32_t len = next_rd16(Pkt.data + pos);
					ok &= pos + 2 + len <= res.payloadSize;
					if (!ok) break;
					int dn = opus_decode(d, Pkt.data + pos + 2, (opus_int32)len, pcm, 5760, 0);
					ok &= dn == ((h.complexityFrame >> 4) ? 480 : 960);
					pos += 2 + len;
					opusFrames++;
				}
				ok &= pos == res.payloadSize;
			}
		}
		else
			ok &= res.codec == 0 && res.payloadSize % 4 == 0;
	}
	NextAudioStats st = NextAudio_GetStats();
	printf("  %ld steps, %ld packets, %ld Opus frames decoded, opus errors %u\n", steps, sent, opusFrames, st.opusErrors);
	CHECK(ok && st.opusErrors == 0, "all packets well formed");

	// Garbage arguments
	NextAudio_BeginPacket(NULL, 0);
	NextAudioResult r = NextAudio_Encode(NULL, 0, 1000, 0);
	CHECK(r.payloadSize == 0, "NULL buffer");
	uint8_t tiny[8];
	NextAudioConfig c = NextExt_AudioConfigFromWire(2, 0, 0);
	NextAudio_Start(&c);
	uint32_t off = NextAudio_BeginPacket(tiny, sizeof(tiny));
	r = NextAudio_Encode(tiny, sizeof(tiny), 100000, 0);
	CHECK(off == sizeof(tiny) && r.payloadSize == 0, "buffer smaller than the offset");

	// Exact-size heap buffers of every small size with every codec: ASan reports any stray byte
	long produced = 0;
	for (int round = 0; round < 3000; round++)
	{
		size_t bcap = (size_t)(Rand() % 5000);
		uint8_t* b = malloc(bcap ? bcap : 1);
		memset(b, 0x11, bcap);
		NextAudioConfig rc = NextExt_AudioConfigFromWire((uint8_t)(Rand() % 4), (uint8_t)Rand(), (uint8_t)Rand());
		NextAudio_Request(&rc);
		for (int k = 0; k < 3; k++)
		{
			uint32_t o = NextAudio_BeginPacket(b, bcap);
			bool okOff = o <= bcap;
			CHECK(okOff, "offset %u beyond buffer %zu", o, bcap);
			if (!okOff)
				break;
			NextAudioResult rr = NextAudio_Encode(b, bcap, (uint32_t)(Rand() % 30000), 0);
			CHECK(rr.payloadSize <= bcap, "payload %u beyond buffer %zu", rr.payloadSize, bcap);
			produced += rr.payloadSize > 0;
		}
		free(b);
	}
	printf("  exact-size heap buffers 0..4999 bytes: %ld packets produced, no out of bounds access\n", produced);
}

// ============================================================================ diagnostics

static void TestDiag(void)
{
	printf("[diagnostics]\n");
	NextCpuSample a = { 1000, 100, 500, 1000, 1, 1 }, b;
	NextDiag_Begin(0, &a);
	uint64_t ts = 1000000;
	// 30 frames with one slow send followed by a gap, one gap after a normal send, one normal slow send
	for (int i = 0; i < 30; i++)
	{
		uint64_t step = 33333;
		if (i == 10) step = 100000; // gap after the slow send of frame 9
		if (i == 20) step = 70000;  // gap after a fast send
		ts += step;
		NextDiag_OnVideoFrame(ts);
		NextDiag_OnVideoSent(i == 9 ? 45000 : (i == 25 ? 30000 : 1000));
	}
	NextDiag_OnAudioPacket(250, 100);
	NextDiag_OnAudioPacket(250, 300);
	NextDiag_SetTos(1, true);
	NextDiag_SetTos(0, true);
	CHECK(!NextDiag_Due(999999) && NextDiag_Due(1000000), "due after 1 s");
	b = (NextCpuSample){ 2001000, 60100, 1400500, 2001000, 1, 1 };
	NextDiagReport r;
	NextDiag_Collect(1000000, &b, &r);
	CHECK(r.intervalMs == 1000 && r.videoFramesSent == 30 && r.videoGrcGaps == 2 && r.gapsAfterSlowSend == 1 && r.videoSendsOver20ms == 2 &&
		r.videoSendBlockMaxUs == 45000 && r.videoSendBlockTotalUs == 28 * 1000 + 45000 + 30000,
		"video counters: frames %u gaps %u afterSlow %u over20 %u max %u total %u", r.videoFramesSent, r.videoGrcGaps,
		r.gapsAfterSlowSend, r.videoSendsOver20ms, r.videoSendBlockMaxUs, r.videoSendBlockTotalUs);
	CHECK(r.audioPackets == 2 && r.audioEncodeTotalUs == 500 && r.audioSendBlockTotalUs == 400 && r.tosFlags == 3, "audio counters, tos");
	CHECK(r.core3IdlePermille == 700 && r.sysdvrCpuPermille == 30, "cpu: idle %u cpu %u", r.core3IdlePermille, r.sysdvrCpuPermille);

	// New window starts empty; a gap across the window edge is still attributed to the slow send
	NextDiag_OnVideoSent(50000);
	ts += 200000;
	NextDiag_OnVideoFrame(ts);
	NextCpuSample c2 = b;
	c2.idleValid = 0;
	c2.wallTicks = b.wallTicks; // no wall progress -> unavailable
	NextDiag_Collect(2000000, &c2, &r);
	CHECK(r.videoFramesSent == 1 && r.videoGrcGaps == 1 && r.gapsAfterSlowSend == 1 && r.audioPackets == 0, "second window");
	CHECK(r.core3IdlePermille == NEXT_DIAG_UNAVAILABLE && r.sysdvrCpuPermille == NEXT_DIAG_UNAVAILABLE, "unavailable -> 0xFFFFFFFF");

	// Timeline reset (new game): not a gap
	NextDiag_OnVideoFrame(5000);
	NextDiag_OnVideoFrame(5000 + 33333);
	NextDiag_Collect(3000000, NULL, &r);
	CHECK(r.videoGrcGaps == 0, "timestamp going backwards is a resync, not a gap");
	NextDiag_Begin(0, NULL);
	NextDiag_Collect(0, NULL, &r);
	CHECK(r.tosFlags == 2, "video session start clears only the video TOS bit");
}

// ============================================================================ session (fake platform)

// A scripted platform: audio capture from the WAV, sends recorded in memory, control bytes fed from
// a script, optional capture failures. Lets the real next_session.c loops run in the unit test.
static struct {
	int sendsLeft;
	int captures;
	int failEvery;
	const uint8_t* ctrl;
	size_t ctrlLen, ctrlPos;
	int ctrlChunk;
	uint8_t slots[512];
	uint32_t sizes[512];
	uint8_t metas[512];
	int sent;
	uint64_t now;
} FP;

static struct __attribute__((packed)) { NextPacketHeader h; uint8_t data[0x54000]; } FakeVideo;

NextPacketHeader* NextPlat_CaptureVideo(void)
{
	static uint64_t n;
	FakeVideo.h.Magic = NEXT_PACKET_MAGIC;
	FakeVideo.h.DataSize = 100;
	FakeVideo.h.Timestamp = 1000000 + 33333 * (n += (n % 50 == 49) ? 3 : 1);
	FakeVideo.h.MetaData = NEXT_META_VIDEO | NEXT_META_DATA;
	FakeVideo.h.ReplaySlot = 0xFF;
	FP.now += 33333;
	return &FakeVideo.h;
}
NextPacketHeader* NextPlat_AudioPacket(void) { return &Pkt.h; }
uint32_t NextPlat_AudioCapacity(void) { return sizeof(Pkt.data); }
bool NextPlat_CaptureAudio(uint32_t offset)
{
	FP.captures++;
	if (FP.failEvery && FP.captures % FP.failEvery == 0)
	{
		memset(Pkt.data, 0, 32);
		Pkt.h.MetaData = NEXT_META_AUDIO | NEXT_META_ERROR;
		Pkt.h.DataSize = 32;
		return false;
	}
	for (uint32_t i = 0; i < 4096; i++)
		memcpy(Pkt.data + offset + 4 * i, &Wav.samples[2 * ((FP.captures * 4096 + i) % Wav.frames)], 4);
	Pkt.h.DataSize = 4096 * 4;
	Pkt.h.Timestamp = 1000000 + (uint64_t)FP.captures * 85333;
	FP.now += 85333;
	return true;
}
bool NextPlat_SendAll(int sock, const void* buf, uint32_t size, bool allowIncoming)
{
	const NextPacketHeader* h = buf;
	if (FP.sent < 512)
	{
		FP.slots[FP.sent] = h->ReplaySlot;
		FP.sizes[FP.sent] = h->DataSize;
		FP.metas[FP.sent] = h->MetaData;
	}
	FP.sent++;
	if (size != h->DataSize + 18u)
		return false;
	return --FP.sendsLeft > 0;
}
int NextPlat_RecvNonBlocking(int sock, void* buf, uint32_t size)
{
	if (FP.ctrlPos >= FP.ctrlLen)
		return 0;
	size_t n = (size_t)FP.ctrlChunk < size ? (size_t)FP.ctrlChunk : size;
	if (n > FP.ctrlLen - FP.ctrlPos)
		n = FP.ctrlLen - FP.ctrlPos;
	memcpy(buf, FP.ctrl + FP.ctrlPos, n);
	FP.ctrlPos += n;
	return (int)n;
}
bool NextPlat_SetTos(int sock, int tos) { return tos == NEXT_IP_TOS_VALUE; }
uint64_t NextPlat_NowUs(void) { return FP.now; }
bool NextPlat_Running(void) { return true; }
void NextPlat_Yield(void) { }
void NextPlat_CpuSample(NextCpuSample* s) { memset(s, 0, sizeof(*s)); s->wallTicks = FP.now; }
void NextPlat_AudioThreadStarted(void) { }
NextAudioWork* NextPlat_AudioWork(void) { return &Work; }

static void TestSessions(void)
{
	printf("[session loops with a scripted platform]\n");
	// Audio: control bytes arrive 5 at a time, with junk, while packets are produced
	uint8_t ctrl[64];
	size_t n = 0;
	memcpy(ctrl, "junk", 4); n = 4;
	NextCtrl_Build(ctrl + n, 2, 0, 0, 0); n += 8;
	memcpy(ctrl + n, "\x01\x02\x03", 3); n += 3;
	NextCtrl_Build(ctrl + n, 3, 32, 0x13, 0); n += 8;
	memset(&FP, 0, sizeof(FP));
	FP.sendsLeft = 40;
	FP.ctrl = ctrl;
	FP.ctrlLen = n;
	FP.ctrlChunk = 5;
	FP.failEvery = 17;
	NextExtConfig ext = NextExt_ParseHandshake((const uint8_t*)"\xAA\xAA\xAA\xAA" "03\x02\x00\x03\x04\x01\x00\x00\x02\x00\x00", 16);
	NextSession_Audio(1, &ext);
	int pcm24 = 0, adpcm = 0, opus = 0, errors = 0;
	bool order = true;
	for (int i = 0; i < FP.sent && i < 512; i++)
	{
		if (FP.metas[i] & NEXT_META_ERROR) { errors++; order &= FP.slots[i] == 0xFF && FP.sizes[i] == 32; continue; }
		pcm24 += FP.slots[i] == 0xE1;
		adpcm += FP.slots[i] == 0xE2;
		opus += FP.slots[i] == 0xE3;
	}
	printf("  audio session: %d packets (PCM24 %d, ADPCM %d, OPUS %d, error packets %d)\n", FP.sent, pcm24, adpcm, opus, errors);
	CHECK(FP.slots[0] == 0xE1 && pcm24 >= 1 && adpcm >= 1 && opus >= 10 && errors >= 2 && order, "codec switches via control messages, error packets");

	// Video with diagnostics: every 1 s (fake clock) a type-3 packet follows a video packet
	memset(&FP, 0, sizeof(FP));
	FP.sendsLeft = 200;
	ext = NextExt_ParseHandshake((const uint8_t*)"\xAA\xAA\xAA\xAA" "03\x01\x07\x00\x04\x00\x00\x00\x03\x00\x00", 16);
	NextSession_Video(1, &ext);
	int diag = 0, video = 0;
	bool diagOk = true;
	for (int i = 0; i < FP.sent && i < 512; i++)
	{
		if ((FP.metas[i] & 3) == 3) { diag++; diagOk &= FP.sizes[i] == NEXT_DIAG_SIZE && FP.slots[i] == 0xFF && FP.metas[i] == 0x07; }
		else video++;
	}
	printf("  video session: %d video packets, %d diag packets\n", video, diag);
	CHECK(diagOk && diag >= 5 && diag <= 7, "one diag packet per second");

	// Without the diag bit: no diag packets at all
	memset(&FP, 0, sizeof(FP));
	FP.sendsLeft = 100;
	ext.diag = false;
	NextSession_Video(1, &ext);
	diag = 0;
	for (int i = 0; i < FP.sent && i < 512; i++)
		diag += (FP.metas[i] & 3) == 3;
	CHECK(diag == 0, "no diag without request");
}

int main(int argc, char** argv)
{
	char err[256];
	if (argc < 2 || !Wav_Load(argv[1], &Wav, err, sizeof(err)))
	{
		printf("usage: next_tests <48k stereo wav> (%s)\n", argc < 2 ? "missing argument" : err);
		return 2;
	}
	if (Wav.frames < 48000 * 60)
	{
		printf("need at least 60 s of audio\n");
		return 2;
	}

	TestHandshake();
	TestControlParser();
	TestHeaders();
	TestFuzzParsers();
	TestHalfband();
	TestAdpcm();
	TestOpus();
	TestPipelineFuzz();
	TestDiag();
	TestSessions();

	printf("\n%d checks, %d failures -> %s\n", Checks, Failures, Failures ? "FAIL" : "PASS");
	return Failures ? 1 : 0;
}
