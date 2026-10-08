// Writes the shared test vectors (see the README.md it generates).
//   next_vectors <48 kHz stereo wav> <output dir>
#include "next_audio.h"
#include "media.h"
#include "ref.h"
#include "sim.h"
#include "opus.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PACKETS 12
#define BATCHING 3
#define FRAMES_PER_PACKET (1024 * (1 + BATCHING))
#define TOTAL_FRAMES (PACKETS * FRAMES_PER_PACKET)
#define START_SECONDS 30
#define TS_BASE 1000000u

static WavData Wav;
static NextAudioWork Work;
static SimAudioPacket Pkt __attribute__((aligned(4096)));
static uint8_t DecMem[32768];
static const char* Dir;

static FILE* Open(const char* name)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", Dir, name);
	FILE* f = fopen(path, "wb");
	if (!f)
	{
		perror(path);
		exit(1);
	}
	return f;
}

static void WriteRaw(const char* name, const void* data, size_t bytes)
{
	FILE* f = Open(name);
	fwrite(data, 1, bytes, f);
	fclose(f);
}

typedef struct {
	const char* name;
	const char* description;
	struct { int at; uint8_t codec, kbpsHalf, cf; } steps[8]; // control messages applied before packet `at`
	int stepCount;
} Vector;

static FILE* Manifest;

static void RunVector(const Vector* v, const int16_t* input)
{
	NextAudio_Init(&Work);
	NextAudio_SetHooks(NULL, NULL); // encodeUs = 0 in the vectors (deterministic)
	NextAudioConfig first = NextExt_AudioConfigFromWire(v->steps[0].codec, v->steps[0].kbpsHalf, v->steps[0].cf);
	NextAudio_Start(&first);

	Sim sim;
	Sim_Init(&sim, &Pkt, input, TOTAL_FRAMES, BATCHING, TS_BASE);

	char name[256];
	snprintf(name, sizeof(name), "%s.stream.bin", v->name);
	FILE* stream = Open(name);
	snprintf(name, sizeof(name), "%s.packets.tsv", v->name);
	FILE* tsv = Open(name);
	fprintf(tsv, "index\toffset\tpacket_bytes\tdata_size\ttimestamp_us\tmeta\treplay_slot\tcodec\tcomplexity\tframe_code\tframe_count\tbitrate_kbps\tsamples_per_channel\tencode_us\n");

	// Expected decode output (PCM24: the 24 kHz samples; ADPCM/PCM48: 48 kHz; Opus: libopus reference decoder)
	int16_t* decoded = calloc(TOTAL_FRAMES * 2 + 48000, 4);
	size_t decodedFrames = 0;
	OpusDecoder* dec = (OpusDecoder*)DecMem;
	opus_decoder_init(dec, 48000, 2);

	long offset = 0;
	int step = 1;
	int codecsSeen = 0;
	for (int p = 0; p < PACKETS; p++)
	{
		if (step < v->stepCount && v->steps[step].at == p)
		{
			NextAudioConfig c = NextExt_AudioConfigFromWire(v->steps[step].codec, v->steps[step].kbpsHalf, v->steps[step].cf);
			NextAudio_Request(&c);
			step++;
		}
		NextAudioResult r = Sim_Step(&sim, 0);
		if (r.payloadSize == 0)
			continue;
		codecsSeen |= 1 << r.codec;
		uint32_t bytes = NEXT_PACKET_HEADER_SIZE + r.payloadSize;
		fwrite(&Pkt, 1, bytes, stream);

		NextExtAudioHeader h;
		memset(&h, 0, sizeof(h));
		bool hasHdr = r.hasExtHeader && NextExt_ReadAudioHeader(Pkt.data, r.payloadSize, &h);
		fprintf(tsv, "%d\t%ld\t%u\t%u\t%llu\t0x%02X\t0x%02X\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n", p, offset, bytes, r.payloadSize,
			(unsigned long long)Pkt.h.Timestamp, Pkt.h.MetaData, Pkt.h.ReplaySlot, r.codec, hasHdr ? (h.complexityFrame & 15) : 0,
			hasHdr ? (h.complexityFrame >> 4) : 0, hasHdr ? h.frameCount : 0, hasHdr ? h.bitrateKbps : 0,
			hasHdr ? h.samplesPerChannel : r.samplesPerChannel, hasHdr ? h.encodeUs : 0);
		offset += bytes;

		switch (r.codec)
		{
		case NextCodec_PCM48:
			memcpy(decoded + 2 * decodedFrames, Pkt.data, r.payloadSize);
			decodedFrames += r.payloadSize / 4;
			break;
		case NextCodec_PCM24:
			memcpy(decoded + 2 * decodedFrames, Pkt.data + 12, (size_t)h.samplesPerChannel * 4);
			decodedFrames += h.samplesPerChannel;
			break;
		case NextCodec_ADPCM:
			NextAdpcm_DecodePacket(Pkt.data + 12, r.payloadSize - 12, h.samplesPerChannel, decoded + 2 * decodedFrames);
			decodedFrames += h.samplesPerChannel;
			break;
		case NextCodec_OPUS:
		{
			uint32_t pos = 12;
			for (int f = 0; f < h.frameCount; f++)
			{
				uint32_t len = next_rd16(Pkt.data + pos);
				int n = opus_decode(dec, Pkt.data + pos + 2, (opus_int32)len, decoded + 2 * decodedFrames, 5760, 0);
				if (n > 0)
					decodedFrames += (size_t)n;
				pos += 2 + len;
			}
			break;
		}
		}
	}
	fclose(stream);
	fclose(tsv);

	bool single = (codecsSeen & (codecsSeen - 1)) == 0;
	if (single)
	{
		const char* suffix = codecsSeen == (1 << NextCodec_PCM24) ? "expected_24k_s16le_stereo.raw" :
			codecsSeen == (1 << NextCodec_OPUS) ? "ref_decoded_48k_s16le_stereo.raw" : "expected_48k_s16le_stereo.raw";
		snprintf(name, sizeof(name), "%s.%s", v->name, suffix);
		WriteRaw(name, decoded, decodedFrames * 4);
	}

	static int written;
	fprintf(Manifest, "%s    {\"name\": \"%s\", \"description\": \"%s\", \"stream\": \"%s.stream.bin\", \"packets\": \"%s.packets.tsv\", \"stream_bytes\": %ld, "
		"\"decoded_frames\": %zu, \"captured_frames\": %llu}", written ? ",\n" : "", v->name, v->description,
		v->name, v->name, offset, decodedFrames, (unsigned long long)sim.captured);
	written++;
	printf("  %-22s %ld bytes in the stream, %zu decoded frames\n", v->name, offset, decodedFrames);

	if (!strcmp(v->name, "pcm24"))
	{
		// Reference 48 kHz reconstruction as in audio_codec_eval.py up48(): zero-stuff, x2, 127-tap low-pass at 11.5 kHz.
		// This is only what the offline evaluation did; the client may use any good upsampler.
		double hup[127];
		Ref_LowpassFir(11500, 127, hup);
		int16_t* up = malloc(decodedFrames * 2 * 4);
		for (size_t n = 0; n < decodedFrames * 2; n++)
			for (int c = 0; c < 2; c++)
			{
				double acc = 0;
				for (int k = 0; k < 127; k++)
				{
					long idx = (long)n + 63 - k; // 'same' convolution
					if (idx >= 0 && (size_t)idx < decodedFrames * 2 && (idx % 2) == 0)
						acc += hup[k] * 2.0 * decoded[2 * (idx / 2) + c];
				}
				double rr = rint(acc);
				up[2 * n + c] = (int16_t)(rr < -32768 ? -32768 : (rr > 32767 ? 32767 : rr));
			}
		WriteRaw("pcm24.ref_up48_s16le_stereo.raw", up, decodedFrames * 2 * 4);
		free(up);
	}
	free(decoded);
}

static const char* Readme =
"# NSDVR extension test vectors (v1)\n"
"\n"
"Generated by `NSDVR-server/host/build/next_vectors` (`make -C NSDVR-server/host vectors`) with exactly the\n"
"sysmodule's encoder code (`sysmodule/source/next/next_audio.c`, libopus 1.6.1 fixed point). Protocol:\n"
"`docs/nsdvr-ext-protocol.md`. All integers little endian, all PCM is s16le interleaved stereo.\n"
"\n"
"## Input\n"
"\n"
"- `input_48k_s16le_stereo.raw` (+ `input.wav`): 49152 frames = 1.024 s of `audio_eval/src/aliens_48k.wav`\n"
"  starting at 30.0 s. That is 12 grc captures with AudioBatching = 3 (4 x 1024 frames each).\n"
"- Capture n (0..11) has the grc timestamp `1000000 + round(n * 4096 * 1e6 / 48000)` microseconds.\n"
"\n"
"## Per vector\n"
"\n"
"- `<name>.stream.bin` – the exact bytes the server writes to the 9922 socket after the handshake:\n"
"  a sequence of `PacketHeader` (18 bytes: u32 magic 0xCCCCCCCC, u32 DataSize, u64 Timestamp, u8 MetaData,\n"
"  u8 ReplaySlot) + DataSize payload bytes. Feed it to the client's packet parser as is.\n"
"- `<name>.packets.tsv` – one line per packet: byte offset in the stream, header fields and the parsed\n"
"  ExtAudioHeader. `encode_us` is 0 in the vectors (the real server fills in the measured time).\n"
"- Expected decoder output (only for single-codec vectors):\n"
"  - `pcm24.expected_24k_s16le_stereo.raw` – the 24 kHz samples, i.e. the concatenated packet bodies.\n"
"    Bit exact: this is what any correct parser must produce.\n"
"    `pcm24.ref_up48_s16le_stereo.raw` – for reference only: 48 kHz output of the offline evaluation's\n"
"    upsampler (`up48()` in tools/audio_codec_eval.py: zero-stuffing, gain 2, 127-tap low-pass at 11.5 kHz,\n"
"    centered). The client may use a different upsampler; compare with a tolerance.\n"
"  - `adpcm.expected_48k_s16le_stereo.raw` – bit exact IMA ADPCM decode (every packet decoded from its own\n"
"    8-byte state header; the result is identical to decoding the whole stream continuously).\n"
"  - `opus_*.ref_decoded_48k_s16le_stereo.raw` – libopus 1.6.1 (fixed point) `opus_decode()` of every frame\n"
"    in order with one decoder. Another Opus decoder (float libopus, platform codec) will differ slightly:\n"
"    compare with a tolerance (e.g. SNR against this file > 40 dB, or after aligning, max abs diff of a few LSB).\n"
"    Note the Opus encoder delay: decoded sample k corresponds to input sample k - 120 (2.5 ms).\n"
"  - `pcm48.expected_48k_s16le_stereo.raw` – identical to the input.\n"
"- `switch.*` – control messages applied between packets (PCM24 -> ADPCM -> Opus 96k/20ms -> Opus 32k/10ms\n"
"  (live change, no reset) -> PCM48 -> Opus 64k/20ms c10). Tests the dispatcher: codec marker changes,\n"
"  dropped Opus carry on codec switches (< 20 ms), timestamps. No expected decode file: decode per packet.\n"
"\n"
"## Packet facts worth testing\n"
"\n"
"- ReplaySlot is 0xE0 | codec on every extension audio packet; MetaData is 0x06 (audio | data).\n"
"- PCM24/ADPCM: one packet per capture, `samples_per_channel` = 2048 (PCM24, 24 kHz) / 4096 (ADPCM).\n"
"- Opus: frames are cut from a continuous stream, so a packet holds 4 or 5 frames (20 ms) or 8 or 9\n"
"  (10 ms); the remainder (< 1 frame) is carried into the next packet. The packet timestamp is the\n"
"  time of the first sample of its first frame, so timestamps are continuous: next = ts + spc * 1e6 / 48000\n"
"  (+-1 us rounding).\n"
"- Opus is constant bitrate: every frame is exactly kbps * frame_samples / 384 bytes (240 bytes at 96 kbps / 20 ms).\n"
"\n"
"## Control message / handshake bytes (hex)\n"
"\n"
"- Handshake, audio channel, AudioBatching 3, ext flag, Opus 96 kbps complexity 5 20 ms, TOS:\n"
"  `AA AA AA AA 30 33 02 00 03 04 03 30 05 02 00 00`\n"
"- Handshake, video channel, NAL hash + SPS/PPS injection, ext flag, diagnostics + TOS:\n"
"  `AA AA AA AA 30 33 01 07 00 04 00 00 00 03 00 00`\n"
"- Control message: switch to Opus 64 kbps, complexity 10, 10 ms frames: `53 44 56 58 03 20 1A 00`\n"
"- Control message: switch to ADPCM: `53 44 56 58 02 00 00 00`\n"
"\n"
"`manifest.json` lists every file with sizes and frame counts.\n";

int main(int argc, char** argv)
{
	char err[256];
	if (argc < 3 || !Wav_Load(argv[1], &Wav, err, sizeof(err)))
	{
		printf("usage: next_vectors <aliens_48k.wav> <output dir>\n");
		return 2;
	}
	Dir = argv[2];
	mkdir(Dir, 0755);
	const int16_t* input = Wav.samples + 48000 * START_SECONDS * 2;

	WriteRaw("input_48k_s16le_stereo.raw", input, TOTAL_FRAMES * 4);
	char path[1024];
	snprintf(path, sizeof(path), "%s/input.wav", Dir);
	Wav_Write(path, input, TOTAL_FRAMES, 48000);

	Manifest = Open("manifest.json");
	fprintf(Manifest, "{\n  \"version\": 1,\n  \"generator\": \"NSDVR-server/host/build/next_vectors (%s, OPUS_APPLICATION_RESTRICTED_CELT, CBR)\",\n",
		opus_get_version_string());
	fprintf(Manifest, "  \"input\": {\"file\": \"input_48k_s16le_stereo.raw\", \"frames\": %d, \"source\": \"audio_eval/src/aliens_48k.wav @ %d s\", "
		"\"audio_batching\": %d, \"captures\": %d, \"first_timestamp_us\": %u},\n  \"vectors\": [\n", TOTAL_FRAMES, START_SECONDS, BATCHING, PACKETS, TS_BASE);

	Vector vectors[] = {
		{ "pcm48", "PCM48 with the extension marker 0xE0 (payload identical to official)", { { 0, 0, 0, 0 } }, 1 },
		{ "pcm24", "PCM24: 63-tap half-band, 24 kHz", { { 0, 1, 0, 0 } }, 1 },
		{ "adpcm", "IMA ADPCM, per-packet state", { { 0, 2, 0, 0 } }, 1 },
		{ "opus_96k_20ms_c5", "Opus 96 kbps, complexity 5, 20 ms (client default)", { { 0, 3, 48, 0x05 } }, 1 },
		{ "opus_64k_10ms_c0", "Opus 64 kbps, complexity 0, 10 ms", { { 0, 3, 32, 0x10 } }, 1 },
		{ "opus_256k_20ms_c10", "Opus 256 kbps, complexity 10, 20 ms", { { 0, 3, 128, 0x0A } }, 1 },
		{ "switch", "codec / parameter switches via control messages",
			{ { 0, 1, 0, 0 }, { 2, 2, 0, 0 }, { 4, 3, 48, 0x05 }, { 6, 3, 16, 0x15 }, { 8, 0, 0, 0 }, { 10, 3, 32, 0x0A } }, 6 },
	};
	for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++)
		RunVector(&vectors[i], input);
	fprintf(Manifest, "\n  ]\n}\n");
	fclose(Manifest);

	FILE* f = Open("README.md");
	fputs(Readme, f);
	fclose(f);
	printf("vectors written to %s\n", Dir);
	return 0;
}
