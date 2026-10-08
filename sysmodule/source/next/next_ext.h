#pragma once
// NSDVR experimental protocol extension (v1), see docs/nsdvr-ext-protocol.md in the
// NSDVR client repository. This header and the next_*.c files next to it are portable C:
// they are compiled both into the sysmodule (devkitA64) and into the macOS host build
// (host/, unit tests + sysdvr_hostmock). Nothing here may include switch.h or POSIX headers.
//
// Everything on the wire is little endian. All parsers treat their input as hostile:
// every value is validated/clamped and no input can make them read or write out of bounds.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------------- handshake

// Official request layout (struct ProtoHandshakeRequest, 16 bytes):
//   u32 Magic, u16 ProtoVer, u8 MetaFlags, u8 VideoFlags, u8 AudioBatching, u8 FeatureFlags, u8 Reserved[6]
#define NEXT_HANDSHAKE_SIZE 16
#define NEXT_HANDSHAKE_OFF_FEATURES 9
#define NEXT_HANDSHAKE_OFF_RESERVED 10

// FeatureFlags bit 2. Bits 0/1 are the official TurnOffScreen/MemoryDiag.
#define NEXT_FEATURE_FLAG_EXT 0x04

enum {
	NextCodec_PCM48 = 0,
	NextCodec_PCM24 = 1,
	NextCodec_ADPCM = 2,
	NextCodec_OPUS = 3,
	NextCodec_Count = 4,
};

// Opus frame length codes (high nibble of the complexity/frame byte)
enum {
	NextOpusFrame_20ms = 0,
	NextOpusFrame_10ms = 1,
};

#define NEXT_OPUS_DEFAULT_KBPS 96
#define NEXT_OPUS_MIN_KBPS 16
#define NEXT_OPUS_MAX_KBPS 256
#define NEXT_OPUS_DEFAULT_COMPLEXITY 5
#define NEXT_OPUS_MAX_COMPLEXITY 10

// Reserved[3] bits
#define NEXT_RES3_DIAG 0x01
#define NEXT_RES3_TOS 0x02

// Audio encoding parameters. Always kept in the validated/clamped form.
typedef struct {
	uint8_t codec;       // NextCodec_*
	uint8_t complexity;  // 0..10
	uint8_t frameCode;   // NextOpusFrame_*
	uint8_t reserved;
	uint16_t opusKbps;   // 16..256
} NextAudioConfig;

typedef struct {
	bool enabled;        // FeatureFlags & NEXT_FEATURE_FLAG_EXT
	bool diag;           // Reserved[3] bit0 (only meaningful on the video channel)
	bool tos;            // Reserved[3] bit1
	NextAudioConfig audio; // Reserved[0..2] (only meaningful on the audio channel)
} NextExtConfig;

// Default audio configuration (PCM48, Opus parameters at their defaults)
NextAudioConfig NextExt_DefaultAudioConfig(void);

// Clamps raw wire values into a valid NextAudioConfig (shared by handshake and control messages).
//   codecRaw: 0..3, anything else -> PCM48
//   kbpsHalfRaw: bitrate in 2 kbps units, 0 -> default 96 kbps, then clamped to 16..256 kbps
//   cfRaw: low nibble complexity (>10 -> 10), high nibble frame code (1 -> 10 ms, anything else -> 20 ms)
NextAudioConfig NextExt_AudioConfigFromWire(uint8_t codecRaw, uint8_t kbpsHalfRaw, uint8_t cfRaw);

// Parses the extension part of a handshake request. `len` must be NEXT_HANDSHAKE_SIZE, otherwise
// (or when the ext flag is not set) the result has enabled = false and everything else at defaults.
NextExtConfig NextExt_ParseHandshake(const uint8_t* req, size_t len);

// Portable re-implementation of the official ProtoHandshakeVersion + channel check, used by the
// host mock (the sysmodule keeps using the official code in proto.c).
// channel: 1 = video only port, 2 = audio only port, 0 = any. Returns an official result code (6 = OK).
enum {
	NextHs_UnknownFailure = 0,
	NextHs_WrongVersion = 1,
	NextHs_InvalidArg = 2,
	NextHs_InvalidSize = 3,
	NextHs_InvalidMeta = 4,
	NextHs_WrongMagic = 5,
	NextHs_Ok = 6,
	NextHs_InvalidChannel = 7,
};
int NextExt_ValidateHandshake(const uint8_t* req, size_t len, int channel, const char protoVer[2]);

// ----------------------------------------------------------------------------- packets

#define NEXT_PACKET_MAGIC 0xCCCCCCCCu
#define NEXT_PACKET_HEADER_SIZE 18

// Same layout as the official PacketHeader (capture.h)
typedef struct __attribute__((packed)) {
	uint32_t Magic;
	uint32_t DataSize;
	uint64_t Timestamp;
	uint8_t MetaData;
	uint8_t ReplaySlot;
} NextPacketHeader;

_Static_assert(sizeof(NextPacketHeader) == NEXT_PACKET_HEADER_SIZE, "packet header layout");

#define NEXT_META_VIDEO 0x01
#define NEXT_META_AUDIO 0x02
#define NEXT_META_TYPE_MASK 0x03
// Type value 3 (both type bits) is used for diagnostics packets
#define NEXT_META_TYPE_DIAG 0x03
#define NEXT_META_DATA 0x04
#define NEXT_META_REPLAY 0x08
#define NEXT_META_MULTINAL 0x10
#define NEXT_META_ERROR 0x20

// ReplaySlot marker on extension audio packets
#define NEXT_AUDIO_SLOT_BASE 0xE0
#define NEXT_AUDIO_SLOT(codec) ((uint8_t)(NEXT_AUDIO_SLOT_BASE | ((codec) & 0x0F)))

// ExtAudioHeader (compressed codecs only)
#define NEXT_EXT_AUDIO_HEADER_SIZE 12
#define NEXT_EXT_AUDIO_VERSION 1
#define NEXT_ADPCM_STATE_SIZE 8 // 2 channels x (i16 predictor, u8 index, u8 reserved)

typedef struct {
	uint8_t codec;
	uint8_t complexityFrame; // low nibble complexity, high nibble frame code (Opus only)
	uint8_t frameCount;
	uint16_t bitrateKbps;
	uint16_t samplesPerChannel;
	uint32_t encodeUs;
} NextExtAudioHeader;

void NextExt_WriteAudioHeader(uint8_t* out, const NextExtAudioHeader* h);
// Returns false if the buffer is too short or the version is unknown
bool NextExt_ReadAudioHeader(const uint8_t* in, size_t len, NextExtAudioHeader* h);
// Overwrites only the encodeUs field of an already written header
void NextExt_PatchEncodeUs(uint8_t* hdr, uint64_t us);

// ----------------------------------------------------------------------------- control messages

#define NEXT_CTRL_MAGIC 0x58564453u // bytes 'S' 'D' 'V' 'X'
#define NEXT_CTRL_SIZE 8

typedef struct {
	uint8_t buf[NEXT_CTRL_SIZE];
	uint8_t len;
	uint32_t validMessages;   // statistics
	uint32_t droppedBytes;
	uint32_t invalidMessages;
} NextCtrlParser;

void NextCtrl_Init(NextCtrlParser* p);
// Feeds raw bytes from the socket (any length, any alignment, any content). Whenever a complete
// message with a valid magic and codec is found, *latest is overwritten with its clamped config.
// Returns the number of valid messages found in this call.
int NextCtrl_Feed(NextCtrlParser* p, const uint8_t* data, size_t len, NextAudioConfig* latest);
// Builds a control message (client side / tests)
void NextCtrl_Build(uint8_t out[NEXT_CTRL_SIZE], uint8_t codec, uint8_t kbpsHalf, uint8_t complexity, uint8_t frameCode);

// ----------------------------------------------------------------------------- diagnostics

#define NEXT_DIAG_SIZE 56
#define NEXT_DIAG_VERSION 1
#define NEXT_DIAG_UNAVAILABLE 0xFFFFFFFFu

typedef struct {
	uint32_t intervalMs;
	uint32_t videoFramesSent;
	uint32_t videoGrcGaps;
	uint32_t videoSendBlockTotalUs;
	uint32_t videoSendBlockMaxUs;
	uint32_t videoSendsOver20ms;
	uint32_t gapsAfterSlowSend;
	uint32_t audioPackets;
	uint32_t audioEncodeTotalUs;
	uint32_t audioSendBlockTotalUs;
	uint32_t core3IdlePermille;
	uint32_t sysdvrCpuPermille;
	uint32_t tosFlags;
} NextDiagReport;

void NextExt_WriteDiag(uint8_t out[NEXT_DIAG_SIZE], const NextDiagReport* r);
bool NextExt_ReadDiag(const uint8_t* in, size_t len, NextDiagReport* r);

// ----------------------------------------------------------------------------- little endian helpers

static inline void next_wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void next_wr32(uint8_t* p, uint32_t v) { next_wr16(p, (uint16_t)v); next_wr16(p + 2, (uint16_t)(v >> 16)); }
static inline void next_wr64(uint8_t* p, uint64_t v) { next_wr32(p, (uint32_t)v); next_wr32(p + 4, (uint32_t)(v >> 32)); }
static inline uint16_t next_rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t next_rd32(const uint8_t* p) { return (uint32_t)next_rd16(p) | ((uint32_t)next_rd16(p + 2) << 16); }
static inline uint64_t next_rd64(const uint8_t* p) { return (uint64_t)next_rd32(p) | ((uint64_t)next_rd32(p + 4) << 32); }

static inline uint32_t next_sat32(uint64_t v) { return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v; }

#ifdef __cplusplus
}
#endif
