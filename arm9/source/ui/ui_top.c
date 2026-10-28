/*
 * Top screen — Now Playing display
 *
 * Layout (256x192):
 *   [0-15]    Header: "DSi Media Player" + battery
 *   [16-17]   Separator line
 *   [18-90]   Cover art (left) + metadata (right of art or full width)
 *   [92-100]  Progress bar
 *   [102-112] Time display
 *   [114-170] Visualizer area
 *   [172-191] Playback status + mode indicators
 */

#include "ui.h"
#include "player.h"
#include "cover_art.h"
#include "lyrics.h"
#include "decoder.h"
#include <nds.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    TOP_VIEW_NOWPLAYING,
    TOP_VIEW_LYRICS,
    TOP_VIEW_COUNT
} TopView;

static TopView topView = TOP_VIEW_NOWPLAYING;
static char lastTrackPath[512];
static Lyrics lyrics;
static char lyricsTrackPath[512];

/* Metadata-only preview state */
static bool pvActive;
static bool pvIsFolder;
static char pvPath[512];
static AudioMeta pvMeta;
static AudioInfo pvInfo;

/* Folder art cache — avoid re-searching the same folder */
static char pvCachedArtDir[512];
static bool pvCachedArtFound;

/* Metadata cache — avoid re-parsing files we already visited */
#define META_CACHE_SIZE 16

typedef struct {
    char path[256];
    AudioMeta meta;
    AudioInfo info;
} MetaCacheEntry;

static MetaCacheEntry metaCache[META_CACHE_SIZE];
static int metaCacheIdx;
static int metaCacheCount;

static MetaCacheEntry *metaCacheLookup(const char *path) {
    for (int i = 0; i < metaCacheCount; i++) {
        if (strcmp(metaCache[i].path, path) == 0)
            return &metaCache[i];
    }
    return NULL;
}

static MetaCacheEntry *metaCacheInsert(const char *path, const AudioMeta *meta, const AudioInfo *info) {
    MetaCacheEntry *e = &metaCache[metaCacheIdx];
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';
    e->meta = *meta;
    e->info = *info;
    metaCacheIdx = (metaCacheIdx + 1) % META_CACHE_SIZE;
    if (metaCacheCount < META_CACHE_SIZE) metaCacheCount++;
    return e;
}

/* Volume overlay */
static int volOverlayTimer;
static u8 hwVol;

static const char *formatName(AudioFormat fmt) {
    switch (fmt) {
        case FORMAT_WAV:    return "WAV";
        case FORMAT_FLAC:   return "FLAC";
        case FORMAT_MP3:    return "MP3";
        case FORMAT_VORBIS: return "OGG";
        default:            return "???";
    }
}

static const char *repeatLabel(RepeatMode mode) {
    switch (mode) {
        case REPEAT_ALL: return "RPT:ALL";
        case REPEAT_ONE: return "RPT:ONE";
        default:         return "RPT:OFF";
    }
}

static void formatTime(u32 ms, char *buf, int bufLen) {
    u32 totalSec = ms / 1000;
    u32 min = totalSec / 60;
    u32 sec = totalSec % 60;
    snprintf(buf, bufLen, "%u:%02u", (unsigned)min, (unsigned)sec);
}

static void drawBattery(u16 *fb, int x, int y) {
    u8 level = getBatteryLevel();
    u16 color;

    if (level <= 1) color = COL_BATTERY_LOW;
    else color = COL_BATTERY_OK;

    drawRectOutline(fb, x, y, 20, 10, COL_ICON);
    drawRect(fb, x + 20, y + 3, 2, 4, COL_ICON);

    int bars = level;
    if (bars > 4) bars = 4;
    for (int i = 0; i < bars; i++) {
        drawRect(fb, x + 2 + i * 4, y + 2, 3, 6, color);
    }
}

/*
 * FFT-based spectrum analyzer.
 * 512-point radix-2 DIT FFT with Hann window, computed from
 * actual PCM data peeked from the ring buffer at the playback position.
 * 16 log-spaced bands from ~86 Hz to ~20 kHz. All fixed-point Q14.
 */

#define VIS_BANDS     16
#define FFT_N         512
#define FFT_LOG2N     9
#define VIS_DECAY     3
#define VIS_ATTACK_FAST  1
#define VIS_PEAK_HOLD    8

static s16 visSampleBuf[FFT_N * 2];
static s32 fftRe[FFT_N], fftIm[FFT_N];
static s32 visBandLevel[VIS_BANDS];
static s32 visPeakLevel[VIS_BANDS];
static s32 visPeakHold[VIS_BANDS];

static const s16 fftSinQ[129] = {
         0,    201,    402,    603,    804,   1005,   1205,   1406,   1606,   1806,   2006,   2205,
      2404,   2603,   2801,   2999,   3196,   3393,   3590,   3786,   3981,   4176,   4370,   4563,
      4756,   4948,   5139,   5330,   5520,   5708,   5897,   6084,   6270,   6455,   6639,   6823,
      7005,   7186,   7366,   7545,   7723,   7900,   8076,   8250,   8423,   8595,   8765,   8935,
      9102,   9269,   9434,   9598,   9760,   9921,  10080,  10238,  10394,  10549,  10702,  10853,
     11003,  11151,  11297,  11442,  11585,  11727,  11866,  12004,  12140,  12274,  12406,  12537,
     12665,  12792,  12916,  13039,  13160,  13279,  13395,  13510,  13623,  13733,  13842,  13949,
     14053,  14155,  14256,  14354,  14449,  14543,  14635,  14724,  14811,  14896,  14978,  15059,
     15137,  15213,  15286,  15357,  15426,  15493,  15557,  15619,  15679,  15736,  15791,  15843,
     15893,  15941,  15986,  16029,  16069,  16107,  16143,  16176,  16207,  16235,  16261,  16284,
     16305,  16324,  16340,  16353,  16364,  16373,  16379,  16383,  16384,
};

static const s16 fftWindow[FFT_N] = {
        0,     1,     2,     6,    10,    15,    22,    30,    39,    50,    62,    75,
       89,   104,   121,   138,   157,   178,   199,   222,   246,   271,   297,   324,
      353,   383,   413,   446,   479,   513,   549,   586,   624,   663,   703,   744,
      787,   830,   875,   920,   967,  1015,  1064,  1114,  1165,  1218,  1271,  1325,
     1381,  1437,  1494,  1553,  1612,  1673,  1734,  1796,  1859,  1924,  1989,  2055,
     2122,  2190,  2259,  2329,  2399,  2471,  2543,  2617,  2691,  2765,  2841,  2918,
     2995,  3073,  3152,  3232,  3312,  3393,  3475,  3558,  3641,  3725,  3809,  3895,
     3980,  4067,  4154,  4242,  4330,  4419,  4509,  4599,  4689,  4781,  4872,  4964,
     5057,  5150,  5244,  5338,  5432,  5527,  5622,  5718,  5814,  5910,  6007,  6104,
     6202,  6299,  6397,  6495,  6594,  6693,  6791,  6891,  6990,  7090,  7189,  7289,
     7389,  7489,  7589,  7690,  7790,  7890,  7991,  8091,  8192,  8293,  8393,  8494,
     8594,  8694,  8795,  8895,  8995,  9095,  9195,  9294,  9394,  9493,  9593,  9691,
     9790,  9889,  9987, 10085, 10182, 10280, 10377, 10474, 10570, 10666, 10762, 10857,
    10952, 11046, 11140, 11234, 11327, 11420, 11512, 11603, 11695, 11785, 11875, 11965,
    12054, 12142, 12230, 12317, 12404, 12489, 12575, 12659, 12743, 12826, 12909, 12991,
    13072, 13152, 13232, 13311, 13389, 13466, 13543, 13619, 13693, 13767, 13841, 13913,
    13985, 14055, 14125, 14194, 14262, 14329, 14395, 14460, 14525, 14588, 14650, 14711,
    14772, 14831, 14890, 14947, 15003, 15059, 15113, 15166, 15219, 15270, 15320, 15369,
    15417, 15464, 15509, 15554, 15597, 15640, 15681, 15721, 15760, 15798, 15835, 15871,
    15905, 15938, 15971, 16001, 16031, 16060, 16087, 16113, 16138, 16162, 16185, 16206,
    16227, 16246, 16263, 16280, 16295, 16309, 16322, 16334, 16345, 16354, 16362, 16369,
    16374, 16378, 16382, 16383, 16384, 16383, 16382, 16378, 16374, 16369, 16362, 16354,
    16345, 16334, 16322, 16309, 16295, 16280, 16263, 16246, 16227, 16206, 16185, 16162,
    16138, 16113, 16087, 16060, 16031, 16001, 15971, 15938, 15905, 15871, 15835, 15798,
    15760, 15721, 15681, 15640, 15597, 15554, 15509, 15464, 15417, 15369, 15320, 15270,
    15219, 15166, 15113, 15059, 15003, 14947, 14890, 14831, 14772, 14711, 14650, 14588,
    14525, 14460, 14395, 14329, 14262, 14194, 14125, 14055, 13985, 13913, 13841, 13767,
    13693, 13619, 13543, 13466, 13389, 13311, 13232, 13152, 13072, 12991, 12909, 12826,
    12743, 12659, 12575, 12489, 12404, 12317, 12230, 12142, 12054, 11965, 11875, 11785,
    11695, 11603, 11512, 11420, 11327, 11234, 11140, 11046, 10952, 10857, 10762, 10666,
    10570, 10474, 10377, 10280, 10182, 10085,  9987,  9889,  9790,  9691,  9593,  9493,
     9394,  9294,  9195,  9095,  8995,  8895,  8795,  8694,  8594,  8494,  8393,  8293,
     8192,  8091,  7991,  7890,  7790,  7690,  7589,  7489,  7389,  7289,  7189,  7090,
     6990,  6891,  6791,  6693,  6594,  6495,  6397,  6299,  6202,  6104,  6007,  5910,
     5814,  5718,  5622,  5527,  5432,  5338,  5244,  5150,  5057,  4964,  4872,  4781,
     4689,  4599,  4509,  4419,  4330,  4242,  4154,  4067,  3980,  3895,  3809,  3725,
     3641,  3558,  3475,  3393,  3312,  3232,  3152,  3073,  2995,  2918,  2841,  2765,
     2691,  2617,  2543,  2471,  2399,  2329,  2259,  2190,  2122,  2055,  1989,  1924,
     1859,  1796,  1734,  1673,  1612,  1553,  1494,  1437,  1381,  1325,  1271,  1218,
     1165,  1114,  1064,  1015,   967,   920,   875,   830,   787,   744,   703,   663,
      624,   586,   549,   513,   479,   446,   413,   383,   353,   324,   297,   271,
      246,   222,   199,   178,   157,   138,   121,   104,    89,    75,    62,    50,
       39,    30,    22,    15,    10,     6,     2,     1,
};

static const u16 bandEdge[VIS_BANDS + 1] = {
    1, 2, 3, 4, 5, 6, 7, 9, 13, 18, 26, 38, 54, 78, 112, 162, 232
};

static s16 fftSin(int k) {
    k = k & (FFT_N - 1);
    if (k < FFT_N / 4)     return  fftSinQ[k];
    if (k < FFT_N / 2)     return  fftSinQ[FFT_N / 2 - k];
    if (k < 3 * FFT_N / 4) return -fftSinQ[k - FFT_N / 2];
    return -fftSinQ[FFT_N - k];
}

static s16 fftCos(int k) {
    return fftSin(k + FFT_N / 4);
}

static u32 isqrt32(u32 x) {
    if (x == 0) return 0;
    u32 g = 1;
    while (g * g < x && g < 0xFFFF) g <<= 1;
    for (int i = 0; i < 8; i++) g = (g + x / g) >> 1;
    return g;
}

void uiSetHwVolume(u8 vol) {
    hwVol = vol;
}

void uiShowVolumeOverlay(void) {
    volOverlayTimer = 30;
}

static void getParentDir(const char *path, char *dir, int dirSize) {
    strncpy(dir, path, dirSize - 1);
    dir[dirSize - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) *(slash + 1) = '\0';
    else dir[0] = '\0';
}

void uiPreviewLoad(const char *path) {
    if (pvActive && strcmp(pvPath, path) == 0) return;

    AudioMeta meta;
    AudioInfo info;
    bool found = false;

    /* Check metadata cache first */
    MetaCacheEntry *cached = metaCacheLookup(path);
    if (cached) {
        meta = cached->meta;
        info = cached->info;
        found = true;
    }

    /* Try library (already parsed, no file I/O) */
    if (!found) {
        Library *lib = uiGetLibrary();
        if (lib->state == LIB_READY || lib->state == LIB_PRECACHING) {
            const LibEntry *le = libraryFindByPath(lib, path);
            if (le) {
                memset(&meta, 0, sizeof(meta));
                memset(&info, 0, sizeof(info));
                strncpy(meta.title, le->title, META_MAX_LEN - 1);
                strncpy(meta.artist, le->artist, META_MAX_LEN - 1);
                strncpy(meta.album, le->album, META_MAX_LEN - 1);
                found = true;
            }
        }
    }

    /* Last resort: parse from file (slow) */
    if (!found && decoderParseMeta(path, &meta, &info)) {
        metaCacheInsert(path, &meta, &info);
        found = true;
    }

    if (found) {
        pvMeta = meta;
        pvInfo = info;

        coverArtSetPreviewMode(true);

        /* Check if we're in the same folder — reuse cached art */
        char dir[512];
        getParentDir(path, dir, sizeof(dir));
        if (strcmp(dir, pvCachedArtDir) != 0) {
            coverArtClear();
            coverArtLoadFromFolder(path);
            strncpy(pvCachedArtDir, dir, sizeof(pvCachedArtDir) - 1);
            pvCachedArtFound = coverArtAvailable();
        }

        strncpy(pvPath, path, sizeof(pvPath) - 1);
        pvPath[sizeof(pvPath) - 1] = '\0';
        pvActive = true;
        pvIsFolder = false;
    }
}

void uiPreviewLoadFromLib(const LibEntry *entry) {
    if (!entry) return;
    if (pvActive && strcmp(pvPath, entry->path) == 0) return;

    memset(&pvMeta, 0, sizeof(pvMeta));
    memset(&pvInfo, 0, sizeof(pvInfo));
    strncpy(pvMeta.title, entry->title, META_MAX_LEN - 1);
    strncpy(pvMeta.artist, entry->artist, META_MAX_LEN - 1);
    strncpy(pvMeta.album, entry->album, META_MAX_LEN - 1);

    coverArtSetPreviewMode(true);

    char dir[512];
    getParentDir(entry->path, dir, sizeof(dir));
    if (strcmp(dir, pvCachedArtDir) != 0) {
        coverArtClear();
        coverArtLoadFromFolder(entry->path);
        strncpy(pvCachedArtDir, dir, sizeof(pvCachedArtDir) - 1);
        pvCachedArtFound = coverArtAvailable();
    }

    strncpy(pvPath, entry->path, sizeof(pvPath) - 1);
    pvPath[sizeof(pvPath) - 1] = '\0';
    pvActive = true;
    pvIsFolder = false;
}

void uiPreviewLoadDir(const char *dirPath) {
    if (pvActive && pvIsFolder && strcmp(pvPath, dirPath) == 0) return;

    coverArtSetPreviewMode(true);

    memset(&pvMeta, 0, sizeof(pvMeta));
    memset(&pvInfo, 0, sizeof(pvInfo));

    coverArtClear();
    coverArtLoadFromDir(dirPath);

    const char *name = strrchr(dirPath, '/');
    if (name && name[1]) {
        name++;
    } else {
        name = dirPath;
    }
    size_t dlen = strlen(dirPath);
    if (dlen > 0 && dirPath[dlen - 1] == '/') {
        const char *p = dirPath + dlen - 2;
        while (p > dirPath && *p != '/') p--;
        if (*p == '/') p++;
        name = p;
    }
    strncpy(pvMeta.title, name, META_MAX_LEN - 1);
    pvMeta.title[META_MAX_LEN - 1] = '\0';
    size_t tlen = strlen(pvMeta.title);
    if (tlen > 0 && pvMeta.title[tlen - 1] == '/') pvMeta.title[tlen - 1] = '\0';

    strncpy(pvPath, dirPath, sizeof(pvPath) - 1);
    pvPath[sizeof(pvPath) - 1] = '\0';
    pvActive = true;
    pvIsFolder = true;
}

void uiPreviewClear(void) {
    if (!pvActive) return;
    pvActive = false;
    pvPath[0] = '\0';
    coverArtSetPreviewMode(false);
}

bool uiPreviewActive(void) {
    return pvActive;
}

static void fftCompute(void) {
    /* Bit-reversal permutation */
    for (int i = 0; i < FFT_N; i++) {
        int j = 0;
        for (int b = 0; b < FFT_LOG2N; b++)
            if (i & (1 << b)) j |= (1 << (FFT_LOG2N - 1 - b));
        if (j > i) {
            s32 tmp = fftRe[i]; fftRe[i] = fftRe[j]; fftRe[j] = tmp;
        }
        fftIm[i] = 0;
    }

    /* Radix-2 DIT butterflies */
    for (int stage = 0; stage < FFT_LOG2N; stage++) {
        int halfSize = 1 << stage;
        int fullSize = halfSize << 1;
        int twiddleStep = FFT_N >> (stage + 1);

        for (int group = 0; group < FFT_N; group += fullSize) {
            for (int k = 0; k < halfSize; k++) {
                int tw = k * twiddleStep;
                s32 wr = fftCos(tw);
                s32 wi = -fftSin(tw);

                int i0 = group + k;
                int i1 = group + k + halfSize;

                s32 tRe = (wr * fftRe[i1] - wi * fftIm[i1]) >> 14;
                s32 tIm = (wr * fftIm[i1] + wi * fftRe[i1]) >> 14;

                fftRe[i1] = fftRe[i0] - tRe;
                fftIm[i1] = fftIm[i0] - tIm;
                fftRe[i0] = fftRe[i0] + tRe;
                fftIm[i0] = fftIm[i0] + tIm;
            }
        }
    }
}

static void drawVuMeter(u16 *fb, int y, int height) {
    StreamState state = playerGetState();
    if (state != STREAM_PLAYING) {
        for (int i = 0; i < VIS_BANDS; i++) {
            visBandLevel[i] = 0;
            visPeakLevel[i] = 0;
            visPeakHold[i] = 0;
        }
        drawHLine(fb, 20, y + height / 2, SCREEN_W - 40, COL_TEXT_DIM);
        return;
    }

    u32 got = audioStreamPeekPlayback(visSampleBuf, FFT_N);
    if (got < FFT_N) {
        drawHLine(fb, 20, y + height / 2, SCREEN_W - 40, COL_TEXT_DIM);
        return;
    }

    /* Mix stereo to mono, apply Hann window, load into FFT buffer */
    for (u32 i = 0; i < FFT_N; i++) {
        s32 mono = ((s32)visSampleBuf[i * 2] + visSampleBuf[i * 2 + 1]) >> 1;
        fftRe[i] = (mono * fftWindow[i]) >> 14;
    }

    fftCompute();

    /* Compute magnitude for each band (sum of bin magnitudes) */
    for (int band = 0; band < VIS_BANDS; band++) {
        u32 sumMag = 0;
        int bStart = bandEdge[band];
        int bEnd   = bandEdge[band + 1];
        int nBins  = bEnd - bStart;

        for (int k = bStart; k < bEnd; k++) {
            s32 re = fftRe[k] >> 7;
            s32 im = fftIm[k] >> 7;
            u32 mag2 = (u32)(re * re + im * im);
            sumMag += isqrt32(mag2);
        }

        if (nBins > 1) sumMag /= nBins;

        /* dB-like logarithmic scaling with boosted sensitivity */
        s32 level = 0;
        if (sumMag > 0) {
            s32 logVal = 0;
            u32 tmp = sumMag;
            while (tmp > 1) { logVal++; tmp >>= 1; }
            level = (logVal * height) / 14;
            if (level > height) level = height;
        }

        /* Smooth attack and decay */
        if (level > visBandLevel[band]) {
            visBandLevel[band] = level;
        } else {
            visBandLevel[band] -= VIS_DECAY;
            if (visBandLevel[band] < 0) visBandLevel[band] = 0;
        }

        /* Peak hold then slow decay */
        if (level > visPeakLevel[band]) {
            visPeakLevel[band] = level;
            visPeakHold[band] = VIS_PEAK_HOLD;
        } else if (visPeakHold[band] > 0) {
            visPeakHold[band]--;
        } else if (visPeakLevel[band] > 0) {
            visPeakLevel[band]--;
        }
    }

    /* Draw bars */
    int totalW = SCREEN_W - 40;
    int barW = totalW / VIS_BANDS;
    int barGap = 1;

    for (int i = 0; i < VIS_BANDS; i++) {
        int barH = visBandLevel[i];
        int x = 20 + i * barW;

        if (barH > 0) {
            /* Gradient: draw bar in segments */
            u16 colLow = uiThemeAccent();
            u16 colHigh = uiThemeAccentLight();

            int threshold = height * 3 / 4;
            int lowH = (barH < threshold) ? barH : threshold;
            int highH = barH - lowH;

            if (lowH > 0)
                drawRect(fb, x, y + height - lowH, barW - barGap, lowH, colLow);
            if (highH > 0)
                drawRect(fb, x, y + height - barH, barW - barGap, highH, colHigh);
        }

        /* Peak indicator */
        int peakH = visPeakLevel[i];
        if (peakH > 1) {
            int peakY = y + height - peakH;
            if (peakY >= y)
                drawRect(fb, x, peakY, barW - barGap, 1, COL_TEXT_BRIGHT);
        }
    }
}

void uiTopCycleView(void) {
    topView = (TopView)((topView + 1) % TOP_VIEW_COUNT);
}

void uiTopRender(void) {
    u16 *fb = getTopBuffer();
    drawClear(fb, COL_BG);

    /* Always track cover art + lyrics loading regardless of view mode */
    StreamState state = playerGetState();
    const AudioMeta *meta;
    const AudioInfo *info;
    const char *displayPath;

    coverArtSetPreviewMode(pvActive);
    if (pvActive) {
        meta = &pvMeta;
        info = &pvInfo;
        displayPath = pvPath;
    } else {
        meta = playerGetMeta();
        info = playerGetInfo();
        displayPath = playerGetCurrentFile();
    }

    /* Load cover art + lyrics on track change (only when not previewing) */
    if (!pvActive) {
        const char *curPath = playerGetCurrentFile();
        if (strcmp(curPath, lastTrackPath) != 0) {
            strncpy(lastTrackPath, curPath, sizeof(lastTrackPath) - 1);
            lastTrackPath[sizeof(lastTrackPath) - 1] = '\0';
            coverArtClear();
            if (meta->hasArt && meta->artOffset > 0) {
                ArtSourceType srcType = (info->format == FORMAT_MP3) ? ART_SOURCE_MP3 : ART_SOURCE_FLAC;
                coverArtLoad(curPath, meta->artOffset, meta->artSize, srcType);
            }
            if (!coverArtAvailable()) {
                coverArtLoadFromFolder(curPath);
            }
            /* Load lyrics for new track */
            if (strcmp(curPath, lyricsTrackPath) != 0) {
                strncpy(lyricsTrackPath, curPath, sizeof(lyricsTrackPath) - 1);
                lyricsTrackPath[sizeof(lyricsTrackPath) - 1] = '\0';
                lyricsLoad(&lyrics, curPath);
            }
        }
    }

    /* --- LYRICS FULLSCREEN MODE --- */
    if (topView == TOP_VIEW_LYRICS && !pvActive) {
        u32 posMs = playerGetPositionMs();
        lyricsDrawFull(fb, &lyrics, posMs);
        return;
    }

    /* --- NORMAL RENDERING --- */
    /* Header */
    drawRect(fb, 0, 0, SCREEN_W, 16, COL_BG_HEADER);
    drawString(fb, 4, 4, "DSi Media Player", COL_TEXT_BRIGHT);
    drawBattery(fb, SCREEN_W - 28, 3);
    drawHLine(fb, 0, 16, SCREEN_W, COL_BORDER);

    if (!pvActive && state == STREAM_STOPPED && playerGetCurrentFile()[0] == '\0') {
        drawStringCentered(fb, 80, "No track loaded", COL_TEXT_DIM);
        drawStringCentered(fb, 96, "Browse files below", COL_TEXT_DIM);
        return;
    }

    if (pvActive) {
        /* --- PREVIEW MODE: large centered cover art + metadata below --- */
        drawString(fb, SCREEN_W - drawStringWidth("PREVIEW") - 4, 18, "PREVIEW", uiThemeAccentLight());

        if (coverArtAvailable()) {
            /* 2x scaled art (128x128), centered horizontally */
            int artSize = ART_DISPLAY_SIZE * 2;
            int artX = (SCREEN_W - artSize) / 2;
            int artY = 20;
            coverArtDrawScaled(fb, artX, artY, 2);
            drawRectOutline(fb, artX - 1, artY - 1, artSize + 2, artSize + 2, COL_BORDER);

            /* Metadata below art */
            int textY = artY + artSize + 4;
            if (meta->title[0]) {
                drawStringUCentered(fb, textY, meta->title, COL_TEXT_BRIGHT);
            } else {
                const char *name = strrchr(displayPath, '/');
                if (name) name++; else name = displayPath;
                drawStringCentered(fb, textY, name, COL_TEXT_BRIGHT);
            }
            textY += FONT_H + 2;

            if (meta->artist[0]) {
                drawStringUCentered(fb, textY, meta->artist, COL_TEXT);
                textY += FONT_H + 2;
            }

            if (meta->album[0]) {
                drawStringUCentered(fb, textY, meta->album, COL_TEXT_DIM);
                textY += FONT_H + 2;
            }
        } else {
            /* No art: show metadata prominently */
            int textY = 28;

            if (pvIsFolder) {
                drawStringCentered(fb, 70, "No cover art found", COL_TEXT_DIM);
            }

            if (meta->title[0]) {
                drawStringUCentered(fb, textY, meta->title, COL_TEXT_BRIGHT);
            } else {
                const char *name = strrchr(displayPath, '/');
                if (name) name++; else name = displayPath;
                drawStringCentered(fb, textY, name, COL_TEXT_BRIGHT);
            }
            textY += FONT_H + 4;

            if (meta->artist[0]) {
                drawStringUCentered(fb, textY, meta->artist, COL_TEXT);
                textY += FONT_H + 4;
            }

            if (meta->album[0]) {
                drawStringUCentered(fb, textY, meta->album, COL_TEXT_DIM);
                textY += FONT_H + 4;
            }

            /* Format and duration info */
            int infoY = 90;
            if (info->sampleRate > 0) {
                char fmtBuf[64];
                snprintf(fmtBuf, sizeof(fmtBuf), "%s  %uHz  %uch  %ubit",
                         formatName(info->format),
                         (unsigned)info->sampleRate,
                         (unsigned)info->channels,
                         (unsigned)info->bitsPerSample);
                drawStringCentered(fb, infoY, fmtBuf, COL_TEXT_DIM);
                infoY += FONT_H + 3;
            }

            if (info->durationMs > 0) {
                char durBuf[16];
                formatTime(info->durationMs, durBuf, sizeof(durBuf));
                char lenBuf[32];
                snprintf(lenBuf, sizeof(lenBuf), "Length: %s", durBuf);
                drawStringCentered(fb, infoY, lenBuf, COL_TEXT_DIM);
                infoY += FONT_H + 3;
            }

            if (meta->genre[0]) {
                drawStringUCentered(fb, infoY, meta->genre, COL_TEXT_DIM);
            }
        }
    } else {
        /* --- PLAYBACK MODE --- */
        int metaX = 8;
        int metaMaxW = SCREEN_W - 16;
        int metaY = 20;

        if (coverArtAvailable()) {
            coverArtDraw(fb, 8, 20);
            drawRectOutline(fb, 7, 19, ART_DISPLAY_SIZE + 2, ART_DISPLAY_SIZE + 2, COL_BORDER);
            metaX = 8 + ART_DISPLAY_SIZE + 8;
            metaMaxW = SCREEN_W - metaX - 4;
        }

        if (meta->title[0]) {
            drawStringUClipped(fb, metaX, metaY, metaMaxW, meta->title, COL_TEXT_BRIGHT);
        } else {
            const char *name = strrchr(displayPath, '/');
            if (name) name++; else name = displayPath;
            drawStringUClipped(fb, metaX, metaY, metaMaxW, name, COL_TEXT_BRIGHT);
        }
        metaY += FONT_H + 3;

        if (meta->artist[0])
            drawStringUClipped(fb, metaX, metaY, metaMaxW, meta->artist, COL_TEXT);
        metaY += FONT_H + 3;

        if (meta->album[0])
            drawStringUClipped(fb, metaX, metaY, metaMaxW, meta->album, COL_TEXT_DIM);

        /* Progress bar */
        int progY = 92;
        u32 posMs = playerGetPositionMs();
        u32 durMs = playerGetDurationMs();
        float progress = 0.0f;
        if (durMs > 0) progress = (float)posMs / (float)durMs;

        drawProgressBar(fb, 8, progY, SCREEN_W - 16, 6, progress, COL_PROGRESS_BG, uiThemeProgressFg());

        char timeBuf[32];
        char posBuf[16], durBuf[16];
        formatTime(posMs, posBuf, sizeof(posBuf));
        formatTime(durMs, durBuf, sizeof(durBuf));
        snprintf(timeBuf, sizeof(timeBuf), "%s / %s", posBuf, durBuf);

        int timeW = drawStringWidth(timeBuf);
        drawString(fb, (SCREEN_W - timeW) / 2, progY + 10, timeBuf, COL_TEXT);

        drawVuMeter(fb, 114, 50);
    }

    /* Playback state + track info */
    int statusY = 168;
    const char *stateStr = "STOPPED";
    u16 stateCol = COL_TEXT_DIM;
    switch (state) {
        case STREAM_PLAYING: stateStr = "PLAYING"; stateCol = uiThemeAccent(); break;
        case STREAM_PAUSED:  stateStr = "PAUSED";  stateCol = COL_TEXT; break;
        case STREAM_LOADING: stateStr = "LOADING"; stateCol = COL_TEXT; break;
        default: break;
    }
    drawString(fb, 8, statusY, stateStr, stateCol);

    /* Playlist position */
    Playlist *pl = playerGetPlaylist();
    if (pl->count > 0 && pl->current >= 0) {
        char plBuf[24];
        snprintf(plBuf, sizeof(plBuf), "%d/%d", pl->current + 1, pl->count);
        drawString(fb, 8, statusY + FONT_H + 2, plBuf, COL_TEXT_DIM);
    }

    /* Mode indicators — right side */
    const char *repStr = repeatLabel(playerGetRepeat());
    drawString(fb, SCREEN_W - drawStringWidth(repStr) - 8, statusY, repStr, COL_TEXT_DIM);

    if (playerGetShuffle()) {
        drawString(fb, SCREEN_W - drawStringWidth("SHUFFLE") - 8, statusY + FONT_H + 2, "SHUFFLE", uiThemeAccentLight());
    }

    /* Volume overlay — fades after 2 seconds */
    if (volOverlayTimer > 0) {
        volOverlayTimer--;
        int barX = 56;
        int barY = 82;
        int barW = 144;
        int barH = 28;

        drawRect(fb, barX, barY, barW, barH, COL_BG_HEADER);
        drawRectOutline(fb, barX, barY, barW, barH, COL_BORDER);

        char volBuf[16];
        snprintf(volBuf, sizeof(volBuf), "VOL %d/%d", hwVol, 31);
        int textW = drawStringWidth(volBuf);
        drawString(fb, barX + (barW - textW) / 2, barY + 3, volBuf, COL_TEXT_BRIGHT);

        int fillW = barW - 12;
        int fillH = 6;
        int fillX = barX + 6;
        int fillY = barY + 16;
        drawRect(fb, fillX, fillY, fillW, fillH, COL_PROGRESS_BG);
        int filled = (fillW * hwVol) / 31;
        if (filled > 0)
            drawRect(fb, fillX, fillY, filled, fillH, uiThemeProgressFg());
    }

    /* Error display */
    const char *err = uiGetError();
    if (err && err[0]) {
        drawRect(fb, 0, SCREEN_H - 12, SCREEN_W, 12, RGB15(10, 2, 2));
        drawStringClipped(fb, 4, SCREEN_H - 10, SCREEN_W - 8, err, COL_ERROR);
    }
}
