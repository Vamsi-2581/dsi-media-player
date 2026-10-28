#ifndef SETTINGS_H
#define SETTINGS_H

#include <nds/ndstypes.h>
#include "player.h"

#define SETTINGS_PATH "/data/dsiplayer.cfg"
#define RESUME_PLAYLIST_PATH "/data/dsiplayer_resume.m3u"

typedef struct {
    u8         volume;
    RepeatMode repeat;
    bool       shuffle;
    char       lastPath[512];
    char       lastDir[512];
    u32        resumePositionMs;
    int        resumePlaylistIdx;
} Settings;

void settingsLoad(Settings *s);
void settingsSave(const Settings *s);
void settingsApply(const Settings *s);
void settingsCapture(Settings *s);
bool settingsResume(const Settings *s);

#endif
