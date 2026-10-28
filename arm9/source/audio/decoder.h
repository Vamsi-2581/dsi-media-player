#ifndef DECODER_H
#define DECODER_H

#include <nds/ndstypes.h>
#include <stdio.h>

typedef enum {
    DECODE_OK = 0,
    DECODE_EOF,
    DECODE_ERROR,
    DECODE_UNSUPPORTED
} DecodeResult;

typedef enum {
    FORMAT_UNKNOWN = 0,
    FORMAT_WAV,
    FORMAT_FLAC,
    FORMAT_MP3,
    FORMAT_VORBIS
} AudioFormat;

typedef struct {
    AudioFormat format;
    u32 sampleRate;
    u8  channels;
    u8  bitsPerSample;
    u32 totalSamples;
    u32 durationMs;
} AudioInfo;

#define META_MAX_LEN 128

typedef struct {
    char title[META_MAX_LEN];
    char artist[META_MAX_LEN];
    char album[META_MAX_LEN];
    char trackNum[8];
    char year[8];
    char genre[META_MAX_LEN];
    bool hasArt;
    u32  artOffset;
    u32  artSize;
} AudioMeta;

typedef struct Decoder Decoder;

struct Decoder {
    AudioFormat format;
    AudioInfo   info;
    AudioMeta   meta;
    void       *ctx;
    FILE       *file;

    DecodeResult (*open)(Decoder *dec, const char *path);
    DecodeResult (*readSamples)(Decoder *dec, s16 *outBuf, u32 numSamples, u32 *samplesRead);
    DecodeResult (*seek)(Decoder *dec, u32 samplePos);
    u32          (*tell)(Decoder *dec);
    void         (*close)(Decoder *dec);
};

AudioFormat detectFormat(const char *path);

void decoderInitWav(Decoder *dec);
void decoderInitFlac(Decoder *dec);
void decoderInitMp3(Decoder *dec);
void decoderInitVorbis(Decoder *dec);

bool decoderInit(Decoder *dec, const char *path);
void decoderCleanup(Decoder *dec);

bool decoderParseMeta(const char *path, AudioMeta *meta, AudioInfo *info);

#endif
