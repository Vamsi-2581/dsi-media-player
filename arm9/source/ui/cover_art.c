#include "cover_art.h"
#include "jpegthumb.h"
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ART_CACHE_DIR "/data/artcache"
#define THUMB_BYTES (ART_DISPLAY_SIZE * ART_DISPLAY_SIZE * sizeof(u16))
#define THUMB_MAGIC 0x54485542  /* "THUB" */

static u16 artPlayback[ART_DISPLAY_SIZE * ART_DISPLAY_SIZE];
static bool artPlaybackLoaded;

static u16 artPreview[ART_DISPLAY_SIZE * ART_DISPLAY_SIZE];
static bool artPreviewLoaded;

static bool usePreview;
static bool cacheInited;

void coverArtInit(void) {
    artPlaybackLoaded = false;
    artPreviewLoaded = false;
    usePreview = false;
    cacheInited = false;
}

void coverArtSetPreviewMode(bool preview) {
    usePreview = preview;
}

void coverArtClear(void) {
    if (usePreview)
        artPreviewLoaded = false;
    else
        artPlaybackLoaded = false;
}

void coverArtClearPreview(void) {
    artPreviewLoaded = false;
}

bool coverArtAvailable(void) {
    return usePreview ? artPreviewLoaded : artPlaybackLoaded;
}

/* --- Thumbnail disk cache --- */

static u32 hashPath(const char *path) {
    u32 h = 5381;
    while (*path) {
        h = ((h << 5) + h) ^ (u8)*path;
        path++;
    }
    return h;
}

static void thumbCachePath(const char *srcPath, char *out, int outLen) {
    if (!cacheInited) {
        mkdir("/data", 0755);
        mkdir(ART_CACHE_DIR, 0755);
        cacheInited = true;
    }
    u32 h = hashPath(srcPath);
    snprintf(out, outLen, ART_CACHE_DIR "/%08lx.thm", (unsigned long)h);
}

static bool loadThumbCache(const char *srcPath, u16 *cache) {
    char path[64];
    thumbCachePath(srcPath, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    u32 magic;
    if (fread(&magic, 4, 1, f) != 1 || magic != THUMB_MAGIC) {
        fclose(f);
        return false;
    }
    if (fread(cache, 1, THUMB_BYTES, f) != THUMB_BYTES) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static void saveThumbCache(const char *srcPath, const u16 *cache) {
    char path[64];
    thumbCachePath(srcPath, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (!f) return;

    u32 magic = THUMB_MAGIC;
    fwrite(&magic, 4, 1, f);
    fwrite(cache, 1, THUMB_BYTES, f);
    fclose(f);
}

static bool isJpeg(const u8 *data, u32 len) {
    return len >= 2 && data[0] == 0xFF && data[1] == 0xD8;
}

/* Decode image data (JPEG) into the current art buffer, with disk caching */
static bool decodeAndCache(const u8 *imgData, u32 dataLen, const char *cacheKey) {
    u16 *cache = usePreview ? artPreview : artPlayback;

    if (cacheKey && loadThumbCache(cacheKey, cache)) {
        return true;
    }

    if (!isJpeg(imgData, dataLen)) {
        return false;
    }

    bool ok = jpegThumbDecode(imgData, dataLen, cache, ART_DISPLAY_SIZE, ART_DISPLAY_SIZE);
    if (ok) {
        if (cacheKey)
            saveThumbCache(cacheKey, cache);
    }

    return ok;
}

static u32 readU32BE(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static bool loadFlacArt(FILE *f, u32 artOffset, u32 artBlockSize, const char *cacheKey) {
    u16 *cache = usePreview ? artPreview : artPlayback;
    if (cacheKey && loadThumbCache(cacheKey, cache))
        return true;

    fseek(f, artOffset, SEEK_SET);

    u8 hdr[4];
    if (fread(hdr, 1, 4, f) != 4) return false;
    if (fread(hdr, 1, 4, f) != 4) return false;
    u32 mimeLen = readU32BE(hdr);
    if (mimeLen > 256) return false;
    fseek(f, mimeLen, SEEK_CUR);

    if (fread(hdr, 1, 4, f) != 4) return false;
    u32 descLen = readU32BE(hdr);
    if (descLen > 1024) return false;
    fseek(f, descLen, SEEK_CUR);

    fseek(f, 16, SEEK_CUR);

    if (fread(hdr, 1, 4, f) != 4) return false;
    u32 dataLen = readU32BE(hdr);
    if (dataLen == 0 || dataLen > 2 * 1024 * 1024) return false;

    u8 *imgData = malloc(dataLen);
    if (!imgData) return false;

    if (fread(imgData, 1, dataLen, f) != dataLen) {
        free(imgData);
        return false;
    }

    bool ok = decodeAndCache(imgData, dataLen, cacheKey);
    free(imgData);
    return ok;
}

static bool loadMp3Art(FILE *f, u32 artOffset, u32 artBlockSize, const char *cacheKey) {
    u16 *cache = usePreview ? artPreview : artPlayback;
    if (cacheKey && loadThumbCache(cacheKey, cache))
        return true;

    fseek(f, artOffset, SEEK_SET);

    u8 *frameData = malloc(artBlockSize);
    if (!frameData) return false;

    if (fread(frameData, 1, artBlockSize, f) != artBlockSize) {
        free(frameData);
        return false;
    }

    u32 pos = 0;
    if (pos >= artBlockSize) { free(frameData); return false; }
    u8 encoding = frameData[pos++];

    while (pos < artBlockSize && frameData[pos] != 0) pos++;
    if (pos >= artBlockSize) { free(frameData); return false; }
    pos++;

    if (pos >= artBlockSize) { free(frameData); return false; }
    pos++;

    if (encoding == 1 || encoding == 2) {
        while (pos + 1 < artBlockSize) {
            if (frameData[pos] == 0 && frameData[pos + 1] == 0) {
                pos += 2;
                break;
            }
            pos += 2;
        }
    } else {
        while (pos < artBlockSize && frameData[pos] != 0) pos++;
        if (pos < artBlockSize) pos++;
    }

    if (pos >= artBlockSize) { free(frameData); return false; }

    u32 imgLen = artBlockSize - pos;
    bool ok = decodeAndCache(frameData + pos, imgLen, cacheKey);
    free(frameData);
    return ok;
}

void coverArtLoad(const char *audioPath, u32 artOffset, u32 artBlockSize, ArtSourceType srcType) {
    if (usePreview)
        artPreviewLoaded = false;
    else
        artPlaybackLoaded = false;
    if (!audioPath || artBlockSize == 0) {
        return;
    }

    FILE *f = fopen(audioPath, "rb");
    if (!f) {
        return;
    }

    bool ok = false;
    if (srcType == ART_SOURCE_FLAC)
        ok = loadFlacArt(f, artOffset, artBlockSize, audioPath);
    else if (srcType == ART_SOURCE_MP3)
        ok = loadMp3Art(f, artOffset, artBlockSize, audioPath);

    fclose(f);
    if (usePreview)
        artPreviewLoaded = ok;
    else
        artPlaybackLoaded = ok;
}

void coverArtLoadFile(const char *imagePath) {
    if (usePreview)
        artPreviewLoaded = false;
    else
        artPlaybackLoaded = false;
    if (!imagePath) return;

    u16 *cache = usePreview ? artPreview : artPlayback;
    if (loadThumbCache(imagePath, cache)) {
        if (usePreview) artPreviewLoaded = true;
        else artPlaybackLoaded = true;
        return;
    }

    FILE *f = fopen(imagePath, "rb");
    if (!f) {
        return;
    }

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fileSize <= 0 || fileSize > 2 * 1024 * 1024) {
        fclose(f);
        return;
    }

    u8 *imgData = malloc((u32)fileSize);
    if (!imgData) {
        fclose(f);
        return;
    }

    if (fread(imgData, 1, (u32)fileSize, f) != (u32)fileSize) {
        free(imgData);
        fclose(f);
        return;
    }
    fclose(f);

    bool ok = decodeAndCache(imgData, (u32)fileSize, imagePath);
    free(imgData);
    if (usePreview)
        artPreviewLoaded = ok;
    else
        artPlaybackLoaded = ok;
}

void coverArtLoadFromFolder(const char *audioPath) {
    if (usePreview)
        artPreviewLoaded = false;
    else
        artPlaybackLoaded = false;
    if (!audioPath) return;

    char dir[512];
    strncpy(dir, audioPath, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) *(slash + 1) = '\0';
    else return;

    static const char *names[] = {
        "cover.jpg", "cover.png",
        "folder.jpg", "folder.png",
        "front.jpg", "front.png",
        "album.jpg", "album.png",
        "Cover.jpg", "Cover.png",
        "Folder.jpg", "Folder.png",
        NULL
    };

    char path[512];
    for (int i = 0; names[i]; i++) {
        snprintf(path, sizeof(path), "%s%s", dir, names[i]);
        FILE *f = fopen(path, "rb");
        if (f) {
            fclose(f);
            coverArtLoadFile(path);
            if (coverArtAvailable()) return;
        }
    }
}

void coverArtLoadFromDir(const char *dirPath) {
    if (usePreview)
        artPreviewLoaded = false;
    else
        artPlaybackLoaded = false;
    if (!dirPath) return;

    char dir[512];
    strncpy(dir, dirPath, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    size_t len = strlen(dir);
    if (len > 0 && dir[len - 1] != '/') {
        if (len < sizeof(dir) - 1) {
            dir[len] = '/';
            dir[len + 1] = '\0';
        }
    }

    static const char *names[] = {
        "cover.jpg", "cover.png",
        "folder.jpg", "folder.png",
        NULL
    };

    char path[512];
    for (int i = 0; names[i]; i++) {
        snprintf(path, sizeof(path), "%s%s", dir, names[i]);
        FILE *f = fopen(path, "rb");
        if (f) {
            fclose(f);
            coverArtLoadFile(path);
            if (coverArtAvailable()) return;
        }
    }
}

bool coverArtPreCacheDir(const char *dirPath) {
    if (!dirPath) return false;

    char dir[512];
    strncpy(dir, dirPath, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    size_t dlen = strlen(dir);
    if (dlen > 0 && dir[dlen - 1] != '/') {
        if (dlen < sizeof(dir) - 1) { dir[dlen] = '/'; dir[dlen + 1] = '\0'; }
    }

    static const char *names[] = { "cover.jpg", "folder.jpg", NULL };
    char path[512];

    for (int i = 0; names[i]; i++) {
        snprintf(path, sizeof(path), "%s%s", dir, names[i]);

        /* Already cached? */
        char cachePath[64];
        thumbCachePath(path, cachePath, sizeof(cachePath));
        FILE *cf = fopen(cachePath, "rb");
        if (cf) { fclose(cf); return true; }

        FILE *f = fopen(path, "rb");
        if (!f) continue;

        fseek(f, 0, SEEK_END);
        long fileSize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fileSize <= 0 || fileSize > 2 * 1024 * 1024) { fclose(f); continue; }

        u8 *imgData = malloc((u32)fileSize);
        if (!imgData) { fclose(f); continue; }
        if (fread(imgData, 1, (u32)fileSize, f) != (u32)fileSize) {
            free(imgData); fclose(f); continue;
        }
        fclose(f);

        if (!isJpeg(imgData, (u32)fileSize)) { free(imgData); continue; }

        /* Decode into a temp buffer, save to disk cache */
        u16 *tmpBuf = malloc(THUMB_BYTES);
        if (!tmpBuf) { free(imgData); continue; }

        bool ok = jpegThumbDecode(imgData, (u32)fileSize, tmpBuf,
                                  ART_DISPLAY_SIZE, ART_DISPLAY_SIZE);
        free(imgData);
        if (ok) saveThumbCache(path, tmpBuf);
        free(tmpBuf);
        return ok;
    }
    return false;
}

bool coverArtLoadCachedDir(const char *dirPath, u16 *outBuf) {
    if (!dirPath || !outBuf) return false;

    char dir[512];
    strncpy(dir, dirPath, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    size_t dlen = strlen(dir);
    if (dlen > 0 && dir[dlen - 1] != '/') {
        if (dlen < sizeof(dir) - 1) { dir[dlen] = '/'; dir[dlen + 1] = '\0'; }
    }

    static const char *names[] = { "cover.jpg", "folder.jpg", "front.jpg", NULL };
    char path[512];
    for (int i = 0; names[i]; i++) {
        snprintf(path, sizeof(path), "%s%s", dir, names[i]);
        if (loadThumbCache(path, outBuf)) return true;
    }
    return false;
}

void coverArtDraw(u16 *fb, int x, int y) {
    u16 *cache = usePreview ? artPreview : artPlayback;
    bool loaded = usePreview ? artPreviewLoaded : artPlaybackLoaded;
    if (!loaded) return;
    for (int row = 0; row < ART_DISPLAY_SIZE; row++) {
        int py = y + row;
        if (py < 0 || py >= 192) continue;
        for (int col = 0; col < ART_DISPLAY_SIZE; col++) {
            int px = x + col;
            if (px < 0 || px >= 256) continue;
            fb[py * 256 + px] = cache[row * ART_DISPLAY_SIZE + col];
        }
    }
}

void coverArtDrawScaled(u16 *fb, int x, int y, int scale) {
    u16 *cache = usePreview ? artPreview : artPlayback;
    bool loaded = usePreview ? artPreviewLoaded : artPlaybackLoaded;
    if (!loaded || scale < 1) return;
    int outSize = ART_DISPLAY_SIZE * scale;
    for (int row = 0; row < outSize; row++) {
        int py = y + row;
        if (py < 0 || py >= 192) continue;
        int srcRow = row / scale;
        for (int col = 0; col < outSize; col++) {
            int px = x + col;
            if (px < 0 || px >= 256) continue;
            fb[py * 256 + px] = cache[srcRow * ART_DISPLAY_SIZE + col / scale];
        }
    }
}
