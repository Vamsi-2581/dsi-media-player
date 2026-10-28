#include <nds.h>
#include "lyrics.h"
#include "ui.h"
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lyricsInit(Lyrics *lrc) {
    memset(lrc, 0, sizeof(Lyrics));
}

static u32 parseLrcTime(const char *s) {
    int min = 0, sec = 0, cs = 0;
    const char *p = s;

    while (*p >= '0' && *p <= '9') { min = min * 10 + (*p - '0'); p++; }
    if (*p == ':') p++;
    while (*p >= '0' && *p <= '9') { sec = sec * 10 + (*p - '0'); p++; }
    if (*p == '.' || *p == ':') {
        p++;
        int digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 3) {
            cs = cs * 10 + (*p - '0');
            p++;
            digits++;
        }
        if (digits == 2) cs *= 10;
        if (digits == 1) cs *= 100;
    }
    return (u32)(min * 60000 + sec * 1000 + cs);
}

bool lyricsLoad(Lyrics *lrc, const char *audioPath) {
    lyricsClear(lrc);
    if (!audioPath) return false;

    char lrcPath[512];
    strncpy(lrcPath, audioPath, sizeof(lrcPath) - 5);
    lrcPath[sizeof(lrcPath) - 5] = '\0';

    char *dot = strrchr(lrcPath, '.');
    if (dot) *dot = '\0';
    strcat(lrcPath, ".lrc");

    FILE *f = fopen(lrcPath, "r");
    if (!f) return false;

    char line[256];
    while (fgets(line, sizeof(line), f) && lrc->count < LRC_MAX_LINES) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '[') continue;
        p++;

        if (*p < '0' || *p > '9') continue;

        u32 timeMs = parseLrcTime(p);

        char *close = strchr(p, ']');
        if (!close) continue;
        p = close + 1;

        while (*p == ' ') p++;

        size_t len = strlen(p);
        while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r' || p[len-1] == ' '))
            len--;

        LrcLine *ll = &lrc->lines[lrc->count];
        ll->timeMs = timeMs;
        if (len > 0) {
            size_t copyLen = (len < LRC_LINE_LEN - 1) ? len : LRC_LINE_LEN - 1;
            memcpy(ll->text, p, copyLen);
            ll->text[copyLen] = '\0';
        } else {
            ll->text[0] = '\0';
        }
        lrc->count++;
    }
    fclose(f);

    /* Sort by timestamp (usually already sorted but just in case) */
    for (int i = 1; i < lrc->count; i++) {
        LrcLine tmp = lrc->lines[i];
        int j = i - 1;
        while (j >= 0 && lrc->lines[j].timeMs > tmp.timeMs) {
            lrc->lines[j + 1] = lrc->lines[j];
            j--;
        }
        lrc->lines[j + 1] = tmp;
    }

    lrc->loaded = (lrc->count > 0);
    return lrc->loaded;
}

void lyricsClear(Lyrics *lrc) {
    lrc->count = 0;
    lrc->loaded = false;
}

int lyricsGetCurrentLine(Lyrics *lrc, u32 posMs) {
    if (!lrc->loaded || lrc->count == 0) return -1;
    int cur = -1;
    for (int i = 0; i < lrc->count; i++) {
        if (lrc->lines[i].timeMs <= posMs)
            cur = i;
        else
            break;
    }
    return cur;
}

void lyricsDraw(u16 *fb, Lyrics *lrc, u32 posMs, int y, int height) {
    if (!lrc->loaded || lrc->count == 0) {
        drawStringCentered(fb, y + height / 2 - 4, "No lyrics", COL_TEXT_DIM);
        return;
    }

    int curLine = lyricsGetCurrentLine(lrc, posMs);
    int lineH = FONT_H + 2;
    int visibleLines = height / lineH;
    int centerSlot = visibleLines / 2;

    int startLine = curLine - centerSlot;
    if (startLine < 0) startLine = 0;

    int maxTextW = SCREEN_W - 4;

    for (int i = 0; i < visibleLines; i++) {
        int li = startLine + i;
        if (li >= lrc->count) break;

        int py = y + i * lineH;
        if (py + FONT_H > y + height) break;

        bool isCurrent = (li == curLine);
        u16 col = isCurrent ? COL_TEXT_BRIGHT : COL_TEXT_DIM;

        const char *text = lrc->lines[li].text;
        if (text[0] == '\0') continue;

        int tw = drawStringUWidth(text);

        if (isCurrent) {
            drawRect(fb, 0, py - 1, SCREEN_W, lineH, COL_BG_SELECTED);
        }

        if (tw <= maxTextW) {
            int tx = (SCREEN_W - tw) / 2;
            if (tx < 2) tx = 2;
            drawStringU(fb, tx, py, text, col);
        } else if (isCurrent) {
            int overflow = tw - maxTextW;
            u32 lineStart = lrc->lines[li].timeMs;
            u32 elapsed = (posMs > lineStart) ? posMs - lineStart : 0;
            int scrollPx = 0;
            if (elapsed > 500)
                scrollPx = (int)((u64)(elapsed - 500) * 40 / 1000);
            if (scrollPx > overflow) scrollPx = overflow;
            drawStringU(fb, 2 - scrollPx, py, text, col);
        } else {
            drawStringUClipped(fb, 2, py, maxTextW, text, col);
        }
    }
}

void lyricsDrawFull(u16 *fb, Lyrics *lrc, u32 posMs) {
    int lineH = FONT_H + 4;
    int topMargin = 4;
    int usableH = SCREEN_H - topMargin - 4;
    int visibleLines = usableH / lineH;

    if (!lrc->loaded || lrc->count == 0) {
        drawStringCentered(fb, SCREEN_H / 2 - 8,
                           "No lyrics loaded", COL_TEXT_DIM);
        drawStringCentered(fb, SCREEN_H / 2 + 4,
                           "Place .lrc next to audio file", COL_TEXT_DIM);
        return;
    }

    int curLine = lyricsGetCurrentLine(lrc, posMs);
    int centerSlot = visibleLines / 2;

    int startLine = curLine - centerSlot;
    if (startLine < 0) startLine = 0;

    int maxTextW = SCREEN_W - 4;

    for (int i = 0; i < visibleLines; i++) {
        int li = startLine + i;
        if (li >= lrc->count) break;

        int py = topMargin + i * lineH;

        bool isCurrent = (li == curLine);
        u16 col;
        if (isCurrent) {
            col = COL_TEXT_BRIGHT;
            drawRect(fb, 0, py - 1, SCREEN_W, lineH, COL_BG_SELECTED);
        } else {
            int dist = (curLine >= 0) ? abs(li - curLine) : 1;
            if (dist <= 1) col = COL_TEXT;
            else col = COL_TEXT_DIM;
        }

        const char *text = lrc->lines[li].text;
        if (text[0] == '\0') continue;

        int tw = drawStringUWidth(text);
        if (tw <= maxTextW) {
            int tx = (SCREEN_W - tw) / 2;
            drawStringU(fb, tx, py, text, col);
        } else if (isCurrent) {
            int overflow = tw - maxTextW;
            u32 lineStart = lrc->lines[li].timeMs;
            u32 elapsed = (posMs > lineStart) ? posMs - lineStart : 0;
            int scrollPx = 0;
            if (elapsed > 500)
                scrollPx = (int)((u64)(elapsed - 500) * 40 / 1000);
            if (scrollPx > overflow) scrollPx = overflow;
            drawStringU(fb, 2 - scrollPx, py, text, col);
        } else {
            drawStringUClipped(fb, 2, py, maxTextW, text, col);
        }
    }

    u32 durMs = playerGetDurationMs();
    if (durMs > 0) {
        int barW = (int)((u64)SCREEN_W * posMs / durMs);
        drawRect(fb, 0, SCREEN_H - 2, barW, 2, COL_PROGRESS_FG);
    }
}
