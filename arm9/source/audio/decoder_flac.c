/*
 * FLAC decoder — wraps dr_flac (public domain, single-header)
 *
 * dr_flac is integer-only, making it ideal for ARM9 without FPU.
 * At 133 MHz DSi-mode clock, 16-bit/44.1-48 kHz decodes with comfortable
 * margin. 24-bit/96 kHz is best-effort (mainly limited by SD I/O).
 *
 * Download dr_flac.h from: https://github.com/mackron/dr_libs
 * Place in: libs/dr_flac.h
 */

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#include "dr_flac.h"

#include "decoder.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    drflac *flac;
    u32 currentSample;
} FlacContext;

static size_t flacReadCb(void *pUserData, void *pBufferOut, size_t bytesToRead) {
    FILE *f = (FILE *)pUserData;
    return fread(pBufferOut, 1, bytesToRead, f);
}

static drflac_bool32 flacSeekCb(void *pUserData, int offset, drflac_seek_origin origin) {
    FILE *f = (FILE *)pUserData;
    int whence;
    switch (origin) {
        case DRFLAC_SEEK_SET: whence = SEEK_SET; break;
        case DRFLAC_SEEK_CUR: whence = SEEK_CUR; break;
        case DRFLAC_SEEK_END: whence = SEEK_END; break;
        default: return DRFLAC_FALSE;
    }
    return fseek(f, offset, whence) == 0;
}

static drflac_bool32 flacTellCb(void *pUserData, drflac_int64 *pCursor) {
    FILE *f = (FILE *)pUserData;
    long pos = ftell(f);
    if (pos < 0) return DRFLAC_FALSE;
    *pCursor = (drflac_int64)pos;
    return DRFLAC_TRUE;
}

static void parseFlacMeta(Decoder *dec, drflac *flac) {
    drflac_vorbis_comment_iterator iter;
    const drflac_metadata *tags = NULL;

    /* dr_flac exposes Vorbis comments via iteration after opening */
    /* We search through FLAC metadata for VORBIS_COMMENT blocks  */
    (void)flac;

    /* Parse comments from the flac context's internal metadata */
    /* dr_flac stores these during drflac_open; we access via the public fields */
    /* For a minimal implementation, we'll parse on a re-read of metadata */

    FILE *f = (FILE *)dec->file;
    long savedPos = ftell(f);

    fseek(f, 0, SEEK_SET);

    u8 magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "fLaC", 4) != 0) {
        fseek(f, savedPos, SEEK_SET);
        return;
    }

    bool lastBlock = false;
    while (!lastBlock) {
        u8 blockHeader[4];
        if (fread(blockHeader, 1, 4, f) != 4) break;

        lastBlock = (blockHeader[0] & 0x80) != 0;
        u8 blockType = blockHeader[0] & 0x7F;
        u32 blockLen = (blockHeader[1] << 16) | (blockHeader[2] << 8) | blockHeader[3];

        if (blockType == 4) { /* VORBIS_COMMENT */
            /* Vendor string */
            u8 lenBuf[4];
            if (fread(lenBuf, 1, 4, f) != 4) break;
            u32 vendorLen = lenBuf[0] | (lenBuf[1] << 8) | (lenBuf[2] << 16) | (lenBuf[3] << 24);
            fseek(f, vendorLen, SEEK_CUR);

            if (fread(lenBuf, 1, 4, f) != 4) break;
            u32 numComments = lenBuf[0] | (lenBuf[1] << 8) | (lenBuf[2] << 16) | (lenBuf[3] << 24);

            for (u32 i = 0; i < numComments && i < 64; i++) {
                if (fread(lenBuf, 1, 4, f) != 4) break;
                u32 commentLen = lenBuf[0] | (lenBuf[1] << 8) | (lenBuf[2] << 16) | (lenBuf[3] << 24);
                if (commentLen > 4096) { fseek(f, commentLen, SEEK_CUR); continue; }

                char comment[512];
                u32 readLen = (commentLen > sizeof(comment) - 1) ? sizeof(comment) - 1 : commentLen;
                fread(comment, 1, readLen, f);
                comment[readLen] = '\0';
                if (readLen < commentLen) fseek(f, commentLen - readLen, SEEK_CUR);

                char *eq = strchr(comment, '=');
                if (!eq) continue;
                *eq = '\0';
                char *key = comment;
                char *val = eq + 1;

                /* Case-insensitive key comparison */
                for (char *p = key; *p; p++) {
                    if (*p >= 'a' && *p <= 'z') *p -= 32;
                }

                u32 valLen = strlen(val);
                if (valLen >= META_MAX_LEN) valLen = META_MAX_LEN - 1;

                if (strcmp(key, "TITLE") == 0) {
                    memcpy(dec->meta.title, val, valLen); dec->meta.title[valLen] = '\0';
                } else if (strcmp(key, "ARTIST") == 0) {
                    memcpy(dec->meta.artist, val, valLen); dec->meta.artist[valLen] = '\0';
                } else if (strcmp(key, "ALBUM") == 0) {
                    memcpy(dec->meta.album, val, valLen); dec->meta.album[valLen] = '\0';
                } else if (strcmp(key, "TRACKNUMBER") == 0) {
                    u32 tLen = (valLen > 7) ? 7 : valLen;
                    memcpy(dec->meta.trackNum, val, tLen); dec->meta.trackNum[tLen] = '\0';
                } else if (strcmp(key, "DATE") == 0) {
                    u32 yLen = (valLen > 7) ? 7 : valLen;
                    memcpy(dec->meta.year, val, yLen); dec->meta.year[yLen] = '\0';
                } else if (strcmp(key, "GENRE") == 0) {
                    memcpy(dec->meta.genre, val, valLen); dec->meta.genre[valLen] = '\0';
                }
            }
        } else if (blockType == 6) { /* PICTURE */
            dec->meta.hasArt = true;
            dec->meta.artOffset = ftell(f);
            dec->meta.artSize = blockLen;
            fseek(f, blockLen, SEEK_CUR);
        } else {
            fseek(f, blockLen, SEEK_CUR);
        }
    }

    fseek(f, savedPos, SEEK_SET);
    (void)tags;
    (void)iter;
}

static DecodeResult flacOpen(Decoder *dec, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return DECODE_ERROR;

    drflac *flac = drflac_open(flacReadCb, flacSeekCb, flacTellCb, f, NULL);
    if (!flac) {
        fclose(f);
        return DECODE_ERROR;
    }

    FlacContext *ctx = calloc(1, sizeof(FlacContext));
    if (!ctx) {
        drflac_close(flac);
        fclose(f);
        return DECODE_ERROR;
    }

    /* The shared decode buffer is sized for stereo (DECODE_CHUNK * 2), but
       dr_flac writes frames * channels. More than 2 channels would run off
       the end of it, so refuse the file rather than corrupt memory. */
    if (flac->channels == 0 || flac->channels > 2) {
        free(ctx);
        drflac_close(flac);
        fclose(f);
        return DECODE_UNSUPPORTED;
    }

    ctx->flac = flac;
    ctx->currentSample = 0;

    dec->info.sampleRate = flac->sampleRate;
    dec->info.channels = flac->channels;
    dec->info.bitsPerSample = flac->bitsPerSample;
    dec->info.totalSamples = (u32)flac->totalPCMFrameCount;
    if (flac->sampleRate > 0) {
        dec->info.durationMs = (u32)((u64)flac->totalPCMFrameCount * 1000 / flac->sampleRate);
    }

    dec->ctx = ctx;
    dec->file = f;

    parseFlacMeta(dec, flac);

    return DECODE_OK;
}

static DecodeResult flacRead(Decoder *dec, s16 *outBuf, u32 numSamples, u32 *samplesRead) {
    FlacContext *ctx = (FlacContext *)dec->ctx;

    /* dr_flac_read_pcm_frames_s16 outputs interleaved s16 samples */
    drflac_uint64 framesRead = drflac_read_pcm_frames_s16(ctx->flac, numSamples, outBuf);

    if (dec->info.channels == 1) {
        /* Expand mono to stereo (in-place, backwards) */
        for (int i = (int)framesRead - 1; i >= 0; i--) {
            outBuf[i * 2 + 1] = outBuf[i];
            outBuf[i * 2]     = outBuf[i];
        }
    }

    ctx->currentSample += (u32)framesRead;
    *samplesRead = (u32)framesRead;

    return (framesRead == 0) ? DECODE_EOF : DECODE_OK;
}

static DecodeResult flacSeek(Decoder *dec, u32 samplePos) {
    FlacContext *ctx = (FlacContext *)dec->ctx;
    if (!drflac_seek_to_pcm_frame(ctx->flac, samplePos))
        return DECODE_ERROR;
    ctx->currentSample = samplePos;
    return DECODE_OK;
}

static u32 flacTell(Decoder *dec) {
    FlacContext *ctx = (FlacContext *)dec->ctx;
    if (!ctx) return 0;
    return ctx->currentSample;
}

static void flacClose(Decoder *dec) {
    FlacContext *ctx = (FlacContext *)dec->ctx;
    if (ctx) {
        if (ctx->flac) drflac_close(ctx->flac);
        free(ctx);
        dec->ctx = NULL;
    }
    if (dec->file) { fclose(dec->file); dec->file = NULL; }
}

void decoderInitFlac(Decoder *dec) {
    dec->open        = flacOpen;
    dec->readSamples = flacRead;
    dec->seek        = flacSeek;
    dec->tell        = flacTell;
    dec->close       = flacClose;
}
