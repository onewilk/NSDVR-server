#if PERFLOG

#include <string.h>
#include <stdalign.h>
#include <stdatomic.h>
#include "perflog.h"

#define PERF_DIR "/config/sysdvr/perf"
// Write the buffered lines to the SD card every few seconds (and whenever the buffer fills up)
#define FLUSH_EVERY_S 5
// Query the foreground title this often
#define TITLE_EVERY_S 5
// Stop writing once the file gets this big (about four days at one line per second)
#define MAX_FILE_SIZE (48u * 1024 * 1024)

// Samplers for cores 0-2 run at the highest priority SysDVR may use so they read the idle counter on
// time; they wake once per second for a few microseconds. The writer runs on core 3 below SysDVR's
// streaming threads (0x2C) so formatting and SD writes never delay a frame.
#define SAMPLER_PRIO 24
#define WRITER_PRIO 0x30

static u8 alignas(0x1000) SamplerStacks[3][0x1000];
static u8 alignas(0x1000) WriterStack[0x2000];
static Thread Samplers[3];
static Thread Writer;

static FsFileSystem* Sd;
static atomic_uint_fast64_t SentBytes;
// Busy permille of cores 0-2 for the window that just ended, -1 = unavailable
static atomic_int CoreBusy[3] = { -1, -1, -1 };

__attribute__((weak)) void PerfLog_AudioTag(char* out, u32 cap)
{
	if (cap)
		out[0] = 0;
}

__attribute__((weak)) void PerfLog_TakeAudioEncode(u32* encodeUs, u32* packets)
{
	*encodeUs = 0;
	*packets = 0;
}

void PerfLog_AddSent(u32 bytes)
{
	atomic_fetch_add(&SentBytes, bytes);
}

// ----------------------------------------------------------------------------- formatting without printf

static char* PutU64(char* p, u64 v)
{
	char tmp[20];
	int n = 0;
	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n)
		*p++ = tmp[--n];
	return p;
}

static char* PutI64(char* p, s64 v)
{
	if (v < 0)
	{
		*p++ = '-';
		return PutU64(p, (u64)(-v));
	}
	return PutU64(p, (u64)v);
}

static char* PutHex16(char* p, u64 v)
{
	static const char digits[] = "0123456789abcdef";
	for (int i = 15; i >= 0; --i)
		*p++ = digits[(v >> (i * 4)) & 0xF];
	return p;
}

static char* PutStr(char* p, const char* s)
{
	while (*s)
		*p++ = *s++;
	return p;
}

// ----------------------------------------------------------------------------- CPU counters

static bool IdleTicks(u64* out)
{
	// Only valid for the calling thread's core, which is why every core has its own sampler thread
	return R_SUCCEEDED(svcGetInfo(out, InfoType_IdleTickCount, INVALID_HANDLE, UINT64_MAX));
}

static bool ThreadTicks(Handle h, u64* out)
{
	if (h == INVALID_HANDLE)
		return false;
	// [13.0.0+] InfoType_ThreadTickCount, older firmware only has the deprecated id
	if (R_SUCCEEDED(svcGetInfo(out, InfoType_ThreadTickCount, h, UINT64_MAX)))
		return true;
	return R_SUCCEEDED(svcGetInfo(out, InfoType_ThreadTickCountDeprecated, h, UINT64_MAX));
}

static int Permille(u64 num, u64 den)
{
	if (den == 0)
		return -1;
	u64 p = (num * 1000 + den / 2) / den;
	return p > 1000 ? 1000 : (int)p;
}

// Sleeps until the next whole period after `start`, never drifting; returns the tick it woke for
static u64 SleepUntilNext(u64* next, u64 period)
{
	u64 now = armGetSystemTick();
	if (now + period * 2 < *next || now > *next + period * 2)
		*next = now + period; // the clock jumped or we were starved: start over
	if (*next > now)
		svcSleepThread(armTicksToNs(*next - now));
	u64 target = *next;
	*next += period;
	return target;
}

static void SamplerMain(void* arg)
{
	int core = (int)(uintptr_t)arg;
	u64 period = armGetSystemTickFreq();
	u64 next = armGetSystemTick() + period;
	u64 lastIdle = 0, lastWall = 0;
	bool have = false;
	while (true)
	{
		SleepUntilNext(&next, period);
		u64 idle;
		u64 wall = armGetSystemTick();
		if (!IdleTicks(&idle))
		{
			atomic_store(&CoreBusy[core], -1);
			have = false;
			continue;
		}
		if (have && wall > lastWall && idle >= lastIdle)
		{
			int idlePermille = Permille(idle - lastIdle, wall - lastWall);
			atomic_store(&CoreBusy[core], idlePermille < 0 ? -1 : 1000 - idlePermille);
		}
		lastIdle = idle;
		lastWall = wall;
		have = true;
	}
}

// CPU time of one thread between two samples; the handle may change (thread restarted) or vanish
typedef struct {
	Handle handle;
	u64 last;
	bool have;
} ThreadSlot;

static u64 ThreadDelta(ThreadSlot* s, Handle h)
{
	u64 now;
	if (h == INVALID_HANDLE || !ThreadTicks(h, &now))
	{
		s->have = false;
		return 0;
	}
	u64 delta = (s->have && s->handle == h && now >= s->last) ? now - s->last : 0;
	s->handle = h;
	s->last = now;
	s->have = true;
	return delta;
}

// ----------------------------------------------------------------------------- SD card output

static FsFile File;
static bool FileOpen;
static u64 FileOffset;
static char Pending[4096];
static u32 PendingLen;

static void Flush(void)
{
	if (!FileOpen || PendingLen == 0)
		return;
	if (FileOffset + PendingLen > MAX_FILE_SIZE ||
		R_FAILED(fsFileWrite(&File, FileOffset, Pending, PendingLen, FsWriteOption_Flush)))
	{
		// SD card full, removed or the size cap was reached: stop logging, keep streaming
		fsFileClose(&File);
		FileOpen = false;
		PendingLen = 0;
		return;
	}
	FileOffset += PendingLen;
	PendingLen = 0;
}

static void Append(const char* line, u32 len)
{
	if (!FileOpen)
		return;
	if (PendingLen + len > sizeof(Pending))
		Flush();
	if (len > sizeof(Pending))
		return;
	memcpy(Pending + PendingLen, line, len);
	PendingLen += len;
}

static void OpenLogFile(u64 unix)
{
	// The folders may already exist: ignore those errors
	fsFsCreateDirectory(Sd, "/config");
	fsFsCreateDirectory(Sd, "/config/sysdvr");
	fsFsCreateDirectory(Sd, PERF_DIR);

	char path[96];
	char* p = PutStr(path, PERF_DIR "/perf_");
	if (unix)
		p = PutU64(p, unix);
	else
	{
		p = PutStr(p, "tick");
		p = PutU64(p, armGetSystemTick());
	}
	p = PutStr(p, ".csv");
	*p = 0;

	fsFsCreateFile(Sd, path, 0, 0);
	if (R_FAILED(fsFsOpenFile(Sd, path, FsOpenMode_Write | FsOpenMode_Append, &File)))
		return;
	s64 size = 0;
	fsFileGetSize(&File, &size);
	FileOpen = true;
	FileOffset = (u64)size;

	static const char header[] =
		"# SysDVR perf log. cpu/thread columns are permille of one core; c0-c3 = busy permille of each core\n"
		"unix,uptime_s,title_id,c0,c1,c2,c3,sysdvr_cpu,video_cpu,audio_cpu,main_cpu,log_cpu,"
		"send_kbps,audio,enc_us,audio_pkts,cpu_mhz,gpu_mhz,sys_free_kb\n";
	Append(header, sizeof(header) - 1);
	Flush();
}

// ----------------------------------------------------------------------------- system information

static bool PmReady, ClkReady, TimeReady;
static ClkrstSession CpuClock, GpuClock;

static void InitServices(void)
{
	PmReady = R_SUCCEEDED(pmdmntInitialize()) && R_SUCCEEDED(pminfoInitialize());
	if (hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize()))
	{
		ClkReady = R_SUCCEEDED(clkrstOpenSession(&CpuClock, PcvModuleId_CpuBus, 3)) &&
			R_SUCCEEDED(clkrstOpenSession(&GpuClock, PcvModuleId_GPU, 3));
	}
	TimeReady = R_SUCCEEDED(timeInitialize());
}

static u64 UnixTime(void)
{
	u64 t = 0;
	if (!TimeReady)
		return 0;
	// Prefer the network clock (synced), the user clock is what the console shows otherwise
	if (R_SUCCEEDED(timeGetCurrentTime(TimeType_NetworkSystemClock, &t)) && t)
		return t;
	if (R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock, &t)))
		return t;
	return 0;
}

static u64 ForegroundTitle(void)
{
	u64 pid = 0, tid = 0;
	if (!PmReady || R_FAILED(pmdmntGetApplicationProcessId(&pid)))
		return 0; // no game running (home menu)
	if (R_FAILED(pminfoGetProgramId(&tid, pid)))
		return 0;
	return tid;
}

static u32 ClockMhz(ClkrstSession* s)
{
	u32 hz = 0;
	if (!ClkReady || R_FAILED(clkrstGetClockRate(s, &hz)))
		return 0;
	return hz / 1000000;
}

static s64 SystemFreeKb(void)
{
	u64 total = 0, used = 0;
	if (R_FAILED(svcGetSystemInfo(&total, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, PhysicalMemorySystemInfo_System)) ||
		R_FAILED(svcGetSystemInfo(&used, SystemInfoType_UsedPhysicalMemorySize, INVALID_HANDLE, PhysicalMemorySystemInfo_System)))
		return -1;
	return total >= used ? (s64)((total - used) / 1024) : -1;
}

// ----------------------------------------------------------------------------- writer (core 3)

static void WriterMain(void* arg)
{
	(void)arg;
	InitServices();
	OpenLogFile(UnixTime());
	if (!FileOpen)
		return;

	u64 freq = armGetSystemTickFreq();
	u64 next = armGetSystemTick() + freq;
	u64 lastIdle = 0, lastWall = armGetSystemTick();
	bool haveIdle = IdleTicks(&lastIdle);
	ThreadSlot video = { 0 }, audio = { 0 }, mainThread = { 0 }, logger[4] = { 0 };
	u64 title = 0;
	u32 seconds = 0;
	atomic_store(&SentBytes, 0);

	while (FileOpen)
	{
		SleepUntilNext(&next, freq);
		u64 wall = armGetSystemTick();
		u64 dWall = wall - lastWall;
		lastWall = wall;

		int c3 = -1;
		u64 idle;
		if (IdleTicks(&idle))
		{
			if (haveIdle && idle >= lastIdle)
			{
				int idlePermille = Permille(idle - lastIdle, dWall);
				c3 = idlePermille < 0 ? -1 : 1000 - idlePermille;
			}
			lastIdle = idle;
			haveIdle = true;
		}
		else
			haveIdle = false;

		u64 vT = ThreadDelta(&video, PerfLog_VideoThreadHandle());
		u64 aT = ThreadDelta(&audio, PerfLog_AudioThreadHandle());
		u64 mT = ThreadDelta(&mainThread, envGetMainThreadHandle());
		u64 lT = ThreadDelta(&logger[0], Writer.handle);
		for (int i = 0; i < 3; i++)
			lT += ThreadDelta(&logger[i + 1], Samplers[i].handle);

		u64 sent = atomic_exchange(&SentBytes, 0);
		u32 encUs = 0, pkts = 0;
		PerfLog_TakeAudioEncode(&encUs, &pkts);
		if (seconds % TITLE_EVERY_S == 0)
			title = ForegroundTitle();

		char line[512];
		char* p = line;
		p = PutU64(p, UnixTime()); *p++ = ',';
		p = PutU64(p, wall / freq); *p++ = ',';
		p = PutHex16(p, title); *p++ = ',';
		for (int i = 0; i < 3; i++)
		{
			p = PutI64(p, atomic_load(&CoreBusy[i])); *p++ = ',';
		}
		p = PutI64(p, c3); *p++ = ',';
		p = PutI64(p, Permille(vT + aT + mT, dWall)); *p++ = ',';
		p = PutI64(p, Permille(vT, dWall)); *p++ = ',';
		p = PutI64(p, Permille(aT, dWall)); *p++ = ',';
		p = PutI64(p, Permille(mT, dWall)); *p++ = ',';
		p = PutI64(p, Permille(lT, dWall)); *p++ = ',';
		p = PutU64(p, dWall ? sent * 8 * freq / dWall / 1000 : 0); *p++ = ',';
		// audio: the extension's encoding while it sends packets; "official" when the official PCM path
		// is streaming (its thread uses CPU, no extension packets); "-" when no audio is streaming
		char tag[24];
		tag[0] = 0;
		if (pkts > 0)
			PerfLog_AudioTag(tag, sizeof(tag));
		tag[sizeof(tag) - 1] = 0;
		p = PutStr(p, pkts > 0 && tag[0] ? tag : (Permille(aT, dWall) > 0 ? "official" : "-")); *p++ = ',';
		p = PutU64(p, encUs); *p++ = ',';
		p = PutU64(p, pkts); *p++ = ',';
		p = PutU64(p, ClockMhz(&CpuClock)); *p++ = ',';
		p = PutU64(p, ClockMhz(&GpuClock)); *p++ = ',';
		p = PutI64(p, SystemFreeKb()); *p++ = '\n';
		Append(line, (u32)(p - line));

		if (++seconds % FLUSH_EVERY_S == 0)
			Flush();
	}
}

void PerfLog_Start(FsFileSystem* sd)
{
	if (!sd)
		return;
	Sd = sd;
	for (int i = 0; i < 3; i++)
	{
		if (R_SUCCEEDED(threadCreate(&Samplers[i], SamplerMain, (void*)(uintptr_t)i, SamplerStacks[i],
				sizeof(SamplerStacks[i]), SAMPLER_PRIO, i)))
			threadStart(&Samplers[i]);
	}
	if (R_SUCCEEDED(threadCreate(&Writer, WriterMain, NULL, WriterStack, sizeof(WriterStack), WRITER_PRIO, 3)))
		threadStart(&Writer);
}

#endif
