// NextPlat_* for the macOS host mock: a file backed "virtual grc" (30 fps H.264, real time
// 1024-sample audio blocks), POSIX sockets with the same send/poll semantics as the sysmodule.
#include "hostmock.h"
#include "next_platform.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <mach/mach.h>

#define VBUF_SZ 0x54000
#define ABUF_SZ 0x1000
#define MAX_ABATCHING 5

// Same layout as the sysmodule: 18 byte header followed by the data (so data is only 2-byte aligned)
typedef struct __attribute__((packed)) {
	NextPacketHeader h;
	uint8_t data[VBUF_SZ];
} HostVideoPacket;

typedef struct __attribute__((packed)) {
	NextPacketHeader h;
	uint8_t data[ABUF_SZ * (1 + MAX_ABATCHING) + NEXT_AUDIO_HEADROOM];
} HostAudioPacket;

static HostVideoPacket VPkt __attribute__((aligned(4096)));
static HostAudioPacket APkt __attribute__((aligned(4096)));
static NextAudioWork AudioWork;

static const uint8_t SwitchSPS[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x0C, 0x20, 0xAC, 0x2B, 0x40, 0x28, 0x02, 0xDD, 0x35, 0x01, 0x0D, 0x01, 0xE0, 0x80 };
static const uint8_t SwitchPPS[] = { 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0 };

uint64_t NextPlat_NowUs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void SleepUntil(uint64_t targetUs)
{
	while (atomic_load(&g_running))
	{
		uint64_t now = NextPlat_NowUs();
		if (now >= targetUs)
			return;
		uint64_t d = targetUs - now;
		if (d > 100000)
			d = 100000;
		usleep((useconds_t)d);
	}
}

// ----------------------------------------------------------------------------- video (virtual grc)

static struct {
	int sock;
	bool hash, hashOnlyIdr, inject;
	uint64_t t0;          // time of frame 0
	uint64_t tsBase;
	uint64_t next;        // next frame index the reader gets
	uint64_t fileIndex;
	uint64_t dropped;     // dropped because the reader was late
	uint64_t forcedDrops; // --drop-every
	uint32_t hashes[64];
	bool forceMeta;
	int idrCount;
	uint64_t nextStallUs;
	uint64_t stalls;
} VG;

void Plat_VideoConnected(int sock, uint8_t flags)
{
	memset(&VG, 0, sizeof(VG));
	VG.sock = sock;
	VG.hash = flags & 1;
	VG.inject = flags & 2;
	VG.hashOnlyIdr = flags & 4;
	VG.t0 = NextPlat_NowUs();
	VG.tsBase = VG.t0;
	VG.forceMeta = true;
	if (g_opt.stallEverySec > 0)
		VG.nextStallUs = VG.t0 + (uint64_t)(g_opt.stallEverySec * 1e6);
}

void Plat_VideoDisconnected(void)
{
	if (VG.dropped || VG.forcedDrops || VG.stalls)
		Log("[video] virtual grc: %llu frames lost while the reader was late, %llu dropped by --drop-every, %llu stalls injected",
			(unsigned long long)VG.dropped, (unsigned long long)VG.forcedDrops, (unsigned long long)VG.stalls);
	VG.sock = -1;
}

uint64_t Plat_VideoDropped(void) { return VG.dropped; }

NextPacketHeader* NextPlat_CaptureVideo(void)
{
	const uint64_t period = (uint64_t)(1e6 / (g_opt.fps > 0 ? g_opt.fps : 30));
	const H264Frame* f = NULL;
	uint64_t n = 0;

	for (;;)
	{
		uint64_t now = NextPlat_NowUs();
		uint64_t avail = now >= VG.t0 ? (now - VG.t0) / period : 0; // newest frame index already produced
		if (VG.next > avail)
		{
			SleepUntil(VG.t0 + VG.next * period);
			if (!atomic_load(&g_running))
				break;
		}
		else if (avail - VG.next > (uint64_t)g_opt.grcQueue)
		{
			// The reader fell behind: like grc, only the newest few frames are still queued
			uint64_t skip = avail - (uint64_t)g_opt.grcQueue - VG.next;
			VG.dropped += skip;
			VG.fileIndex += skip;
			VG.next += skip;
		}

		n = VG.next++;
		f = &g_video.frames[VG.fileIndex++ % g_video.count];
		if (g_opt.dropEvery > 0 && !f->idr && n > 0 && n % (uint64_t)g_opt.dropEvery == 0)
		{
			VG.forcedDrops++;
			continue;
		}
		break;
	}

	NextPacketHeader* h = &VPkt.h;
	h->Magic = NEXT_PACKET_MAGIC;
	h->Timestamp = VG.tsBase + n * period;
	h->ReplaySlot = 0xFF;
	h->MetaData = NEXT_META_VIDEO | NEXT_META_DATA;

	if (!f || f->size > VBUF_SZ)
	{
		// Mirrors a failed grc read (ErrorPacket, ERROR_TYPE_VIDEO_CAP)
		uint8_t* e = VPkt.data;
		memset(e, 0, 32);
		next_wr32(e, 1);
		next_wr32(e + 4, 0xCD4);
		next_wr64(e + 8, f ? f->size : 0);
		h->MetaData = NEXT_META_VIDEO | NEXT_META_ERROR;
		h->DataSize = 32;
		return h;
	}

	if (VG.hash && (f->idr || !VG.hashOnlyIdr))
	{
		uint32_t crc = Crc32(f->data, f->size);
		uint8_t slot = crc & 63;
		h->ReplaySlot = slot;
		if (VG.hashes[slot] == crc)
		{
			h->MetaData = NEXT_META_VIDEO | NEXT_META_REPLAY;
			h->DataSize = 0;
			return h;
		}
		VG.hashes[slot] = crc;
	}

	uint32_t size = f->size;
	memcpy(VPkt.data, f->data, size);
	if (VG.inject)
	{
		if (f->idr)
			VG.idrCount++;
		const uint8_t* ps = g_video.spsPpsSize ? g_video.spsPps : NULL;
		uint8_t sw[sizeof(SwitchSPS) + sizeof(SwitchPPS)];
		uint32_t psLen = g_video.spsPpsSize;
		if (!ps)
		{
			memcpy(sw, SwitchSPS, sizeof(SwitchSPS));
			memcpy(sw + sizeof(SwitchSPS), SwitchPPS, sizeof(SwitchPPS));
			ps = sw;
			psLen = sizeof(sw);
		}
		bool emit = VG.forceMeta || (f->idr && VG.idrCount >= 5);
		if (emit && VBUF_SZ - size >= psLen)
		{
			VG.idrCount = 0;
			VG.forceMeta = false;
			memmove(VPkt.data + psLen, VPkt.data, size);
			memcpy(VPkt.data, ps, psLen);
			size += psLen;
			h->MetaData |= NEXT_META_MULTINAL;
		}
	}
	h->DataSize = size;
	return h;
}

// ----------------------------------------------------------------------------- audio (virtual grc)

static struct {
	int sock;
	int batching;
	uint64_t t0, tsBase;
	uint64_t block;      // next grc block index
	uint64_t wavPos;     // frame position in the WAV (loops)
	uint64_t nextStallUs;
	uint32_t lastKey;
} AG;

void Plat_AudioConnected(int sock, uint8_t batching)
{
	memset(&AG, 0, sizeof(AG));
	AG.sock = sock;
	AG.batching = batching > MAX_ABATCHING ? MAX_ABATCHING : batching;
	AG.t0 = NextPlat_NowUs();
	AG.tsBase = AG.t0;
	if (g_opt.stallAudioEverySec > 0)
		AG.nextStallUs = AG.t0 + (uint64_t)(g_opt.stallAudioEverySec * 1e6);
	APkt.h.Magic = NEXT_PACKET_MAGIC;
	APkt.h.MetaData = NEXT_META_AUDIO | NEXT_META_DATA;
	APkt.h.ReplaySlot = 0xFF;
}

void Plat_AudioDisconnected(void)
{
	AG.sock = -1;
}

NextPacketHeader* NextPlat_AudioPacket(void) { return &APkt.h; }
uint32_t NextPlat_AudioCapacity(void) { return sizeof(APkt.data); }
NextAudioWork* NextPlat_AudioWork(void) { return g_opt.noOpus ? NULL : &AudioWork; }

static uint64_t BlockTimeUs(uint64_t block)
{
	return (block * 1024u * 1000000u + 24000u) / 48000u;
}

bool NextPlat_CaptureAudio(uint32_t offset)
{
	const uint32_t blocks = 1 + (uint32_t)AG.batching;
	if ((uint64_t)offset + (uint64_t)blocks * ABUF_SZ > sizeof(APkt.data))
	{
		// Same shape as the sysmodule's defensive check: an error packet instead of an overflow
		memset(APkt.data, 0, 32);
		next_wr32(APkt.data, 2);
		APkt.h.MetaData = NEXT_META_AUDIO | NEXT_META_ERROR;
		APkt.h.DataSize = 32;
		return false;
	}

	uint64_t first = AG.block;
	for (uint32_t b = 0; b < blocks; b++)
	{
		// Block k is complete once its last sample has been "played"
		SleepUntil(AG.t0 + BlockTimeUs(AG.block + 1));
		uint8_t* dst = APkt.data + offset + b * ABUF_SZ;
		for (uint32_t i = 0; i < 1024; i++)
		{
			const int16_t* s = &g_audio.samples[2 * (AG.wavPos % g_audio.frames)];
			memcpy(dst + 4 * i, s, 4);
			AG.wavPos++;
		}
		AG.block++;
	}

	APkt.h.Magic = NEXT_PACKET_MAGIC;
	APkt.h.DataSize = blocks * ABUF_SZ;
	APkt.h.Timestamp = AG.tsBase + BlockTimeUs(first);
	return true;
}

// ----------------------------------------------------------------------------- sockets

static void LogAudioPacket(const NextPacketHeader* h, const uint8_t* payload)
{
	static const char* names[] = { "PCM48", "PCM24", "ADPCM", "OPUS" };
	uint8_t slot = h->ReplaySlot;
	NextExtAudioHeader eh;
	memset(&eh, 0, sizeof(eh));
	bool hasHdr = slot > NEXT_AUDIO_SLOT(NextCodec_PCM48) && slot <= NEXT_AUDIO_SLOT(NextCodec_OPUS) &&
		NextExt_ReadAudioHeader(payload, h->DataSize, &eh);
	uint32_t key = slot | ((uint32_t)eh.bitrateKbps << 8) | ((uint32_t)eh.complexityFrame << 24);
	if (key == AG.lastKey)
		return;
	AG.lastKey = key;
	if (slot < NEXT_AUDIO_SLOT(0) || slot > NEXT_AUDIO_SLOT(3))
		Log("[audio] sending official PCM packets (slot 0x%02X)", slot);
	else if (hasHdr && eh.codec == NextCodec_OPUS)
		Log("[audio] now sending OPUS %u kbps, complexity %u, %s frames", eh.bitrateKbps, eh.complexityFrame & 15,
			(eh.complexityFrame >> 4) == 1 ? "10 ms" : "20 ms");
	else
		Log("[audio] now sending %s (slot 0x%02X)", names[slot & 3], slot);
}

static void LogDiag(const uint8_t* payload)
{
	NextDiagReport r;
	if (g_opt.quietDiag || !NextExt_ReadDiag(payload, NEXT_DIAG_SIZE, &r))
		return;
	char idle[16], cpu[16];
	if (r.core3IdlePermille == NEXT_DIAG_UNAVAILABLE) snprintf(idle, sizeof idle, "n/a"); else snprintf(idle, sizeof idle, "%u", r.core3IdlePermille);
	if (r.sysdvrCpuPermille == NEXT_DIAG_UNAVAILABLE) snprintf(cpu, sizeof cpu, "n/a"); else snprintf(cpu, sizeof cpu, "%u", r.sysdvrCpuPermille);
	Log("[diag] %ums frames=%u gaps=%u block=%uus max=%uus >20ms=%u gapsAfterSlow=%u | audio pk=%u enc=%uus send=%uus | idle3=%s cpu=%s tos=%u",
		r.intervalMs, r.videoFramesSent, r.videoGrcGaps, r.videoSendBlockTotalUs, r.videoSendBlockMaxUs, r.videoSendsOver20ms,
		r.gapsAfterSlowSend, r.audioPackets, r.audioEncodeTotalUs, r.audioSendBlockTotalUs, idle, cpu, r.tosFlags);
}

bool NextPlat_SendAll(int sock, const void* buf, uint32_t size, bool allowIncoming)
{
	if (sock < 0)
		return false;

	const NextPacketHeader* h = (const NextPacketHeader*)buf;
	if (size >= NEXT_PACKET_HEADER_SIZE && h->Magic == NEXT_PACKET_MAGIC)
	{
		uint8_t type = h->MetaData & NEXT_META_TYPE_MASK;
		uint64_t now = NextPlat_NowUs();
		if (type == NEXT_META_VIDEO && sock == VG.sock && VG.nextStallUs && now >= VG.nextStallUs)
		{
			// Artificial "send blocked" (e.g. Wi-Fi retries): inside the timed send, so it shows up in
			// videoSendBlock*; the virtual grc keeps producing frames meanwhile and drops the old ones.
			VG.nextStallUs = now + (uint64_t)(g_opt.stallEverySec * 1e6);
			VG.stalls++;
			if (g_opt.verbose)
				Log("[video] injecting a %d ms send stall", g_opt.stallMs);
			usleep((useconds_t)g_opt.stallMs * 1000);
		}
		else if (type == NEXT_META_AUDIO && sock == AG.sock && AG.nextStallUs && now >= AG.nextStallUs)
		{
			AG.nextStallUs = now + (uint64_t)(g_opt.stallAudioEverySec * 1e6);
			usleep((useconds_t)g_opt.stallAudioMs * 1000);
		}
		if (type == NEXT_META_AUDIO && sock == AG.sock && !(h->MetaData & NEXT_META_ERROR))
			LogAudioPacket(h, (const uint8_t*)buf + NEXT_PACKET_HEADER_SIZE);
		if (type == NEXT_META_TYPE_DIAG && size >= NEXT_PACKET_HEADER_SIZE + NEXT_DIAG_SIZE)
			LogDiag((const uint8_t*)buf + NEXT_PACKET_HEADER_SIZE);
	}

	uint32_t sent = 0;
	while (sent < size)
	{
		ssize_t r = send(sock, (const char*)buf + sent, size - sent, 0);
		if (r < 0)
		{
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				return false;

			// Same policy as the sysmodule's SocketSendAll: poll 1 s at a time, give up after 10 s,
			// and (unless allowIncoming) treat incoming data as a disconnection.
			int tries = 0;
			for (;;)
			{
				struct pollfd p = { .fd = sock, .events = (short)(allowIncoming ? POLLOUT : (POLLOUT | POLLIN)) };
				int pr = poll(&p, 1, 1000);
				if (!atomic_load(&g_running))
					return false;
				if (++tries >= 10)
					return false;
				if (pr > 0)
				{
					if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
						return false;
					if (!allowIncoming && (p.revents & POLLIN))
						return false;
					if (p.revents & POLLOUT)
						break;
				}
			}
			continue;
		}
		if (r == 0)
			return false;
		sent += (uint32_t)r;
	}
	return true;
}

int NextPlat_RecvNonBlocking(int sock, void* buf, uint32_t size)
{
	ssize_t r = recv(sock, buf, size, MSG_DONTWAIT);
	if (r > 0)
		return (int)r;
	if (r == 0)
		return -1; // orderly shutdown by the peer
	return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
}

bool NextPlat_SetTos(int sock, int tos)
{
	int v = tos;
	return setsockopt(sock, IPPROTO_IP, IP_TOS, &v, sizeof(v)) == 0;
}

bool NextPlat_Running(void)
{
	return atomic_load(&g_running);
}

void NextPlat_Yield(void)
{
	sched_yield();
}

void NextPlat_AudioThreadStarted(void)
{
}

void NextPlat_CpuSample(NextCpuSample* s)
{
	memset(s, 0, sizeof(*s));
	s->wallTicks = NextPlat_NowUs();

	// "SysDVR threads": the whole mock process (all its threads), in microseconds
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) == 0)
	{
		s->busyTicks = (uint64_t)ru.ru_utime.tv_sec * 1000000u + (uint64_t)ru.ru_utime.tv_usec +
			(uint64_t)ru.ru_stime.tv_sec * 1000000u + (uint64_t)ru.ru_stime.tv_usec;
		s->busyValid = 1;
	}

	// "Core 3 idle": the idle share of the Mac's CPU #3 (mach ticks)
	natural_t cpus = 0;
	processor_info_array_t info = NULL;
	mach_msg_type_number_t infoCount = 0;
	if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &cpus, &info, &infoCount) == KERN_SUCCESS)
	{
		if (cpus > 3)
		{
			processor_cpu_load_info_t load = (processor_cpu_load_info_t)info;
			uint64_t total = 0;
			for (int st = 0; st < CPU_STATE_MAX; st++)
				total += load[3].cpu_ticks[st];
			s->idleTicks = load[3].cpu_ticks[CPU_STATE_IDLE];
			s->idleTotalTicks = total;
			s->idleValid = 1;
		}
		vm_deallocate(mach_task_self(), (vm_address_t)info, (vm_size_t)infoCount * sizeof(integer_t));
	}
}

void Plat_Init(void)
{
	VG.sock = AG.sock = -1;
	Crc32((const uint8_t*)"", 0); // build the table before the threads start
}
