/*
 * Ogg Vorbis decoder — STUB
 *
 * Full Vorbis support requires the Tremor library (integer-only Vorbis,
 * BSD licensed) from Xiph.org. Tremor is the correct choice for ARM9
 * without FPU — it uses fixed-point arithmetic throughout.
 *
 * At 133 MHz DSi-mode, Tremor should decode Vorbis q5-q8 (~160-256 kbps)
 * in real-time without issues.
 *
 * To integrate Tremor:
 *   1. Get source: https://gitlab.xiph.org/xiph/tremor
 *   2. Build as a static library for ARM9 using devkitARM
 *   3. Implement the functions below using ov_open_callbacks(),
 *      ov_read(), ov_pcm_seek(), ov_pcm_tell(), ov_clear()
 *   4. Add -ltremor to LIBS in arm9/Makefile
 *
 * This stub returns DECODE_UNSUPPORTED so the player gracefully skips
 * Ogg files with an error message rather than crashing.
 */

#include "decoder.h"
#include <string.h>

static DecodeResult vorbisOpen(Decoder *dec, const char *path) {
    (void)dec; (void)path;
    return DECODE_UNSUPPORTED;
}

static DecodeResult vorbisRead(Decoder *dec, s16 *outBuf, u32 numSamples, u32 *samplesRead) {
    (void)dec; (void)outBuf; (void)numSamples;
    *samplesRead = 0;
    return DECODE_UNSUPPORTED;
}

static DecodeResult vorbisSeek(Decoder *dec, u32 samplePos) {
    (void)dec; (void)samplePos;
    return DECODE_UNSUPPORTED;
}

static u32 vorbisTell(Decoder *dec) {
    (void)dec;
    return 0;
}

static void vorbisClose(Decoder *dec) {
    (void)dec;
}

void decoderInitVorbis(Decoder *dec) {
    dec->open        = vorbisOpen;
    dec->readSamples = vorbisRead;
    dec->seek        = vorbisSeek;
    dec->tell        = vorbisTell;
    dec->close       = vorbisClose;
}
