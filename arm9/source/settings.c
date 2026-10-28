#include "settings.h"
#include "audio/audio_stream.h"
#include "ui/ui.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void ensureDataDir(void) {
    mkdir("/data", 0755);
}

static int parseUint(const char *s) {
    int v = 0;
    for (const char *p = s; *p >= '0' && *p <= '9'; p++)
        v = v * 10 + (*p - '0');
    return v;
}

void settingsLoad(Settings *s) {
    s->volume = 127;
    s->repeat = REPEAT_OFF;
    s->shuffle = false;
    s->theme = THEME_BLUE;
    s->showAlbumTiles = true;
    s->lastPath[0] = '\0';
    strcpy(s->lastDir, "/");
    s->resumePositionMs = 0;
    s->resumePlaylistIdx = -1;

    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;

    char line[600];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        if (strcmp(key, "volume") == 0) {
            int v = parseUint(val);
            if (v > 127) v = 127;
            s->volume = (u8)v;
        } else if (strcmp(key, "repeat") == 0) {
            if (strcmp(val, "all") == 0) s->repeat = REPEAT_ALL;
            else if (strcmp(val, "one") == 0) s->repeat = REPEAT_ONE;
            else s->repeat = REPEAT_OFF;
        } else if (strcmp(key, "shuffle") == 0) {
            s->shuffle = (strcmp(val, "1") == 0);
        } else if (strcmp(key, "theme") == 0) {
            int theme = parseUint(val);
            if (theme < THEME_COUNT) s->theme = (u8)theme;
        } else if (strcmp(key, "albumtiles") == 0) {
            s->showAlbumTiles = (strcmp(val, "1") == 0);
        } else if (strcmp(key, "lastpath") == 0) {
            strncpy(s->lastPath, val, sizeof(s->lastPath) - 1);
        } else if (strcmp(key, "lastdir") == 0) {
            strncpy(s->lastDir, val, sizeof(s->lastDir) - 1);
        } else if (strcmp(key, "resumepos") == 0) {
            s->resumePositionMs = (u32)parseUint(val);
        } else if (strcmp(key, "resumeidx") == 0) {
            s->resumePlaylistIdx = parseUint(val);
        }
    }
    fclose(f);
}

void settingsSave(const Settings *s) {
    ensureDataDir();
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;

    fprintf(f, "volume=%d\n", s->volume);
    fprintf(f, "repeat=%s\n",
            s->repeat == REPEAT_ALL ? "all" :
            s->repeat == REPEAT_ONE ? "one" : "off");
    fprintf(f, "shuffle=%d\n", s->shuffle ? 1 : 0);
    fprintf(f, "theme=%u\n", s->theme);
    fprintf(f, "albumtiles=%d\n", s->showAlbumTiles ? 1 : 0);
    if (s->lastPath[0])
        fprintf(f, "lastpath=%s\n", s->lastPath);
    if (s->lastDir[0])
        fprintf(f, "lastdir=%s\n", s->lastDir);
    fprintf(f, "resumepos=%lu\n", (unsigned long)s->resumePositionMs);
    if (s->resumePlaylistIdx >= 0)
        fprintf(f, "resumeidx=%d\n", s->resumePlaylistIdx);

    fclose(f);
}

void settingsApply(const Settings *s) {
    uiSetTheme((ThemeColor)s->theme);
    uiSetAlbumTiles(s->showAlbumTiles);
    playerSetVolume(s->volume);
    while (playerGetRepeat() != s->repeat) playerCycleRepeat();
    if (playerGetShuffle() != s->shuffle) playerToggleShuffle();
}

void settingsCapture(Settings *s) {
    s->theme = (u8)uiGetTheme();
    s->showAlbumTiles = uiGetAlbumTiles();
    s->volume = playerGetVolume();
    s->repeat = playerGetRepeat();
    s->shuffle = playerGetShuffle();
    const char *curFile = playerGetCurrentFile();
    if (curFile && curFile[0]) {
        strncpy(s->lastPath, curFile, sizeof(s->lastPath) - 1);
    }
    s->resumePositionMs = playerGetPositionMs();
    Playlist *pl = playerGetPlaylist();
    s->resumePlaylistIdx = pl->current;

    if (pl->count > 0) {
        playerPlaylistSave(RESUME_PLAYLIST_PATH);
    }
}

bool settingsResume(const Settings *s) {
    if (s->lastPath[0] == '\0') return false;

    if (playerPlaylistLoad(RESUME_PLAYLIST_PATH)) {
        Playlist *pl = playerGetPlaylist();
        if (pl->count > 0 && s->resumePlaylistIdx >= 0 &&
            s->resumePlaylistIdx < pl->count) {
            playerPlayFromPlaylist(s->resumePlaylistIdx);
            if (s->resumePositionMs > 0) {
                playerSeekTo(s->resumePositionMs);
            }
            audioStreamPause();
            return true;
        }
    }

    if (playerPlayFile(s->lastPath)) {
        if (s->resumePositionMs > 0) {
            playerSeekTo(s->resumePositionMs);
        }
        audioStreamPause();
        return true;
    }
    return false;
}
