/*
 * Bottom screen — File browser, playlist, and transport controls
 *
 * Layout (256x192):
 *   [0-15]    Tab bar: [Browser] [Playlist]
 *   [16-155]  Content area (browser list or playlist list)
 *   [156-167] Seek bar (touch-draggable)
 *   [168-191] Transport controls + repeat/shuffle toggles
 */

#include "ui.h"
#include "player.h"
#include "file_browser.h"
#include "library.h"
#include "cover_art.h"
#include <nds.h>
#include <stdio.h>
#include <string.h>

#define TAB_BAR_H      16
#define CONTENT_TOP     16
#define CONTENT_H       116
#define TRACK_INFO_TOP  132
#define TRACK_INFO_H    22
#define SEEK_BAR_TOP    154
#define SEEK_BAR_H      10
#define CONTROL_TOP     166
#define CONTROL_H       26
#define LIST_ITEM_H     12
#define LIST_VISIBLE    (CONTENT_H / LIST_ITEM_H)

/* Marquee scroll delay (render frames before scrolling starts) */
#define MARQUEE_DELAY   40
#define MARQUEE_SPEED   1

/* Preview delay (render frames before preview starts) */
#define PREVIEW_DELAY   10

/* Touch regions for transport controls */
static const TouchRegion rPrev      = {  4, CONTROL_TOP + 2, 36, 22};
static const TouchRegion rPlayPause = { 44, CONTROL_TOP,     40, 24};
static const TouchRegion rNext      = { 88, CONTROL_TOP + 2, 36, 22};
static const TouchRegion rStop      = {128, CONTROL_TOP + 2, 36, 22};
static const TouchRegion rRepeat    = {172, CONTROL_TOP + 2, 36, 22};
static const TouchRegion rShuffle   = {216, CONTROL_TOP + 2, 36, 22};

/* Touch regions for tab bar (3 tabs: 85+86+85 = 256) */
static const TouchRegion rTabBrowser  = {0, 0, 85, TAB_BAR_H};
static const TouchRegion rTabSettings = {85, 0, 86, TAB_BAR_H};
static const TouchRegion rTabLibrary  = {171, 0, 85, TAB_BAR_H};

/* Touch region for seek bar */
static const TouchRegion rSeekBar = {8, SEEK_BAR_TOP, 240, SEEK_BAR_H};

/* Track info marquee */
static u32 trackInfoMarqueeTimer;
static int trackInfoMarqueeOfs;
static char trackInfoLastTitle[128];

static FileBrowser browser;
static bool browserInitialized = false;

static Library library;
static bool libraryInitialized = false;

/* Seek bar drag state */
static bool seekDragging = false;
static float seekDragPos = 0.0f;

/* Marquee state */
static int marqueeSelIdx = -1;
static BottomTab marqueeTab = TAB_BROWSER;
static u32 marqueeTimer = 0;
static int marqueeOffset = 0;

/* Preview state */
static int previewSelIdx = -1;
static u32 previewTimer = 0;
static bool previewStarted = false;

/* Library preview state */
static int libPreviewSelIdx = -1;
static u32 libPreviewTimer = 0;
static bool libPreviewStarted = false;

/* Animated now-playing indicator */
static u32 animFrame = 0;

static void drawNowPlayingBars(u16 *fb, int x, int y, u16 color) {
    static const u8 barPattern[4][3] = {
        {6, 3, 5}, {4, 6, 3}, {5, 4, 6}, {3, 5, 4}
    };
    int phase = (animFrame / 4) % 4;
    for (int b = 0; b < 3; b++) {
        int h = barPattern[phase][b];
        int bx = x + b * 3;
        int by = y + 8 - h;
        for (int row = 0; row < h; row++) {
            int py = by + row;
            if (py < 0 || py >= 192) continue;
            fb[py * 256 + bx] = color | BIT(15);
            fb[py * 256 + bx + 1] = color | BIT(15);
        }
    }
}

/* Sleep timer */
static int sleepTimerMs = 0;
static bool sleepTimerRunning = false;
static int settingsSelectedItem = 0;
#define SETTINGS_ITEM_COUNT 6
static const int sleepOptions[] = { 0, 15, 30, 45, 60, 90, 120 };
#define SLEEP_OPTION_COUNT 7

/* Theme / personalisation */
static ThemeColor currentTheme = THEME_BLUE;
static bool showAlbumTiles = true;

void uiSetTheme(ThemeColor theme) {
    if (theme >= THEME_BLUE && theme < THEME_COUNT)
        currentTheme = theme;
}

ThemeColor uiGetTheme(void) {
    return currentTheme;
}

void uiSetAlbumTiles(bool enabled) {
    showAlbumTiles = enabled;
}

bool uiGetAlbumTiles(void) {
    return showAlbumTiles;
}

static const char *themeNames[] = { "Blue", "Purple", "Green", "Red", "Orange" };

static u16 themeAccent(void) {
    switch (currentTheme) {
    case THEME_PURPLE: return RGB15(16, 8, 28);
    case THEME_GREEN:  return RGB15(6, 22, 10);
    case THEME_RED:    return RGB15(28, 8, 8);
    case THEME_ORANGE: return RGB15(28, 16, 4);
    default:           return COL_ACCENT;
    }
}

static u16 themeAccentLight(void) {
    switch (currentTheme) {
    case THEME_PURPLE: return RGB15(20, 12, 30);
    case THEME_GREEN:  return RGB15(10, 26, 14);
    case THEME_RED:    return RGB15(30, 12, 12);
    case THEME_ORANGE: return RGB15(30, 20, 8);
    default:           return COL_ACCENT_LIGHT;
    }
}

static u16 themeProgressFg(void) {
    switch (currentTheme) {
    case THEME_PURPLE: return RGB15(14, 8, 26);
    case THEME_GREEN:  return RGB15(8, 24, 10);
    case THEME_RED:    return RGB15(26, 8, 8);
    case THEME_ORANGE: return RGB15(26, 16, 6);
    default:           return COL_PROGRESS_FG;
    }
}

static u16 themeIconActive(void) {
    switch (currentTheme) {
    case THEME_PURPLE: return RGB15(22, 14, 31);
    case THEME_GREEN:  return RGB15(10, 28, 14);
    case THEME_RED:    return RGB15(31, 14, 10);
    case THEME_ORANGE: return RGB15(31, 22, 10);
    default:           return COL_ICON_ACTIVE;
    }
}

u16 uiThemeAccent(void) { return themeAccent(); }
u16 uiThemeAccentLight(void) { return themeAccentLight(); }
u16 uiThemeProgressFg(void) { return themeProgressFg(); }
u16 uiThemeIconActive(void) { return themeIconActive(); }

/* Album tile cache */
#define TILE_COLS       4
#define TILE_ART_SIZE   48
#define TILE_CELL_W     64
#define TILE_CELL_H     60
#define TILE_CACHE_MAX  12

static u16 tileCacheBuf[TILE_CACHE_MAX][ART_DISPLAY_SIZE * ART_DISPLAY_SIZE];
static int tileCacheAlbum[TILE_CACHE_MAX];
static int tileCacheState[TILE_CACHE_MAX];  /* 0=empty, 1=loaded, 2=no_art */
static int tileCacheNext = 0;
static int tileCacheViewId = -1;

static void tileCacheInvalidate(void) {
    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        tileCacheState[i] = 0;
        tileCacheAlbum[i] = -1;
    }
    tileCacheNext = 0;
}

static const u16 *tileCacheGet(int albumIdx) {
    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        if (tileCacheAlbum[i] == albumIdx && tileCacheState[i] == 1)
            return tileCacheBuf[i];
    }
    return NULL;
}

static bool tileCacheQueried(int albumIdx) {
    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        if (tileCacheAlbum[i] == albumIdx && tileCacheState[i] != 0)
            return true;
    }
    return false;
}

static const u16 *tileCacheLoad(Library *lib, int albumIdx) {
    if (albumIdx < 0 || albumIdx >= lib->viewCount) return NULL;

    const char *albumName = lib->viewList[albumIdx];
    const char *dir = libraryGetAlbumDir(lib, albumName);

    int slot = tileCacheNext;
    tileCacheNext = (tileCacheNext + 1) % TILE_CACHE_MAX;

    if (dir && coverArtLoadCachedDir(dir, tileCacheBuf[slot])) {
        tileCacheAlbum[slot] = albumIdx;
        tileCacheState[slot] = 1;
        return tileCacheBuf[slot];
    }

    tileCacheAlbum[slot] = albumIdx;
    tileCacheState[slot] = 2;
    return NULL;
}

void uiSetSleepTimer(int minutes) {
    if (minutes <= 0) { sleepTimerRunning = false; sleepTimerMs = 0; return; }
    sleepTimerMs = minutes * 60 * 1000;
    sleepTimerRunning = true;
}
int uiGetSleepTimerRemaining(void) { return sleepTimerMs / 1000; }
bool uiSleepTimerActive(void) { return sleepTimerRunning; }
void uiSleepTimerTick(void) {
    if (!sleepTimerRunning) return;
    sleepTimerMs -= 16;
    if (sleepTimerMs <= 0) {
        sleepTimerMs = 0;
        sleepTimerRunning = false;
        playerStop();
    }
}

/* Track change detection for auto-advance */
static char lastPlayingFile[512];

static void ensureBrowserInit(const char *startPath) {
    if (!browserInitialized) {
        browserInit(&browser);
        browserNavigate(&browser, startPath);
        browserInitialized = true;
    }
}

FileBrowser *uiGetBrowser(void) {
    ensureBrowserInit("/");
    return &browser;
}

Library *uiGetLibrary(void) {
    if (!libraryInitialized) {
        libraryInit(&library);
        libraryInitialized = true;
    }
    return &library;
}

bool uiIsSeekDragging(void) {
    return seekDragging;
}

float uiGetSeekDragPos(void) {
    return seekDragPos;
}

static void drawStringScrolled(u16 *fb, int x, int y, int maxW,
                                 const char *str, u16 color, int scrollOfs) {
    int strW = drawStringWidth(str);
    if (strW <= maxW || scrollOfs == 0) {
        drawStringClipped(fb, x, y, maxW, str, color);
        return;
    }

    int maxChars = maxW / FONT_W;
    int charOfs = scrollOfs / FONT_W;
    int len = strlen(str);

    if (charOfs >= len) charOfs = len - 1;

    const char *start = str + charOfs;
    int remaining = len - charOfs;
    int showChars = (remaining < maxChars) ? remaining : maxChars;

    char buf[CHARS_PER_LINE + 1];
    if (showChars > CHARS_PER_LINE) showChars = CHARS_PER_LINE;
    memcpy(buf, start, showChars);
    buf[showChars] = '\0';

    drawString(fb, x, y, buf, color);
}

static void marqueeUpdate(int selectedIdx, BottomTab tab) {
    if (selectedIdx != marqueeSelIdx || tab != marqueeTab) {
        marqueeSelIdx = selectedIdx;
        marqueeTab = tab;
        marqueeTimer = 0;
        marqueeOffset = 0;
        return;
    }
    marqueeTimer++;
    if (marqueeTimer > MARQUEE_DELAY) {
        marqueeOffset += MARQUEE_SPEED;
    }
}

/* Check if a browser entry matches the currently playing file */
static bool isEntryPlaying(BrowserEntry *e) {
    if (e->isDir) return false;
    const char *curFile = playerGetCurrentFile();
    if (!curFile || curFile[0] == '\0') return false;

    const char *curName = strrchr(curFile, '/');
    if (curName) curName++; else curName = curFile;

    return (strcmp(curName, e->name) == 0);
}

/* Library preview — uses pre-parsed metadata, no file I/O for meta */
static void libPreviewUpdate(void) {
    Library *lib = uiGetLibrary();
    if (lib->state != LIB_READY && lib->state != LIB_PRECACHING) {
        if (libPreviewStarted) { uiPreviewClear(); libPreviewStarted = false; }
        libPreviewSelIdx = -1;
        return;
    }
    if (lib->view != LIB_VIEW_TRACKS && lib->view != LIB_VIEW_ALBUMS && !lib->searchActive) {
        if (libPreviewStarted) { uiPreviewClear(); libPreviewStarted = false; }
        libPreviewSelIdx = -1;
        libPreviewTimer = 0;
        return;
    }

    int sel = lib->selectedIndex;
    if (sel != libPreviewSelIdx) {
        libPreviewStarted = false;
        libPreviewSelIdx = sel;
        libPreviewTimer = 0;
        return;
    }
    if (libPreviewStarted) return;

    libPreviewTimer++;
    if (libPreviewTimer >= 10) {
        if (lib->view == LIB_VIEW_ALBUMS && !lib->searchActive) {
            if (sel >= 0 && sel < lib->viewCount) {
                const char *albumName = lib->viewList[sel];
                const char *dir = libraryGetAlbumDir(lib, albumName);
                if (dir) {
                    uiPreviewLoadDir(dir);
                    libPreviewStarted = true;
                }
            }
        } else {
            const LibEntry *entry = libraryGetSelectedEntry(lib);
            if (entry) {
                uiPreviewLoadFromLib(entry);
                libPreviewStarted = true;
            }
        }
    }
}

/* Preview update — loads metadata when hovering on a file.
   Keeps old preview visible while waiting for new one to avoid
   flashing back to playback view between selections. */
static void previewUpdate(void) {
    BottomTab tab = uiGetTab();

    if (libraryInitialized && library.state == LIB_SCANNING) return;

    if (tab == TAB_LIBRARY) {
        /* Library has its own preview mechanism */
        if (previewStarted) {
            uiPreviewClear();
            previewStarted = false;
        }
        previewSelIdx = -1;
        previewTimer = 0;
        libPreviewUpdate();
        return;
    }

    if (tab != TAB_BROWSER) {
        if (previewStarted) {
            uiPreviewClear();
            previewStarted = false;
        }
        if (libPreviewStarted) {
            uiPreviewClear();
            libPreviewStarted = false;
        }
        previewSelIdx = -1;
        previewTimer = 0;
        libPreviewSelIdx = -1;
        return;
    }

    ensureBrowserInit("/");
    int sel = browser.selectedIndex;

    if (sel != previewSelIdx) {
        /* Selection changed — reset timer but keep old preview visible */
        previewStarted = false;
        previewSelIdx = sel;
        previewTimer = 0;

        /* If landing on currently playing file, clear preview to show playback */
        if (sel >= 0 && sel < browser.count &&
            !browser.entries[sel].isDir && isEntryPlaying(&browser.entries[sel])) {
            uiPreviewClear();
        }
        return;
    }

    if (previewStarted) return;

    /* Skip preview for currently playing file */
    if (sel >= 0 && sel < browser.count &&
        !browser.entries[sel].isDir && isEntryPlaying(&browser.entries[sel]))
        return;

    if (sel >= 0 && sel < browser.count) {
        previewTimer++;
        if (previewTimer >= PREVIEW_DELAY) {
            const char *path = browserGetSelectedPath(&browser);
            if (path) {
                if (browser.entries[sel].isDir) {
                    uiPreviewLoadDir(path);
                } else if (browserSelectedIsAudio(&browser)) {
                    uiPreviewLoad(path);
                }
                previewStarted = true;
            }
        }
    }
}

static void drawTabBar(u16 *fb) {
    BottomTab tab = uiGetTab();

    u16 activeBg = COL_BG_HEADER;
    u16 inactiveBg = COL_BG;

    drawRect(fb, 0, 0, 85, TAB_BAR_H, (tab == TAB_BROWSER) ? activeBg : inactiveBg);
    drawRect(fb, 85, 0, 86, TAB_BAR_H, (tab == TAB_SETTINGS) ? activeBg : inactiveBg);
    drawRect(fb, 171, 0, 85, TAB_BAR_H, (tab == TAB_LIBRARY) ? activeBg : inactiveBg);

    u16 colBr = (tab == TAB_BROWSER) ? COL_TEXT_BRIGHT : COL_TEXT_DIM;
    u16 colSt = (tab == TAB_SETTINGS) ? COL_TEXT_BRIGHT : COL_TEXT_DIM;
    u16 colLb = (tab == TAB_LIBRARY) ? COL_TEXT_BRIGHT : COL_TEXT_DIM;

    int brW = drawStringWidth("Files");
    int stW = drawStringWidth("Settings");
    int lbW = drawStringWidth("Library");
    drawString(fb, (85 - brW) / 2, 4, "Files", colBr);
    drawString(fb, 85 + (86 - stW) / 2, 4, "Settings", colSt);
    drawString(fb, 171 + (85 - lbW) / 2, 4, "Library", colLb);

    drawVLine(fb, 85, 0, TAB_BAR_H - 1, COL_BORDER);
    drawVLine(fb, 171, 0, TAB_BAR_H - 1, COL_BORDER);
    drawHLine(fb, 0, TAB_BAR_H - 1, SCREEN_W, COL_BORDER);
}

static void drawBrowserContent(u16 *fb) {
    ensureBrowserInit("/");

    /* Path breadcrumb */
    drawRect(fb, 0, CONTENT_TOP, SCREEN_W, LIST_ITEM_H, COL_BG_HEADER);
    drawStringClipped(fb, 4, CONTENT_TOP + 2, SCREEN_W - 8, browser.path, COL_TEXT_DIM);

    int listTop = CONTENT_TOP + LIST_ITEM_H;
    int visible = (CONTENT_H - LIST_ITEM_H) / LIST_ITEM_H;

    if (browser.selectedIndex < browser.scrollOffset)
        browser.scrollOffset = browser.selectedIndex;
    if (browser.selectedIndex >= browser.scrollOffset + visible)
        browser.scrollOffset = browser.selectedIndex - visible + 1;
    if (browser.scrollOffset < 0)
        browser.scrollOffset = 0;

    if (browser.count == 0) {
        drawString(fb, 8, listTop + 10, "(empty)", COL_TEXT_DIM);
        return;
    }

    marqueeUpdate(browser.selectedIndex, TAB_BROWSER);

    /* Auto-advance selection when track changes */
    const char *curFile = playerGetCurrentFile();
    if (curFile && curFile[0] && strcmp(curFile, lastPlayingFile) != 0) {
        strncpy(lastPlayingFile, curFile, sizeof(lastPlayingFile) - 1);
        lastPlayingFile[sizeof(lastPlayingFile) - 1] = '\0';
        const char *curName = strrchr(curFile, '/');
        if (curName) curName++; else curName = curFile;
        for (int i = 0; i < browser.count; i++) {
            if (!browser.entries[i].isDir && strcmp(browser.entries[i].name, curName) == 0) {
                browserSelectIndex(&browser, i);
                if (previewStarted) {
                    uiPreviewClear();
                    previewStarted = false;
                }
                previewSelIdx = i;
                previewTimer = 0;
                break;
            }
        }
    }

    for (int i = 0; i < visible && (browser.scrollOffset + i) < browser.count; i++) {
        int idx = browser.scrollOffset + i;
        BrowserEntry *e = &browser.entries[idx];
        int y = listTop + i * LIST_ITEM_H;

        bool isSelected = (idx == browser.selectedIndex);
        bool isPlaying = isEntryPlaying(e);

        if (isSelected) {
            drawRect(fb, 0, y, SCREEN_W, LIST_ITEM_H, COL_BG_SELECTED);
        } else if (isPlaying) {
            drawRect(fb, 0, y, SCREEN_W, LIST_ITEM_H, COL_BG_PLAYING);
        }

        /* Now-playing indicator */
        int nameX = 8;
        u16 nameCol = COL_TEXT;

        if (e->isDir) {
            drawString(fb, 2, y + 2, "[", COL_DIR);
            drawString(fb, 2 + FONT_W, y + 2, "]", COL_DIR);
            nameCol = COL_DIR;
            nameX = FONT_W * 2 + 4;
        } else if (isPlaying) {
            drawNowPlayingBars(fb, 2, y + 2, themeIconActive());
            nameCol = themeIconActive();
        }

        int nameMaxW = SCREEN_W - nameX - 4;

        if (isSelected && drawStringWidth(e->name) > nameMaxW) {
            drawStringScrolled(fb, nameX, y + 2, nameMaxW, e->name, nameCol, marqueeOffset);
        } else {
            drawStringClipped(fb, nameX, y + 2, nameMaxW, e->name, nameCol);
        }
    }

    /* Scroll indicator */
    if (browser.count > visible) {
        int totalH = CONTENT_H - LIST_ITEM_H;
        int thumbH = (visible * totalH) / browser.count;
        if (thumbH < 8) thumbH = 8;
        int thumbY = listTop + (browser.scrollOffset * (totalH - thumbH)) / (browser.count - visible);
        drawRect(fb, SCREEN_W - 3, thumbY, 2, thumbH, themeAccent());
    }
}

static void drawSettingsRow(u16 *fb, int y, int itemH, int row,
                            const char *label, const char *value, u16 valCol) {
    if (row == settingsSelectedItem)
        drawRect(fb, 0, y, SCREEN_W, itemH, COL_BG_SELECTED);
    drawString(fb, 8, y + 2, label, COL_TEXT);
    int vw = drawStringWidth(value);
    drawString(fb, SCREEN_W - vw - 8, y + 2, value, valCol);
}

static void drawSettingsContent(u16 *fb) {
    int itemH = 16;
    int gap = 2;
    int y = CONTENT_TOP + LIST_ITEM_H + 4;

    drawRect(fb, 0, CONTENT_TOP, SCREEN_W, LIST_ITEM_H, COL_BG_HEADER);
    drawString(fb, 4, CONTENT_TOP + 2, "Settings", COL_TEXT_DIM);

    u16 activeCol = themeIconActive();
    int row = 0;

    /* Sleep Timer */
    if (row == settingsSelectedItem)
        drawRect(fb, 0, y, SCREEN_W, itemH, COL_BG_SELECTED);
    drawString(fb, 8, y + 2, "Sleep Timer", COL_TEXT);
    if (sleepTimerRunning) {
        int sec = sleepTimerMs / 1000;
        int m = sec / 60, s = sec % 60;
        char buf[16];
        snprintf(buf, sizeof(buf), "%dm %02ds", m, s);
        int w = drawStringWidth(buf);
        drawString(fb, SCREEN_W - w - 8, y + 2, buf, activeCol);
    } else {
        drawString(fb, SCREEN_W - drawStringWidth("Off") - 8, y + 2, "Off", COL_TEXT_DIM);
    }
    y += itemH + gap;
    row++;

    /* Repeat mode */
    RepeatMode rep = playerGetRepeat();
    const char *repStr = (rep == REPEAT_ALL) ? "All" : (rep == REPEAT_ONE) ? "One" : "Off";
    u16 repCol = (rep != REPEAT_OFF) ? activeCol : COL_TEXT_DIM;
    drawSettingsRow(fb, y, itemH, row, "Repeat", repStr, repCol);
    y += itemH + gap;
    row++;

    /* Shuffle */
    bool shuf = playerGetShuffle();
    drawSettingsRow(fb, y, itemH, row, "Shuffle", shuf ? "On" : "Off",
                    shuf ? activeCol : COL_TEXT_DIM);
    y += itemH + gap;
    row++;

    /* Theme color */
    drawSettingsRow(fb, y, itemH, row, "Theme", themeNames[currentTheme], activeCol);
    y += itemH + gap;
    row++;

    /* Album tiles toggle */
    drawSettingsRow(fb, y, itemH, row, "Album Tiles", showAlbumTiles ? "On" : "Off",
                    showAlbumTiles ? activeCol : COL_TEXT_DIM);
    y += itemH + gap;
    row++;

    /* Rescan library */
    if (row == settingsSelectedItem)
        drawRect(fb, 0, y, SCREEN_W, itemH, COL_BG_SELECTED);
    drawString(fb, 8, y + 2, "Rescan Library", COL_TEXT);
    Library *lib = uiGetLibrary();
    if (lib->state == LIB_SCANNING) {
        drawString(fb, SCREEN_W - drawStringWidth("Scanning...") - 8, y + 2, "Scanning...", activeCol);
    } else {
        char cBuf[16];
        snprintf(cBuf, sizeof(cBuf), "%d tracks", lib->count);
        drawString(fb, SCREEN_W - drawStringWidth(cBuf) - 8, y + 2, cBuf, COL_TEXT_DIM);
    }
}

static void drawTrackInfo(u16 *fb) {
    drawHLine(fb, 0, TRACK_INFO_TOP - 1, SCREEN_W, COL_BORDER);
    drawRect(fb, 0, TRACK_INFO_TOP, SCREEN_W, TRACK_INFO_H, COL_BG_HEADER);

    StreamState state = playerGetState();
    if (state == STREAM_STOPPED && playerGetCurrentFile()[0] == '\0')
        return;

    const AudioMeta *meta = playerGetMeta();
    const char *curFile = playerGetCurrentFile();

    /* Line 1: title + time */
    const char *title = meta->title;
    char nameBuf[128];
    if (!title[0]) {
        const char *n = strrchr(curFile, '/');
        if (n) n++; else n = curFile;
        strncpy(nameBuf, n, sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        title = nameBuf;
    }

    char timeBuf[20];
    u32 posMs = playerGetPositionMs();
    u32 durMs = playerGetDurationMs();
    u32 ps = posMs / 1000, pm = ps / 60; ps %= 60;
    u32 ds = durMs / 1000, dm = ds / 60; ds %= 60;
    snprintf(timeBuf, sizeof(timeBuf), "%u:%02u/%u:%02u",
             (unsigned)pm, (unsigned)ps, (unsigned)dm, (unsigned)ds);
    int timeW = drawStringWidth(timeBuf);

    int titleMaxW = SCREEN_W - timeW - 12;

    /* Title marquee */
    if (strcmp(title, trackInfoLastTitle) != 0) {
        strncpy(trackInfoLastTitle, title, sizeof(trackInfoLastTitle) - 1);
        trackInfoLastTitle[sizeof(trackInfoLastTitle) - 1] = '\0';
        trackInfoMarqueeTimer = 0;
        trackInfoMarqueeOfs = 0;
    }
    if (drawStringWidth(title) > titleMaxW) {
        trackInfoMarqueeTimer++;
        if (trackInfoMarqueeTimer > 60)
            trackInfoMarqueeOfs += MARQUEE_SPEED;
    }

    drawStringScrolled(fb, 4, TRACK_INFO_TOP + 2, titleMaxW, title, COL_TEXT_BRIGHT, trackInfoMarqueeOfs);
    drawString(fb, SCREEN_W - timeW - 4, TRACK_INFO_TOP + 2, timeBuf, COL_TEXT_DIM);

    /* Line 2: artist - album */
    char infoBuf[256];
    infoBuf[0] = '\0';
    if (meta->artist[0] && meta->album[0]) {
        snprintf(infoBuf, sizeof(infoBuf), "%s - %s", meta->artist, meta->album);
    } else if (meta->artist[0]) {
        strncpy(infoBuf, meta->artist, sizeof(infoBuf) - 1);
    } else if (meta->album[0]) {
        strncpy(infoBuf, meta->album, sizeof(infoBuf) - 1);
    }
    if (infoBuf[0]) {
        drawStringClipped(fb, 4, TRACK_INFO_TOP + 12, SCREEN_W - 8, infoBuf, COL_TEXT_DIM);
    }
}

/* On-screen keyboard layout for search */
#define KB_ROWS     3
#define KB_KEY_W    20
#define KB_KEY_H    14
#define KB_TOP      (CONTENT_TOP + CONTENT_H - KB_ROWS * KB_KEY_H - 2)
#define KB_LEFT     4

static const char *kbRows[KB_ROWS] = {
    "QWERTYUIOP",
    "ASDFGHJKL",
    " ZXCVBNM<"
};

static int kbRowLen[KB_ROWS] = { 10, 9, 9 };

static void drawKeyboard(u16 *fb) {
    drawRect(fb, 0, KB_TOP - 2, SCREEN_W, KB_ROWS * KB_KEY_H + 4, COL_BG_HEADER);
    drawHLine(fb, 0, KB_TOP - 3, SCREEN_W, COL_BORDER);

    for (int r = 0; r < KB_ROWS; r++) {
        int rowOfs = (r == 1) ? 10 : (r == 2) ? 20 : 0;
        for (int k = 0; k < kbRowLen[r]; k++) {
            char c = kbRows[r][k];
            int kx = KB_LEFT + rowOfs + k * KB_KEY_W;
            int ky = KB_TOP + r * KB_KEY_H;

            drawRectOutline(fb, kx, ky, KB_KEY_W - 2, KB_KEY_H - 2, COL_BORDER);

            char buf[2] = { c, '\0' };
            if (c == '<') buf[0] = '<';
            if (c == ' ') buf[0] = '_';
            int tw = drawStringWidth(buf);
            drawString(fb, kx + (KB_KEY_W - 2 - tw) / 2, ky + 3, buf, COL_TEXT);
        }
    }
}

static bool handleKbTouch(Library *lib, int tx, int ty) {
    if (ty < KB_TOP || ty >= KB_TOP + KB_ROWS * KB_KEY_H) return false;

    for (int r = 0; r < KB_ROWS; r++) {
        int rowOfs = (r == 1) ? 10 : (r == 2) ? 20 : 0;
        int ky = KB_TOP + r * KB_KEY_H;
        if (ty < ky || ty >= ky + KB_KEY_H) continue;

        for (int k = 0; k < kbRowLen[r]; k++) {
            int kx = KB_LEFT + rowOfs + k * KB_KEY_W;
            if (tx >= kx && tx < kx + KB_KEY_W - 2) {
                char c = kbRows[r][k];
                if (c == '<') {
                    librarySearchBackspace(lib);
                } else if (c == ' ') {
                    librarySearchAddChar(lib, ' ');
                } else {
                    char lower = c;
                    if (lower >= 'A' && lower <= 'Z') lower += 32;
                    librarySearchAddChar(lib, lower);
                }
                return true;
            }
        }
    }
    return false;
}

static void drawTileArt(u16 *fb, int x, int y, int drawSize, const u16 *artData) {
    for (int row = 0; row < drawSize; row++) {
        int py = y + row;
        if (py < CONTENT_TOP || py >= CONTENT_TOP + CONTENT_H) continue;
        int srcRow = row * ART_DISPLAY_SIZE / drawSize;
        for (int col = 0; col < drawSize; col++) {
            int px = x + col;
            if (px < 0 || px >= SCREEN_W) continue;
            fb[py * 256 + px] = artData[srcRow * ART_DISPLAY_SIZE + col * ART_DISPLAY_SIZE / drawSize];
        }
    }
}

static void drawAlbumTileView(u16 *fb, Library *lib) {
    int listTop = CONTENT_TOP + LIST_ITEM_H;
    int availH = CONTENT_TOP + CONTENT_H - listTop;
    int visRows = (availH + TILE_CELL_H - 1) / TILE_CELL_H;
    int totalRows = (lib->viewCount + TILE_COLS - 1) / TILE_COLS;

    int selRow = lib->selectedIndex / TILE_COLS;
    int scrollRow = lib->scrollOffset / TILE_COLS;

    if (selRow < scrollRow) scrollRow = selRow;
    if (selRow >= scrollRow + visRows) scrollRow = selRow - visRows + 1;
    if (scrollRow < 0) scrollRow = 0;
    lib->scrollOffset = scrollRow * TILE_COLS;

    int viewId = lib->viewCount * 1000 + (int)(lib->filterArtist[0]);
    if (viewId != tileCacheViewId) {
        tileCacheInvalidate();
        tileCacheViewId = viewId;
    }

    u16 accent = themeAccentLight();

    for (int r = 0; r < visRows + 1 && (scrollRow + r) < totalRows; r++) {
        for (int c = 0; c < TILE_COLS; c++) {
            int idx = (scrollRow + r) * TILE_COLS + c;
            if (idx >= lib->viewCount) break;

            int cellX = c * TILE_CELL_W;
            int cellY = listTop + r * TILE_CELL_H;
            int artX = cellX + (TILE_CELL_W - TILE_ART_SIZE) / 2;
            int artY = cellY;

            bool selected = (idx == lib->selectedIndex);
            if (selected) {
                drawRect(fb, cellX, cellY, TILE_CELL_W, TILE_CELL_H, COL_BG_SELECTED);
            }

            const u16 *art = tileCacheGet(idx);
            if (!art && !tileCacheQueried(idx)) {
                art = tileCacheLoad(lib, idx);
            }

            if (art) {
                drawTileArt(fb, artX, artY, TILE_ART_SIZE, art);
            } else {
                drawRect(fb, artX, artY, TILE_ART_SIZE, TILE_ART_SIZE, COL_BG_HEADER);
                int noteW = drawStringWidth("#");
                drawString(fb, artX + (TILE_ART_SIZE - noteW) / 2,
                           artY + TILE_ART_SIZE / 2 - 4, "#", COL_TEXT_DIM);
            }

            if (selected) {
                drawRectOutline(fb, artX - 1, artY - 1,
                                TILE_ART_SIZE + 2, TILE_ART_SIZE + 2, accent);
            }

            const char *label = lib->viewList[idx];
            int textY = artY + TILE_ART_SIZE + 2;
            if (textY < CONTENT_TOP + CONTENT_H - FONT_H) {
                drawStringClipped(fb, cellX + 2, textY,
                                  TILE_CELL_W - 4, label,
                                  selected ? COL_TEXT_BRIGHT : COL_TEXT);
            }
        }
    }

    if (totalRows > visRows) {
        int totalH = availH;
        int thumbH = (visRows * totalH) / totalRows;
        if (thumbH < 8) thumbH = 8;
        int thumbY = listTop + (scrollRow * (totalH - thumbH)) / (totalRows - visRows);
        drawRect(fb, SCREEN_W - 3, thumbY, 2, thumbH, themeAccent());
    }
}

static void drawLibraryContent(u16 *fb) {
    Library *lib = uiGetLibrary();

    if (lib->state == LIB_IDLE) {
        drawStringCentered(fb, CONTENT_TOP + 30, "No library cache found", COL_TEXT);
        drawStringCentered(fb, CONTENT_TOP + 50, "Press A to scan SD card", COL_TEXT_DIM);
        return;
    }

    if (lib->state == LIB_SCANNING) {
        char buf[64];
        drawStringCentered(fb, CONTENT_TOP + 30, "Scanning library...", COL_TEXT);

        snprintf(buf, sizeof(buf), "%d tracks found", lib->count);
        drawStringCentered(fb, CONTENT_TOP + 50, buf, COL_TEXT_DIM);

        if (lib->scanTotal > 0) {
            snprintf(buf, sizeof(buf), "Folder %d / %d",
                     lib->scanProgress + 1, lib->scanTotal);
            drawStringCentered(fb, CONTENT_TOP + 66, buf, COL_TEXT_DIM);
        }

        drawStringCentered(fb, CONTENT_TOP + 86, "Audio keeps playing", COL_TEXT_DIM);
        return;
    }

    if (lib->state != LIB_READY && lib->state != LIB_PRECACHING) return;

    /* Header: view breadcrumb */
    drawRect(fb, 0, CONTENT_TOP, SCREEN_W, LIST_ITEM_H, COL_BG_HEADER);

    const char *header;
    if (lib->searchActive) {
        static char searchHeader[48];
        snprintf(searchHeader, sizeof(searchHeader), "Search: %s_", lib->searchQuery);
        header = searchHeader;
    } else {
        switch (lib->view) {
        case LIB_VIEW_ARTISTS: header = "All Artists"; break;
        case LIB_VIEW_ALBUMS:  header = lib->filterArtist; break;
        case LIB_VIEW_TRACKS:  header = lib->filterAlbum; break;
        default: header = "Library"; break;
        }
    }
    drawStringClipped(fb, 4, CONTENT_TOP + 2, SCREEN_W - 8, header, COL_TEXT_DIM);

    int listTop = CONTENT_TOP + LIST_ITEM_H;
    int availH = CONTENT_H - LIST_ITEM_H;
    if (lib->searchActive) {
        availH = KB_TOP - listTop - 4;
    }
    int visible = availH / LIST_ITEM_H;

    if (lib->selectedIndex < lib->scrollOffset)
        lib->scrollOffset = lib->selectedIndex;
    if (lib->selectedIndex >= lib->scrollOffset + visible)
        lib->scrollOffset = lib->selectedIndex - visible + 1;
    if (lib->scrollOffset < 0)
        lib->scrollOffset = 0;

    if (lib->viewCount == 0) {
        const char *emptyMsg = lib->searchActive ? "No matches" : "(empty)";
        drawString(fb, 8, listTop + 10, emptyMsg, COL_TEXT_DIM);
    } else if (showAlbumTiles && lib->view == LIB_VIEW_ALBUMS && !lib->searchActive) {
        drawAlbumTileView(fb, lib);
    } else {
        const char *curFile = playerGetCurrentFile();
        bool isPlaying = (playerGetState() == STREAM_PLAYING && curFile && curFile[0]);
        const char *playingTitle = NULL;
        if (isPlaying && lib->view == LIB_VIEW_TRACKS) {
            const AudioMeta *pmeta = playerGetMeta();
            if (pmeta && pmeta->title[0])
                playingTitle = pmeta->title;
        }

        for (int i = 0; i < visible && (lib->scrollOffset + i) < lib->viewCount; i++) {
            int idx = lib->scrollOffset + i;
            int y = listTop + i * LIST_ITEM_H;

            bool isSelected = (idx == lib->selectedIndex);
            const char *label = lib->viewList[idx];
            if (!label) continue;

            bool trackPlaying = false;
            if (isPlaying && lib->view == LIB_VIEW_TRACKS && playingTitle) {
                trackPlaying = (strcmp(label, playingTitle) == 0);
            }

            if (isSelected)
                drawRect(fb, 0, y, SCREEN_W, LIST_ITEM_H, COL_BG_SELECTED);
            else if (trackPlaying)
                drawRect(fb, 0, y, SCREEN_W, LIST_ITEM_H, COL_BG_PLAYING);

            int nameX = 4;
            u16 nameCol = COL_TEXT;

            if (!lib->searchActive && lib->view != LIB_VIEW_TRACKS) {
                drawString(fb, 2, y + 2, ">", COL_DIR);
                nameX = FONT_W + 4;
                nameCol = COL_DIR;
            } else if (trackPlaying) {
                drawNowPlayingBars(fb, 2, y + 2, themeIconActive());
                nameX = 12;
                nameCol = themeIconActive();
            }

            drawStringClipped(fb, nameX, y + 2, SCREEN_W - nameX - 4, label, nameCol);
        }

        if (lib->viewCount > visible) {
            int totalH = availH;
            int thumbH = (visible * totalH) / lib->viewCount;
            if (thumbH < 8) thumbH = 8;
            int thumbY = listTop + (lib->scrollOffset * (totalH - thumbH)) /
                         (lib->viewCount - visible);
            drawRect(fb, SCREEN_W - 3, thumbY, 2, thumbH, themeAccent());
        }
    }

    /* Count at bottom of header */
    char countBuf[16];
    snprintf(countBuf, sizeof(countBuf), "%d", lib->viewCount);
    drawString(fb, SCREEN_W - drawStringWidth(countBuf) - 4, CONTENT_TOP + 2, countBuf, COL_TEXT_DIM);

    if (lib->searchActive) {
        drawKeyboard(fb);
    }
}

static void drawSeekBar(u16 *fb) {
    drawHLine(fb, 0, SEEK_BAR_TOP - 1, SCREEN_W, COL_BORDER);

    u32 posMs = playerGetPositionMs();
    u32 durMs = playerGetDurationMs();
    float progress = 0.0f;

    if (seekDragging) {
        progress = seekDragPos;
    } else if (durMs > 0) {
        progress = (float)posMs / (float)durMs;
    }

    drawProgressBar(fb, rSeekBar.x, SEEK_BAR_TOP + 1, rSeekBar.w, SEEK_BAR_H - 2,
                    progress, COL_PROGRESS_BG, themeProgressFg());

    int handleX = rSeekBar.x + (int)(progress * rSeekBar.w);
    if (handleX < rSeekBar.x) handleX = rSeekBar.x;
    if (handleX > rSeekBar.x + rSeekBar.w - 2) handleX = rSeekBar.x + rSeekBar.w - 2;
    drawRect(fb, handleX - 1, SEEK_BAR_TOP, 4, SEEK_BAR_H, COL_TEXT_BRIGHT);
}

static void drawTransportControls(u16 *fb) {
    drawHLine(fb, 0, CONTROL_TOP - 1, SCREEN_W, COL_BORDER);
    drawRect(fb, 0, CONTROL_TOP, SCREEN_W, CONTROL_H, COL_BG_HEADER);

    StreamState state = playerGetState();
    u16 playCol = (state == STREAM_PLAYING) ? themeIconActive() : COL_ICON;

    drawSkipIcon(fb, rPrev.x + rPrev.w / 2, rPrev.y + rPrev.h / 2, 7, COL_ICON, false);

    if (state == STREAM_PLAYING) {
        drawPauseIcon(fb, rPlayPause.x + rPlayPause.w / 2, rPlayPause.y + rPlayPause.h / 2, 7, playCol);
    } else {
        int cx = rPlayPause.x + rPlayPause.w / 2;
        int cy = rPlayPause.y + rPlayPause.h / 2;
        int sz = 7;
        for (int row = 0; row < sz * 2; row++) {
            int py = cy - sz + row;
            if (py < 0 || py >= SCREEN_H) continue;
            int w = (row < sz) ? (row + 1) : (sz * 2 - row);
            for (int col = 0; col < w; col++) {
                int px = cx - sz / 2 + col;
                if (px >= 0 && px < SCREEN_W)
                    fb[py * 256 + px] = playCol | BIT(15);
            }
        }
    }

    drawSkipIcon(fb, rNext.x + rNext.w / 2, rNext.y + rNext.h / 2, 7, COL_ICON, true);
    drawStopIcon(fb, rStop.x + rStop.w / 2, rStop.y + rStop.h / 2, 5, COL_ICON);

    RepeatMode rep = playerGetRepeat();
    u16 repCol = (rep != REPEAT_OFF) ? themeIconActive() : COL_TEXT_DIM;
    const char *repLabel;
    switch (rep) {
        case REPEAT_ALL: repLabel = "ALL"; break;
        case REPEAT_ONE: repLabel = "1"; break;
        default:         repLabel = "RPT"; break;
    }
    int repLabelW = drawStringWidth(repLabel);
    drawString(fb, rRepeat.x + (rRepeat.w - repLabelW) / 2, rRepeat.y + 6, repLabel, repCol);

    bool shuf = playerGetShuffle();
    u16 shufCol = shuf ? themeIconActive() : COL_TEXT_DIM;
    int shufLabelW = drawStringWidth("SHF");
    drawString(fb, rShuffle.x + (rShuffle.w - shufLabelW) / 2, rShuffle.y + 6, "SHF", shufCol);
}

void uiBottomRender(void) {
    u16 *fb = getBottomBuffer();
    drawClear(fb, COL_BG);
    animFrame++;

    previewUpdate();

    drawTabBar(fb);

    BottomTab tab = uiGetTab();
    if (tab == TAB_BROWSER) {
        drawBrowserContent(fb);
    } else if (tab == TAB_SETTINGS) {
        drawSettingsContent(fb);
    } else if (tab == TAB_LIBRARY) {
        drawLibraryContent(fb);
    }

    drawTrackInfo(fb);
    drawSeekBar(fb);
    drawTransportControls(fb);
}

/* --- Input handling --- */

void uiHandleTouch(int tx, int ty) {
    if (touchInRegion(tx, ty, &rTabBrowser))  { uiSetTab(TAB_BROWSER); return; }
    if (touchInRegion(tx, ty, &rTabSettings)) { uiSetTab(TAB_SETTINGS); return; }
    if (touchInRegion(tx, ty, &rTabLibrary)) { uiSetTab(TAB_LIBRARY); return; }

    if (touchInRegion(tx, ty, &rPrev))      { playerPrev(); return; }
    if (touchInRegion(tx, ty, &rPlayPause)) { playerPlayPause(); return; }
    if (touchInRegion(tx, ty, &rNext))      { playerNext(); return; }
    if (touchInRegion(tx, ty, &rStop))      { playerStop(); return; }
    if (touchInRegion(tx, ty, &rRepeat))    { playerCycleRepeat(); return; }
    if (touchInRegion(tx, ty, &rShuffle))   { playerToggleShuffle(); return; }

    if (ty >= CONTENT_TOP && ty < CONTENT_TOP + CONTENT_H) {
        BottomTab tab = uiGetTab();
        if (tab == TAB_BROWSER) {
            ensureBrowserInit("/");
            int listTop = CONTENT_TOP + LIST_ITEM_H;
            if (ty >= listTop) {
                int tapIdx = (ty - listTop) / LIST_ITEM_H + browser.scrollOffset;
                if (tapIdx >= 0 && tapIdx < browser.count) {
                    browserSelectIndex(&browser, tapIdx);
                }
            }
        } else if (tab == TAB_SETTINGS) {
            int itemH = 16 + 2;
            int tapIdx = (ty - CONTENT_TOP - LIST_ITEM_H - 4) / itemH;
            if (tapIdx >= 0 && tapIdx < SETTINGS_ITEM_COUNT) {
                settingsSelectedItem = tapIdx;
            }
        } else if (tab == TAB_LIBRARY) {
            Library *lib = uiGetLibrary();
            if (lib->searchActive && handleKbTouch(lib, tx, ty))
                return;

            int listTop = CONTENT_TOP + LIST_ITEM_H;
            bool tileMode = showAlbumTiles && lib->view == LIB_VIEW_ALBUMS && !lib->searchActive;

            if (tileMode && ty >= listTop) {
                int scrollRow = lib->scrollOffset / TILE_COLS;
                int tapRow = (ty - listTop) / TILE_CELL_H + scrollRow;
                int tapCol = tx / TILE_CELL_W;
                if (tapCol >= TILE_COLS) tapCol = TILE_COLS - 1;
                int tapIdx = tapRow * TILE_COLS + tapCol;
                if (tapIdx >= 0 && tapIdx < lib->viewCount) {
                    lib->selectedIndex = tapIdx;
                }
            } else if (ty >= listTop) {
                int tapIdx = (ty - listTop) / LIST_ITEM_H + lib->scrollOffset;
                if (tapIdx >= 0 && tapIdx < lib->viewCount) {
                    lib->selectedIndex = tapIdx;
                }
            }
        }
    }
}

void uiHandleTouchHeld(int tx, int ty) {
    if (ty >= SEEK_BAR_TOP - 4 && ty <= SEEK_BAR_TOP + SEEK_BAR_H + 4 &&
        tx >= rSeekBar.x && tx <= rSeekBar.x + rSeekBar.w) {
        seekDragging = true;
        seekDragPos = (float)(tx - rSeekBar.x) / (float)rSeekBar.w;
        if (seekDragPos < 0) seekDragPos = 0;
        if (seekDragPos > 1) seekDragPos = 1;
    }
}

void uiHandleTouchRelease(void) {
    if (seekDragging) {
        seekDragging = false;
        u32 durMs = playerGetDurationMs();
        if (durMs > 0) {
            playerSeekTo((u32)(seekDragPos * durMs));
        }
    }
}

void uiHandleButtons(u32 kDown, u32 kHeld) {
    BottomTab tab = uiGetTab();

    if (kDown & KEY_X) {
        uiTopCycleView();
        return;
    }
    if (kDown & KEY_L) playerPrev();
    if (kDown & KEY_R) playerNext();

    if (kDown & KEY_SELECT) {
        BottomTab next = (tab + 1) % TAB_COUNT;
        uiSetTab(next);
        return;
    }

    if (kDown & KEY_START) {
        playerPlayPause();
        return;
    }

    if (tab == TAB_BROWSER) {
        ensureBrowserInit("/");

        u32 kRepeat = keysDownRepeat();

        if (kRepeat & KEY_UP)    browserSelectPrev(&browser);
        if (kRepeat & KEY_DOWN)  browserSelectNext(&browser);
        if (kRepeat & KEY_LEFT)  browserPageUp(&browser, LIST_VISIBLE);
        if (kRepeat & KEY_RIGHT) browserPageDown(&browser, LIST_VISIBLE);

        if (kDown & KEY_A) {
            if (previewStarted) {
                uiPreviewClear();
                previewStarted = false;
                previewSelIdx = -1;
            }

            if (browserSelectedIsDir(&browser)) {
                browserEnter(&browser);
            } else if (browserSelectedIsAudio(&browser)) {
                const char *path = browserGetSelectedPath(&browser);
                if (path) {
                    playerPlaylistClear();
                    browserAddAllToPlaylist(&browser);

                    Playlist *pl = playerGetPlaylist();
                    const char *selName = browser.entries[browser.selectedIndex].name;
                    for (int i = 0; i < pl->count; i++) {
                        const char *n = strrchr(pl->entries[i], '/');
                        if (n) n++; else n = pl->entries[i];
                        if (strcmp(n, selName) == 0) {
                            playerPlayFromPlaylist(i);
                            break;
                        }
                    }
                }
            } else if (browserSelectedIsPlaylist(&browser)) {
                const char *path = browserGetSelectedPath(&browser);
                if (path) {
                    playerPlaylistLoad(path);
                    if (playerGetPlaylist()->count > 0) {
                        playerPlayFromPlaylist(0);
                    }
                }
            }
        }

        if (kDown & KEY_B) {
            if (previewStarted) {
                uiPreviewClear();
                previewStarted = false;
                previewSelIdx = -1;
            }
            browserGoUp(&browser);
        }

    } else if (tab == TAB_SETTINGS) {
        u32 kRepeat = keysDownRepeat();

        if (kRepeat & KEY_UP) {
            if (settingsSelectedItem > 0) settingsSelectedItem--;
        }
        if (kRepeat & KEY_DOWN) {
            if (settingsSelectedItem < SETTINGS_ITEM_COUNT - 1) settingsSelectedItem++;
        }

        if (kDown & KEY_A) {
            switch (settingsSelectedItem) {
            case 0: {
                int cur = sleepTimerRunning ? sleepTimerMs / 60000 : 0;
                bool found = false;
                for (int i = 0; i < SLEEP_OPTION_COUNT; i++) {
                    if (sleepOptions[i] > cur) {
                        uiSetSleepTimer(sleepOptions[i]);
                        found = true;
                        break;
                    }
                }
                if (!found) uiSetSleepTimer(0);
                break;
            }
            case 1: playerCycleRepeat(); break;
            case 2: playerToggleShuffle(); break;
            case 3:
                currentTheme = (currentTheme + 1) % THEME_COUNT;
                break;
            case 4:
                showAlbumTiles = !showAlbumTiles;
                break;
            case 5: {
                Library *lib = uiGetLibrary();
                if (lib->state != LIB_SCANNING)
                    libraryScanStart(lib, "/");
                break;
            }
            }
        }

        if (settingsSelectedItem == 0) {
            if (kDown & KEY_RIGHT) {
                int cur = sleepTimerMs / 60000;
                for (int i = 0; i < SLEEP_OPTION_COUNT; i++) {
                    if (sleepOptions[i] > cur) {
                        uiSetSleepTimer(sleepOptions[i]);
                        break;
                    }
                }
            }
            if (kDown & KEY_LEFT) {
                int cur = sleepTimerMs / 60000;
                for (int i = SLEEP_OPTION_COUNT - 1; i >= 0; i--) {
                    if (sleepOptions[i] < cur) {
                        uiSetSleepTimer(sleepOptions[i]);
                        break;
                    }
                }
            }
        }

        if (settingsSelectedItem == 3) {
            if (kDown & KEY_RIGHT)
                currentTheme = (currentTheme + 1) % THEME_COUNT;
            if (kDown & KEY_LEFT)
                currentTheme = (currentTheme + THEME_COUNT - 1) % THEME_COUNT;
        }

        if (kDown & KEY_B) {
            uiSetTab(TAB_BROWSER);
        }

    } else if (tab == TAB_LIBRARY) {
        Library *lib = uiGetLibrary();

        if (lib->state == LIB_IDLE) {
            if (kDown & KEY_A) {
                libraryScanStart(lib, "/");
            }
            return;
        }
        if (lib->state != LIB_READY && lib->state != LIB_PRECACHING) return;

        u32 kRepeat = keysDownRepeat();

        bool tileMode = showAlbumTiles && lib->view == LIB_VIEW_ALBUMS && !lib->searchActive;

        if (tileMode) {
            if (kRepeat & KEY_UP) {
                int next = lib->selectedIndex - TILE_COLS;
                if (next >= 0) lib->selectedIndex = next;
            }
            if (kRepeat & KEY_DOWN) {
                int next = lib->selectedIndex + TILE_COLS;
                if (next < lib->viewCount) lib->selectedIndex = next;
            }
            if (kRepeat & KEY_LEFT) {
                if (lib->selectedIndex > 0) lib->selectedIndex--;
            }
            if (kRepeat & KEY_RIGHT) {
                if (lib->selectedIndex < lib->viewCount - 1) lib->selectedIndex++;
            }
        } else {
            if (kRepeat & KEY_UP)    librarySelectPrev(lib);
            if (kRepeat & KEY_DOWN)  librarySelectNext(lib);
            if (kRepeat & KEY_LEFT)  libraryPageUp(lib, LIST_VISIBLE);
            if (kRepeat & KEY_RIGHT) libraryPageDown(lib, LIST_VISIBLE);
        }

        if (kDown & KEY_A) {
            bool isTrackView = (lib->view == LIB_VIEW_TRACKS);
            bool searchTrack = lib->searchActive && lib->view == LIB_VIEW_TRACKS;
            if (isTrackView || searchTrack) {
                const LibEntry *entry = libraryGetSelectedEntry(lib);
                if (entry) {
                    uiPreviewClear();
                    libPreviewStarted = false;
                    libPreviewSelIdx = -1;
                    if (isTrackView && !lib->searchActive) {
                        playerPlaylistClear();
                        int playIdx = 0;
                        int added = 0;
                        for (int i = 0; i < lib->count; i++) {
                            if (libraryArtistMatch(lib->entries[i].artist, lib->filterArtist) &&
                                strcasecmp(lib->entries[i].album, lib->filterAlbum) == 0) {
                                playerPlaylistAdd(lib->entries[i].path);
                                if (&lib->entries[i] == entry) playIdx = added;
                                added++;
                            }
                        }
                        if (added > 0) {
                            playerPlayFromPlaylist(playIdx);
                        }
                    } else {
                        playerPlayFile(entry->path);
                    }
                }
            } else {
                if (lib->searchActive) {
                    librarySearchToggle(lib);
                }
                libraryEnter(lib);
            }
        }

        if (kDown & KEY_B) {
            libraryGoBack(lib);
        }

        if (kDown & KEY_Y) {
            librarySearchToggle(lib);
        }
    }
}
