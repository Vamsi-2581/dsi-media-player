#ifndef LYRICS_H
#define LYRICS_H

#include <nds/ndstypes.h>
#include <stdbool.h>

#define LRC_MAX_LINES 200
#define LRC_LINE_LEN  80

typedef struct {
    u32 timeMs;
    char text[LRC_LINE_LEN];
} LrcLine;

typedef struct {
    LrcLine lines[LRC_MAX_LINES];
    int count;
    bool loaded;
} Lyrics;

void lyricsInit(Lyrics *lrc);
bool lyricsLoad(Lyrics *lrc, const char *audioPath);
void lyricsClear(Lyrics *lrc);
int  lyricsGetCurrentLine(Lyrics *lrc, u32 posMs);
void lyricsDraw(u16 *fb, Lyrics *lrc, u32 posMs, int y, int height);
void lyricsDrawFull(u16 *fb, Lyrics *lrc, u32 posMs);

#endif
