#ifndef COVER_ART_H
#define COVER_ART_H

#include <nds/ndstypes.h>

#define ART_DISPLAY_SIZE 64

typedef enum {
    ART_SOURCE_FLAC,
    ART_SOURCE_MP3
} ArtSourceType;

void coverArtInit(void);
void coverArtLoad(const char *audioPath, u32 artOffset, u32 artBlockSize, ArtSourceType srcType);
void coverArtLoadFile(const char *imagePath);
void coverArtLoadFromFolder(const char *audioPath);
void coverArtLoadFromDir(const char *dirPath);
void coverArtClear(void);
bool coverArtPreCacheDir(const char *dirPath);
void coverArtClearPreview(void);
bool coverArtAvailable(void);
void coverArtDraw(u16 *fb, int x, int y);
void coverArtDrawScaled(u16 *fb, int x, int y, int scale);
bool coverArtLoadCachedDir(const char *dirPath, u16 *outBuf);
void coverArtSetPreviewMode(bool preview);

#endif
