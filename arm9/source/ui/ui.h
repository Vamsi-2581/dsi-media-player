#ifndef UI_H
#define UI_H

#include <nds/ndstypes.h>

/*
 * DS/DSi screen layout:
 *   Top screen:    256x192, engine 0 (VRAM_A), now-playing display
 *   Bottom screen: 256x192, engine 1 (VRAM_C), browser + controls (touch)
 *
 * Both screens use 16-bit (RGB555) framebuffer mode.
 * DSi XL has larger physical screens (4.2" vs 3.25") but same 256x192 pixel
 * resolution, so pixels are bigger — UI elements are comfortably large.
 */

#define SCREEN_W 256
#define SCREEN_H 192

/* Color palette (RGB555 format) */
#define COL_BG           RGB15(1,  1,  2)
#define COL_BG_HEADER    RGB15(2,  3,  5)
#define COL_BG_SELECTED  RGB15(4,  6, 10)
#define COL_BG_PLAYING   RGB15(3,  5,  8)
#define COL_TEXT          RGB15(28, 28, 28)
#define COL_TEXT_DIM      RGB15(16, 16, 18)
#define COL_TEXT_BRIGHT   RGB15(31, 31, 31)
#define COL_ACCENT        RGB15(6,  16, 28)
#define COL_ACCENT_LIGHT  RGB15(10, 20, 30)
#define COL_PROGRESS_BG   RGB15(4,  4,  6)
#define COL_PROGRESS_FG   RGB15(8,  18, 28)
#define COL_DIR           RGB15(20, 24, 12)
#define COL_ERROR         RGB15(28, 8,  8)
#define COL_BORDER        RGB15(6,  6,  8)
#define COL_ICON          RGB15(22, 22, 24)
#define COL_ICON_ACTIVE   RGB15(10, 22, 31)
#define COL_BATTERY_LOW   RGB15(28, 10, 4)
#define COL_BATTERY_OK    RGB15(8,  24, 8)

/* Font */
#define FONT_W 6
#define FONT_H 8
#define CHARS_PER_LINE (SCREEN_W / FONT_W)   /* 42 chars */
#define LINES_PER_SCREEN (SCREEN_H / FONT_H) /* 24 lines */

/* Touch regions for bottom screen */
typedef struct {
    int x, y, w, h;
} TouchRegion;

/* Drawing primitives */
void drawInit(void);
u16 *getTopBuffer(void);
u16 *getBottomBuffer(void);

void drawSwapBuffers(void);
void drawClear(u16 *fb, u16 color);
void drawRect(u16 *fb, int x, int y, int w, int h, u16 color);
void drawRectOutline(u16 *fb, int x, int y, int w, int h, u16 color);
void drawHLine(u16 *fb, int x, int y, int w, u16 color);
void drawVLine(u16 *fb, int x, int y, int h, u16 color);
void drawChar(u16 *fb, int x, int y, char c, u16 color);
void drawString(u16 *fb, int x, int y, const char *str, u16 color);
void drawStringClipped(u16 *fb, int x, int y, int maxW, const char *str, u16 color);
void drawStringCentered(u16 *fb, int y, const char *str, u16 color);
int  drawStringWidth(const char *str);

/* Unicode-aware string drawing (supports Japanese via JP bitmap font) */
void drawStringU(u16 *fb, int x, int y, const char *str, u16 color);
void drawStringUClipped(u16 *fb, int x, int y, int maxW, const char *str, u16 color);
void drawStringUCentered(u16 *fb, int y, const char *str, u16 color);
int  drawStringUWidth(const char *str);

void drawProgressBar(u16 *fb, int x, int y, int w, int h, float progress, u16 bgCol, u16 fgCol);
void drawTriangle(u16 *fb, int cx, int cy, int size, u16 color, bool right);
void drawPauseIcon(u16 *fb, int cx, int cy, int size, u16 color);
void drawStopIcon(u16 *fb, int cx, int cy, int size, u16 color);
void drawSkipIcon(u16 *fb, int cx, int cy, int size, u16 color, bool forward);

/* Screen render functions */
void uiTopRender(void);
void uiBottomRender(void);

/* Touch hit testing */
bool touchInRegion(int tx, int ty, const TouchRegion *r);

typedef enum {
    TAB_BROWSER = 0,
    TAB_SETTINGS,
    TAB_LIBRARY,
    TAB_COUNT
} BottomTab;

typedef enum {
    THEME_BLUE = 0,
    THEME_PURPLE,
    THEME_GREEN,
    THEME_RED,
    THEME_ORANGE,
    THEME_COUNT
} ThemeColor;

/* Theme accessors */
u16 uiThemeAccent(void);
u16 uiThemeAccentLight(void);
u16 uiThemeProgressFg(void);
u16 uiThemeIconActive(void);
void uiSetTheme(ThemeColor theme);
ThemeColor uiGetTheme(void);
void uiSetAlbumTiles(bool enabled);
bool uiGetAlbumTiles(void);

/* Sleep timer */
void uiSetSleepTimer(int minutes);
int  uiGetSleepTimerRemaining(void);
bool uiSleepTimerActive(void);
void uiSleepTimerTick(void);

void uiSetTab(BottomTab tab);
BottomTab uiGetTab(void);

/* Error display */
void uiSetError(const char *msg);
void uiClearError(void);
const char *uiGetError(void);

/* Input handling (implemented in ui_bottom.c) */
void uiHandleTouch(int tx, int ty);
void uiHandleTouchHeld(int tx, int ty);
void uiHandleTouchRelease(void);
void uiHandleButtons(u32 kDown, u32 kHeld);
bool uiIsSeekDragging(void);
float uiGetSeekDragPos(void);

/* Metadata preview (no audio — just cover art + tags) */
void uiPreviewLoad(const char *path);
void uiPreviewLoadDir(const char *dirPath);
void uiPreviewClear(void);
bool uiPreviewActive(void);

/* Top screen view cycling */
void uiTopCycleView(void);

/* Volume overlay (hardware volume from MCU, 0-31) */
void uiShowVolumeOverlay(void);
void uiSetHwVolume(u8 vol);

/* Browser access — include file_browser.h for the full type */
#include "file_browser.h"
FileBrowser *uiGetBrowser(void);

/* Library access */
#include "library.h"
Library *uiGetLibrary(void);

/* Library preview — uses pre-parsed LibEntry, no file I/O for metadata */
void uiPreviewLoadFromLib(const LibEntry *entry);

#endif
