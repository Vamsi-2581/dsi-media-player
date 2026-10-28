/*
 * MP3 decoder — wraps Helix fixed-point MP3 decoder
 *
 * Helix uses only integer math (no float), making it dramatically
 * faster than minimp3 on ARM9 without FPU. Designed for embedded ARM.
 */

#include "mp3dec.h"
#include "decoder.h"
#include <stdlib.h>
#include <string.h>

#define MP3_INBUF_SIZE  (2 * MAINBUF_SIZE)
#define MP3_READ_SIZE   1024

typedef struct {
    HMP3Decoder     hDecoder;
    u8              inBuf[MP3_INBUF_SIZE];
    int             inBufBytes;
    int             inBufReadOfs;
    u32             fileSize;
    u32             fileOffset;
    s16             pcmBuf[MAX_NGRAN * MAX_NSAMP * MAX_NCHAN];
    u32             pcmBufSamples;
    u32             pcmBufPos;
    u32             currentSample;
    u32             firstFrameOffset;
    int             channels;
} HelixMp3Context;

static void parseId3v1(Decoder *dec, FILE *f) {
    long saved = ftell(f);
    fseek(f, -128, SEEK_END);
    u8 tag[128];
    if (fread(tag, 1, 128, f) != 128 || memcmp(tag, "TAG", 3) != 0) {
        fseek(f, saved, SEEK_SET);
        return;
    }

    char buf[31];

    memcpy(buf, tag + 3, 30); buf[30] = '\0';
    for (int i = 29; i >= 0 && buf[i] == ' '; i--) buf[i] = '\0';
    if (buf[0] && !dec->meta.title[0])
        strncpy(dec->meta.title, buf, META_MAX_LEN - 1);

    memcpy(buf, tag + 33, 30); buf[30] = '\0';
    for (int i = 29; i >= 0 && buf[i] == ' '; i--) buf[i] = '\0';
    if (buf[0] && !dec->meta.artist[0])
        strncpy(dec->meta.artist, buf, META_MAX_LEN - 1);

    memcpy(buf, tag + 63, 30); buf[30] = '\0';
    for (int i = 29; i >= 0 && buf[i] == ' '; i--) buf[i] = '\0';
    if (buf[0] && !dec->meta.album[0])
        strncpy(dec->meta.album, buf, META_MAX_LEN - 1);

    memcpy(buf, tag + 93, 4); buf[4] = '\0';
    for (int i = 3; i >= 0 && buf[i] == ' '; i--) buf[i] = '\0';
    if (buf[0] && !dec->meta.year[0])
        strncpy(dec->meta.year, buf, 7);

    fseek(f, saved, SEEK_SET);
}

static u32 parseId3v2(Decoder *dec, FILE *f) {
    long saved = ftell(f);
    fseek(f, 0, SEEK_SET);

    u8 header[10];
    if (fread(header, 1, 10, f) != 10 || memcmp(header, "ID3", 3) != 0) {
        fseek(f, saved, SEEK_SET);
        return 0;
    }

    u32 tagSize = ((header[6] & 0x7F) << 21) |
                  ((header[7] & 0x7F) << 14) |
                  ((header[8] & 0x7F) << 7)  |
                   (header[9] & 0x7F);
    u32 tagEnd = 10 + tagSize;

    u8 majorVer = header[3];

    while ((u32)ftell(f) + 10 <= tagEnd) {
        u8 frameHeader[10];
        if (fread(frameHeader, 1, 10, f) != 10) break;
        if (frameHeader[0] == 0) break;

        u32 frameSize;
        if (majorVer >= 4) {
            frameSize = ((frameHeader[4] & 0x7F) << 21) |
                        ((frameHeader[5] & 0x7F) << 14) |
                        ((frameHeader[6] & 0x7F) << 7)  |
                         (frameHeader[7] & 0x7F);
        } else {
            frameSize = (frameHeader[4] << 24) | (frameHeader[5] << 16) |
                        (frameHeader[6] << 8)  |  frameHeader[7];
        }

        if (frameSize == 0 || frameSize > tagSize) break;

        char *dest = NULL;
        u32 maxLen = META_MAX_LEN - 1;

        if (memcmp(frameHeader, "TIT2", 4) == 0) dest = dec->meta.title;
        else if (memcmp(frameHeader, "TPE1", 4) == 0) dest = dec->meta.artist;
        else if (memcmp(frameHeader, "TALB", 4) == 0) dest = dec->meta.album;
        else if (memcmp(frameHeader, "TRCK", 4) == 0) { dest = dec->meta.trackNum; maxLen = 7; }
        else if (memcmp(frameHeader, "TDRC", 4) == 0 || memcmp(frameHeader, "TYER", 4) == 0)
            { dest = dec->meta.year; maxLen = 7; }
        else if (memcmp(frameHeader, "TCON", 4) == 0) dest = dec->meta.genre;
        else if (memcmp(frameHeader, "APIC", 4) == 0) {
            dec->meta.hasArt = true;
            dec->meta.artOffset = ftell(f);
            dec->meta.artSize = frameSize;
            fseek(f, frameSize, SEEK_CUR);
            continue;
        }

        if (dest && frameSize > 1) {
            u8 encoding = 0;
            fread(&encoding, 1, 1, f);
            u32 textLen = frameSize - 1;
            if (textLen > maxLen) textLen = maxLen;

            if (encoding == 0 || encoding == 3) {
                fread(dest, 1, textLen, f);
                dest[textLen] = '\0';
                if (textLen < frameSize - 1) fseek(f, frameSize - 1 - textLen, SEEK_CUR);
            } else {
                fseek(f, frameSize - 1, SEEK_CUR);
            }
        } else {
            fseek(f, frameSize, SEEK_CUR);
        }
    }

    fseek(f, saved, SEEK_SET);
    return tagEnd;
}

static int refillBuffer(HelixMp3Context *ctx, FILE *f) {
    /* Move remaining bytes to front */
    int remaining = ctx->inBufBytes - ctx->inBufReadOfs;
    if (remaining > 0 && ctx->inBufReadOfs > 0) {
        memmove(ctx->inBuf, ctx->inBuf + ctx->inBufReadOfs, remaining);
    }
    ctx->inBufReadOfs = 0;
    ctx->inBufBytes = remaining;

    /* Read more from file */
    int toRead = MP3_INBUF_SIZE - remaining;
    int fileRemaining = ctx->fileSize - ctx->fileOffset;
    if (toRead > fileRemaining) toRead = fileRemaining;

    if (toRead > 0) {
        int got = fread(ctx->inBuf + remaining, 1, toRead, f);
        ctx->inBufBytes += got;
        ctx->fileOffset += got;
    }
    return ctx->inBufBytes;
}

static DecodeResult mp3Open(Decoder *dec, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return DECODE_ERROR;

    HelixMp3Context *ctx = calloc(1, sizeof(HelixMp3Context));
    if (!ctx) { fclose(f); return DECODE_ERROR; }

    fseek(f, 0, SEEK_END);
    ctx->fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    dec->file = f;
    dec->ctx = ctx;

    u32 id3v2End = parseId3v2(dec, f);
    parseId3v1(dec, f);

    ctx->firstFrameOffset = id3v2End;
    fseek(f, ctx->firstFrameOffset, SEEK_SET);
    ctx->fileOffset = ctx->firstFrameOffset;

    ctx->hDecoder = MP3InitDecoder();
    if (!ctx->hDecoder) {
        free(ctx);
        fclose(f);
        dec->ctx = NULL;
        dec->file = NULL;
        return DECODE_ERROR;
    }

    refillBuffer(ctx, f);

    /* Find and decode first frame to get format info */
    int offset = MP3FindSyncWord(ctx->inBuf, ctx->inBufBytes);
    if (offset < 0) {
        MP3FreeDecoder(ctx->hDecoder);
        free(ctx);
        fclose(f);
        dec->ctx = NULL;
        dec->file = NULL;
        return DECODE_ERROR;
    }
    ctx->inBufReadOfs = offset;

    MP3FrameInfo frameInfo;
    if (MP3GetNextFrameInfo(ctx->hDecoder, &frameInfo, ctx->inBuf + ctx->inBufReadOfs) != 0) {
        MP3FreeDecoder(ctx->hDecoder);
        free(ctx);
        fclose(f);
        dec->ctx = NULL;
        dec->file = NULL;
        return DECODE_ERROR;
    }

    dec->info.sampleRate = frameInfo.samprate;
    dec->info.channels = frameInfo.nChans;
    dec->info.bitsPerSample = 16;
    ctx->channels = frameInfo.nChans;

    if (frameInfo.bitrate > 0) {
        u32 audioBytes = ctx->fileSize - ctx->firstFrameOffset;
        dec->info.durationMs = (u32)((u64)audioBytes * 8 * 1000 / (u64)frameInfo.bitrate);
        dec->info.totalSamples = (u32)((u64)dec->info.durationMs * frameInfo.samprate / 1000);
    }

    /* Decode first frame */
    unsigned char *readPtr = ctx->inBuf + ctx->inBufReadOfs;
    int bytesLeft = ctx->inBufBytes - ctx->inBufReadOfs;
    int err = MP3Decode(ctx->hDecoder, &readPtr, &bytesLeft, ctx->pcmBuf, 0);
    ctx->inBufReadOfs = ctx->inBufBytes - bytesLeft;

    if (err == 0) {
        MP3GetLastFrameInfo(ctx->hDecoder, &frameInfo);
        ctx->pcmBufSamples = frameInfo.outputSamps / frameInfo.nChans;
        ctx->pcmBufPos = 0;
    }

    return DECODE_OK;
}

static DecodeResult mp3Read(Decoder *dec, s16 *outBuf, u32 numSamples, u32 *samplesRead) {
    HelixMp3Context *ctx = (HelixMp3Context *)dec->ctx;
    u32 written = 0;

    while (written < numSamples) {
        /* Drain buffered PCM first */
        if (ctx->pcmBufPos < ctx->pcmBufSamples) {
            u32 avail = ctx->pcmBufSamples - ctx->pcmBufPos;
            u32 need = numSamples - written;
            u32 copy = (avail < need) ? avail : need;

            if (ctx->channels == 2) {
                memcpy(outBuf + written * 2,
                       ctx->pcmBuf + ctx->pcmBufPos * 2,
                       copy * 2 * sizeof(s16));
            } else {
                for (u32 i = 0; i < copy; i++) {
                    s16 s = ctx->pcmBuf[ctx->pcmBufPos + i];
                    outBuf[(written + i) * 2]     = s;
                    outBuf[(written + i) * 2 + 1] = s;
                }
            }
            ctx->pcmBufPos += copy;
            written += copy;
            ctx->currentSample += copy;
            continue;
        }

        /* Need more data? */
        int available = ctx->inBufBytes - ctx->inBufReadOfs;
        if (available < MP3_READ_SIZE) {
            if (refillBuffer(ctx, dec->file) == 0) break;
            available = ctx->inBufBytes - ctx->inBufReadOfs;
        }

        /* Find next sync word */
        int offset = MP3FindSyncWord(ctx->inBuf + ctx->inBufReadOfs, available);
        if (offset < 0) {
            /* Whole file consumed and no sync word left: what remains is a
               trailing non-audio tag (ID3v1, Lyrics3, APE). refillBuffer
               cannot advance here — it keeps returning the same buffered
               bytes — so bailing is the only way out of this loop. */
            if (ctx->fileOffset >= ctx->fileSize) break;
            if (refillBuffer(ctx, dec->file) == 0) break;
            continue;
        }
        ctx->inBufReadOfs += offset;

        /* Decode frame */
        unsigned char *readPtr = ctx->inBuf + ctx->inBufReadOfs;
        int bytesLeft = ctx->inBufBytes - ctx->inBufReadOfs;
        int err = MP3Decode(ctx->hDecoder, &readPtr, &bytesLeft, ctx->pcmBuf, 0);
        ctx->inBufReadOfs = ctx->inBufBytes - bytesLeft;

        if (err == 0) {
            MP3FrameInfo info;
            MP3GetLastFrameInfo(ctx->hDecoder, &info);
            ctx->pcmBufSamples = info.outputSamps / info.nChans;
            ctx->pcmBufPos = 0;
        } else if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            if (refillBuffer(ctx, dec->file) == 0) break;
        } else {
            /* Skip bad frame */
            ctx->inBufReadOfs++;
        }
    }

    *samplesRead = written;
    return (written == 0) ? DECODE_EOF : DECODE_OK;
}

static DecodeResult mp3Seek(Decoder *dec, u32 samplePos) {
    HelixMp3Context *ctx = (HelixMp3Context *)dec->ctx;

    if (samplePos == 0 || samplePos < ctx->currentSample) {
        fseek(dec->file, ctx->firstFrameOffset, SEEK_SET);
        ctx->fileOffset = ctx->firstFrameOffset;
        ctx->inBufBytes = 0;
        ctx->inBufReadOfs = 0;
        ctx->pcmBufSamples = 0;
        ctx->pcmBufPos = 0;
        ctx->currentSample = 0;
        MP3FreeDecoder(ctx->hDecoder);
        ctx->hDecoder = MP3InitDecoder();
        refillBuffer(ctx, dec->file);
    }

    static s16 seekTmpBuf[MAX_NSAMP * MAX_NCHAN * 2];
    while (ctx->currentSample < samplePos) {
        u32 skip = samplePos - ctx->currentSample;
        u32 got;
        DecodeResult res = mp3Read(dec, seekTmpBuf, (skip > 1152) ? 1152 : skip, &got);
        if (res == DECODE_EOF || got == 0) return DECODE_ERROR;
    }

    return DECODE_OK;
}

static u32 mp3Tell(Decoder *dec) {
    HelixMp3Context *ctx = (HelixMp3Context *)dec->ctx;
    if (!ctx) return 0;
    return ctx->currentSample;
}

static void mp3Close(Decoder *dec) {
    HelixMp3Context *ctx = (HelixMp3Context *)dec->ctx;
    if (ctx) {
        if (ctx->hDecoder) MP3FreeDecoder(ctx->hDecoder);
        free(ctx);
        dec->ctx = NULL;
    }
    if (dec->file) { fclose(dec->file); dec->file = NULL; }
}

void decoderInitMp3(Decoder *dec) {
    dec->open        = mp3Open;
    dec->readSamples = mp3Read;
    dec->seek        = mp3Seek;
    dec->tell        = mp3Tell;
    dec->close       = mp3Close;
}
