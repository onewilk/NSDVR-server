/* NSDVR only uses OPUS_APPLICATION_RESTRICTED_CELT, which never allocates or runs the SILK
 * encoder (see opus_encoder.c in 1.6.x: every silk_InitEncoder()/silk_Encode() call is behind
 * application != RESTRICTED_CELT or mode != MODE_CELT_ONLY). silk/enc_API.c is therefore not
 * compiled and these three entry points are replaced by stubs, which lets the linker drop the
 * whole SILK encoder (~130 KB of code). The stubs fail cleanly if they are ever reached. */
#include "API.h"

opus_int silk_Get_Encoder_Size(opus_int *encSizeBytes, opus_int channels)
{
    (void)channels;
    *encSizeBytes = 0;
    return SILK_NO_ERROR;
}

opus_int silk_InitEncoder(void *encState, int channels, int arch, silk_EncControlStruct *encStatus)
{
    (void)encState; (void)channels; (void)arch; (void)encStatus;
    return SILK_ENC_INVALID_NUMBER_OF_CHANNELS_ERROR;
}

opus_int silk_Encode(void *encState, silk_EncControlStruct *encControl, const opus_res *samplesIn,
                     opus_int nSamplesIn, ec_enc *psRangeEnc, opus_int32 *nBytesOut,
                     const opus_int prefillFlag, int activity)
{
    (void)encState; (void)encControl; (void)samplesIn; (void)nSamplesIn; (void)psRangeEnc;
    (void)prefillFlag; (void)activity;
    *nBytesOut = 0;
    return SILK_ENC_INVALID_NUMBER_OF_CHANNELS_ERROR;
}
