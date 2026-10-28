#include "audio_stream.h"
#include <nds.h>
#include <maxmod9.h>
#include <string.h>
#include <stdlib.h>

/*
 * Audio streaming engine using maxmod's mm_stream API.
 *
 * Architecture:
 *   Main loop (audioStreamUpdate) → decodes audio into ring buffer
 *   maxmod timer callback         → copies from ring buffer to SPU output
 *
 * The ring buffer decouples decode timing from output timing, preventing
 * underruns when the decoder stalls briefly (e.g. SD card seek).
 * With 16 MB RAM in DSi mode, we can afford a generous buffer.
 */

#define RING_SIZE_SHIFT 16
#define RING_SIZE       (1 << RING_SIZE_SHIFT)   /* 65536 stereo samples (~1.5s at 44.1kHz) */
#define RING_MASK       (RING_SIZE - 1)

#define DECODE_CHUNK    1024   /* samples per decode iteration */
#define STREAM_BUF_LEN  2048   /* maxmod stream buffer length */

static s16 ringBuffer[RING_SIZE * 2] ALIGN(4);
static volatile u32 ringWritePos;
static volatile u32 ringReadPos;

static s16 decodeBuf[DECODE_CHUNK * 2] ALIGN(4);
static s16 resampleBuf[DECODE_CHUNK * 2 * 2] ALIGN(4);

static Decoder currentDecoder;
static StreamState streamState;
static u8 currentVolume = 127;
static bool streamOpen;
static char currentPath[512];
static bool decoderEof;

static u32 outputSampleRate;

static inline u32 ringAvailable(void) {
    return (ringWritePos - ringReadPos) & RING_MASK;
}

static inline u32 ringFree(void) {
    return RING_SIZE - 1 - ringAvailable();
}

static void ringClear(void) {
    ringWritePos = 0;
    ringReadPos = 0;
}

static void ringWrite(const s16 *data, u32 numSamples) {
    u32 pos = ringWritePos & RING_MASK;
    u32 first = RING_SIZE - pos;
    if (first > numSamples) first = numSamples;

    memcpy(&ringBuffer[pos * 2], data, first * 2 * sizeof(s16));
    if (numSamples > first) {
        memcpy(&ringBuffer[0], data + first * 2, (numSamples - first) * 2 * sizeof(s16));
    }
    ringWritePos += numSamples;
}

static u32 ringRead(s16 *dest, u32 numSamples) {
    u32 avail = ringAvailable();
    if (numSamples > avail) numSamples = avail;
    if (numSamples == 0) return 0;

    u32 pos = ringReadPos & RING_MASK;
    u32 first = RING_SIZE - pos;
    if (first > numSamples) first = numSamples;

    memcpy(dest, &ringBuffer[pos * 2], first * 2 * sizeof(s16));
    if (numSamples > first) {
        memcpy(dest + first * 2, &ringBuffer[0], (numSamples - first) * 2 * sizeof(s16));
    }
    ringReadPos += numSamples;
    return numSamples;
}

/*
 * maxmod stream callback — runs in timer interrupt context.
 * Must be fast: just copies from the ring buffer, no decoding here.
 */
static mm_word streamCallback(mm_word length, mm_addr dest, mm_stream_formats format) {
    (void)format;
    s16 *out = (s16 *)dest;
    u32 got = ringRead(out, length);

    /* If underrun, fill remainder with silence */
    if (got < length) {
        memset(out + got * 2, 0, (length - got) * 2 * sizeof(s16));
    }
    return length;
}

/* Simple linear resampler for source rates != output rate */
static u32 resamplePos;
static u32 resampleFrac;
static s16 resamplePrev[2];

static void resampleInit(void) {
    resamplePos = 0;
    resampleFrac = 0;
    resamplePrev[0] = 0;
    resamplePrev[1] = 0;
}

/* Averaging decimator for exact integer ratios (e.g. 96→48kHz = 2:1).
   Sums N input samples per output sample — acts as a low-pass filter
   that prevents aliasing, unlike linear interpolation. */
static u32 decimateBlock(const s16 *src, u32 srcSamples, s16 *dest, u32 maxOut, u32 ratio) {
    u32 outCount = 0;
    u32 i = 0;
    while (i + ratio <= srcSamples && outCount < maxOut) {
        s32 sumL = 0, sumR = 0;
        for (u32 j = 0; j < ratio; j++) {
            sumL += src[(i + j) * 2];
            sumR += src[(i + j) * 2 + 1];
        }
        dest[outCount * 2]     = (s16)(sumL / (s32)ratio);
        dest[outCount * 2 + 1] = (s16)(sumR / (s32)ratio);
        outCount++;
        i += ratio;
    }
    return outCount;
}

/* General-purpose linear resampler for non-integer ratios.
   Returns number of output samples written. */
static u32 resampleBlock(const s16 *src, u32 srcSamples, s16 *dest, u32 maxOut,
                          u32 srcRate, u32 dstRate) {
    if (srcRate == dstRate) {
        u32 n = (srcSamples < maxOut) ? srcSamples : maxOut;
        memcpy(dest, src, n * 2 * sizeof(s16));
        return n;
    }

    /* Use fast averaging decimator for exact integer ratios */
    if (srcRate > dstRate && (srcRate % dstRate) == 0) {
        return decimateBlock(src, srcSamples, dest, maxOut, srcRate / dstRate);
    }

    u32 step = (srcRate << 16) / dstRate;
    u32 outCount = 0;

    while (outCount < maxOut) {
        u32 idx = resampleFrac >> 16;
        if (idx >= srcSamples) {
            resampleFrac -= (srcSamples << 16);
            break;
        }

        u32 frac = (resampleFrac & 0xFFFF);
        s16 l0, r0, l1, r1;

        if (idx == 0 && resampleFrac < (1 << 16)) {
            l0 = resamplePrev[0];
            r0 = resamplePrev[1];
        } else {
            l0 = src[(idx > 0 ? idx - 1 : 0) * 2];
            r0 = src[(idx > 0 ? idx - 1 : 0) * 2 + 1];
        }
        l1 = src[idx * 2];
        r1 = src[idx * 2 + 1];

        dest[outCount * 2]     = l0 + (s16)(((s32)(l1 - l0) * (s32)frac) >> 16);
        dest[outCount * 2 + 1] = r0 + (s16)(((s32)(r1 - r0) * (s32)frac) >> 16);
        outCount++;
        resampleFrac += step;
    }

    if (srcSamples > 0) {
        resamplePrev[0] = src[(srcSamples - 1) * 2];
        resamplePrev[1] = src[(srcSamples - 1) * 2 + 1];
    }
    return outCount;
}

void audioStreamInit(void) {
    /* Empty soundbank header: 0 modules, 0 samples */
    static const mm_word emptySoundbank[] = { 0, 0 };
    mmInitDefaultMem((const void *)emptySoundbank);

    streamState = STREAM_STOPPED;
    streamOpen = false;
    currentVolume = 127;
    outputSampleRate = 44100;
    ringClear();
    memset(&currentDecoder, 0, sizeof(currentDecoder));
}

static void openMmStream(u32 sampleRate) {
    if (streamOpen) {
        mmStreamClose();
        streamOpen = false;
    }

    outputSampleRate = sampleRate;
    if (outputSampleRate > 48000) outputSampleRate = 32000;

    mm_stream stream;
    stream.sampling_rate = outputSampleRate;
    stream.buffer_length = STREAM_BUF_LEN;
    stream.callback = streamCallback;
    stream.format = MM_STREAM_16BIT_STEREO;
    stream.timer = MM_TIMER0;
    stream.manual = false;
    mmStreamOpen(&stream);
    streamOpen = true;
}

static void closeMmStream(void) {
    if (streamOpen) {
        mmStreamClose();
        streamOpen = false;
    }
}

bool audioStreamOpen(const char *path) {
    audioStreamStop();

    strncpy(currentPath, path, sizeof(currentPath) - 1);
    currentPath[sizeof(currentPath) - 1] = '\0';

    streamState = STREAM_LOADING;

    if (!decoderInit(&currentDecoder, path)) {
        streamState = STREAM_STOPPED;
        return false;
    }

    ringClear();
    resampleInit();

    /* Pre-fill ring buffer before starting playback */
    for (int i = 0; i < 32; i++) {
        u32 got = 0;
        if (ringFree() < DECODE_CHUNK) break;
        DecodeResult res = currentDecoder.readSamples(&currentDecoder, decodeBuf, DECODE_CHUNK, &got);
        if (got > 0) {
            if (currentDecoder.info.sampleRate != outputSampleRate &&
                currentDecoder.info.sampleRate != 0) {
                u32 resOut = resampleBlock(decodeBuf, got, resampleBuf,
                                           DECODE_CHUNK * 2,
                                           currentDecoder.info.sampleRate,
                                           outputSampleRate);
                ringWrite(resampleBuf, resOut);
            } else {
                ringWrite(decodeBuf, got);
            }
        }
        if (res == DECODE_EOF) break;
    }

    u32 targetRate = currentDecoder.info.sampleRate;
    if (targetRate == 0) targetRate = 44100;
    if (targetRate > 48000) targetRate = 32000;

    if (!streamOpen || targetRate != outputSampleRate) {
        openMmStream(targetRate);
        resampleInit();
    }

    decoderEof = false;
    streamState = STREAM_PLAYING;
    return true;
}

bool audioStreamOpenGapless(const char *path) {
    if (currentDecoder.close) {
        decoderCleanup(&currentDecoder);
    }

    strncpy(currentPath, path, sizeof(currentPath) - 1);
    currentPath[sizeof(currentPath) - 1] = '\0';

    if (!decoderInit(&currentDecoder, path)) {
        decoderEof = true;
        return false;
    }

    u32 newRate = currentDecoder.info.sampleRate;
    if (newRate == 0) newRate = 44100;
    if (newRate > 48000) newRate = 32000;

    if (newRate != outputSampleRate) {
        resampleInit();
    }

    decoderEof = false;
    return true;
}

void audioStreamPlay(void) {
    if (streamState == STREAM_PAUSED) {
        if (!streamOpen) {
            openMmStream(outputSampleRate);
        }
        streamState = STREAM_PLAYING;
    }
}

void audioStreamPause(void) {
    if (streamState == STREAM_PLAYING) {
        closeMmStream();
        streamState = STREAM_PAUSED;
    }
}

void audioStreamStop(void) {
    closeMmStream();
    if (currentDecoder.close) {
        decoderCleanup(&currentDecoder);
    }
    ringClear();
    resampleInit();
    streamState = STREAM_STOPPED;
    currentPath[0] = '\0';
}

void audioStreamUpdate(void) {
    if (streamState != STREAM_PLAYING) return;

    /* Decode multiple chunks per frame to keep the ring buffer well-fed.
       Critical for high sample rates (96kHz) where one chunk per frame
       barely keeps up with real-time playback. */
    int maxIter = 32;
    DecodeResult res = DECODE_OK;

    while (maxIter-- > 0) {
        u32 freeSpace = ringFree();
        if (freeSpace < DECODE_CHUNK) break;

        u32 got = 0;
        res = currentDecoder.readSamples(&currentDecoder, decodeBuf, DECODE_CHUNK, &got);

        if (got > 0) {
            u32 srcRate = currentDecoder.info.sampleRate;
            if (srcRate != outputSampleRate && srcRate != 0) {
                u32 resOut = resampleBlock(decodeBuf, got, resampleBuf,
                                           DECODE_CHUNK * 2,
                                           srcRate, outputSampleRate);
                ringWrite(resampleBuf, resOut);
            } else {
                ringWrite(decodeBuf, got);
            }
        }

        if (res == DECODE_EOF) break;
    }

    if (res == DECODE_EOF) {
        decoderEof = true;
        if (ringAvailable() == 0) {
            streamState = STREAM_STOPPED;
        }
    }
}

bool audioStreamIsDecoderDone(void) {
    return decoderEof;
}

void audioStreamSetVolume(u8 volume) {
    currentVolume = volume;
    /* maxmod doesn't expose per-stream volume directly;
       we scale samples in the callback instead if needed,
       or use the hardware channel volume via mmSetModuleVolume */
}

u8 audioStreamGetVolume(void) {
    return currentVolume;
}

bool audioStreamSeek(u32 samplePos) {
    if (streamState == STREAM_STOPPED) return false;
    DecodeResult res = currentDecoder.seek(&currentDecoder, samplePos);
    if (res != DECODE_OK) return false;
    ringClear();
    resampleInit();
    return true;
}

bool audioStreamSeekMs(u32 ms) {
    if (currentDecoder.info.sampleRate == 0) return false;
    u32 sample = (u32)((u64)ms * currentDecoder.info.sampleRate / 1000);
    return audioStreamSeek(sample);
}

StreamState audioStreamGetState(void) {
    return streamState;
}

const AudioInfo *audioStreamGetInfo(void) {
    return &currentDecoder.info;
}

const AudioMeta *audioStreamGetMeta(void) {
    return &currentDecoder.meta;
}

u32 audioStreamGetPosition(void) {
    if (currentDecoder.tell)
        return currentDecoder.tell(&currentDecoder);
    return 0;
}

u32 audioStreamGetPositionMs(void) {
    u32 pos = audioStreamGetPosition();
    u32 rate = currentDecoder.info.sampleRate;
    if (rate == 0) return 0;
    u32 buffered = ringAvailable();
    if (pos > buffered)
        pos -= buffered;
    else
        pos = 0;
    return (u32)((u64)pos * 1000 / rate);
}

u32 audioStreamGetDurationMs(void) {
    return currentDecoder.info.durationMs;
}

const char *audioStreamGetFilePath(void) {
    return currentPath;
}

u32 audioStreamRingAvailable(void) {
    return ringAvailable();
}

u32 audioStreamPeekSamples(s16 *dest, u32 numSamples) {
    u32 avail = ringAvailable();
    if (numSamples > avail) numSamples = avail;
    if (numSamples == 0) return 0;

    u32 startPos = (ringWritePos - numSamples) & RING_MASK;
    u32 first = RING_SIZE - startPos;
    if (first > numSamples) first = numSamples;

    memcpy(dest, &ringBuffer[startPos * 2], first * 2 * sizeof(s16));
    if (numSamples > first) {
        memcpy(dest + first * 2, &ringBuffer[0], (numSamples - first) * 2 * sizeof(s16));
    }
    return numSamples;
}

u32 audioStreamPeekPlayback(s16 *dest, u32 numSamples) {
    u32 avail = ringAvailable();
    if (numSamples > avail) numSamples = avail;
    if (numSamples == 0) return 0;

    u32 startPos = ringReadPos & RING_MASK;
    u32 first = RING_SIZE - startPos;
    if (first > numSamples) first = numSamples;

    memcpy(dest, &ringBuffer[startPos * 2], first * 2 * sizeof(s16));
    if (numSamples > first) {
        memcpy(dest + first * 2, &ringBuffer[0], (numSamples - first) * 2 * sizeof(s16));
    }
    return numSamples;
}
