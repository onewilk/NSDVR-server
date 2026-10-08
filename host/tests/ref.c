#include "ref.h"
#include <math.h>

static double I0(double x)
{
	double s = 1, t = 1;
	for (int k = 1; k < 500; k++)
	{
		t *= (x / 2 / k) * (x / 2 / k);
		s += t;
		if (t < 1e-20 * s)
			break;
	}
	return s;
}

void Ref_LowpassFir(double cutoffHz, int taps, double* h)
{
	double a = (taps - 1) / 2.0, sum = 0;
	for (int n = 0; n < taps; n++)
	{
		double m = n - a;
		double x = 2 * cutoffHz / 48000.0 * m;
		double sinc = x == 0 ? 1.0 : sin(M_PI * x) / (M_PI * x);
		double r = (n - a) / a;
		double w = I0(8.0 * sqrt(fmax(0.0, 1 - r * r))) / I0(8.0);
		h[n] = sinc * w;
		sum += h[n];
	}
	for (int n = 0; n < taps; n++)
		h[n] /= sum;
}

void Ref_Down24(const int16_t* x, size_t frames, int16_t* y)
{
	double h[63];
	Ref_LowpassFir(12000, 63, h);
	for (size_t j = 0; 2 * j < frames; j++)
	{
		for (int c = 0; c < 2; c++)
		{
			// np.convolve 'same' with an odd kernel: out[n] = sum_k h[k] x[n + 31 - k]
			double acc = 0;
			size_t n = 2 * j;
			for (int k = 0; k < 63; k++)
			{
				long idx = (long)n + 31 - k;
				if (idx >= 0 && (size_t)idx < frames)
					acc += h[k] * x[2 * idx + c];
			}
			double r = rint(acc); // round half to even, like np.round
			y[2 * j + c] = (int16_t)(r < -32768 ? -32768 : (r > 32767 ? 32767 : r));
		}
	}
}

static const int Steps[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
	97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
	724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
	4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
	18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int IndexAdj[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

void Ref_AdpcmChannel(const int16_t* x, size_t n, size_t stride, int16_t* recon, uint8_t* codes)
{
	// Line by line port of adpcm_channel() with shaping = 0 (target = s - 0.0 * err_prev = s)
	long pred = 0, idx = 0;
	for (size_t i = 0; i < n; i++)
	{
		long target = x[i * stride];
		long step = Steps[idx];
		long diff = target - pred;
		int code = 0;
		if (diff < 0) { code = 8; diff = -diff; }
		long delta = step >> 3;
		if (diff >= step) { code |= 4; diff -= step; delta += step; }
		if (diff >= step >> 1) { code |= 2; diff -= step >> 1; delta += step >> 1; }
		if (diff >= step >> 2) { code |= 1; delta += step >> 2; }
		pred = (code & 8) ? pred - delta : pred + delta;
		pred = pred < -32768 ? -32768 : (pred > 32767 ? 32767 : pred);
		idx += IndexAdj[code & 7];
		idx = idx < 0 ? 0 : (idx > 88 ? 88 : idx);
		recon[i * stride] = (int16_t)pred;
		if (codes)
			codes[i] = (uint8_t)code;
	}
}
