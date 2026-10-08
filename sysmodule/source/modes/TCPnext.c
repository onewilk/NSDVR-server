#if !defined(USB_ONLY)
// NSDVR: the platform layer (next/next_platform.h) for the sysmodule. The protocol, codecs,
// diagnostics and streaming loops are portable C in source/next (also built and tested on macOS).
#include <stddef.h>
#include <string.h>
#include "modes.h"
#include "../capture.h"
#include "../net/sockets.h"
#include "../next/next_platform.h"

_Static_assert(sizeof(PacketHeader) == sizeof(NextPacketHeader), "PacketHeader layout");
_Static_assert(offsetof(PacketHeader, DataSize) == offsetof(NextPacketHeader, DataSize), "PacketHeader layout");
_Static_assert(offsetof(PacketHeader, Timestamp) == offsetof(NextPacketHeader, Timestamp), "PacketHeader layout");
_Static_assert(offsetof(PacketHeader, MetaData) == offsetof(NextPacketHeader, MetaData), "PacketHeader layout");
_Static_assert(offsetof(PacketHeader, ReplaySlot) == offsetof(NextPacketHeader, ReplaySlot), "PacketHeader layout");
_Static_assert(offsetof(AudioPacket, Data) == NEXT_PACKET_HEADER_SIZE, "AudioPacket layout");
_Static_assert(offsetof(VideoPacket, Data) == NEXT_PACKET_HEADER_SIZE, "VideoPacket layout");
// The encoders never ask for a capture offset larger than the headroom reserved in APkt
_Static_assert(NEXT_AUDIO_HEADROOM <= AudioExtHeadroom, "APkt headroom too small for the NSDVR encoders");
_Static_assert(PacketMeta_Type_Mask == NEXT_META_TYPE_MASK && PacketMeta_Content_Data == NEXT_META_DATA &&
	PacketMeta_Content_Error == NEXT_META_ERROR, "PacketMeta values");

NextPacketHeader* NextPlat_CaptureVideo(void)
{
	CaptureReadVideo();
	return (NextPacketHeader*)&VPkt.Header;
}

NextPacketHeader* NextPlat_AudioPacket(void)
{
	return (NextPacketHeader*)&APkt.Header;
}

uint32_t NextPlat_AudioCapacity(void)
{
	return sizeof(APkt.Data);
}

bool NextPlat_CaptureAudio(uint32_t offset)
{
	return CaptureReadAudioAt(offset);
}

bool NextPlat_SendAll(int sock, const void* buf, uint32_t size, bool allowIncoming)
{
	return SocketSendAllEx(sock, buf, size, allowIncoming);
}

int NextPlat_RecvNonBlocking(int sock, void* buf, uint32_t size)
{
	return SocketRecvNonBlocking(sock, buf, size);
}

bool NextPlat_SetTos(int sock, int tos)
{
	return SocketSetTos(sock, tos);
}

uint64_t NextPlat_NowUs(void)
{
	// 19.2 MHz system counter
	return armGetSystemTick() * 5 / 96;
}

bool NextPlat_Running(void)
{
	return IsThreadRunning;
}

void NextPlat_Yield(void)
{
	// Yield to other threads of the same priority on this core (the video thread) without migrating
	svcSleepThread(0);
}

static Handle AudioThreadHandle = INVALID_HANDLE;

void NextPlat_AudioThreadStarted(void)
{
	AudioThreadHandle = threadGetCurHandle();
}

NextAudioWork* NextPlat_AudioWork(void)
{
	return &Buffers.TcpMode.AudioWork;
}

static bool ThreadTicks(Handle h, u64* out)
{
	if (h == INVALID_HANDLE)
		return false;
	// [13.0.0+] InfoType_ThreadTickCount, older firmware only has the deprecated id
	if (R_SUCCEEDED(svcGetInfo(out, InfoType_ThreadTickCount, h, TickCountInfo_Total)))
		return true;
	return R_SUCCEEDED(svcGetInfo(out, InfoType_ThreadTickCountDeprecated, h, TickCountInfo_Total));
}

// Called from the video thread, which runs on core 3 like every SysDVR thread
void NextPlat_CpuSample(NextCpuSample* s)
{
	memset(s, 0, sizeof(*s));
	s->wallTicks = armGetSystemTick();

	u64 idle = 0;
	// IdleTickCount only works for the calling thread's core: TickCountInfo_Total means "current core"
	if (R_SUCCEEDED(svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, TickCountInfo_Total)))
	{
		s->idleTicks = idle;
		s->idleTotalTicks = s->wallTicks;
		s->idleValid = 1;
	}

	u64 video = 0, audio = 0, mainThread = 0;
	if (ThreadTicks(threadGetCurHandle(), &video))
	{
		ThreadTicks(AudioThreadHandle, &audio);
		ThreadTicks(envGetMainThreadHandle(), &mainThread);
		s->busyTicks = video + audio + mainThread;
		s->busyValid = 1;
	}
}

#endif
