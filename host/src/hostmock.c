// sysdvr_hostmock: a SysDVR sysmodule stand-in for macOS. Speaks TCP Bridge protocol 03 exactly
// like the sysmodule (hello, 16 byte handshake, 72 byte response, packets) plus the NSDVR extension
// (audio codecs, control messages, diagnostics, IP_TOS). The extension code paths are the very same
// C files the sysmodule is built from (sysmodule/source/next/*.c); only the thin platform layer
// (plat_host.c) differs.
#include "hostmock.h"
#include "next_platform.h"
#include "next_session.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

MockOptions g_opt;
H264File g_video;
WavData g_audio;
atomic_bool g_running = true;
static atomic_int ServersDone;

static pthread_mutex_t LogLock = PTHREAD_MUTEX_INITIALIZER;

void Log(const char* fmt, ...)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	localtime_r(&ts.tv_sec, &tm);
	pthread_mutex_lock(&LogLock);
	printf("%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	pthread_mutex_unlock(&LogLock);
}

// ----------------------------------------------------------------------------- handshake

static bool RecvExact(int sock, void* buf, size_t len, int timeoutMs)
{
	size_t got = 0;
	uint64_t deadline = NextPlat_NowUs() + (uint64_t)timeoutMs * 1000u;
	while (got < len && atomic_load(&g_running))
	{
		struct pollfd p = { .fd = sock, .events = POLLIN };
		int left = (int)((deadline - NextPlat_NowUs()) / 1000);
		if (NextPlat_NowUs() >= deadline)
			return false;
		if (poll(&p, 1, left > 200 ? 200 : left) <= 0)
			continue;
		ssize_t r = recv(sock, (char*)buf + got, len - got, 0);
		if (r > 0)
			got += (size_t)r;
		else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			return false;
	}
	return got == len;
}

static const char* CodecName(int c)
{
	static const char* n[] = { "PCM48", "PCM24", "ADPCM", "OPUS" };
	return c >= 0 && c < 4 ? n[c] : "?";
}

// Returns true when streaming should start
static bool Handshake(int sock, bool video, uint8_t req[NEXT_HANDSHAKE_SIZE], int* code)
{
	static const char hello[] = "SysDVR|03"; // 10 bytes with the terminator, like PROTO_HANDSHAKE_HELLO
	if (!NextPlat_SendAll(sock, hello, sizeof(hello), false))
		return false;
	if (!RecvExact(sock, req, NEXT_HANDSHAKE_SIZE, 10000))
	{
		Log("[%s] no handshake received", video ? "video" : "audio");
		return false;
	}

	*code = NextExt_ValidateHandshake(req, NEXT_HANDSHAKE_SIZE, video ? 1 : 2, "03");

	uint8_t resp[72];
	memset(resp, 0, sizeof(resp));
	next_wr32(resp, (uint32_t)*code);
	if (*code == NextHs_Ok && (req[NEXT_HANDSHAKE_OFF_FEATURES] & 2))
	{
		// ExtraFeatureFlags_MemoryDiag: plausible fake memory pool numbers (application/applet/system/system unsafe)
		const uint64_t mb = 1024 * 1024;
		uint64_t pools[8] = { 3285 * mb, 1800 * mb, 507 * mb, 400 * mb, 590 * mb, 586 * mb, 64 * mb, 10 * mb };
		next_wr32(resp + 4, 0);
		for (int i = 0; i < 8; i++)
			next_wr64(resp + 8 + 8 * i, pools[i]);
	}
	else
		next_wr32(resp + 4, 0xFFFFFFFFu);

	if (!NextPlat_SendAll(sock, resp, sizeof(resp), false))
		return false;
	return *code == NextHs_Ok;
}

// ----------------------------------------------------------------------------- official loops

// What the sysmodule does for clients that do not set the extension flag
static void OfficialVideo(int sock)
{
	while (atomic_load(&g_running))
	{
		NextPacketHeader* p = NextPlat_CaptureVideo();
		if (!atomic_load(&g_running))
			break;
		if (!NextPlat_SendAll(sock, p, p->DataSize + NEXT_PACKET_HEADER_SIZE, false))
			break;
	}
}

static void OfficialAudio(int sock)
{
	NextPacketHeader* p = NextPlat_AudioPacket();
	while (atomic_load(&g_running))
	{
		NextPlat_CaptureAudio(0);
		if (!atomic_load(&g_running))
			break;
		// Official packets: raw PCM, ReplaySlot 0xFF, no extension header
		p->ReplaySlot = 0xFF;
		if (!NextPlat_SendAll(sock, p, p->DataSize + NEXT_PACKET_HEADER_SIZE, false))
			break;
	}
}

// ----------------------------------------------------------------------------- servers

typedef struct {
	bool video;
	int port;
} ServerArgs;

static int Listen(const char* addr, int port)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, addr, &a.sin_addr) != 1)
	{
		close(s);
		return -1;
	}
	if (bind(s, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(s, 1) < 0)
	{
		close(s);
		return -1;
	}
	return s;
}

static void* ServerThread(void* arg)
{
	// Keep the streaming threads on performance cores so pacing and encode timings are stable
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	ServerArgs* sa = (ServerArgs*)arg;
	const char* name = sa->video ? "video" : "audio";
	int ls = Listen(g_opt.listenAddr, sa->port);
	if (ls < 0)
	{
		Log("[%s] cannot listen on %s:%d: %s", name, g_opt.listenAddr, sa->port, strerror(errno));
		atomic_store(&g_running, false);
		return NULL;
	}
	Log("[%s] listening on %s:%d", name, g_opt.listenAddr, sa->port);

	while (atomic_load(&g_running))
	{
		struct pollfd p = { .fd = ls, .events = POLLIN };
		if (poll(&p, 1, 200) <= 0)
			continue;
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		int c = accept(ls, (struct sockaddr*)&peer, &plen);
		if (c < 0)
			continue;

		char ip[64];
		inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
		Log("[%s] client %s:%d connected", name, ip, ntohs(peer.sin_port));

		int one = 1;
		setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
		if (g_opt.sndBuf > 0)
			setsockopt(c, SOL_SOCKET, SO_SNDBUF, &g_opt.sndBuf, sizeof(g_opt.sndBuf));
		fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK); // like the sysmodule's sockets

		uint8_t req[NEXT_HANDSHAKE_SIZE];
		int code = 0;
		if (Handshake(c, sa->video, req, &code))
		{
			NextExtConfig ext = NextExt_ParseHandshake(req, sizeof(req));
			if (ext.enabled)
				Log("[%s] handshake OK meta=%#x vflags=%#x batching=%u features=%#x, NSDVR ext: codec=%s opus=%ukbps/c%u/%s diag=%d tos=%d",
					name, req[6], req[7], req[8], req[9], CodecName(ext.audio.codec), ext.audio.opusKbps, ext.audio.complexity,
					ext.audio.frameCode ? "10ms" : "20ms", ext.diag, ext.tos);
			else
				Log("[%s] handshake OK meta=%#x vflags=%#x batching=%u features=%#x (official protocol, no extension)",
					name, req[6], req[7], req[8], req[9]);

			usleep(500000); // the sysmodule also gives the client 500 ms

			// The virtual grc starts now (a real console would have queued frames meanwhile and
			// drop all but the newest few: that would show up as one gap in the first diag window)
			if (sa->video)
				Plat_VideoConnected(c, req[7]);
			else
				Plat_AudioConnected(c, req[8]);

			if (ext.enabled)
				sa->video ? NextSession_Video(c, &ext) : NextSession_Audio(c, &ext);
			else
				sa->video ? OfficialVideo(c) : OfficialAudio(c);

			if (sa->video)
				Plat_VideoDisconnected();
			else
				Plat_AudioDisconnected();
			Log("[%s] connection closed", name);
		}
		else
			Log("[%s] handshake rejected with code %d", name, code);

		close(c);
		if (g_opt.once)
			break;
	}
	close(ls);
	atomic_fetch_add(&ServersDone, 1);
	return NULL;
}

static void* BeaconThread(void* arg)
{
	(void)arg;
	char msg[128];
	int len = snprintf(msg, sizeof(msg), "SysDVR|6.3|03|%s", g_opt.serial) + 1; // including the terminator
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	while (atomic_load(&g_running))
	{
		char targets[256];
		snprintf(targets, sizeof(targets), "%s", g_opt.beaconTargets);
		for (char* t = strtok(targets, ","); t; t = strtok(NULL, ","))
		{
			struct sockaddr_in a;
			memset(&a, 0, sizeof(a));
			a.sin_family = AF_INET;
			a.sin_port = htons(19999);
			if (inet_pton(AF_INET, t, &a.sin_addr) == 1)
				sendto(s, msg, (size_t)len, 0, (struct sockaddr*)&a, sizeof(a));
		}
		for (int i = 0; i < 20 && atomic_load(&g_running); i++)
			usleep(100000);
	}
	close(s);
	return NULL;
}

static void OnSignal(int sig)
{
	(void)sig;
	atomic_store(&g_running, false);
}

static void Usage(void)
{
	printf(
		"sysdvr_hostmock: SysDVR TCP Bridge (protocol 03) + NSDVR extension mock server\n\n"
		"  --h264 FILE           H.264 Annex-B file, looped at --fps (default: synthetic, not decodable)\n"
		"  --wav FILE            48 kHz 16-bit WAV, looped in real time (default: synthetic tones)\n"
		"  --listen ADDR         listen address (default 0.0.0.0)\n"
		"  --video-port N        default 9911\n"
		"  --audio-port N        default 9922\n"
		"  --fps F               video frame rate (default 30)\n"
		"  --stall-every SEC     block a video send every SEC seconds ...\n"
		"  --stall-ms MS         ... for MS milliseconds (default 300)\n"
		"  --stall-audio-every SEC / --stall-audio-ms MS   same for audio sends\n"
		"  --drop-every N        drop every Nth non-IDR frame at the source (a grc gap without a slow send)\n"
		"  --grc-queue N         frames the virtual grc keeps queued when the reader is late (default 3)\n"
		"  --sndbuf BYTES        SO_SNDBUF for client sockets (default: OS default)\n"
		"  --no-opus             behave as if the Opus encoder memory were missing (Opus falls back to PCM48)\n"
		"  --no-beacon           do not send UDP 19999 beacons\n"
		"  --beacon-targets LIST comma separated (default 255.255.255.255,127.0.0.1)\n"
		"  --serial S            serial in the beacon (default XAW00000000000)\n"
		"  --once                serve one connection per port, then exit\n"
		"  --duration SEC        exit after SEC seconds\n"
		"  --quiet-diag          do not print every diagnostics packet\n"
		"  --verbose\n");
}

int main(int argc, char** argv)
{
	g_opt.listenAddr = "0.0.0.0";
	g_opt.videoPort = 9911;
	g_opt.audioPort = 9922;
	g_opt.fps = 30;
	g_opt.stallMs = 300;
	g_opt.stallAudioMs = 300;
	g_opt.grcQueue = 3;
	g_opt.beacon = true;
	g_opt.beaconTargets = "255.255.255.255,127.0.0.1";
	g_opt.serial = "XAW00000000000";

	enum { O_H264 = 256, O_WAV, O_LISTEN, O_VPORT, O_APORT, O_FPS, O_STALL_EVERY, O_STALL_MS, O_ASTALL_EVERY, O_ASTALL_MS,
		O_DROP, O_QUEUE, O_SNDBUF, O_NOOPUS, O_NOBEACON, O_TARGETS, O_SERIAL, O_ONCE, O_DURATION, O_QUIETDIAG, O_VERBOSE, O_HELP };
	static const struct option opts[] = {
		{ "h264", required_argument, 0, O_H264 }, { "wav", required_argument, 0, O_WAV },
		{ "listen", required_argument, 0, O_LISTEN }, { "video-port", required_argument, 0, O_VPORT },
		{ "audio-port", required_argument, 0, O_APORT }, { "fps", required_argument, 0, O_FPS },
		{ "stall-every", required_argument, 0, O_STALL_EVERY }, { "stall-ms", required_argument, 0, O_STALL_MS },
		{ "stall-audio-every", required_argument, 0, O_ASTALL_EVERY }, { "stall-audio-ms", required_argument, 0, O_ASTALL_MS },
		{ "drop-every", required_argument, 0, O_DROP }, { "grc-queue", required_argument, 0, O_QUEUE },
		{ "sndbuf", required_argument, 0, O_SNDBUF }, { "no-opus", no_argument, 0, O_NOOPUS },
		{ "no-beacon", no_argument, 0, O_NOBEACON }, { "beacon-targets", required_argument, 0, O_TARGETS },
		{ "serial", required_argument, 0, O_SERIAL }, { "once", no_argument, 0, O_ONCE },
		{ "duration", required_argument, 0, O_DURATION }, { "quiet-diag", no_argument, 0, O_QUIETDIAG },
		{ "verbose", no_argument, 0, O_VERBOSE }, { "help", no_argument, 0, O_HELP }, { 0, 0, 0, 0 },
	};

	int ch;
	while ((ch = getopt_long(argc, argv, "h", opts, NULL)) != -1)
	{
		switch (ch)
		{
		case O_H264: g_opt.h264Path = optarg; break;
		case O_WAV: g_opt.wavPath = optarg; break;
		case O_LISTEN: g_opt.listenAddr = optarg; break;
		case O_VPORT: g_opt.videoPort = atoi(optarg); break;
		case O_APORT: g_opt.audioPort = atoi(optarg); break;
		case O_FPS: g_opt.fps = atof(optarg); break;
		case O_STALL_EVERY: g_opt.stallEverySec = atof(optarg); break;
		case O_STALL_MS: g_opt.stallMs = atoi(optarg); break;
		case O_ASTALL_EVERY: g_opt.stallAudioEverySec = atof(optarg); break;
		case O_ASTALL_MS: g_opt.stallAudioMs = atoi(optarg); break;
		case O_DROP: g_opt.dropEvery = atoi(optarg); break;
		case O_QUEUE: g_opt.grcQueue = atoi(optarg); break;
		case O_SNDBUF: g_opt.sndBuf = atoi(optarg); break;
		case O_NOOPUS: g_opt.noOpus = true; break;
		case O_NOBEACON: g_opt.beacon = false; break;
		case O_TARGETS: g_opt.beaconTargets = optarg; break;
		case O_SERIAL: g_opt.serial = optarg; break;
		case O_ONCE: g_opt.once = true; break;
		case O_DURATION: g_opt.durationSec = atof(optarg); break;
		case O_QUIETDIAG: g_opt.quietDiag = true; break;
		case O_VERBOSE: g_opt.verbose = true; break;
		default: Usage(); return ch == O_HELP || ch == 'h' ? 0 : 2;
		}
	}
	if (g_opt.fps <= 0 || g_opt.fps > 240)
		g_opt.fps = 30;
	if (g_opt.grcQueue < 0)
		g_opt.grcQueue = 0;
	if (g_opt.stallMs < 0)
		g_opt.stallMs = 0;

	char err[256];
	if (g_opt.h264Path)
	{
		if (!H264_Load(g_opt.h264Path, &g_video, err, sizeof(err)))
		{
			fprintf(stderr, "%s\n", err);
			return 1;
		}
		Log("video: %s, %u frames (%u IDR), SPS/PPS for injection: %s", g_opt.h264Path, g_video.count, g_video.idrCount,
			g_video.spsPpsSize ? "from file" : "Switch defaults");
	}
	else
	{
		H264_Synthetic(&g_video);
		Log("video: synthetic frames (not decodable), use --h264 for real video");
	}

	if (g_opt.wavPath)
	{
		if (!Wav_Load(g_opt.wavPath, &g_audio, err, sizeof(err)))
		{
			fprintf(stderr, "%s\n", err);
			return 1;
		}
		if (g_audio.rate != 48000)
			Log("warning: %s is %u Hz, it is sent as if it were 48 kHz", g_opt.wavPath, g_audio.rate);
		Log("audio: %s, %.1f s", g_opt.wavPath, g_audio.frames / 48000.0);
	}
	else
	{
		Wav_Synthetic(&g_audio, 48000 * 4);
		Log("audio: synthetic tones, use --wav for music");
	}
	if (g_audio.frames == 0)
	{
		fprintf(stderr, "empty audio\n");
		return 1;
	}

	signal(SIGINT, OnSignal);
	signal(SIGTERM, OnSignal);
	signal(SIGPIPE, SIG_IGN);
	Plat_Init();

	if (g_opt.stallEverySec > 0)
		Log("stall injection: video sends block for %d ms every %.1f s (virtual grc keeps %d frames)", g_opt.stallMs,
			g_opt.stallEverySec, g_opt.grcQueue);
	if (g_opt.dropEvery > 0)
		Log("drop injection: every %d-th non-IDR frame is dropped at the source", g_opt.dropEvery);

	pthread_t vt, at, bt;
	ServerArgs va = { true, g_opt.videoPort }, aa = { false, g_opt.audioPort };
	pthread_create(&vt, NULL, ServerThread, &va);
	pthread_create(&at, NULL, ServerThread, &aa);
	if (g_opt.beacon)
		pthread_create(&bt, NULL, BeaconThread, NULL);

	uint64_t start = NextPlat_NowUs();
	while (atomic_load(&g_running))
	{
		usleep(100000);
		if (g_opt.durationSec > 0 && NextPlat_NowUs() - start >= (uint64_t)(g_opt.durationSec * 1e6))
			atomic_store(&g_running, false);
		if (atomic_load(&ServersDone) == 2)
			atomic_store(&g_running, false);
	}

	pthread_join(vt, NULL);
	pthread_join(at, NULL);
	if (g_opt.beacon)
		pthread_join(bt, NULL);
	Log("bye");
	H264_Free(&g_video);
	Wav_Free(&g_audio);
	return 0;
}
