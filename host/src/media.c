#include "media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint32_t rd32le(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16le(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint8_t* ReadFile(const char* path, size_t* size)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0)
	{
		fclose(f);
		return NULL;
	}
	uint8_t* buf = malloc((size_t)n);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*size = (size_t)n;
	return buf;
}

bool Wav_Load(const char* path, WavData* out, char* err, size_t errLen)
{
	memset(out, 0, sizeof(*out));
	size_t size = 0;
	uint8_t* buf = ReadFile(path, &size);
	if (!buf)
	{
		snprintf(err, errLen, "cannot read %s", path);
		return false;
	}
	if (size < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4))
	{
		snprintf(err, errLen, "%s is not a RIFF/WAVE file", path);
		free(buf);
		return false;
	}

	uint16_t fmt = 0, channels = 0, bits = 0;
	uint32_t rate = 0;
	const uint8_t* data = NULL;
	uint32_t dataLen = 0;
	size_t pos = 12;
	while (pos + 8 <= size)
	{
		uint32_t len = rd32le(buf + pos + 4);
		const uint8_t* body = buf + pos + 8;
		size_t avail = size - pos - 8;
		if (len > avail)
			len = (uint32_t)avail;
		if (!memcmp(buf + pos, "fmt ", 4) && len >= 16)
		{
			fmt = rd16le(body);
			channels = rd16le(body + 2);
			rate = rd32le(body + 4);
			bits = rd16le(body + 14);
			if (fmt == 0xFFFE && len >= 26) // WAVE_FORMAT_EXTENSIBLE: sub format GUID starts with the format tag
				fmt = rd16le(body + 24);
		}
		else if (!memcmp(buf + pos, "data", 4))
		{
			data = body;
			dataLen = len;
		}
		pos += 8 + (size_t)len + (len & 1);
	}

	if (fmt != 1 || bits != 16 || (channels != 1 && channels != 2) || !data)
	{
		snprintf(err, errLen, "%s: need 16-bit PCM mono/stereo (fmt %u, %u ch, %u bits)", path, fmt, channels, bits);
		free(buf);
		return false;
	}

	uint32_t frames = dataLen / (2u * channels);
	out->samples = malloc((size_t)frames * 4 + 4);
	out->frames = frames;
	out->rate = rate;
	for (uint32_t i = 0; i < frames; i++)
	{
		int16_t l, r;
		memcpy(&l, data + (size_t)i * 2 * channels, 2);
		if (channels == 2)
			memcpy(&r, data + (size_t)i * 4 + 2, 2);
		else
			r = l;
		out->samples[2 * i] = l;
		out->samples[2 * i + 1] = r;
	}
	free(buf);
	return true;
}

static void wr32le(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void wr16le(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

bool Wav_Write(const char* path, const int16_t* stereo, uint32_t frames, uint32_t rate)
{
	FILE* f = fopen(path, "wb");
	if (!f)
		return false;
	uint8_t h[44];
	memcpy(h, "RIFF", 4);
	wr32le(h + 4, 36 + frames * 4);
	memcpy(h + 8, "WAVEfmt ", 8);
	wr32le(h + 16, 16);
	wr16le(h + 20, 1);
	wr16le(h + 22, 2);
	wr32le(h + 24, rate);
	wr32le(h + 28, rate * 4);
	wr16le(h + 32, 4);
	wr16le(h + 34, 16);
	memcpy(h + 36, "data", 4);
	wr32le(h + 40, frames * 4);
	bool ok = fwrite(h, 1, 44, f) == 44 && fwrite(stereo, 4, frames, f) == frames;
	fclose(f);
	return ok;
}

void Wav_Free(WavData* w)
{
	free(w->samples);
	memset(w, 0, sizeof(*w));
}

void Wav_Synthetic(WavData* out, uint32_t frames)
{
	out->samples = malloc((size_t)frames * 4);
	out->frames = frames;
	out->rate = 48000;
	for (uint32_t i = 0; i < frames; i++)
	{
		double t = i / 48000.0;
		double level = 6000 + 4000 * sin(2 * M_PI * 0.25 * t);
		out->samples[2 * i] = (int16_t)(level * sin(2 * M_PI * 440 * t));
		out->samples[2 * i + 1] = (int16_t)(level * sin(2 * M_PI * 660 * t));
	}
}

// ----------------------------------------------------------------------------- H.264

typedef struct { size_t start, end; int type; } Nal;

static int NalType(const uint8_t* p, size_t len)
{
	size_t i = (len > 2 && p[2] == 1) ? 3 : 4;
	return i < len ? (p[i] & 0x1F) : -1;
}

bool H264_Load(const char* path, H264File* out, char* err, size_t errLen)
{
	memset(out, 0, sizeof(*out));
	size_t size = 0;
	uint8_t* buf = ReadFile(path, &size);
	if (!buf)
	{
		snprintf(err, errLen, "cannot read %s", path);
		return false;
	}

	// Find start codes
	size_t cap = 1024, n = 0;
	size_t* starts = malloc(cap * sizeof(size_t));
	for (size_t i = 0; i + 3 <= size;)
	{
		if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
		{
			size_t s = (i > 0 && buf[i - 1] == 0) ? i - 1 : i;
			if (n == cap)
				starts = realloc(starts, (cap *= 2) * sizeof(size_t));
			starts[n++] = s;
			i += 3;
		}
		else
			i++;
	}
	if (n == 0)
	{
		snprintf(err, errLen, "%s: no Annex-B start codes", path);
		free(starts);
		free(buf);
		return false;
	}

	// Build frames into a new blob: pending non-VCL NALs are prepended to the next VCL NAL
	out->blob = malloc(size + 16);
	out->frames = malloc(n * sizeof(H264Frame));
	size_t blobPos = 0, pendingStart = 0;
	bool havePending = false;
	uint32_t spsLen = 0, ppsLen = 0;
	uint8_t sps[128], pps[128];
	for (size_t k = 0; k < n; k++)
	{
		size_t s = starts[k], e = (k + 1 < n) ? starts[k + 1] : size;
		int t = NalType(buf + s, e - s);
		if (t == 9)
			continue; // AUD
		if (t == 7 && !spsLen && e - s <= sizeof(sps)) { memcpy(sps, buf + s, e - s); spsLen = (uint32_t)(e - s); }
		if (t == 8 && !ppsLen && e - s <= sizeof(pps)) { memcpy(pps, buf + s, e - s); ppsLen = (uint32_t)(e - s); }
		if (!havePending)
		{
			pendingStart = blobPos;
			havePending = true;
		}
		memcpy(out->blob + blobPos, buf + s, e - s);
		blobPos += e - s;
		if (t == 1 || t == 5)
		{
			H264Frame* f = &out->frames[out->count++];
			f->data = out->blob + pendingStart;
			f->size = (uint32_t)(blobPos - pendingStart);
			f->idr = t == 5;
			out->idrCount += f->idr;
			havePending = false;
		}
	}
	if (spsLen && ppsLen && spsLen + ppsLen <= sizeof(out->spsPps))
	{
		memcpy(out->spsPps, sps, spsLen);
		memcpy(out->spsPps + spsLen, pps, ppsLen);
		out->spsPpsSize = spsLen + ppsLen;
	}
	free(starts);
	free(buf);
	if (out->count == 0)
	{
		snprintf(err, errLen, "%s: no slices found", path);
		H264_Free(out);
		return false;
	}
	return true;
}

void H264_Synthetic(H264File* out)
{
	memset(out, 0, sizeof(*out));
	const uint32_t frames = 300, idrSize = 40000;
	size_t total = idrSize + 5;
	for (uint32_t i = 0; i < frames; i++)
		total += 5 + 3000 + (i * 997) % 9000;
	out->blob = malloc(total);
	out->frames = malloc(frames * sizeof(H264Frame));
	// One shared IDR payload so NAL hashing sees repeats
	uint8_t* idr = out->blob;
	memcpy(idr, "\x00\x00\x00\x01\x65", 5);
	for (uint32_t j = 0; j < idrSize; j++)
		idr[5 + j] = (uint8_t)((j * 7) & 0xFF ? (j * 7) & 0xFF : 1);
	size_t pos = idrSize + 5;
	for (uint32_t i = 0; i < frames; i++)
	{
		H264Frame* f = &out->frames[out->count++];
		if (i % 30 == 0)
		{
			f->data = idr;
			f->size = idrSize + 5;
			f->idr = true;
			out->idrCount++;
			continue;
		}
		uint32_t sz = 3000 + (i * 997) % 9000;
		uint8_t* p = out->blob + pos;
		memcpy(p, "\x00\x00\x00\x01\x41", 5);
		for (uint32_t j = 0; j < sz; j++)
			p[5 + j] = (uint8_t)(((i + j) * 13) & 0xFF ? ((i + j) * 13) & 0xFF : 1);
		f->data = p;
		f->size = sz + 5;
		f->idr = false;
		pos += sz + 5;
	}
}

void H264_Free(H264File* f)
{
	free(f->blob);
	free(f->frames);
	memset(f, 0, sizeof(*f));
}

uint32_t Crc32(const uint8_t* data, size_t len)
{
	static uint32_t table[256];
	static int ready;
	if (!ready)
	{
		for (uint32_t i = 0; i < 256; i++)
		{
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		ready = 1;
	}
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++)
		crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}
