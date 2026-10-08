#pragma once
// Double precision ports of the reference algorithms in NSDVR/tools/audio_codec_eval.py
#include <stdint.h>
#include <stddef.h>

// lowpass_fir(cutoff, taps): sinc(2 cutoff / sr * n) * kaiser(taps, 8.0), normalised to sum 1
void Ref_LowpassFir(double cutoffHz, int taps, double* h);
// down24(x): q16(filt(x, HALFBAND_63)[::2]); filt = np.convolve(mode='same') per channel.
// `frames` stereo input frames -> (frames + 1) / 2 output frames. Rounding: half to even like numpy.
void Ref_Down24(const int16_t* x, size_t frames, int16_t* y);
// adpcm_channel(samples) with shaping = 0 for one channel (stride = 2 for interleaved stereo).
// Writes the reconstruction (what a decoder outputs) and the 4-bit codes.
void Ref_AdpcmChannel(const int16_t* x, size_t n, size_t stride, int16_t* recon, uint8_t* codes);
