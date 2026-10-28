#ifndef LIBRARY_H
#define LIBRARY_H

#include <nds/ndstypes.h>

#define LIB_MAX_ENTRIES  1024
#define LIB_STR_SHORT    64
#define LIB_STR_PATH     256

#define LIB_CACHE_PATH   "/data/dsiplayer_library.dat"

typedef struct {
    char path[LIB_STR_PATH];
    char title[LIB_STR_SHORT];
    char artist[LIB_STR_SHORT];
    char album[LIB_STR_SHORT];
    char trackNum[8];
    char year[8];
} LibEntry;

typedef enum {
    LIB_VIEW_ARTISTS,
    LIB_VIEW_ALBUMS,
    LIB_VIEW_TRACKS
} LibView;

typedef enum {
    LIB_IDLE,
    LIB_SCANNING,
    LIB_PRECACHING,
    LIB_READY
} LibState;

typedef struct {
    LibEntry  entries[LIB_MAX_ENTRIES];
    int       count;
    LibState  state;
    int       scanProgress;
    int       scanTotal;

    LibView   view;
    int       selectedIndex;
    int       scrollOffset;

    char      filterArtist[LIB_STR_SHORT];
    char      filterAlbum[LIB_STR_SHORT];

    char     *viewList[LIB_MAX_ENTRIES];
    int       viewCount;

    char      searchQuery[32];
    int       searchLen;
    bool      searchActive;
} Library;

void libraryInit(Library *lib);
bool libraryLoadCache(Library *lib);
bool librarySaveCache(Library *lib);

void libraryScanStart(Library *lib, const char *rootPath);
bool libraryScanStep(Library *lib);
bool libraryPreCacheStep(Library *lib);

void libraryBuildView(Library *lib);
void libraryEnter(Library *lib);
void libraryGoBack(Library *lib);
void librarySelectNext(Library *lib);
void librarySelectPrev(Library *lib);
void libraryPageDown(Library *lib, int pageSize);
void libraryPageUp(Library *lib, int pageSize);

const char *libraryGetSelectedDisplay(Library *lib);
const LibEntry *libraryGetSelectedEntry(Library *lib);

void librarySearchToggle(Library *lib);
void librarySearchAddChar(Library *lib, char c);
void librarySearchBackspace(Library *lib);
void librarySearchClear(Library *lib);

int libraryGetTracksForAlbum(Library *lib, const char *artist,
                             const char *album, const LibEntry **out, int maxOut);
const LibEntry *libraryFindByPath(Library *lib, const char *path);
bool libraryArtistMatch(const char *entryArtist, const char *filterArtist);
const char *libraryGetAlbumDir(Library *lib, const char *albumName);

#endif
