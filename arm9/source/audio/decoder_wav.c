#include "decoder.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    u32 dataOffset;
    u32 dataSize;
    u32 bytesPerSample;
    u32 blockAlign;
    u32 currentSample;
} WavContext;

static u32 readU32LE(FILE *f) {
    u8 b[4];
    fread(b, 1, 4, f);
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
}

static u16 readU16LE(FILE *f) {
    u8 b[2];
    fread(b, 1, 2, f);
    return b[0] | (b[1] << 8);
}

static void parseInfoChunk(Decoder *dec, FILE *f, u32 chunkSize) {
    u32 end = ftell(f) + chunkSize;
    while ((u32)ftell(f) + 8 <= end) {
        u8 tag[4];
        fread(tag, 1, 4, f);
        u32 sz = readU32LE(f);
        u32 readSz = (sz > META_MAX_LEN - 1) ? META_MAX_LEN - 1 : sz;

        if (memcmp(tag, "INAM", 4) == 0) {
            fread(dec->meta.title, 1, readSz, f);
            dec->meta.title[readSz] = '\0';
        } else if (memcmp(tag, "IART", 4) == 0) {
            fread(dec->meta.artist, 1, readSz, f);
            dec->meta.artist[readSz] = '\0';
        } else if (memcmp(tag, "IPRD", 4) == 0) {
            fread(dec->meta.album, 1, readSz, f);
            dec->meta.album[readSz] = '\0';
        } else {
            fseek(f, sz, SEEK_CUR);
            continue;
        }
        if (readSz < sz) fseek(f, sz - readSz, SEEK_CUR);
        if (sz & 1) fseek(f, 1, SEEK_CUR);
    }
}

static DecodeResult wavOpen(Decoder *dec, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return DECODE_ERROR;

    u8 header[12];
    if (fread(header, 1, 12, f) != 12 ||
        memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVE", 4) != 0) {
        fclose(f);
        return DECODE_ERROR;
    }

    WavContext *ctx = calloc(1, sizeof(WavContext));
    if (!ctx) { fclose(f); return DECODE_ERROR; }

    bool foundFmt = false;
    bool foundData = false;

    while (!foundData) {
        u8 chunkId[4];
        if (fread(chunkId, 1, 4, f) != 4) break;
        u32 chunkSize = readU32LE(f);

        /* fseek takes a signed long, so a chunkSize above LONG_MAX seeks
           backwards and re-reads this same chunk forever. */
        if (chunkSize > 0x7FFFFFFFu) break;

        if (memcmp(chunkId, "fmt ", 4) == 0) {
            u16 audioFmt = readU16LE(f);
            if (audioFmt != 1) { // PCM only
                free(ctx); fclose(f);
                return DECODE_UNSUPPORTED;
            }
            dec->info.channels = readU16LE(f);
            dec->info.sampleRate = readU32LE(f);
            /*byteRate*/ readU32LE(f);
            ctx->blockAlign = readU16LE(f);
            dec->info.bitsPerSample = readU16LE(f);
            ctx->bytesPerSample = dec->info.bitsPerSample / 8;

            if (dec->info.channels < 1 || dec->info.channels > 2 ||
                (dec->info.bitsPerSample != 8 && dec->info.bitsPerSample != 16 &&
                 dec->info.bitsPerSample != 24)) {
                free(ctx); fclose(f);
                return DECODE_UNSUPPORTED;
            }
            if (chunkSize > 16) fseek(f, chunkSize - 16, SEEK_CUR);
            foundFmt = true;

        } else if (memcmp(chunkId, "data", 4) == 0) {
            if (!foundFmt || ctx->blockAlign == 0) {
                free(ctx); fclose(f); return DECODE_ERROR;
            }
            ctx->dataOffset = ftell(f);
            ctx->dataSize = chunkSize;
            dec->info.totalSamples = chunkSize / ctx->blockAlign;
            if (dec->info.sampleRate > 0) {
                dec->info.durationMs = (u32)((u64)dec->info.totalSamples * 1000 / dec->info.sampleRate);
            }
            foundData = true;

        } else if (memcmp(chunkId, "LIST", 4) == 0) {
            if (chunkSize < 4) break;   /* chunkSize - 4 would underflow */
            u8 listType[4];
            if (fread(listType, 1, 4, f) != 4) break;
            if (memcmp(listType, "INFO", 4) == 0) {
                parseInfoChunk(dec, f, chunkSize - 4);
            } else {
                fseek(f, chunkSize - 4, SEEK_CUR);
            }

        } else {
            fseek(f, chunkSize, SEEK_CUR);
        }
        if (chunkSize & 1) fseek(f, 1, SEEK_CUR);
    }

    if (!foundData) {
        free(ctx); fclose(f);
        return DECODE_ERROR;
    }

    ctx->currentSample = 0;
    dec->ctx = ctx;
    dec->file = f;
    return DECODE_OK;
}

static DecodeResult wavRead(Decoder *dec, s16 *outBuf, u32 numSamples, u32 *samplesRead) {
    WavContext *ctx = (WavContext *)dec->ctx;
    u32 remaining = dec->info.totalSamples - ctx->currentSample;
    if (remaining == 0) { *samplesRead = 0; return DECODE_EOF; }
    if (numSamples > remaining) numSamples = remaining;

    u32 channels = dec->info.channels;
    *samplesRead = 0;

    if (dec->info.bitsPerSample == 16) {
        u32 bytesToRead = numSamples * channels * 2;
        u32 read = fread(outBuf, 1, bytesToRead, dec->file);
        u32 samplesGot = read / (channels * 2);
        if (channels == 1) {
            for (int i = samplesGot - 1; i >= 0; i--) {
                outBuf[i * 2 + 1] = outBuf[i];
                outBuf[i * 2]     = outBuf[i];
            }
        }
        *samplesRead = samplesGot;
    } else if (dec->info.bitsPerSample == 8) {
        u8 tmpBuf[1024];
        u32 done = 0;
        while (done < numSamples) {
            u32 batch = numSamples - done;
            u32 maxBatch = sizeof(tmpBuf) / channels;
            if (batch > maxBatch) batch = maxBatch;
            u32 read = fread(tmpBuf, channels, batch, dec->file);
            if (read == 0) break;
            for (u32 i = 0; i < read; i++) {
                s16 left  = ((s16)tmpBuf[i * channels] - 128) << 8;
                s16 right = (channels == 2) ?
                    ((s16)tmpBuf[i * channels + 1] - 128) << 8 : left;
                outBuf[(done + i) * 2]     = left;
                outBuf[(done + i) * 2 + 1] = right;
            }
            done += read;
        }
        *samplesRead = done;
    } else if (dec->info.bitsPerSample == 24) {
        u8 tmpBuf[1024 * 3];
        u32 done = 0;
        while (done < numSamples) {
            u32 batch = numSamples - done;
            u32 maxBatch = sizeof(tmpBuf) / (3 * channels);
            if (batch > maxBatch) batch = maxBatch;
            u32 read = fread(tmpBuf, 3 * channels, batch, dec->file);
            if (read == 0) break;
            for (u32 i = 0; i < read; i++) {
                u32 off = i * 3 * channels;
                s16 left  = (s16)((tmpBuf[off + 1]) | (tmpBuf[off + 2] << 8));
                s16 right;
                if (channels == 2) {
                    right = (s16)((tmpBuf[off + 4]) | (tmpBuf[off + 5] << 8));
                } else {
                    right = left;
                }
                outBuf[(done + i) * 2]     = left;
                outBuf[(done + i) * 2 + 1] = right;
            }
            done += read;
        }
        *samplesRead = done;
    }

    ctx->currentSample += *samplesRead;
    if (*samplesRead == 0) return DECODE_EOF;
    return DECODE_OK;
}

static DecodeResult wavSeek(Decoder *dec, u32 samplePos) {
    WavContext *ctx = (WavContext *)dec->ctx;
    if (samplePos >= dec->info.totalSamples) return DECODE_ERROR;
    u32 byteOffset = samplePos * ctx->blockAlign;
    fseek(dec->file, ctx->dataOffset + byteOffset, SEEK_SET);
    ctx->currentSample = samplePos;
    return DECODE_OK;
}

static u32 wavTell(Decoder *dec) {
    WavContext *ctx = (WavContext *)dec->ctx;
    if (!ctx) return 0;
    return ctx->currentSample;
}

static void wavClose(Decoder *dec) {
    if (dec->ctx) { free(dec->ctx); dec->ctx = NULL; }
    if (dec->file) { fclose(dec->file); dec->file = NULL; }
}

void decoderInitWav(Decoder *dec) {
    dec->open        = wavOpen;
    dec->readSamples = wavRead;
    dec->seek        = wavSeek;
    dec->tell        = wavTell;
    dec->close       = wavClose;
}
