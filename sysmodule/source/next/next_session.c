#include <stdatomic.h>
#include <string.h>
#include "next_session.h"
#include "next_platform.h"

// Extension video sessions that have ended so far. An extension audio session ends together with the
// video session of its client (see NextSession_Audio).
static atomic_uint VideoSessionsEnded;

// ----------------------------------------------------------------------------- video

void NextSession_Video(int sock, const NextExtConfig* ext)
{
	NextCpuSample cpu;
	memset(&cpu, 0, sizeof(cpu));
	NextPlat_CpuSample(&cpu);
	NextDiag_Begin(NextPlat_NowUs(), &cpu);

	if (ext->tos)
		NextDiag_SetTos(0, NextPlat_SetTos(sock, NEXT_IP_TOS_VALUE));

	uint8_t diagPacket[NEXT_PACKET_HEADER_SIZE + NEXT_DIAG_SIZE];
	uint64_t lastVideoTs = 0;

	while (true)
	{
		// Do not check errors here: like the official loop, a grc error is sent as an error packet
		NextPacketHeader* pkt = NextPlat_CaptureVideo();

		if (!NextPlat_Running())
			break;

		if (!(pkt->MetaData & NEXT_META_ERROR))
		{
			lastVideoTs = pkt->Timestamp;
			NextDiag_OnVideoFrame(pkt->Timestamp);
		}

		uint64_t t0 = NextPlat_NowUs();
		bool ok = NextPlat_SendAll(sock, pkt, pkt->DataSize + NEXT_PACKET_HEADER_SIZE, false);
		uint64_t t1 = NextPlat_NowUs();
		if (!ok)
			break;

		NextDiag_OnVideoSent(t1 - t0);

		if (ext->diag && NextDiag_Due(t1))
		{
			NextDiagReport report;
			NextPlat_CpuSample(&cpu);
			NextDiag_Collect(t1, &cpu, &report);

			NextPacketHeader h;
			h.Magic = NEXT_PACKET_MAGIC;
			h.DataSize = NEXT_DIAG_SIZE;
			h.Timestamp = lastVideoTs;
			h.MetaData = NEXT_META_TYPE_DIAG | NEXT_META_DATA;
			h.ReplaySlot = 0xFF;
			memcpy(diagPacket, &h, sizeof(h));
			NextExt_WriteDiag(diagPacket + NEXT_PACKET_HEADER_SIZE, &report);

			if (!NextPlat_SendAll(sock, diagPacket, sizeof(diagPacket), false))
				break;
		}
	}

	atomic_fetch_add(&VideoSessionsEnded, 1);
}

// ----------------------------------------------------------------------------- audio

// Reads whatever control bytes are pending (bounded per call). Returns false once the peer closed
// its side or the socket failed. The NSDVR client never half-closes, so that means it is gone.
static bool PollControl(int sock, NextCtrlParser* parser)
{
	uint8_t tmp[64];
	for (int i = 0; i < 4; i++)
	{
		int n = NextPlat_RecvNonBlocking(sock, tmp, sizeof(tmp));
		if (n < 0)
			return false;
		if (n == 0)
			break;
		if ((unsigned)n > sizeof(tmp))
			n = sizeof(tmp);

		NextAudioConfig latest;
		if (NextCtrl_Feed(parser, tmp, (size_t)n, &latest) > 0)
			NextAudio_Request(&latest);

		if ((unsigned)n < sizeof(tmp))
			break;
	}
	return true;
}

// A send fails only after the socket buffer has stayed full for 10 s. When a client vanishes without
// its FIN or RST reaching the console (e.g. the app was swiped away), video and PCM audio hit that in
// about 10 s, but with Opus 64k the audio session lingered for 40-80 s in testing, and the audio port
// refuses the client's next connection all that time. So the audio session also ends when the client
// closes its side, and when a video session ends (the video port notices in about 10 s). A client
// whose video merely reconnected gets its audio back within a second, as after a network change.
void NextSession_Audio(int sock, const NextExtConfig* ext)
{
	const unsigned videoEnded = atomic_load(&VideoSessionsEnded);

	NextPlat_AudioThreadStarted();

	if (ext->tos)
		NextDiag_SetTos(1, NextPlat_SetTos(sock, NEXT_IP_TOS_VALUE));
	else
		NextDiag_SetTos(1, false);

	NextAudio_Init(NextPlat_AudioWork());
	NextAudio_SetHooks(NextPlat_Yield, NextPlat_NowUs);
	NextAudio_Start(&ext->audio);

	NextCtrlParser parser;
	NextCtrl_Init(&parser);

	NextPacketHeader* pkt = NextPlat_AudioPacket();
	uint8_t* data = (uint8_t*)pkt + NEXT_PACKET_HEADER_SIZE;
	const uint32_t cap = NextPlat_AudioCapacity();

	while (true)
	{
		if (!PollControl(sock, &parser))
			break;
		if (atomic_load(&VideoSessionsEnded) != videoEnded)
			break;

		// Packet boundary: pending control changes take effect here
		uint32_t offset = NextAudio_BeginPacket(data, cap);
		bool captured = NextPlat_CaptureAudio(offset);

		if (!NextPlat_Running())
			break;

		uint64_t encodeUs = 0;
		uint32_t sendSize;
		if (!captured)
		{
			// Official error packet, unchanged (no ExtAudioHeader, ReplaySlot 0xFF)
			NextAudio_Discontinuity();
			pkt->ReplaySlot = 0xFF;
			sendSize = pkt->DataSize + NEXT_PACKET_HEADER_SIZE;
		}
		else
		{
			NextAudioResult r = NextAudio_Encode(data, cap, pkt->DataSize, pkt->Timestamp);
			encodeUs = r.encodeUs; // measured by the encoder, excluding the yields between Opus frames

			if (r.payloadSize == 0)
				continue; // not enough samples for a single Opus frame yet

			if (r.hasExtHeader)
				NextExt_PatchEncodeUs(data, encodeUs);

			pkt->Magic = NEXT_PACKET_MAGIC;
			pkt->DataSize = r.payloadSize;
			pkt->Timestamp = r.timestamp;
			pkt->MetaData = NEXT_META_AUDIO | NEXT_META_DATA;
			pkt->ReplaySlot = r.replaySlot;
			sendSize = r.payloadSize + NEXT_PACKET_HEADER_SIZE;
		}

		uint64_t s0 = NextPlat_NowUs();
		bool ok = NextPlat_SendAll(sock, pkt, sendSize, true);
		uint64_t s1 = NextPlat_NowUs();
		if (!ok)
			break;

		NextDiag_OnAudioPacket(encodeUs, s1 - s0);
	}
}
