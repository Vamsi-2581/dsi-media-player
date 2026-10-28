#ifndef FILE_BROWSER_H
#define FILE_BROWSER_H

#include <nds/ndstypes.h>

#define BROWSER_MAX_ENTRIES 256
#define BROWSER_MAX_PATH    512
#define BROWSER_NAME_LEN    128

typedef struct {
    char name[BROWSER_NAME_LEN];
    bool isDir;
    u32  size;
} BrowserEntry;

typedef struct {
    char          path[BROWSER_MAX_PATH];
    BrowserEntry  entries[BROWSER_MAX_ENTRIES];
    int           count;
    int           scrollOffset;
    int           selectedIndex;
    char          errorMsg[64];
} FileBrowser;

void browserInit(FileBrowser *b);
bool browserNavigate(FileBrowser *b, const char *path);
bool browserEnter(FileBrowser *b);
bool browserGoUp(FileBrowser *b);
void browserSelectNext(FileBrowser *b);
void browserSelectPrev(FileBrowser *b);
void browserPageDown(FileBrowser *b, int pageSize);
void browserPageUp(FileBrowser *b, int pageSize);
void browserSelectIndex(FileBrowser *b, int index);

const char *browserGetSelectedPath(FileBrowser *b);
bool browserSelectedIsDir(FileBrowser *b);
bool browserSelectedIsAudio(FileBrowser *b);
bool browserSelectedIsPlaylist(FileBrowser *b);

void browserAddAllToPlaylist(FileBrowser *b);

bool isAudioFile(const char *name);
bool isPlaylistFile(const char *name);

#endif
