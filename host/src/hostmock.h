#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "media.h"
#include "next_ext.h"

typedef struct {
	const char* listenAddr;
	int videoPort, audioPort;
	const char* h264Path;
	const char* wavPath;
	double fps;
	double stallEverySec;   // artificial blocking of video sends
	int stallMs;
	double stallAudioEverySec; // same for the audio channel (optional)
	int stallAudioMs;
	int dropEvery;          // drop every Nth non-IDR frame at the (virtual) grc source
	int grcQueue;           // frames the virtual grc keeps when the reader falls behind
	int sndBuf;             // SO_SNDBUF override, 0 = OS default
	bool beacon;
	const char* beaconTargets;
	const char* serial;
	bool once;
	double durationSec;
	bool verbose;
	bool quietDiag;
	bool noOpus;
} MockOptions;

extern MockOptions g_opt;
extern H264File g_video;
extern WavData g_audio;
extern atomic_bool g_running;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// plat_host.c: per connection setup of the virtual grc and the stall logic
void Plat_Init(void);
void Plat_VideoConnected(int sock, uint8_t videoFlags);
void Plat_VideoDisconnected(void);
void Plat_AudioConnected(int sock, uint8_t batching);
void Plat_AudioDisconnected(void);
uint64_t Plat_VideoDropped(void);
