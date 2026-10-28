#include "ui.h"
#include "font_data.h"
#include "font_jp_data.h"
#include <nds.h>
#include <string.h>

static u16 *topFB;
static u16 *bottomFB;
static u16 *bottomVRAM;
static u16 bottomBackBuf[256 * 192];
static int topPage;
static BottomTab currentTab = TAB_BROWSER;
static char errorMsg[128];

void drawInit(void) {
    /* Top screen: engine 0, double-buffered bitmap */
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
    vramSetBankB(VRAM_B_MAIN_BG_0x06020000);

    bgInit(2, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 8, 0);
    /* Start drawing to page 1 (hidden), display page 0 */
    topPage = 1;
    topFB = (u16 *)BG_BMP_RAM(8);

    /* Bottom screen: engine 1 (sub), double-buffered bitmap */
    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG_0x06200000);
    vramSetBankD(VRAM_D_SUB_SPRITE);

    bgInitSub(2, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    bottomVRAM = (u16 *)BG_BMP_RAM_SUB(0);
    bottomFB = bottomBackBuf;

    drawClear(topFB, COL_BG);
    drawClear(bottomFB, COL_BG);
}

void drawSwapBuffers(void) {
    /* Swap top screen pages */
    if (topPage == 1) {
        bgSetMapBase(2, 8);
        bgSetMapBase(3, 0);
        topFB = (u16 *)BG_BMP_RAM(0);
        topPage = 0;
    } else {
        bgSetMapBase(2, 0);
        bgSetMapBase(3, 8);
        topFB = (u16 *)BG_BMP_RAM(8);
        topPage = 1;
    }
    bgUpdate();

    /* Copy bottom back buffer to VRAM */
    dmaCopy(bottomBackBuf, bottomVRAM, 256 * 192 * sizeof(u16));
}

u16 *getTopBuffer(void) { return topFB; }
u16 *getBottomBuffer(void) { return bottomFB; }

void drawClear(u16 *fb, u16 color) {
    color |= BIT(15);
    for (int i = 0; i < 256 * 192; i++)
        fb[i] = color;
}

void drawRect(u16 *fb, int x, int y, int w, int h, u16 color) {
    color |= BIT(15);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0) return;

    for (int row = y; row < y + h; row++) {
        u16 *line = &fb[row * 256 + x];
        for (int col = 0; col < w; col++)
            line[col] = color;
    }
}

void drawRectOutline(u16 *fb, int x, int y, int w, int h, u16 color) {
    drawHLine(fb, x, y, w, color);
    drawHLine(fb, x, y + h - 1, w, color);
    drawVLine(fb, x, y, h, color);
    drawVLine(fb, x + w - 1, y, h, color);
}

void drawHLine(u16 *fb, int x, int y, int w, u16 color) {
    color |= BIT(15);
    if (y < 0 || y >= SCREEN_H) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (w <= 0) return;
    u16 *line = &fb[y * 256 + x];
    for (int i = 0; i < w; i++) line[i] = color;
}

void drawVLine(u16 *fb, int x, int y, int h, u16 color) {
    color |= BIT(15);
    if (x < 0 || x >= SCREEN_W) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (h <= 0) return;
    for (int i = 0; i < h; i++)
        fb[(y + i) * 256 + x] = color;
}

/* UTF-8 decoder: returns codepoint and advances *p. Returns 0xFFFD on error. */
static u32 decodeUtf8(const char **p) {
    const u8 *s = (const u8 *)*p;
    u32 cp;
    int extra;
    u8 c = *s++;
    if (c < 0x80)      { cp = c; extra = 0; }
    else if (c < 0xC0) { *p = (const char *)s; return 0xFFFD; }
    else if (c < 0xE0) { cp = c & 0x1F; extra = 1; }
    else if (c < 0xF0) { cp = c & 0x0F; extra = 2; }
    else if (c < 0xF8) { cp = c & 0x07; extra = 3; }
    else               { *p = (const char *)s; return 0xFFFD; }
    for (int i = 0; i < extra; i++) {
        if ((*s & 0xC0) != 0x80) { *p = (const char *)s; return 0xFFFD; }
        cp = (cp << 6) | (*s++ & 0x3F);
    }
    *p = (const char *)s;
    return cp;
}

/* Binary search for a codepoint in the JP font table */
static const u8 *jpFontLookup(u32 cp) {
    if (cp > 0xFFFF) return NULL;
    u16 key = (u16)cp;
    int lo = 0, hi = JP_GLYPH_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (jpFontCodepoints[mid] == key)
            return &jpFontData[mid * JP_FONT_H];
        if (jpFontCodepoints[mid] < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* Draw a single Unicode codepoint. Returns the width drawn (6 for ASCII, 8 for CJK). */
static int drawCodepoint(u16 *fb, int x, int y, u32 cp, u16 color) {
    color |= BIT(15);
    if (cp >= 0x20 && cp <= 0x7E) {
        /* ASCII — use the compact 6x8 font */
        const u8 *glyph = font6x8[cp - 32];
        for (int row = 0; row < FONT_H; row++) {
            int py = y + row;
            if (py < 0 || py >= SCREEN_H) continue;
            u8 bits = glyph[row];
            for (int col = 0; col < FONT_W; col++) {
                int px = x + col;
                if (px < 0 || px >= SCREEN_W) continue;
                if (bits & (0x80 >> col))
                    fb[py * 256 + px] = color;
            }
        }
        return FONT_W;
    }
    /* Non-ASCII — use JP font */
    const u8 *glyph = jpFontLookup(cp);
    if (!glyph) {
        /* Unknown char — draw a box */
        for (int row = 0; row < 8; row++) {
            int py = y + row;
            if (py < 0 || py >= SCREEN_H) continue;
            for (int col = 0; col < 8; col++) {
                int px = x + col;
                if (px < 0 || px >= SCREEN_W) continue;
                if (row == 0 || row == 7 || col == 0 || col == 7)
                    fb[py * 256 + px] = color;
            }
        }
        return JP_FONT_W;
    }
    for (int row = 0; row < JP_FONT_H; row++) {
        int py = y + row;
        if (py < 0 || py >= SCREEN_H) continue;
        u8 bits = glyph[row];
        for (int col = 0; col < JP_FONT_W; col++) {
            int px = x + col;
            if (px < 0 || px >= SCREEN_W) continue;
            if (bits & (0x80 >> col))
                fb[py * 256 + px] = color;
        }
    }
    return JP_FONT_W;
}

/* Calculate pixel width of a UTF-8 string */
static int stringWidthU(const char *str) {
    int w = 0;
    while (*str) {
        const char *prev = str;
        u32 cp = decodeUtf8(&str);
        if (cp >= 0x20 && cp <= 0x7E) w += FONT_W;
        else w += JP_FONT_W;
        if (str == prev) break;
    }
    return w;
}

void drawStringU(u16 *fb, int x, int y, const char *str, u16 color) {
    while (*str) {
        if (x + FONT_W > SCREEN_W) break;
        const char *prev = str;
        u32 cp = decodeUtf8(&str);
        if (str == prev) break;
        x += drawCodepoint(fb, x, y, cp, color);
    }
}

void drawStringUClipped(u16 *fb, int x, int y, int maxW, const char *str, u16 color) {
    int totalW = stringWidthU(str);
    if (totalW <= maxW) {
        drawStringU(fb, x, y, str, color);
        return;
    }
    int dotsW = FONT_W * 3;
    int availW = maxW - dotsW;
    if (availW < FONT_W) availW = FONT_W;
    const char *p = str;
    int drawn = 0;
    while (*p) {
        const char *prev = p;
        u32 cp = decodeUtf8(&p);
        if (p == prev) break;
        int cw = (cp >= 0x20 && cp <= 0x7E) ? FONT_W : JP_FONT_W;
        if (drawn + cw > availW) break;
        x += drawCodepoint(fb, x, y, cp, color);
        drawn += cw;
    }
    drawString(fb, x, y, "...", color);
}

void drawStringUCentered(u16 *fb, int y, const char *str, u16 color) {
    int w = stringWidthU(str);
    int x = (SCREEN_W - w) / 2;
    if (x < 0) x = 0;
    drawStringU(fb, x, y, str, color);
}

int drawStringUWidth(const char *str) {
    return stringWidthU(str);
}

void drawChar(u16 *fb, int x, int y, char c, u16 color) {
    color |= BIT(15);
    if (c < 32 || c > 126) c = '?';
    const u8 *glyph = font6x8[c - 32];

    for (int row = 0; row < FONT_H; row++) {
        int py = y + row;
        if (py < 0 || py >= SCREEN_H) continue;
        u8 bits = glyph[row];
        for (int col = 0; col < FONT_W; col++) {
            int px = x + col;
            if (px < 0 || px >= SCREEN_W) continue;
            if (bits & (0x80 >> col)) {
                fb[py * 256 + px] = color;
            }
        }
    }
}

void drawString(u16 *fb, int x, int y, const char *str, u16 color) {
    while (*str) {
        if (x + FONT_W > SCREEN_W) break;
        drawChar(fb, x, y, *str, color);
        x += FONT_W;
        str++;
    }
}

void drawStringClipped(u16 *fb, int x, int y, int maxW, const char *str, u16 color) {
    int maxChars = maxW / FONT_W;
    int len = strlen(str);
    if (len <= maxChars) {
        drawString(fb, x, y, str, color);
        return;
    }
    char buf[CHARS_PER_LINE + 1];
    if (maxChars < 4) maxChars = 4;
    int copyLen = maxChars - 3;
    if (copyLen > (int)sizeof(buf) - 4) copyLen = sizeof(buf) - 4;
    memcpy(buf, str, copyLen);
    buf[copyLen] = '.';
    buf[copyLen + 1] = '.';
    buf[copyLen + 2] = '.';
    buf[copyLen + 3] = '\0';
    drawString(fb, x, y, buf, color);
}

void drawStringCentered(u16 *fb, int y, const char *str, u16 color) {
    int w = drawStringWidth(str);
    int x = (SCREEN_W - w) / 2;
    if (x < 0) x = 0;
    drawString(fb, x, y, str, color);
}

int drawStringWidth(const char *str) {
    return strlen(str) * FONT_W;
}

void drawProgressBar(u16 *fb, int x, int y, int w, int h, float progress, u16 bgCol, u16 fgCol) {
    drawRect(fb, x, y, w, h, bgCol);
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    int fillW = (int)(w * progress);
    if (fillW > 0)
        drawRect(fb, x, y, fillW, h, fgCol);
}

void drawTriangle(u16 *fb, int cx, int cy, int size, u16 color, bool right) {
    color |= BIT(15);
    for (int i = 0; i < size * 2; i++) {
        int py = cy - size + i;
        if (py < 0 || py >= SCREEN_H) continue;
        int rowW;
        if (right) {
            rowW = (i < size) ? i : (size * 2 - i);
        } else {
            rowW = (i < size) ? (size - i) : (i - size);
            rowW = size - ((i < size) ? i : (size * 2 - i - 1));
        }
        int px = cx - rowW / 2;
        for (int j = 0; j < rowW; j++) {
            int x = px + j;
            if (x >= 0 && x < SCREEN_W)
                fb[py * 256 + x] = color;
        }
    }
}

void drawPauseIcon(u16 *fb, int cx, int cy, int size, u16 color) {
    int barW = size / 3;
    if (barW < 2) barW = 2;
    int gap = barW;
    drawRect(fb, cx - gap - barW, cy - size, barW, size * 2, color);
    drawRect(fb, cx + gap, cy - size, barW, size * 2, color);
}

void drawStopIcon(u16 *fb, int cx, int cy, int size, u16 color) {
    drawRect(fb, cx - size, cy - size, size * 2, size * 2, color);
}

void drawSkipIcon(u16 *fb, int cx, int cy, int size, u16 color, bool forward) {
    color |= BIT(15);
    int triSize = size - 2;
    if (triSize < 3) triSize = 3;

    if (forward) {
        for (int i = 0; i < triSize * 2; i++) {
            int py = cy - triSize + i;
            if (py < 0 || py >= SCREEN_H) continue;
            int w = (i < triSize) ? i : (triSize * 2 - i - 1);
            for (int j = 0; j <= w; j++) {
                int px = cx - triSize + j;
                if (px >= 0 && px < SCREEN_W)
                    fb[py * 256 + px] = color;
            }
        }
        drawRect(fb, cx + 1, cy - triSize, 2, triSize * 2, color);
    } else {
        drawRect(fb, cx - triSize - 2, cy - triSize, 2, triSize * 2, color);
        for (int i = 0; i < triSize * 2; i++) {
            int py = cy - triSize + i;
            if (py < 0 || py >= SCREEN_H) continue;
            int w = (i < triSize) ? i : (triSize * 2 - i - 1);
            for (int j = 0; j <= w; j++) {
                int px = cx + triSize - j;
                if (px >= 0 && px < SCREEN_W)
                    fb[py * 256 + px] = color;
            }
        }
    }
}

bool touchInRegion(int tx, int ty, const TouchRegion *r) {
    return tx >= r->x && tx < r->x + r->w &&
           ty >= r->y && ty < r->y + r->h;
}

void uiSetTab(BottomTab tab) { currentTab = tab; }
BottomTab uiGetTab(void) { return currentTab; }

void uiSetError(const char *msg) {
    strncpy(errorMsg, msg, sizeof(errorMsg) - 1);
    errorMsg[sizeof(errorMsg) - 1] = '\0';
}

void uiClearError(void) { errorMsg[0] = '\0'; }
const char *uiGetError(void) { return errorMsg; }
