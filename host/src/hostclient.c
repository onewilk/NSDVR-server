// sysdvr_hostclient: a small TCP Bridge client used to test sysdvr_hostmock (and, in principle, a
// real console). It performs the protocol 03 handshake (optionally with the NSDVR extension),
// validates every packet, decodes all audio codecs, sends control messages on a schedule and
// prints a summary. Exit code 0 = no protocol violations and all --expect checks passed.
#include "media.h"
#include "next_ext.h"
#include "next_audio.h"
#include "opus.h"
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
	double at;
	uint8_t codec, kbpsHalf, complexity, frame;
	bool sent;
} CtrlStep;

static struct {
	const char* host;
	int videoPort, audioPort;
	double seconds;
	bool legacy, video, audio, diag, tos, fuzzCtrl, quiet;
	uint8_t codec, kbpsHalf, complexity, frame, batching;
	CtrlStep steps[32];
	int stepCount;
	const char* dumpWav;
	int expectGapsAfterSlow; // -1 = no check
	int expectMaxGapsAfterSlow;
	int expectMinGaps;
	int expectMaxGaps;
	int expectCodecs;        // bitmask of codecs that must have been seen
	bool expectLegacyAudio;
	bool expectNoDiag;
} O;

static atomic_bool Running = true;
static atomic_int Violations;
static pthread_mutex_t LogLock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t NowUs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void Msg(const char* fmt, ...)
{
	pthread_mutex_lock(&LogLock);
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	pthread_mutex_unlock(&LogLock);
}

static void Violation(const char* fmt, ...)
{
	atomic_fetch_add(&Violations, 1);
	pthread_mutex_lock(&LogLock);
	printf("VIOLATION: ");
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	pthread_mutex_unlock(&LogLock);
}

static int Connect(int port)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)port);
	inet_pton(AF_INET, O.host, &a.sin_addr);
	if (connect(s, (struct sockaddr*)&a, sizeof(a)) < 0)
	{
		close(s);
		return -1;
	}
	int one = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	return s;
}

// Returns 1 on success, 0 on timeout (Running cleared / deadline), -1 on error
static int RecvAll(int s, void* buf, size_t n, uint64_t deadline)
{
	size_t got = 0;
	while (got < n)
	{
		if (!atomic_load(&Running) || NowUs() > deadline)
			return 0;
		struct pollfd p = { .fd = s, .events = POLLIN };
		if (poll(&p, 1, 100) <= 0)
			continue;
		ssize_t r = recv(s, (char*)buf + got, n - got, 0);
		if (r <= 0)
			return -1;
		got += (size_t)r;
	}
	return 1;
}

static bool SendAll(int s, const void* buf, size_t n)
{
	size_t sent = 0;
	while (sent < n)
	{
		ssize_t r = send(s, (const char*)buf + sent, n - sent, 0);
		if (r <= 0)
			return false;
		sent += (size_t)r;
	}
	return true;
}

static bool DoHandshake(int s, bool video)
{
	uint64_t deadline = NowUs() + 5000000;
	char hello[10];
	if (RecvAll(s, hello, sizeof(hello), deadline) != 1 || memcmp(hello, "SysDVR|03", 10))
	{
		Violation("%s: bad hello", video ? "video" : "audio");
		return false;
	}
	uint8_t req[16];
	memset(req, 0, sizeof(req));
	next_wr32(req, 0xAAAAAAAAu);
	req[4] = '0';
	req[5] = '3';
	req[6] = video ? 1 : 2;
	req[7] = video ? 0x07 : 0; // NAL hash (IDR only) + SPS/PPS injection, like the NSDVR client
	req[8] = video ? 0 : O.batching;
	if (!O.legacy)
	{
		req[9] = NEXT_FEATURE_FLAG_EXT;
		req[10] = O.codec;
		req[11] = O.kbpsHalf;
		req[12] = (uint8_t)(O.complexity | (O.frame << 4));
		req[13] = (uint8_t)((O.diag ? NEXT_RES3_DIAG : 0) | (O.tos ? NEXT_RES3_TOS : 0));
	}
	if (!SendAll(s, req, sizeof(req)))
		return false;
	uint8_t resp[72];
	if (RecvAll(s, resp, sizeof(resp), deadline) != 1)
	{
		Violation("%s: no handshake response", video ? "video" : "audio");
		return false;
	}
	uint32_t code = next_rd32(resp);
	if (code != 6)
	{
		Violation("%s: handshake result %u", video ? "video" : "audio", code);
		return false;
	}
	return true;
}

// ----------------------------------------------------------------------------- video

static struct {
	uint64_t packets, replay, errors, diag, bytes;
	uint64_t lastTs;
	NextDiagReport sum;
	uint32_t maxBlockUs;
	uint32_t idleMin, idleMax, cpuMax;
	bool cpuAvail, idleAvail;
} VS;

static void* VideoThread(void* arg)
{
	(void)arg;
	int s = Connect(O.videoPort);
	if (s < 0)
	{
		Violation("video: connect failed");
		return NULL;
	}
	if (!DoHandshake(s, true))
	{
		close(s);
		return NULL;
	}
	VS.idleMin = 1000;
	static uint8_t payload[0x54000 + 64];
	uint64_t end = NowUs() + (uint64_t)(O.seconds * 1e6);
	while (atomic_load(&Running) && NowUs() < end)
	{
		uint8_t h[18];
		int r = RecvAll(s, h, sizeof(h), end);
		if (r == 0)
			break;
		if (r < 0)
		{
			Violation("video: connection lost");
			break;
		}
		uint32_t magic = next_rd32(h), size = next_rd32(h + 4);
		uint64_t ts = next_rd64(h + 8);
		uint8_t meta = h[16], slot = h[17];
		if (magic != NEXT_PACKET_MAGIC || size > sizeof(payload))
		{
			Violation("video: bad packet header magic=%08x size=%u", magic, size);
			break;
		}
		if (size && RecvAll(s, payload, size, end + 2000000) != 1)
			break;

		if ((meta & NEXT_META_TYPE_MASK) == NEXT_META_TYPE_DIAG)
		{
			NextDiagReport d;
			if (O.legacy || !O.diag)
				Violation("video: diagnostics packet without request");
			if (size != NEXT_DIAG_SIZE || !NextExt_ReadDiag(payload, size, &d))
			{
				Violation("video: bad diag packet (size %u)", size);
				continue;
			}
			if (slot != 0xFF)
				Violation("video: diag ReplaySlot %02x", slot);
			VS.diag++;
			VS.sum.videoFramesSent += d.videoFramesSent;
			VS.sum.videoGrcGaps += d.videoGrcGaps;
			VS.sum.videoSendBlockTotalUs += d.videoSendBlockTotalUs;
			VS.sum.videoSendsOver20ms += d.videoSendsOver20ms;
			VS.sum.gapsAfterSlowSend += d.gapsAfterSlowSend;
			VS.sum.audioPackets += d.audioPackets;
			VS.sum.audioEncodeTotalUs += d.audioEncodeTotalUs;
			VS.sum.audioSendBlockTotalUs += d.audioSendBlockTotalUs;
			VS.sum.tosFlags |= d.tosFlags;
			if (d.videoSendBlockMaxUs > VS.maxBlockUs)
				VS.maxBlockUs = d.videoSendBlockMaxUs;
			if (d.core3IdlePermille != NEXT_DIAG_UNAVAILABLE)
			{
				VS.idleAvail = true;
				if (d.core3IdlePermille < VS.idleMin) VS.idleMin = d.core3IdlePermille;
				if (d.core3IdlePermille > VS.idleMax) VS.idleMax = d.core3IdlePermille;
				if (d.core3IdlePermille > 1000) Violation("video: idle permille %u", d.core3IdlePermille);
			}
			if (d.sysdvrCpuPermille != NEXT_DIAG_UNAVAILABLE)
			{
				VS.cpuAvail = true;
				if (d.sysdvrCpuPermille > VS.cpuMax) VS.cpuMax = d.sysdvrCpuPermille;
			}
			if (d.intervalMs < 900 || d.intervalMs > 5000)
				Violation("video: diag interval %u ms", d.intervalMs);
			if (!O.quiet)
				Msg("diag: %ums frames=%u gaps=%u block=%u/%uus >20ms=%u gapsAfterSlow=%u audio=%u enc=%uus asend=%uus idle3=%d cpu=%d tos=%u",
					d.intervalMs, d.videoFramesSent, d.videoGrcGaps, d.videoSendBlockTotalUs, d.videoSendBlockMaxUs,
					d.videoSendsOver20ms, d.gapsAfterSlowSend, d.audioPackets, d.audioEncodeTotalUs, d.audioSendBlockTotalUs,
					(int)d.core3IdlePermille, (int)d.sysdvrCpuPermille, d.tosFlags);
			continue;
		}

		if ((meta & NEXT_META_TYPE_MASK) != NEXT_META_VIDEO)
		{
			Violation("video: unexpected packet type meta=%02x", meta);
			continue;
		}
		VS.packets++;
		VS.bytes += size;
		if (meta & NEXT_META_REPLAY) VS.replay++;
		if (meta & NEXT_META_ERROR) VS.errors++;
		if (VS.lastTs && ts < VS.lastTs)
			Violation("video: timestamp went backwards");
		VS.lastTs = ts;
	}
	close(s);
	return NULL;
}

// ----------------------------------------------------------------------------- audio

static struct {
	uint64_t packets[5]; // per codec, [4] = legacy 0xFF
	uint64_t errors, samples48;
	uint64_t switches;
	int lastCodec;
	uint64_t nextTs;
	int64_t maxJumpUs;
	uint64_t jumpsOver25ms;
	uint64_t encodeUsTotal;
	uint64_t ctrlSent;
	double sumSq;
	uint64_t sumN;
	NextAdpcmChannel adpcmState[2];
	bool adpcmHaveState;
	uint64_t adpcmResyncMismatch;
	int16_t* dump;
	uint64_t dumpFrames, dumpCap;
} AS;

static OpusDecoder* Dec;
static uint8_t DecMem[32768];
static NextAudioWork Work; // provides the libopus pseudostack (also used by the decoder)

static void DumpAppend(const int16_t* s, uint32_t frames, int repeat)
{
	if (!O.dumpWav)
		return;
	if (AS.dumpFrames + (uint64_t)frames * repeat > AS.dumpCap)
	{
		AS.dumpCap = (AS.dumpFrames + (uint64_t)frames * repeat) * 2 + 48000;
		AS.dump = realloc(AS.dump, AS.dumpCap * 4);
	}
	for (uint32_t i = 0; i < frames; i++)
		for (int k = 0; k < repeat; k++)
		{
			AS.dump[2 * AS.dumpFrames] = s[2 * i];
			AS.dump[2 * AS.dumpFrames + 1] = s[2 * i + 1];
			AS.dumpFrames++;
		}
}

static void Level(const int16_t* s, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		AS.sumSq += (double)s[i] * s[i];
	AS.sumN += n;
}

static void CheckTs(uint64_t ts, uint32_t samples, uint32_t rate, int codec)
{
	if (AS.nextTs)
	{
		int64_t jump = (int64_t)ts - (int64_t)AS.nextTs;
		bool switched = codec != AS.lastCodec;
		if (llabs(jump) > llabs(AS.maxJumpUs) && !switched)
			AS.maxJumpUs = jump;
		if (llabs(jump) > 25000 && !switched)
			AS.jumpsOver25ms++;
	}
	AS.nextTs = ts + (uint64_t)samples * 1000000u / rate;
}

static void HandleAudio(uint64_t ts, uint8_t meta, uint8_t slot, const uint8_t* p, uint32_t size)
{
	static int16_t pcm[65536 * 2];
	if (meta & NEXT_META_ERROR)
	{
		AS.errors++;
		return;
	}
	if ((meta & NEXT_META_TYPE_MASK) != NEXT_META_AUDIO)
	{
		Violation("audio: packet type meta=%02x", meta);
		return;
	}

	if (slot == 0xFF)
	{
		if (!O.legacy)
			Violation("audio: extension requested but got an official packet");
		AS.packets[4]++;
		uint32_t frames = size / 4;
		memcpy(pcm, p, (size_t)frames * 4 > sizeof(pcm) ? sizeof(pcm) : (size_t)frames * 4);
		Level(pcm, frames * 2);
		DumpAppend(pcm, frames, 1);
		CheckTs(ts, frames, 48000, 4);
		AS.lastCodec = 4;
		AS.samples48 += frames;
		return;
	}
	if (O.legacy)
		Violation("audio: legacy client got slot %02x", slot);
	if (slot < 0xE0 || slot > 0xE3)
	{
		Violation("audio: unknown ReplaySlot %02x", slot);
		return;
	}

	int codec = slot & 3;
	if (AS.lastCodec != codec && AS.packets[0] + AS.packets[1] + AS.packets[2] + AS.packets[3] > 0)
		AS.switches++;
	AS.packets[codec]++;

	if (codec == NextCodec_PCM48)
	{
		uint32_t frames = size / 4;
		if (size % 4)
			Violation("audio: PCM48 size %u", size);
		memcpy(pcm, p, (size_t)frames * 4 > sizeof(pcm) ? sizeof(pcm) : (size_t)frames * 4);
		Level(pcm, frames * 2);
		DumpAppend(pcm, frames, 1);
		CheckTs(ts, frames, 48000, codec);
		AS.samples48 += frames;
		AS.adpcmHaveState = false;
		AS.lastCodec = codec;
		return;
	}

	NextExtAudioHeader h;
	if (!NextExt_ReadAudioHeader(p, size, &h))
	{
		Violation("audio: bad ExtAudioHeader (size %u)", size);
		return;
	}
	if (h.codec != codec)
		Violation("audio: header codec %u vs slot %02x", h.codec, slot);
	AS.encodeUsTotal += h.encodeUs;
	const uint8_t* body = p + NEXT_EXT_AUDIO_HEADER_SIZE;
	uint32_t bodyLen = size - NEXT_EXT_AUDIO_HEADER_SIZE;

	if (codec == NextCodec_PCM24)
	{
		if (bodyLen != (uint32_t)h.samplesPerChannel * 4 || h.bitrateKbps != 768 || h.frameCount != 1)
			Violation("audio: PCM24 header mismatch (len %u spc %u)", bodyLen, h.samplesPerChannel);
		uint32_t n = h.samplesPerChannel;
		memcpy(pcm, body, (size_t)n * 4);
		Level(pcm, n * 2);
		DumpAppend(pcm, n, 2);
		CheckTs(ts, n, 24000, codec);
		AS.samples48 += 2 * n;
		AS.adpcmHaveState = false;
	}
	else if (codec == NextCodec_ADPCM)
	{
		uint32_t n = h.samplesPerChannel;
		if (bodyLen != NEXT_ADPCM_STATE_SIZE + n || h.bitrateKbps != 384)
			Violation("audio: ADPCM header mismatch (len %u spc %u)", bodyLen, n);
		else
		{
			// The per-packet state must equal the state our decoder reached at the end of the previous packet
			if (AS.adpcmHaveState)
			{
				for (int c = 0; c < 2; c++)
				{
					int16_t pred;
					memcpy(&pred, body + 4 * c, 2);
					if (pred != AS.adpcmState[c].predictor || body[4 * c + 2] != AS.adpcmState[c].index)
						AS.adpcmResyncMismatch++;
				}
			}
			NextAdpcm_DecodePacket(body, bodyLen, n, pcm);
			// Track the running state by decoding the last sample again from the packet's own state
			NextAdpcmChannel ch[2];
			for (int c = 0; c < 2; c++)
			{
				int16_t pred;
				memcpy(&pred, body + 4 * c, 2);
				ch[c].predictor = pred;
				ch[c].index = body[4 * c + 2];
			}
			for (uint32_t i = 0; i < n; i++)
			{
				NextAdpcm_DecodeSample(&ch[0], body[8 + i] & 15);
				NextAdpcm_DecodeSample(&ch[1], body[8 + i] >> 4);
			}
			AS.adpcmState[0] = ch[0];
			AS.adpcmState[1] = ch[1];
			AS.adpcmHaveState = true;
			Level(pcm, n * 2);
			DumpAppend(pcm, n, 1);
			CheckTs(ts, n, 48000, codec);
			AS.samples48 += n;
		}
	}
	else
	{
		uint32_t frameSize = (h.complexityFrame >> 4) == NextOpusFrame_10ms ? 480 : 960;
		uint32_t pos = 0, total = 0;
		if (h.frameCount == 0 || h.samplesPerChannel != h.frameCount * frameSize)
			Violation("audio: Opus frameCount %u spc %u", h.frameCount, h.samplesPerChannel);
		for (uint32_t f = 0; f < h.frameCount; f++)
		{
			if (pos + 2 > bodyLen)
			{
				Violation("audio: Opus frame %u truncated", f);
				break;
			}
			uint32_t len = next_rd16(body + pos);
			pos += 2;
			if (pos + len > bodyLen)
			{
				Violation("audio: Opus frame %u length %u beyond packet", f, len);
				break;
			}
			int r = opus_decode(Dec, body + pos, (opus_int32)len, pcm + 2 * total, 5760, 0);
			if (r != (int)frameSize)
				Violation("audio: opus_decode returned %d (expected %u)", r, frameSize);
			if (r > 0)
				total += (uint32_t)r;
			pos += len;
		}
		if (pos != bodyLen)
			Violation("audio: Opus body %u bytes, frames used %u", bodyLen, pos);
		Level(pcm, total * 2);
		DumpAppend(pcm, total, 1);
		CheckTs(ts, h.samplesPerChannel, 48000, codec);
		AS.samples48 += total;
		AS.adpcmHaveState = false;
	}
	AS.lastCodec = codec;
}

static void* AudioThread(void* arg)
{
	(void)arg;
	int s = Connect(O.audioPort);
	if (s < 0)
	{
		Violation("audio: connect failed");
		return NULL;
	}
	if (!DoHandshake(s, false))
	{
		close(s);
		return NULL;
	}
	AS.lastCodec = -1;
	static uint8_t payload[65536];
	uint64_t start = NowUs(), end = start + (uint64_t)(O.seconds * 1e6), nextFuzz = start;
	unsigned seed = 12345;
	while (atomic_load(&Running) && NowUs() < end)
	{
		double t = (NowUs() - start) / 1e6;
		for (int i = 0; i < O.stepCount; i++)
		{
			CtrlStep* st = &O.steps[i];
			if (!st->sent && t >= st->at)
			{
				uint8_t m[NEXT_CTRL_SIZE];
				NextCtrl_Build(m, st->codec, st->kbpsHalf, st->complexity, st->frame);
				SendAll(s, m, sizeof(m));
				st->sent = true;
				AS.ctrlSent++;
				if (!O.quiet)
					Msg("ctrl @%.1fs: codec=%u kbps=%u complexity=%u frame=%s", t, st->codec, st->kbpsHalf * 2, st->complexity,
						st->frame ? "10ms" : "20ms");
			}
		}
		if (O.fuzzCtrl && NowUs() >= nextFuzz)
		{
			// Random garbage, truncated and misaligned messages, invalid codecs, occasionally a valid switch
			uint8_t junk[97];
			size_t n = 1 + rand_r(&seed) % sizeof(junk);
			for (size_t i = 0; i < n; i++)
				junk[i] = (uint8_t)rand_r(&seed);
			if (rand_r(&seed) % 3 == 0 && n >= 8)
				NextCtrl_Build(junk + rand_r(&seed) % (n - 7), (uint8_t)(rand_r(&seed) % 6), (uint8_t)rand_r(&seed),
					(uint8_t)rand_r(&seed), (uint8_t)(rand_r(&seed) % 3));
			SendAll(s, junk, n);
			AS.ctrlSent++;
			nextFuzz = NowUs() + 20000 + rand_r(&seed) % 80000;
		}

		struct pollfd p = { .fd = s, .events = POLLIN };
		if (poll(&p, 1, 20) <= 0)
			continue;
		uint8_t h[18];
		int r = RecvAll(s, h, sizeof(h), end);
		if (r == 0)
			break;
		if (r < 0)
		{
			Violation("audio: connection lost");
			break;
		}
		uint32_t magic = next_rd32(h), size = next_rd32(h + 4);
		if (magic != NEXT_PACKET_MAGIC || size > sizeof(payload))
		{
			Violation("audio: bad packet header magic=%08x size=%u", magic, size);
			break;
		}
		if (size && RecvAll(s, payload, size, end + 2000000) != 1)
			break;
		HandleAudio(next_rd64(h + 8), h[16], h[17], payload, size);
	}
	close(s);
	return NULL;
}

// ----------------------------------------------------------------------------- main

static int CodecFromName(const char* s)
{
	if (!strcmp(s, "pcm48") || !strcmp(s, "0")) return 0;
	if (!strcmp(s, "pcm24") || !strcmp(s, "1")) return 1;
	if (!strcmp(s, "adpcm") || !strcmp(s, "2")) return 2;
	if (!strcmp(s, "opus") || !strcmp(s, "3")) return 3;
	return atoi(s);
}

static void ParseSteps(const char* spec)
{
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", spec);
	for (char* item = strtok(buf, ","); item && O.stepCount < 32; item = strtok(NULL, ","))
	{
		// T:codec[:kbps[:complexity[:frame_ms]]]
		CtrlStep st = { 0, 0, 48, 5, 0, false };
		char* f[5] = { 0 };
		int n = 0;
		for (char* tok = strsep(&item, ":"); tok && n < 5; tok = strsep(&item, ":"))
			f[n++] = tok;
		if (n < 2)
			continue;
		st.at = atof(f[0]);
		st.codec = (uint8_t)CodecFromName(f[1]);
		if (n > 2) st.kbpsHalf = (uint8_t)(atoi(f[2]) / 2);
		if (n > 3) st.complexity = (uint8_t)atoi(f[3]);
		if (n > 4) st.frame = atoi(f[4]) == 10 ? 1 : 0;
		O.steps[O.stepCount++] = st;
	}
}

static void Usage(void)
{
	printf("sysdvr_hostclient [--host A] [--seconds N] [--legacy] [--codec pcm48|pcm24|adpcm|opus] [--kbps N]\n"
		"  [--complexity N] [--frame 20|10] [--batching N] [--no-diag] [--no-tos] [--no-video] [--no-audio]\n"
		"  [--ctrl T:codec[:kbps[:cx[:frame]]],...] [--fuzz-ctrl] [--dump-wav FILE] [--quiet]\n"
		"  [--expect-gaps-after-slow N] [--expect-max-gaps-after-slow N] [--expect-min-gaps N] [--expect-max-gaps N] [--expect-codecs LIST] [--expect-no-diag]\n");
}

int main(int argc, char** argv)
{
	O.host = "127.0.0.1";
	O.videoPort = 9911;
	O.audioPort = 9922;
	O.seconds = 8;
	O.video = O.audio = O.diag = O.tos = true;
	O.kbpsHalf = 48;
	O.complexity = 5;
	O.batching = 3;
	O.expectGapsAfterSlow = -1;
	O.expectMaxGapsAfterSlow = -1;
	O.expectMinGaps = -1;
	O.expectMaxGaps = -1;

	for (int i = 1; i < argc; i++)
	{
		const char* a = argv[i];
		const char* v = i + 1 < argc ? argv[i + 1] : "";
#define ARG(name) (!strcmp(a, name) && (i++, 1))
		if (ARG("--host")) O.host = v;
		else if (ARG("--video-port")) O.videoPort = atoi(v);
		else if (ARG("--audio-port")) O.audioPort = atoi(v);
		else if (ARG("--seconds")) O.seconds = atof(v);
		else if (!strcmp(a, "--legacy")) O.legacy = true;
		else if (ARG("--codec")) O.codec = (uint8_t)CodecFromName(v);
		else if (ARG("--kbps")) O.kbpsHalf = (uint8_t)(atoi(v) / 2);
		else if (ARG("--complexity")) O.complexity = (uint8_t)atoi(v);
		else if (ARG("--frame")) O.frame = atoi(v) == 10 ? 1 : 0;
		else if (ARG("--batching")) O.batching = (uint8_t)atoi(v);
		else if (!strcmp(a, "--no-diag")) O.diag = false;
		else if (!strcmp(a, "--no-tos")) O.tos = false;
		else if (!strcmp(a, "--no-video")) O.video = false;
		else if (!strcmp(a, "--no-audio")) O.audio = false;
		else if (ARG("--ctrl")) ParseSteps(v);
		else if (!strcmp(a, "--fuzz-ctrl")) O.fuzzCtrl = true;
		else if (ARG("--dump-wav")) O.dumpWav = v;
		else if (!strcmp(a, "--quiet")) O.quiet = true;
		else if (ARG("--expect-gaps-after-slow")) O.expectGapsAfterSlow = atoi(v);
		else if (ARG("--expect-max-gaps-after-slow")) O.expectMaxGapsAfterSlow = atoi(v);
		else if (ARG("--expect-min-gaps")) O.expectMinGaps = atoi(v);
		else if (ARG("--expect-max-gaps")) O.expectMaxGaps = atoi(v);
		else if (ARG("--expect-codecs"))
		{
			char buf[128];
			snprintf(buf, sizeof(buf), "%s", v);
			for (char* t = strtok(buf, ","); t; t = strtok(NULL, ","))
				O.expectCodecs |= 1 << CodecFromName(t);
		}
		else if (!strcmp(a, "--expect-no-diag")) O.expectNoDiag = true;
		else { Usage(); return 2; }
#undef ARG
	}

	int err = 0;
	if (opus_decoder_get_size(2) > (int)sizeof(DecMem))
	{
		fprintf(stderr, "decoder state too big\n");
		return 1;
	}
	NextAudio_Init(&Work);
	Dec = (OpusDecoder*)DecMem;
	opus_decoder_init(Dec, 48000, 2);
	(void)err;

	pthread_t vt, at;
	if (O.video) pthread_create(&vt, NULL, VideoThread, NULL);
	if (O.audio) pthread_create(&at, NULL, AudioThread, NULL);
	if (O.video) pthread_join(vt, NULL);
	if (O.audio) pthread_join(at, NULL);

	printf("\n==== summary (%s, %.1f s) ====\n", O.legacy ? "official protocol" : "NSDVR extension", O.seconds);
	if (O.video)
	{
		printf("video: %llu packets (%.1f fps), %llu replay, %llu error, %.1f KB/s, diag packets %llu\n",
			(unsigned long long)VS.packets, VS.packets / O.seconds, (unsigned long long)VS.replay, (unsigned long long)VS.errors,
			VS.bytes / 1024.0 / O.seconds, (unsigned long long)VS.diag);
		if (VS.diag)
			printf("diag totals: framesSent=%u grcGaps=%u sendsOver20ms=%u gapsAfterSlowSend=%u maxBlock=%uus audioPackets=%u audioEncode=%uus tos=%u idle3=%s cpu(max)=%s\n",
				VS.sum.videoFramesSent, VS.sum.videoGrcGaps, VS.sum.videoSendsOver20ms, VS.sum.gapsAfterSlowSend, VS.maxBlockUs,
				VS.sum.audioPackets, VS.sum.audioEncodeTotalUs, VS.sum.tosFlags, VS.idleAvail ? "yes" : "n/a", VS.cpuAvail ? "yes" : "n/a");
	}
	if (O.audio)
	{
		double rms = AS.sumN ? sqrt(AS.sumSq / AS.sumN) : 0;
		printf("audio: PCM48 %llu, PCM24 %llu, ADPCM %llu, OPUS %llu, official %llu packets; %llu errors; %.2f s decoded; rms %.0f\n",
			(unsigned long long)AS.packets[0], (unsigned long long)AS.packets[1], (unsigned long long)AS.packets[2],
			(unsigned long long)AS.packets[3], (unsigned long long)AS.packets[4], (unsigned long long)AS.errors, AS.samples48 / 48000.0, rms);
		printf("audio: codec switches %llu, control writes %llu, max timestamp jump %lld us, jumps > 25 ms %llu, ADPCM state mismatches %llu, server encode total %llu us\n",
			(unsigned long long)AS.switches, (unsigned long long)AS.ctrlSent, (long long)AS.maxJumpUs, (unsigned long long)AS.jumpsOver25ms,
			(unsigned long long)AS.adpcmResyncMismatch, (unsigned long long)AS.encodeUsTotal);
		if (AS.samples48 > 48000 && rms < 10)
			Violation("audio: decoded audio is silent");
		if (AS.adpcmResyncMismatch)
			Violation("audio: ADPCM packet state does not continue the previous packet");
		if (AS.jumpsOver25ms)
			Violation("audio: %llu timestamp discontinuities", (unsigned long long)AS.jumpsOver25ms);
		for (int c = 0; c < 4; c++)
			if ((O.expectCodecs & (1 << c)) && AS.packets[c] == 0)
				Violation("audio: expected codec %d was never seen", c);
	}
	if (O.video && O.diag && !O.legacy && !O.expectNoDiag && VS.diag + 1 < (uint64_t)O.seconds - 1)
		Violation("video: only %llu diag packets", (unsigned long long)VS.diag);
	if (O.expectNoDiag && VS.diag)
		Violation("video: diag packets present");
	if (O.expectGapsAfterSlow >= 0 && VS.sum.gapsAfterSlowSend < (uint32_t)O.expectGapsAfterSlow)
		Violation("diag: gapsAfterSlowSend %u < %d", VS.sum.gapsAfterSlowSend, O.expectGapsAfterSlow);
	if (O.expectMaxGapsAfterSlow >= 0 && VS.sum.gapsAfterSlowSend > (uint32_t)O.expectMaxGapsAfterSlow)
		Violation("diag: gapsAfterSlowSend %u > %d", VS.sum.gapsAfterSlowSend, O.expectMaxGapsAfterSlow);
	if (O.expectMinGaps >= 0 && VS.sum.videoGrcGaps < (uint32_t)O.expectMinGaps)
		Violation("diag: videoGrcGaps %u < %d", VS.sum.videoGrcGaps, O.expectMinGaps);
	if (O.expectMaxGaps >= 0 && VS.sum.videoGrcGaps > (uint32_t)O.expectMaxGaps)
		Violation("diag: videoGrcGaps %u > %d", VS.sum.videoGrcGaps, O.expectMaxGaps);

	if (O.dumpWav && AS.dumpFrames)
		Wav_Write(O.dumpWav, AS.dump, (uint32_t)AS.dumpFrames, 48000);

	int v = atomic_load(&Violations);
	printf("result: %s (%d violations)\n", v ? "FAIL" : "PASS", v);
	return v ? 1 : 0;
}
