// Encoder timing and libopus memory measurements (release build).
//   next_bench <48 kHz stereo wav>
#include "next_audio.h"
#include "media.h"
#include "sim.h"
#include "opus.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static WavData Wav;
static NextAudioWork Work;
static SimAudioPacket Pkt __attribute__((aligned(4096)));

static double ThreadCpuSec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint64_t WallUs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

// CPU milliseconds per second of audio for one configuration (batching 3, like the client default)
static double TimeConfig(const NextAudioConfig* cfg, const int16_t* pcm, uint64_t frames, double seconds)
{
	NextAudio_Init(&Work);
	NextAudio_SetHooks(NULL, NULL);
	NextAudio_Start(cfg);
	Sim sim;
	Sim_Init(&sim, &Pkt, pcm, frames, 3, 0);
	uint64_t total = (uint64_t)(seconds * 48000);
	double t0 = ThreadCpuSec();
	while (sim.captured < total)
		Sim_Step(&sim, 0);
	double t1 = ThreadCpuSec();
	return (t1 - t0) * 1000.0 / (sim.captured / 48000.0);
}

// Signals for the pseudostack search: music, silence, white noise, clicks
static int16_t* MakeSignal(int kind, uint64_t frames)
{
	int16_t* s = malloc(frames * 4);
	uint32_t rng = 1;
	for (uint64_t i = 0; i < frames; i++)
	{
		int16_t l = 0, r = 0;
		switch (kind)
		{
		case 0: l = Wav.samples[2 * ((i + 48000 * 40) % Wav.frames)]; r = Wav.samples[2 * ((i + 48000 * 40) % Wav.frames) + 1]; break;
		case 1: break;
		case 2: rng = rng * 1664525 + 1013904223; l = (int16_t)(rng >> 16); rng = rng * 1664525 + 1013904223; r = (int16_t)(rng >> 16); break;
		case 3: l = (i % 4800) < 3 ? 32767 : 0; r = (i % 3333) < 2 ? -32768 : 0; break;
		}
		s[2 * i] = l;
		s[2 * i + 1] = r;
	}
	return s;
}

static bool RunsWithLimit(const NextAudioConfig* cfg, const int16_t* sig, uint64_t frames, int limit)
{
	NextAudio_Init(&Work);
	NextAudio_SetPseudoStackLimit(limit);
	NextAudio_Start(cfg);
	Sim sim;
	Sim_Init(&sim, &Pkt, sig, frames, 3, 0);
	while (sim.captured < frames)
	{
		NextAudioResult r = Sim_Step(&sim, 0);
		if (r.opusFailed)
			break;
	}
	bool ok = NextAudio_GetStats().opusFatal == 0 && NextAudio_GetStats().opusErrors == 0;
	NextAudio_SetPseudoStackLimit(NEXT_OPUS_PSEUDOSTACK_BYTES);
	return ok;
}

// Thread stack high-water mark of NextAudio_Encode(), measured on a pthread with a pre-filled stack
typedef struct { NextAudioConfig cfg; } StackArgs;
static void* StackWorker(void* arg)
{
	StackArgs* a = arg;
	NextAudio_Init(&Work);
	NextAudio_Start(&a->cfg);
	Sim sim;
	Sim_Init(&sim, &Pkt, Wav.samples + 48000 * 40 * 2, 48000 * 3, 3, 0);
	while (sim.captured < 48000 * 3)
		Sim_Step(&sim, 0);
	return NULL;
}

static size_t MeasureStack(const NextAudioConfig* cfg)
{
	const size_t size = 256 * 1024;
	uint8_t* stack = NULL;
	posix_memalign((void**)&stack, 16384, size);
	memset(stack, 0x5A, size);
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstack(&attr, stack, size);
	pthread_t t;
	StackArgs a = { *cfg };
	pthread_create(&t, &attr, StackWorker, &a);
	pthread_join(t, NULL);
	size_t untouched = 0;
	while (untouched < size && stack[untouched] == 0x5A)
		untouched++;
	free(stack);
	return size - untouched;
}

int main(int argc, char** argv)
{
	char err[256];
	if (argc < 2 || !Wav_Load(argv[1], &Wav, err, sizeof(err)) || Wav.frames < 48000 * 60)
	{
		printf("usage: next_bench <48k stereo wav, >= 60 s>\n");
		return 2;
	}
	const int16_t* music = Wav.samples + 48000 * 10 * 2;
	const uint64_t musicFrames = Wav.frames - 48000 * 10;

	printf("== encoding cost on this machine (CPU ms per second of audio, batching 3) ==\n");
	struct { const char* name; uint8_t codec, kbpsHalf, cf; } cfgs[] = {
		{ "PCM48 (copy)", 0, 0, 0 }, { "PCM24", 1, 0, 0 }, { "ADPCM", 2, 0, 0 },
		{ "Opus 64k 20ms c0", 3, 32, 0x00 }, { "Opus 64k 20ms c5", 3, 32, 0x05 }, { "Opus 64k 20ms c10", 3, 32, 0x0A },
		{ "Opus 96k 20ms c0", 3, 48, 0x00 }, { "Opus 96k 20ms c5", 3, 48, 0x05 }, { "Opus 96k 20ms c10", 3, 48, 0x0A },
		{ "Opus 96k 10ms c0", 3, 48, 0x10 }, { "Opus 96k 10ms c5", 3, 48, 0x15 }, { "Opus 96k 10ms c10", 3, 48, 0x1A },
		{ "Opus 256k 20ms c10", 3, 128, 0x0A }, { "Opus 256k 10ms c10", 3, 128, 0x1A }, { "Opus 16k 20ms c10", 3, 8, 0x0A },
	};
	for (size_t i = 0; i < sizeof(cfgs) / sizeof(cfgs[0]); i++)
	{
		NextAudioConfig c = NextExt_AudioConfigFromWire(cfgs[i].codec, cfgs[i].kbpsHalf, cfgs[i].cf);
		double best = 1e9;
		for (int rep = 0; rep < 3; rep++)
		{
			double ms = TimeConfig(&c, music, musicFrames, 30);
			if (ms < best) best = ms;
		}
		printf("  %-20s %7.3f ms/s  (%.3f%% of one core)\n", cfgs[i].name, best, best / 10.0);
	}

	printf("\n== libopus memory ==\n");
	NextAudio_Init(&Work);
	printf("  encoder state, RESTRICTED_CELT stereo fixed point: %u bytes (static buffer %d)\n",
		NextAudio_GetStats().opusStateBytes, NEXT_OPUS_STATE_BYTES);

	// Exact pseudostack requirement: the smallest limit for which libopus's own bounds check never fires
	int worst = 0;
	const char* worstCfg = "";
	static char worstBuf[64];
	int16_t* sigs[4];
	const uint64_t sigFrames = 48000;
	for (int k = 0; k < 4; k++)
		sigs[k] = MakeSignal(k, sigFrames);
	int kbpsList[] = { 16, 64, 96, 256 };
	for (int frame = 0; frame < 2; frame++)
		for (int cx = 0; cx <= 10; cx++)
			for (int ki = 0; ki < 4; ki++)
			{
				NextAudioConfig c = NextExt_AudioConfigFromWire(3, (uint8_t)(kbpsList[ki] / 2), (uint8_t)(cx | (frame << 4)));
				int need = 0;
				for (int k = 0; k < 4; k++)
				{
					int lo = 1024, hi = NEXT_OPUS_PSEUDOSTACK_BYTES;
					if (!RunsWithLimit(&c, sigs[k], sigFrames, hi))
					{
						printf("  !! %d kbps c%d %s fails even with the full pseudostack\n", kbpsList[ki], cx, frame ? "10ms" : "20ms");
						need = hi + 1;
						break;
					}
					while (lo < hi)
					{
						int mid = (lo + hi) / 2;
						if (RunsWithLimit(&c, sigs[k], sigFrames, mid)) hi = mid; else lo = mid + 1;
					}
					if (lo > need) need = lo;
				}
				if (need > worst)
				{
					worst = need;
					snprintf(worstBuf, sizeof(worstBuf), "%d kbps, complexity %d, %s", kbpsList[ki], cx, frame ? "10 ms" : "20 ms");
					worstCfg = worstBuf;
				}
				if (ki == 3 && (cx == 0 || cx == 5 || cx == 7 || cx == 8 || cx == 10))
					printf("  pseudostack needed, %s frames, complexity %2d: %d bytes (max over bitrates so far)\n", frame ? "10 ms" : "20 ms", cx, need);
			}
	printf("  WORST CASE pseudostack: %d bytes (%s); configured %d bytes, margin %d bytes\n", worst, worstCfg,
		NEXT_OPUS_PSEUDOSTACK_BYTES, NEXT_OPUS_PSEUDOSTACK_BYTES - worst);

	printf("\n== thread stack used by the audio encode path (host arm64 clang -O2, indicative) ==\n");
	for (size_t i = 0; i < sizeof(cfgs) / sizeof(cfgs[0]); i++)
	{
		if (i != 1 && i != 2 && i != 5 && i != 11 && i != 12)
			continue;
		NextAudioConfig c = NextExt_AudioConfigFromWire(cfgs[i].codec, cfgs[i].kbpsHalf, cfgs[i].cf);
		printf("  %-20s %zu bytes\n", cfgs[i].name, MeasureStack(&c));
	}
	(void)WallUs;
	return 0;
}
