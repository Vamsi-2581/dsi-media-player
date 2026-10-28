#include "library.h"
#include "audio/decoder.h"
#include "fs/file_browser.h"
#include "ui/cover_art.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define SCAN_DIR_MAX 256

static char scanDirs[SCAN_DIR_MAX][LIB_STR_PATH];
static int scanDirCount;
static int scanDirCurrent;
static DIR *scanCurrentDir;

static int strcasecmpLocal(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static bool strcasecontains(const char *haystack, const char *needle) {
    if (!needle[0]) return true;
    int nlen = strlen(needle);
    int hlen = strlen(haystack);
    for (int i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (int j = 0; j < nlen; j++) {
            char a = haystack[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

static bool artistContains(const char *artistField, const char *singleArtist) {
    char buf[LIB_STR_SHORT];
    strncpy(buf, artistField, LIB_STR_SHORT - 1);
    buf[LIB_STR_SHORT - 1] = '\0';
    char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        char *end = p;
        while (*end && *end != ';') end++;
        char saved = *end;
        *end = '\0';
        char *trimEnd = end - 1;
        while (trimEnd > p && *trimEnd == ' ') *trimEnd-- = '\0';
        if (strcasecmpLocal(p, singleArtist) == 0) return true;
        if (!saved) break;
        p = end + 1;
    }
    return false;
}

bool libraryArtistMatch(const char *artistField, const char *singleArtist) {
    return artistContains(artistField, singleArtist);
}

const char *libraryGetAlbumDir(Library *lib, const char *albumName) {
    for (int i = 0; i < lib->count; i++) {
        if (artistContains(lib->entries[i].artist, lib->filterArtist) &&
            strcasecmpLocal(lib->entries[i].album, albumName) == 0) {
            static char dirBuf[LIB_STR_PATH];
            strncpy(dirBuf, lib->entries[i].path, LIB_STR_PATH - 1);
            dirBuf[LIB_STR_PATH - 1] = '\0';
            char *slash = strrchr(dirBuf, '/');
            if (slash) *(slash + 1) = '\0';
            return dirBuf;
        }
    }
    return NULL;
}

void libraryInit(Library *lib) {
    memset(lib, 0, sizeof(Library));
    lib->state = LIB_IDLE;
    lib->view = LIB_VIEW_ARTISTS;
}

/* --- Cache file format ---
 * Line 1: "DSILIB1"
 * Line 2: count
 * Remaining: tab-separated fields per entry:
 *   path \t title \t artist \t album \t trackNum \t year
 */

bool libraryLoadCache(Library *lib) {
    FILE *f = fopen(LIB_CACHE_PATH, "r");
    if (!f) return false;

    char line[1024];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return false; }
    if (strncmp(line, "DSILIB1", 7) != 0) { fclose(f); return false; }

    if (!fgets(line, sizeof(line), f)) { fclose(f); return false; }
    int count = atoi(line);
    if (count <= 0 || count > LIB_MAX_ENTRIES) { fclose(f); return false; }

    lib->count = 0;
    for (int i = 0; i < count; i++) {
        if (!fgets(line, sizeof(line), f)) break;
        int len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';

        LibEntry *e = &lib->entries[lib->count];
        memset(e, 0, sizeof(LibEntry));

        char *fields[6];
        int fi = 0;
        fields[0] = line;
        for (char *p = line; *p && fi < 5; p++) {
            if (*p == '\t') {
                *p = '\0';
                fields[++fi] = p + 1;
            }
        }

        if (fi < 5) continue;

        strncpy(e->path, fields[0], LIB_STR_PATH - 1);
        strncpy(e->title, fields[1], LIB_STR_SHORT - 1);
        strncpy(e->artist, fields[2], LIB_STR_SHORT - 1);
        strncpy(e->album, fields[3], LIB_STR_SHORT - 1);
        strncpy(e->trackNum, fields[4], 7);
        strncpy(e->year, fields[5], 7);

        lib->count++;
    }

    fclose(f);
    lib->state = LIB_READY;
    libraryBuildView(lib);
    return true;
}

bool librarySaveCache(Library *lib) {
    mkdir("/data", 0755);
    FILE *f = fopen(LIB_CACHE_PATH, "w");
    if (!f) return false;

    fprintf(f, "DSILIB1\n%d\n", lib->count);
    for (int i = 0; i < lib->count; i++) {
        LibEntry *e = &lib->entries[i];
        fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\n",
                e->path, e->title, e->artist, e->album,
                e->trackNum, e->year);
    }

    fclose(f);
    return true;
}

/* Phase 1: collect all directories recursively (one dir open at a time) */
static void collectDirs(const char *rootPath) {
    scanDirCount = 0;
    scanDirCurrent = 0;

    strncpy(scanDirs[0], rootPath, LIB_STR_PATH - 1);
    scanDirs[0][LIB_STR_PATH - 1] = '\0';
    scanDirCount = 1;

    for (int i = 0; i < scanDirCount && scanDirCount < SCAN_DIR_MAX; i++) {
        DIR *d = opendir(scanDirs[i]);
        if (!d) continue;

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && scanDirCount < SCAN_DIR_MAX) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            if (ent->d_name[0] == '.') continue;

            char fullPath[LIB_STR_PATH];
            if (strcmp(scanDirs[i], "/") == 0)
                snprintf(fullPath, sizeof(fullPath), "/%s", ent->d_name);
            else
                snprintf(fullPath, sizeof(fullPath), "%s/%s", scanDirs[i], ent->d_name);

            struct stat st;
            if (stat(fullPath, &st) == 0 && S_ISDIR(st.st_mode)) {
                strncpy(scanDirs[scanDirCount], fullPath, LIB_STR_PATH - 1);
                scanDirs[scanDirCount][LIB_STR_PATH - 1] = '\0';
                scanDirCount++;
            }
        }
        closedir(d);
    }
}

void libraryScanStart(Library *lib, const char *rootPath) {
    lib->count = 0;
    lib->state = LIB_SCANNING;
    lib->scanProgress = 0;
    lib->scanTotal = 0;
    scanCurrentDir = NULL;

    collectDirs(rootPath);
    lib->scanTotal = scanDirCount;
}

/* Phase 2: process one audio file per call */
bool libraryScanStep(Library *lib) {
    if (lib->state != LIB_SCANNING)
        return false;

    /* Open next directory if current one is done */
    while (!scanCurrentDir) {
        if (scanDirCurrent >= scanDirCount) {
            librarySaveCache(lib);
            libraryBuildView(lib);
            lib->state = LIB_PRECACHING;
            scanDirCurrent = 0;
            lib->scanProgress = 0;
            return false;
        }
        scanCurrentDir = opendir(scanDirs[scanDirCurrent]);
        if (!scanCurrentDir) {
            scanDirCurrent++;
        }
    }

    /* Read entries from current directory until we find an audio file */
    struct dirent *ent;
    while ((ent = readdir(scanCurrentDir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        if (!isAudioFile(ent->d_name))
            continue;

        if (lib->count >= LIB_MAX_ENTRIES) {
            closedir(scanCurrentDir);
            scanCurrentDir = NULL;
            librarySaveCache(lib);
            libraryBuildView(lib);
            lib->state = LIB_PRECACHING;
            scanDirCurrent = 0;
            lib->scanProgress = 0;
            return false;
        }

        char fullPath[LIB_STR_PATH];
        if (strcmp(scanDirs[scanDirCurrent], "/") == 0)
            snprintf(fullPath, sizeof(fullPath), "/%s", ent->d_name);
        else
            snprintf(fullPath, sizeof(fullPath), "%s/%s",
                     scanDirs[scanDirCurrent], ent->d_name);

        LibEntry *e = &lib->entries[lib->count];
        memset(e, 0, sizeof(LibEntry));
        strncpy(e->path, fullPath, LIB_STR_PATH - 1);

        AudioMeta meta;
        AudioInfo info;
        if (decoderParseMeta(fullPath, &meta, &info)) {
            strncpy(e->title, meta.title, LIB_STR_SHORT - 1);
            strncpy(e->artist, meta.artist, LIB_STR_SHORT - 1);
            strncpy(e->album, meta.album, LIB_STR_SHORT - 1);
            strncpy(e->trackNum, meta.trackNum, 7);
            strncpy(e->year, meta.year, 7);
        }

        if (!e->title[0]) {
            const char *n = strrchr(fullPath, '/');
            if (n) n++; else n = fullPath;
            strncpy(e->title, n, LIB_STR_SHORT - 1);
        }
        if (!e->artist[0]) strncpy(e->artist, "Unknown Artist", LIB_STR_SHORT - 1);
        if (!e->album[0]) strncpy(e->album, "Unknown Album", LIB_STR_SHORT - 1);

        lib->count++;
        lib->scanProgress++;
        return true;
    }

    /* End of directory */
    closedir(scanCurrentDir);
    scanCurrentDir = NULL;
    scanDirCurrent++;
    lib->scanProgress = scanDirCurrent;
    return true;
}

/* Phase 3: pre-cache one directory's cover art per call */
bool libraryPreCacheStep(Library *lib) {
    if (lib->state != LIB_PRECACHING)
        return false;

    while (scanDirCurrent < scanDirCount) {
        int idx = scanDirCurrent++;
        lib->scanProgress = scanDirCurrent;
        coverArtPreCacheDir(scanDirs[idx]);
        return true;
    }

    lib->state = LIB_READY;
    return false;
}

const LibEntry *libraryFindByPath(Library *lib, const char *path) {
    if (!lib || !path || lib->count == 0) return NULL;
    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->entries[i].path, path) == 0)
            return &lib->entries[i];
    }
    return NULL;
}

/* --- View building --- */

static int sortStrPtrs(const void *a, const void *b) {
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    return strcasecmpLocal(sa, sb);
}

static void dedup(char **list, int *count) {
    if (*count <= 1) return;
    int out = 1;
    for (int i = 1; i < *count; i++) {
        if (strcasecmpLocal(list[i], list[out - 1]) != 0) {
            list[out++] = list[i];
        }
    }
    *count = out;
}

void libraryBuildView(Library *lib) {
    lib->viewCount = 0;
    lib->selectedIndex = 0;
    lib->scrollOffset = 0;

    if (lib->searchActive && lib->searchLen > 0) {
        if (lib->view == LIB_VIEW_ARTISTS) {
            static char searchArtists[LIB_MAX_ENTRIES][LIB_STR_SHORT];
            int nameCount = 0;
            for (int i = 0; i < lib->count && nameCount < LIB_MAX_ENTRIES; i++) {
                if (!strcasecontains(lib->entries[i].artist, lib->searchQuery))
                    continue;
                char first[LIB_STR_SHORT];
                strncpy(first, lib->entries[i].artist, LIB_STR_SHORT - 1);
                first[LIB_STR_SHORT - 1] = '\0';
                char *semi = strchr(first, ';');
                if (semi) *semi = '\0';
                int flen = strlen(first);
                while (flen > 0 && first[flen - 1] == ' ') first[--flen] = '\0';

                bool dup = false;
                for (int k = 0; k < nameCount; k++) {
                    if (strcasecmpLocal(searchArtists[k], first) == 0) {
                        dup = true; break;
                    }
                }
                if (!dup) {
                    strncpy(searchArtists[nameCount], first, LIB_STR_SHORT - 1);
                    searchArtists[nameCount][LIB_STR_SHORT - 1] = '\0';
                    lib->viewList[lib->viewCount++] = searchArtists[nameCount];
                    nameCount++;
                }
            }
            qsort(lib->viewList, lib->viewCount, sizeof(char *), sortStrPtrs);
            dedup(lib->viewList, &lib->viewCount);
        } else if (lib->view == LIB_VIEW_ALBUMS) {
            for (int i = 0; i < lib->count && lib->viewCount < LIB_MAX_ENTRIES; i++) {
                if (artistContains(lib->entries[i].artist, lib->filterArtist) &&
                    strcasecontains(lib->entries[i].album, lib->searchQuery)) {
                    lib->viewList[lib->viewCount++] = lib->entries[i].album;
                }
            }
            qsort(lib->viewList, lib->viewCount, sizeof(char *), sortStrPtrs);
            dedup(lib->viewList, &lib->viewCount);
        } else {
            for (int i = 0; i < lib->count && lib->viewCount < LIB_MAX_ENTRIES; i++) {
                LibEntry *e = &lib->entries[i];
                if (strcasecontains(e->title, lib->searchQuery) ||
                    strcasecontains(e->artist, lib->searchQuery) ||
                    strcasecontains(e->album, lib->searchQuery)) {
                    lib->viewList[lib->viewCount++] = e->title;
                }
            }
        }
        return;
    }

    switch (lib->view) {
    case LIB_VIEW_ARTISTS: {
        static char artistNames[LIB_MAX_ENTRIES][LIB_STR_SHORT];
        int nameCount = 0;
        for (int i = 0; i < lib->count && nameCount < LIB_MAX_ENTRIES; i++) {
            char first[LIB_STR_SHORT];
            strncpy(first, lib->entries[i].artist, LIB_STR_SHORT - 1);
            first[LIB_STR_SHORT - 1] = '\0';
            char *semi = strchr(first, ';');
            if (semi) *semi = '\0';
            int len = strlen(first);
            while (len > 0 && first[len - 1] == ' ') first[--len] = '\0';

            bool dup = false;
            for (int k = 0; k < nameCount; k++) {
                if (strcasecmpLocal(artistNames[k], first) == 0) {
                    dup = true; break;
                }
            }
            if (!dup) {
                strncpy(artistNames[nameCount], first, LIB_STR_SHORT - 1);
                artistNames[nameCount][LIB_STR_SHORT - 1] = '\0';
                lib->viewList[lib->viewCount++] = artistNames[nameCount];
                nameCount++;
            }
        }
        qsort(lib->viewList, lib->viewCount, sizeof(char *), sortStrPtrs);
        dedup(lib->viewList, &lib->viewCount);
        break;
    }

    case LIB_VIEW_ALBUMS:
        for (int i = 0; i < lib->count && lib->viewCount < LIB_MAX_ENTRIES; i++) {
            if (artistContains(lib->entries[i].artist, lib->filterArtist)) {
                lib->viewList[lib->viewCount++] = lib->entries[i].album;
            }
        }
        qsort(lib->viewList, lib->viewCount, sizeof(char *), sortStrPtrs);
        dedup(lib->viewList, &lib->viewCount);
        break;

    case LIB_VIEW_TRACKS:
        for (int i = 0; i < lib->count && lib->viewCount < LIB_MAX_ENTRIES; i++) {
            if (artistContains(lib->entries[i].artist, lib->filterArtist) &&
                strcasecmpLocal(lib->entries[i].album, lib->filterAlbum) == 0) {
                lib->viewList[lib->viewCount++] = lib->entries[i].title;
            }
        }
        break;
    }
}

void libraryEnter(Library *lib) {
    if (lib->viewCount == 0) return;
    int idx = lib->selectedIndex;
    if (idx < 0 || idx >= lib->viewCount) return;

    if (lib->searchActive) return;

    switch (lib->view) {
    case LIB_VIEW_ARTISTS:
        strncpy(lib->filterArtist, lib->viewList[idx], LIB_STR_SHORT - 1);
        lib->filterArtist[LIB_STR_SHORT - 1] = '\0';
        lib->view = LIB_VIEW_ALBUMS;
        libraryBuildView(lib);
        break;

    case LIB_VIEW_ALBUMS:
        strncpy(lib->filterAlbum, lib->viewList[idx], LIB_STR_SHORT - 1);
        lib->filterAlbum[LIB_STR_SHORT - 1] = '\0';
        lib->view = LIB_VIEW_TRACKS;
        libraryBuildView(lib);
        break;

    case LIB_VIEW_TRACKS:
        break;
    }
}

void libraryGoBack(Library *lib) {
    if (lib->searchActive) {
        librarySearchClear(lib);
        return;
    }

    switch (lib->view) {
    case LIB_VIEW_ARTISTS:
        break;
    case LIB_VIEW_ALBUMS:
        lib->view = LIB_VIEW_ARTISTS;
        lib->filterArtist[0] = '\0';
        libraryBuildView(lib);
        break;
    case LIB_VIEW_TRACKS:
        lib->view = LIB_VIEW_ALBUMS;
        lib->filterAlbum[0] = '\0';
        libraryBuildView(lib);
        break;
    }
}

void librarySelectNext(Library *lib) {
    if (lib->selectedIndex < lib->viewCount - 1)
        lib->selectedIndex++;
}

void librarySelectPrev(Library *lib) {
    if (lib->selectedIndex > 0)
        lib->selectedIndex--;
}

void libraryPageDown(Library *lib, int pageSize) {
    lib->selectedIndex += pageSize;
    if (lib->selectedIndex >= lib->viewCount)
        lib->selectedIndex = lib->viewCount - 1;
    if (lib->selectedIndex < 0) lib->selectedIndex = 0;
}

void libraryPageUp(Library *lib, int pageSize) {
    lib->selectedIndex -= pageSize;
    if (lib->selectedIndex < 0) lib->selectedIndex = 0;
}

const char *libraryGetSelectedDisplay(Library *lib) {
    if (lib->selectedIndex < 0 || lib->selectedIndex >= lib->viewCount)
        return NULL;
    return lib->viewList[lib->selectedIndex];
}

const LibEntry *libraryGetSelectedEntry(Library *lib) {
    if (lib->viewCount == 0) return NULL;
    int idx = lib->selectedIndex;
    if (idx < 0 || idx >= lib->viewCount) return NULL;

    const char *display = lib->viewList[idx];

    if (lib->searchActive) {
        for (int i = 0; i < lib->count; i++) {
            if (lib->entries[i].title == display)
                return &lib->entries[i];
        }
        return NULL;
    }

    if (lib->view == LIB_VIEW_TRACKS) {
        for (int i = 0; i < lib->count; i++) {
            if (lib->entries[i].title == display &&
                artistContains(lib->entries[i].artist, lib->filterArtist) &&
                strcasecmpLocal(lib->entries[i].album, lib->filterAlbum) == 0)
                return &lib->entries[i];
        }
    }
    return NULL;
}

void librarySearchToggle(Library *lib) {
    lib->searchActive = !lib->searchActive;
    if (!lib->searchActive) {
        lib->searchQuery[0] = '\0';
        lib->searchLen = 0;
        libraryBuildView(lib);
    }
}

void librarySearchAddChar(Library *lib, char c) {
    if (lib->searchLen < (int)sizeof(lib->searchQuery) - 1) {
        lib->searchQuery[lib->searchLen++] = c;
        lib->searchQuery[lib->searchLen] = '\0';
        libraryBuildView(lib);
    }
}

void librarySearchBackspace(Library *lib) {
    if (lib->searchLen > 0) {
        lib->searchLen--;
        lib->searchQuery[lib->searchLen] = '\0';
        libraryBuildView(lib);
    }
}

void librarySearchClear(Library *lib) {
    lib->searchActive = false;
    lib->searchQuery[0] = '\0';
    lib->searchLen = 0;
    libraryBuildView(lib);
}

int libraryGetTracksForAlbum(Library *lib, const char *artist,
                             const char *album, const LibEntry **out, int maxOut) {
    int n = 0;
    for (int i = 0; i < lib->count && n < maxOut; i++) {
        if (artistContains(lib->entries[i].artist, artist) &&
            strcasecmpLocal(lib->entries[i].album, album) == 0) {
            out[n++] = &lib->entries[i];
        }
    }
    return n;
}
