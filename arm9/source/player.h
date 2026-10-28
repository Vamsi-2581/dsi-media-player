#ifndef PLAYER_H
#define PLAYER_H

#include <nds/ndstypes.h>
#include "audio_stream.h"

typedef enum {
    REPEAT_OFF = 0,
    REPEAT_ALL,
    REPEAT_ONE
} RepeatMode;

#define PLAYLIST_MAX 512

typedef struct {
    char    *entries[PLAYLIST_MAX];
    int      count;
    int      current;
    bool     shuffle;
    RepeatMode repeat;
    int      shuffleOrder[PLAYLIST_MAX];
} Playlist;

void playerInit(void);
void playerUpdate(void);

bool playerPlayFile(const char *path);
void playerPlayPause(void);
void playerStop(void);
void playerNext(void);
void playerPrev(void);

void playerSetVolume(u8 vol);
u8   playerGetVolume(void);
void playerVolumeUp(void);
void playerVolumeDown(void);

void playerSeekForward(void);
void playerSeekBackward(void);
void playerSeekTo(u32 ms);

void playerToggleShuffle(void);
void playerCycleRepeat(void);
bool playerGetShuffle(void);
RepeatMode playerGetRepeat(void);

/* Playlist management */
Playlist *playerGetPlaylist(void);
void playerPlaylistClear(void);
void playerPlaylistAdd(const char *path);
void playerPlaylistRemove(int index);
void playerPlaylistMoveUp(int index);
void playerPlaylistMoveDown(int index);
bool playerPlaylistSave(const char *path);
bool playerPlaylistLoad(const char *path);
void playerPlayFromPlaylist(int index);

StreamState playerGetState(void);
const AudioInfo *playerGetInfo(void);
const AudioMeta *playerGetMeta(void);
u32 playerGetPositionMs(void);
u32 playerGetDurationMs(void);
const char *playerGetCurrentFile(void);

#endif
