#include "file_browser.h"
#include "player.h"
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static bool hasExt(const char *name, const char *ext) {
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);
    if (nlen < elen) return false;
    const char *p = name + nlen - elen;
    for (size_t i = 0; i < elen; i++) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != ext[i]) return false;
    }
    return true;
}

bool isAudioFile(const char *name) {
    return hasExt(name, ".wav") || hasExt(name, ".wave") ||
           hasExt(name, ".flac") ||
           hasExt(name, ".mp3") ||
           hasExt(name, ".ogg") || hasExt(name, ".oga");
}

bool isPlaylistFile(const char *name) {
    return hasExt(name, ".m3u") || hasExt(name, ".m3u8");
}

static int compareEntries(const void *a, const void *b) {
    const BrowserEntry *ea = (const BrowserEntry *)a;
    const BrowserEntry *eb = (const BrowserEntry *)b;
    /* Directories first, then alphabetical */
    if (ea->isDir && !eb->isDir) return -1;
    if (!ea->isDir && eb->isDir) return 1;
    return strcasecmp(ea->name, eb->name);
}

void browserInit(FileBrowser *b) {
    memset(b, 0, sizeof(FileBrowser));
    strcpy(b->path, "/");
}

bool browserNavigate(FileBrowser *b, const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        strncpy(b->errorMsg, "Cannot open directory", sizeof(b->errorMsg) - 1);
        return false;
    }

    b->count = 0;
    b->errorMsg[0] = '\0';
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL && b->count < BROWSER_MAX_ENTRIES) {
        if (strcmp(ent->d_name, ".") == 0) continue;
        if (strcmp(ent->d_name, "..") == 0) continue;
        if (ent->d_name[0] == '.' && ent->d_name[1] == '_') continue;

        BrowserEntry *e = &b->entries[b->count];
        strncpy(e->name, ent->d_name, BROWSER_NAME_LEN - 1);
        e->name[BROWSER_NAME_LEN - 1] = '\0';

        char fullPath[BROWSER_MAX_PATH];
        if (strcmp(path, "/") == 0)
            snprintf(fullPath, sizeof(fullPath), "/%s", ent->d_name);
        else
            snprintf(fullPath, sizeof(fullPath), "%s/%s", path, ent->d_name);

        struct stat st;
        if (stat(fullPath, &st) == 0) {
            e->isDir = S_ISDIR(st.st_mode);
            e->size = st.st_size;
        } else {
            e->isDir = (ent->d_type == DT_DIR);
            e->size = 0;
        }

        /* Show only directories, audio files, and playlists */
        if (!e->isDir && !isAudioFile(e->name) && !isPlaylistFile(e->name))
            continue;

        b->count++;
    }
    closedir(dir);

    qsort(b->entries, b->count, sizeof(BrowserEntry), compareEntries);

    strncpy(b->path, path, BROWSER_MAX_PATH - 1);
    b->path[BROWSER_MAX_PATH - 1] = '\0';
    b->scrollOffset = 0;
    b->selectedIndex = 0;

    return true;
}

bool browserEnter(FileBrowser *b) {
    if (b->selectedIndex < 0 || b->selectedIndex >= b->count)
        return false;

    BrowserEntry *e = &b->entries[b->selectedIndex];
    char newPath[BROWSER_MAX_PATH];

    if (strcmp(b->path, "/") == 0)
        snprintf(newPath, sizeof(newPath), "/%s", e->name);
    else
        snprintf(newPath, sizeof(newPath), "%s/%s", b->path, e->name);

    if (e->isDir) {
        return browserNavigate(b, newPath);
    }
    return false;
}

bool browserGoUp(FileBrowser *b) {
    if (strcmp(b->path, "/") == 0) return false;

    char parent[BROWSER_MAX_PATH];
    strncpy(parent, b->path, BROWSER_MAX_PATH);
    char *lastSlash = strrchr(parent, '/');
    if (lastSlash && lastSlash != parent) {
        *lastSlash = '\0';
    } else {
        strcpy(parent, "/");
    }
    return browserNavigate(b, parent);
}

void browserSelectNext(FileBrowser *b) {
    if (b->selectedIndex < b->count - 1)
        b->selectedIndex++;
}

void browserSelectPrev(FileBrowser *b) {
    if (b->selectedIndex > 0)
        b->selectedIndex--;
}

void browserPageDown(FileBrowser *b, int pageSize) {
    b->selectedIndex += pageSize;
    if (b->selectedIndex >= b->count)
        b->selectedIndex = b->count - 1;
    if (b->selectedIndex < 0)
        b->selectedIndex = 0;
}

void browserPageUp(FileBrowser *b, int pageSize) {
    b->selectedIndex -= pageSize;
    if (b->selectedIndex < 0)
        b->selectedIndex = 0;
}

void browserSelectIndex(FileBrowser *b, int index) {
    if (index >= 0 && index < b->count)
        b->selectedIndex = index;
}

const char *browserGetSelectedPath(FileBrowser *b) {
    static char fullPath[BROWSER_MAX_PATH];
    if (b->selectedIndex < 0 || b->selectedIndex >= b->count) return NULL;
    BrowserEntry *e = &b->entries[b->selectedIndex];
    if (strcmp(b->path, "/") == 0)
        snprintf(fullPath, sizeof(fullPath), "/%s", e->name);
    else
        snprintf(fullPath, sizeof(fullPath), "%s/%s", b->path, e->name);
    return fullPath;
}

bool browserSelectedIsDir(FileBrowser *b) {
    if (b->selectedIndex < 0 || b->selectedIndex >= b->count) return false;
    return b->entries[b->selectedIndex].isDir;
}

bool browserSelectedIsAudio(FileBrowser *b) {
    if (b->selectedIndex < 0 || b->selectedIndex >= b->count) return false;
    return isAudioFile(b->entries[b->selectedIndex].name);
}

bool browserSelectedIsPlaylist(FileBrowser *b) {
    if (b->selectedIndex < 0 || b->selectedIndex >= b->count) return false;
    return isPlaylistFile(b->entries[b->selectedIndex].name);
}

void browserAddAllToPlaylist(FileBrowser *b) {
    for (int i = 0; i < b->count; i++) {
        if (b->entries[i].isDir) continue;
        if (!isAudioFile(b->entries[i].name)) continue;

        char fullPath[BROWSER_MAX_PATH];
        if (strcmp(b->path, "/") == 0)
            snprintf(fullPath, sizeof(fullPath), "/%s", b->entries[i].name);
        else
            snprintf(fullPath, sizeof(fullPath), "%s/%s", b->path, b->entries[i].name);

        playerPlaylistAdd(fullPath);
    }
}
