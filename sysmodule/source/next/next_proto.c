#include <string.h>
#include "next_ext.h"

NextAudioConfig NextExt_DefaultAudioConfig(void)
{
	NextAudioConfig c;
	c.codec = NextCodec_PCM48;
	c.complexity = NEXT_OPUS_DEFAULT_COMPLEXITY;
	c.frameCode = NextOpusFrame_20ms;
	c.reserved = 0;
	c.opusKbps = NEXT_OPUS_DEFAULT_KBPS;
	return c;
}

NextAudioConfig NextExt_AudioConfigFromWire(uint8_t codecRaw, uint8_t kbpsHalfRaw, uint8_t cfRaw)
{
	NextAudioConfig c = NextExt_DefaultAudioConfig();

	c.codec = codecRaw < NextCodec_Count ? codecRaw : NextCodec_PCM48;

	unsigned kbps = kbpsHalfRaw == 0 ? NEXT_OPUS_DEFAULT_KBPS : (unsigned)kbpsHalfRaw * 2u;
	if (kbps < NEXT_OPUS_MIN_KBPS)
		kbps = NEXT_OPUS_MIN_KBPS;
	if (kbps > NEXT_OPUS_MAX_KBPS)
		kbps = NEXT_OPUS_MAX_KBPS;
	c.opusKbps = (uint16_t)kbps;

	unsigned complexity = cfRaw & 0x0F;
	c.complexity = (uint8_t)(complexity > NEXT_OPUS_MAX_COMPLEXITY ? NEXT_OPUS_MAX_COMPLEXITY : complexity);
	c.frameCode = ((cfRaw >> 4) == NextOpusFrame_10ms) ? NextOpusFrame_10ms : NextOpusFrame_20ms;
	return c;
}

NextExtConfig NextExt_ParseHandshake(const uint8_t* req, size_t len)
{
	NextExtConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.audio = NextExt_DefaultAudioConfig();

	if (!req || len != NEXT_HANDSHAKE_SIZE)
		return cfg;

	if (!(req[NEXT_HANDSHAKE_OFF_FEATURES] & NEXT_FEATURE_FLAG_EXT))
		return cfg;

	const uint8_t* res = req + NEXT_HANDSHAKE_OFF_RESERVED;
	cfg.enabled = true;
	cfg.audio = NextExt_AudioConfigFromWire(res[0], res[1], res[2]);
	cfg.diag = (res[3] & NEXT_RES3_DIAG) != 0;
	cfg.tos = (res[3] & NEXT_RES3_TOS) != 0;
	// res[4], res[5] are reserved and ignored
	return cfg;
}

int NextExt_ValidateHandshake(const uint8_t* req, size_t len, int channel, const char protoVer[2])
{
	if (!req || len != NEXT_HANDSHAKE_SIZE)
		return NextHs_InvalidSize;

	if (next_rd32(req) != 0xAAAAAAAAu)
		return NextHs_WrongMagic;

	if (req[4] != (uint8_t)protoVer[0] || req[5] != (uint8_t)protoVer[1])
		return NextHs_WrongVersion;

	uint8_t meta = req[6];
	bool video = meta & 1, audio = meta & 2;
	if (!video && !audio)
		return NextHs_InvalidMeta;

	if (channel == 1 && audio)
		return NextHs_InvalidChannel;
	if (channel == 2 && video)
		return NextHs_InvalidChannel;

	// Official code clamps AudioBatching to 0..5 and fails with InvalidArg if it had to clamp
	if (audio && req[8] > 5)
		return NextHs_InvalidArg;

	return NextHs_Ok;
}

void NextExt_WriteAudioHeader(uint8_t* out, const NextExtAudioHeader* h)
{
	out[0] = NEXT_EXT_AUDIO_VERSION;
	out[1] = h->codec;
	out[2] = h->complexityFrame;
	out[3] = h->frameCount;
	next_wr16(out + 4, h->bitrateKbps);
	next_wr16(out + 6, h->samplesPerChannel);
	next_wr32(out + 8, h->encodeUs);
}

bool NextExt_ReadAudioHeader(const uint8_t* in, size_t len, NextExtAudioHeader* h)
{
	if (!in || len < NEXT_EXT_AUDIO_HEADER_SIZE || in[0] != NEXT_EXT_AUDIO_VERSION)
		return false;
	h->codec = in[1];
	h->complexityFrame = in[2];
	h->frameCount = in[3];
	h->bitrateKbps = next_rd16(in + 4);
	h->samplesPerChannel = next_rd16(in + 6);
	h->encodeUs = next_rd32(in + 8);
	return true;
}

void NextExt_PatchEncodeUs(uint8_t* hdr, uint64_t us)
{
	next_wr32(hdr + 8, next_sat32(us));
}

// ----------------------------------------------------------------------------- control messages

static const uint8_t CtrlMagicBytes[4] = { 'S', 'D', 'V', 'X' };

void NextCtrl_Init(NextCtrlParser* p)
{
	memset(p, 0, sizeof(*p));
}

int NextCtrl_Feed(NextCtrlParser* p, const uint8_t* data, size_t len, NextAudioConfig* latest)
{
	int found = 0;
	if (!p || !data)
		return 0;

	// Defensive: the parser state must always be consistent even if the struct was corrupted
	if (p->len > NEXT_CTRL_SIZE)
		p->len = 0;

	for (size_t i = 0; i < len; i++)
	{
		uint8_t b = data[i];

		if (p->len < 4)
		{
			if (b == CtrlMagicBytes[p->len])
			{
				p->buf[p->len++] = b;
			}
			else
			{
				// The magic has no repeated prefix, so the only possible resync point is this very byte
				p->droppedBytes += p->len;
				if (b == CtrlMagicBytes[0])
				{
					p->buf[0] = b;
					p->len = 1;
				}
				else
				{
					p->droppedBytes++;
					p->len = 0;
				}
			}
			continue;
		}

		p->buf[p->len++] = b;
		if (p->len < NEXT_CTRL_SIZE)
			continue;

		p->len = 0;
		// Unknown codec values make the whole message invalid (the handshake maps them to PCM48,
		// but a control message is an explicit request, so an invalid one is ignored)
		if (p->buf[4] >= NextCodec_Count)
		{
			p->invalidMessages++;
			continue;
		}

		if (latest)
			*latest = NextExt_AudioConfigFromWire(p->buf[4], p->buf[5], p->buf[6]);
		p->validMessages++;
		found++;
	}

	return found;
}

void NextCtrl_Build(uint8_t out[NEXT_CTRL_SIZE], uint8_t codec, uint8_t kbpsHalf, uint8_t complexity, uint8_t frameCode)
{
	next_wr32(out, NEXT_CTRL_MAGIC);
	out[4] = codec;
	out[5] = kbpsHalf;
	out[6] = (uint8_t)((complexity & 0x0F) | ((frameCode & 0x0F) << 4));
	out[7] = 0;
}

// ----------------------------------------------------------------------------- diagnostics

void NextExt_WriteDiag(uint8_t out[NEXT_DIAG_SIZE], const NextDiagReport* r)
{
	out[0] = NEXT_DIAG_VERSION;
	out[1] = out[2] = out[3] = 0;
	next_wr32(out + 4, r->intervalMs);
	next_wr32(out + 8, r->videoFramesSent);
	next_wr32(out + 12, r->videoGrcGaps);
	next_wr32(out + 16, r->videoSendBlockTotalUs);
	next_wr32(out + 20, r->videoSendBlockMaxUs);
	next_wr32(out + 24, r->videoSendsOver20ms);
	next_wr32(out + 28, r->gapsAfterSlowSend);
	next_wr32(out + 32, r->audioPackets);
	next_wr32(out + 36, r->audioEncodeTotalUs);
	next_wr32(out + 40, r->audioSendBlockTotalUs);
	next_wr32(out + 44, r->core3IdlePermille);
	next_wr32(out + 48, r->sysdvrCpuPermille);
	next_wr32(out + 52, r->tosFlags);
}

bool NextExt_ReadDiag(const uint8_t* in, size_t len, NextDiagReport* r)
{
	if (!in || len < NEXT_DIAG_SIZE || in[0] != NEXT_DIAG_VERSION)
		return false;
	r->intervalMs = next_rd32(in + 4);
	r->videoFramesSent = next_rd32(in + 8);
	r->videoGrcGaps = next_rd32(in + 12);
	r->videoSendBlockTotalUs = next_rd32(in + 16);
	r->videoSendBlockMaxUs = next_rd32(in + 20);
	r->videoSendsOver20ms = next_rd32(in + 24);
	r->gapsAfterSlowSend = next_rd32(in + 28);
	r->audioPackets = next_rd32(in + 32);
	r->audioEncodeTotalUs = next_rd32(in + 36);
	r->audioSendBlockTotalUs = next_rd32(in + 40);
	r->core3IdlePermille = next_rd32(in + 44);
	r->sysdvrCpuPermille = next_rd32(in + 48);
	r->tosFlags = next_rd32(in + 52);
	return true;
}
