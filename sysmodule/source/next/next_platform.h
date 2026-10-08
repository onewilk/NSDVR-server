#pragma once
// The thin platform layer used by next_session.c. Implemented twice:
//   sysmodule: source/modes/TCPnext.c   (grc:d, bsd sockets, svc)
//   host:      host/src/plat_host.c     (file-backed virtual grc, POSIX sockets)

#include "next_ext.h"
#include "next_audio.h"
#include "next_diag.h"

#ifdef __cplusplus
extern "C" {
#endif

// Blocks until the next video packet is captured. Always returns a complete packet (header +
// payload, possibly an official error packet) in platform owned memory.
NextPacketHeader* NextPlat_CaptureVideo(void);

// The audio packet buffer: an 18 byte header immediately followed by NextPlat_AudioCapacity() bytes
NextPacketHeader* NextPlat_AudioPacket(void);
uint32_t NextPlat_AudioCapacity(void);
// Captures 1 + AudioBatching grc blocks into the audio data area starting at `offset`.
// Success: header DataSize = captured bytes (excluding offset), Timestamp = grc timestamp of the
// first captured sample. Failure: the header/payload hold an official error packet (payload at
// offset 0) and false is returned.
bool NextPlat_CaptureAudio(uint32_t offset);

// Like SocketSendAll. allowIncoming = true: pending data from the peer (control messages) must not
// be treated as a disconnection while waiting for the socket to become writable.
bool NextPlat_SendAll(int sock, const void* buf, uint32_t size, bool allowIncoming);
// Non-blocking receive: > 0 bytes read, 0 nothing pending, < 0 closed or failed
int NextPlat_RecvNonBlocking(int sock, void* buf, uint32_t size);
bool NextPlat_SetTos(int sock, int tos);

uint64_t NextPlat_NowUs(void);
bool NextPlat_Running(void);
void NextPlat_Yield(void);
void NextPlat_CpuSample(NextCpuSample* out);
// Called by the audio session on its own thread so the platform can account its CPU time
void NextPlat_AudioThreadStarted(void);
// Static memory for the encoders (Opus state, pseudostack). May return NULL: no Opus.
NextAudioWork* NextPlat_AudioWork(void);

#ifdef __cplusplus
}
#endif
