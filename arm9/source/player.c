#include "player.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SEEK_STEP_MS 5000
#define VOL_STEP     8

static Playlist playlist;
static bool autoAdvance;


static void shuffleGenerate(void) {
    for (int i = 0; i < playlist.count; i++)
        playlist.shuffleOrder[i] = i;

    for (int i = playlist.count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = playlist.shuffleOrder[i];
        playlist.shuffleOrder[i] = playlist.shuffleOrder[j];
        playlist.shuffleOrder[j] = tmp;
    }
}

static int getPlayIndex(int logical) {
    if (playlist.shuffle && logical >= 0 && logical < playlist.count)
        return playlist.shuffleOrder[logical];
    return logical;
}

static bool playAtIndex(int idx) {
    if (idx < 0 || idx >= playlist.count) return false;
    int fileIdx = getPlayIndex(idx);
    if (!playlist.entries[fileIdx]) return false;

    playlist.current = idx;
    return playerPlayFile(playlist.entries[fileIdx]);
}

void playerInit(void) {
    audioStreamInit();
    memset(&playlist, 0, sizeof(playlist));
    playlist.current = -1;
    playlist.repeat = REPEAT_OFF;
    playlist.shuffle = false;
    autoAdvance = false;
}

static const char *getNextGaplessPath(void) {
    if (playlist.count == 0) return NULL;

    if (playlist.repeat == REPEAT_ONE) {
        int idx = playlist.current;
        if (idx < 0) idx = 0;
        int fileIdx = getPlayIndex(idx);
        return playlist.entries[fileIdx];
    }

    int next = playlist.current + 1;
    if (next >= playlist.count) {
        if (playlist.repeat == REPEAT_ALL) {
            next = 0;
        } else {
            return NULL;
        }
    }

    int fileIdx = getPlayIndex(next);
    if (fileIdx < 0 || fileIdx >= playlist.count) return NULL;
    return playlist.entries[fileIdx];
}

static void advancePlaylistIndex(void) {
    if (playlist.repeat == REPEAT_ONE) return;

    int next = playlist.current + 1;
    if (next >= playlist.count) {
        if (playlist.repeat == REPEAT_ALL) {
            if (playlist.shuffle) shuffleGenerate();
            next = 0;
        } else {
            return;
        }
    }
    playlist.current = next;
}

void playerUpdate(void) {
    audioStreamUpdate();

    if (autoAdvance && audioStreamIsDecoderDone()) {
        if (audioStreamRingAvailable() == 0) {
            const char *nextPath = getNextGaplessPath();
            if (nextPath) {
                advancePlaylistIndex();
                if (audioStreamOpenGapless(nextPath)) {
                    return;
                }
            }
        }
    }

    if (autoAdvance && audioStreamGetState() == STREAM_STOPPED) {
        autoAdvance = false;
        playerNext();
    }
}

bool playerPlayFile(const char *path) {
    autoAdvance = false;
    bool ok = audioStreamOpen(path);
    if (ok) autoAdvance = true;
    return ok;
}

void playerPlayPause(void) {
    StreamState st = audioStreamGetState();
    if (st == STREAM_PLAYING) {
        audioStreamPause();
    } else if (st == STREAM_PAUSED) {
        audioStreamPlay();
        autoAdvance = true;
    } else if (st == STREAM_STOPPED && playlist.current >= 0) {
        playAtIndex(playlist.current);
    }
}

void playerStop(void) {
    autoAdvance = false;
    audioStreamStop();
}

void playerNext(void) {
    if (playlist.count == 0) return;

    if (playlist.repeat == REPEAT_ONE) {
        int idx = playlist.current;
        if (idx < 0) idx = 0;
        playAtIndex(idx);
        autoAdvance = true;
        return;
    }

    int next = playlist.current + 1;
    if (next >= playlist.count) {
        if (playlist.repeat == REPEAT_ALL) {
            if (playlist.shuffle) shuffleGenerate();
            next = 0;
        } else {
            audioStreamStop();
            autoAdvance = false;
            return;
        }
    }

    if (playAtIndex(next)) {
        autoAdvance = true;
    } else {
        /* Skip unplayable files, try next few */
        for (int tries = 0; tries < 5 && next < playlist.count; tries++) {
            next++;
            if (next >= playlist.count && playlist.repeat == REPEAT_ALL) next = 0;
            if (next >= playlist.count) break;
            if (playAtIndex(next)) { autoAdvance = true; return; }
        }
        audioStreamStop();
        autoAdvance = false;
    }
}

void playerPrev(void) {
    if (playlist.count == 0) return;

    /* If >3s into the track, restart it instead of going back */
    if (audioStreamGetPositionMs() > 3000) {
        int idx = playlist.current;
        if (idx < 0) idx = 0;
        playAtIndex(idx);
        autoAdvance = true;
        return;
    }

    int prev = playlist.current - 1;
    if (prev < 0) {
        if (playlist.repeat == REPEAT_ALL) {
            prev = playlist.count - 1;
        } else {
            prev = 0;
        }
    }

    if (playAtIndex(prev)) autoAdvance = true;
}

void playerSetVolume(u8 vol) {
    audioStreamSetVolume(vol);
}

u8 playerGetVolume(void) {
    return audioStreamGetVolume();
}

void playerVolumeUp(void) {
    u8 vol = playerGetVolume();
    if (vol <= 127 - VOL_STEP) vol += VOL_STEP;
    else vol = 127;
    playerSetVolume(vol);
}

void playerVolumeDown(void) {
    u8 vol = playerGetVolume();
    if (vol >= VOL_STEP) vol -= VOL_STEP;
    else vol = 0;
    playerSetVolume(vol);
}

void playerSeekForward(void) {
    u32 pos = audioStreamGetPositionMs();
    u32 dur = audioStreamGetDurationMs();
    pos += SEEK_STEP_MS;
    if (pos > dur && dur > 0) pos = dur - 100;
    audioStreamSeekMs(pos);
}

void playerSeekBackward(void) {
    u32 pos = audioStreamGetPositionMs();
    if (pos > SEEK_STEP_MS) pos -= SEEK_STEP_MS;
    else pos = 0;
    audioStreamSeekMs(pos);
}

void playerSeekTo(u32 ms) {
    audioStreamSeekMs(ms);
}

void playerToggleShuffle(void) {
    playlist.shuffle = !playlist.shuffle;
    if (playlist.shuffle) shuffleGenerate();
}

void playerCycleRepeat(void) {
    playlist.repeat = (RepeatMode)((playlist.repeat + 1) % 3);
}

bool playerGetShuffle(void) { return playlist.shuffle; }
RepeatMode playerGetRepeat(void) { return playlist.repeat; }

Playlist *playerGetPlaylist(void) { return &playlist; }

void playerPlaylistClear(void) {
    for (int i = 0; i < playlist.count; i++) {
        free(playlist.entries[i]);
        playlist.entries[i] = NULL;
    }
    playlist.count = 0;
    playlist.current = -1;
}

void playerPlaylistAdd(const char *path) {
    if (playlist.count >= PLAYLIST_MAX) return;
    playlist.entries[playlist.count] = strdup(path);
    playlist.count++;
    if (playlist.shuffle) shuffleGenerate();
}

void playerPlaylistRemove(int index) {
    if (index < 0 || index >= playlist.count) return;
    free(playlist.entries[index]);
    for (int i = index; i < playlist.count - 1; i++) {
        playlist.entries[i] = playlist.entries[i + 1];
    }
    playlist.entries[playlist.count - 1] = NULL;
    playlist.count--;

    if (playlist.current >= playlist.count) playlist.current = playlist.count - 1;
    if (playlist.current == index) {
        /* Current track removed; adjust but don't auto-play */
    } else if (playlist.current > index) {
        playlist.current--;
    }
    if (playlist.shuffle) shuffleGenerate();
}

void playerPlaylistMoveUp(int index) {
    if (index <= 0 || index >= playlist.count) return;
    char *tmp = playlist.entries[index - 1];
    playlist.entries[index - 1] = playlist.entries[index];
    playlist.entries[index] = tmp;
    if (playlist.current == index) playlist.current--;
    else if (playlist.current == index - 1) playlist.current++;
}

void playerPlaylistMoveDown(int index) {
    if (index < 0 || index >= playlist.count - 1) return;
    char *tmp = playlist.entries[index + 1];
    playlist.entries[index + 1] = playlist.entries[index];
    playlist.entries[index] = tmp;
    if (playlist.current == index) playlist.current++;
    else if (playlist.current == index + 1) playlist.current--;
}

bool playerPlaylistSave(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "#EXTM3U\n");
    for (int i = 0; i < playlist.count; i++) {
        if (playlist.entries[i])
            fprintf(f, "%s\n", playlist.entries[i]);
    }
    fclose(f);
    return true;
}

bool playerPlaylistLoad(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    playerPlaylistClear();

    /* Get directory of the M3U file for resolving relative paths */
    char m3uDir[512];
    strncpy(m3uDir, path, sizeof(m3uDir) - 1);
    m3uDir[sizeof(m3uDir) - 1] = '\0';
    char *lastSlash = strrchr(m3uDir, '/');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
    } else {
        strcpy(m3uDir, "/");
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0 || line[0] == '#') continue;
        if (playlist.count >= PLAYLIST_MAX) break;

        if (line[0] == '/') {
            playerPlaylistAdd(line);
        } else {
            char fullPath[512];
            snprintf(fullPath, sizeof(fullPath), "%s%s", m3uDir, line);
            playerPlaylistAdd(fullPath);
        }
    }

    fclose(f);
    return true;
}

void playerPlayFromPlaylist(int index) {
    if (playAtIndex(index))
        autoAdvance = true;
}

StreamState playerGetState(void) { return audioStreamGetState(); }
const AudioInfo *playerGetInfo(void) { return audioStreamGetInfo(); }
const AudioMeta *playerGetMeta(void) { return audioStreamGetMeta(); }
u32 playerGetPositionMs(void) { return audioStreamGetPositionMs(); }
u32 playerGetDurationMs(void) { return audioStreamGetDurationMs(); }
const char *playerGetCurrentFile(void) { return audioStreamGetFilePath(); }
