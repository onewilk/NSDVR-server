/* Helpers compiled as part of libopus (with OPUS_BUILD, so opus.h does not mark `st` as nonnull). */
#include "opus.h"

/* Exact encoder state size for OPUS_APPLICATION_RESTRICTED_CELT. opus_encoder_get_size() only
 * knows OPUS_APPLICATION_AUDIO and overestimates (29892 vs 10380 bytes for fixed point stereo);
 * in 1.6.x opus_encoder_init(NULL, ...) returns the size for the given application. */
int next_opus_encoder_size_restricted_celt(int channels)
{
    return opus_encoder_init(NULL, 48000, channels, OPUS_APPLICATION_RESTRICTED_CELT);
}
