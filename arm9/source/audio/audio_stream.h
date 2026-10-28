#ifndef AUDIO_STREAM_H
#define AUDIO_STREAM_H

#include <nds/ndstypes.h>
#include "decoder.h"

typedef enum {
    STREAM_STOPPED = 0,
    STREAM_PLAYING,
    STREAM_PAUSED,
    STREAM_LOADING
} StreamState;

void audioStreamInit(void);
bool audioStreamOpen(const char *path);
bool audioStreamOpenGapless(const char *path);
bool audioStreamIsDecoderDone(void);
void audioStreamPlay(void);
void audioStreamPause(void);
void audioStreamStop(void);
void audioStreamUpdate(void);

void audioStreamSetVolume(u8 volume);
u8   audioStreamGetVolume(void);

bool audioStreamSeek(u32 samplePos);
bool audioStreamSeekMs(u32 ms);

StreamState   audioStreamGetState(void);
const AudioInfo *audioStreamGetInfo(void);
const AudioMeta *audioStreamGetMeta(void);
u32  audioStreamGetPosition(void);
u32  audioStreamGetPositionMs(void);
u32  audioStreamGetDurationMs(void);
const char *audioStreamGetFilePath(void);

u32 audioStreamPeekSamples(s16 *dest, u32 numSamples);
u32 audioStreamPeekPlayback(s16 *dest, u32 numSamples);
u32 audioStreamRingAvailable(void);

#endif
