/*
 * DSi Media Player — ARM9 main entry point
 *
 * Controls:
 *   D-Pad Up/Down    Navigate browser / library / settings
 *   D-Pad Left/Right Page up / down; move within album tiles; adjust setting
 *   A                Enter directory / play file / activate setting
 *   B                Go up one level
 *   X                Toggle top screen view (now playing / lyrics)
 *   Y                Toggle search (library)
 *   START            Play / Pause
 *   SELECT           Cycle tabs: Files / Settings / Library
 *   L                Previous track
 *   R                Next track
 *   Touch            Tap items, transport controls, seek bar on bottom screen
 *   Volume buttons   Hardware volume (DSi) — overlay shown on change
 */

#include <nds.h>
#include <calico/nds/pxi.h>
#include <fat.h>
#include <stdio.h>
#include <string.h>

#include "player.h"
#include "settings.h"
#include "ui.h"
#include "cover_art.h"
#include "library.h"

static volatile u8 hwVolume;
static volatile bool hwVolumeChanged;

static void onPxiMsg(void *user, u32 data) {
    (void)user;
    u32 vol = data >> 6;
    if (vol > 31) vol = 31;
    hwVolume = (u8)vol;
    hwVolumeChanged = true;
}

static Settings appSettings;
static u32 settingsSaveTimer = 0;
#define SETTINGS_SAVE_INTERVAL 300

static void fatalError(const char *msg) {
    consoleDemoInit();
    iprintf("\x1b[1;1H");
    iprintf("=== DSi Media Player ===\n\n");
    iprintf("FATAL ERROR:\n%s\n\n", msg);
    iprintf("Press START to exit.");

    while (1) {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_START) break;
    }
}

int main(void) {
    extern char *fake_heap_end;
    fake_heap_end = (char *)0x02F00000;

    defaultExceptionHandler();
    powerOn(POWER_ALL_2D);

    if (isDSiMode()) {
        setCpuClock(true);
    }

    if (!fatInitDefault()) {
        fatalError("Failed to init FAT.\n\nCheck SD card is\ninserted and readable.");
        return 1;
    }

    drawInit();
    coverArtInit();
    playerInit();

    settingsLoad(&appSettings);
    settingsApply(&appSettings);

    /* Listen for hardware volume + hinge state from ARM7 */
    if (isDSiMode()) {
        pxiSetHandler(PxiChannel_User0, onPxiMsg, NULL);
    }

    FileBrowser *browser = uiGetBrowser();
    if (appSettings.lastDir[0]) {
        browserNavigate(browser, appSettings.lastDir);
    }

    settingsResume(&appSettings);

    /* Initialize library — load cache now, scan deferred to Library tab */
    Library *lib = uiGetLibrary();
    libraryLoadCache(lib);

    keysSetRepeat(20, 4);
    u32 touchCooldown = 0;
    u32 frameCount = 0;

    while (1) {
        scanKeys();

        u32 kDown = keysDown();
        u32 kHeld = keysHeld();

        if (kDown & KEY_TOUCH) {
            touchPosition touch;
            touchRead(&touch);
            if (touchCooldown == 0) {
                uiHandleTouch(touch.px, touch.py);
                touchCooldown = 4;
            }
        }
        if (kHeld & KEY_TOUCH) {
            touchPosition touch;
            touchRead(&touch);
            uiHandleTouchHeld(touch.px, touch.py);
        }
        if (!(kHeld & KEY_TOUCH) && !(kDown & KEY_TOUCH)) {
            uiHandleTouchRelease();
        }
        if (touchCooldown > 0) touchCooldown--;

        uiHandleButtons(kDown, kHeld);

        /* Show volume overlay when hardware volume buttons pressed */
        if (hwVolumeChanged) {
            hwVolumeChanged = false;
            uiSetHwVolume(hwVolume);
            uiShowVolumeOverlay();
        }

        /* Decode audio every frame for smooth playback */
        playerUpdate();

        /* Render UI at 30fps — frees CPU for audio decode */
        bool renderFrame = ((frameCount & 1) == 0);
        if (renderFrame) {
            uiTopRender();
            playerUpdate();
            uiBottomRender();
            playerUpdate();
        } else {
            playerUpdate();
            if (lib->state == LIB_SCANNING) {
                libraryScanStep(lib);
            } else if (lib->state == LIB_PRECACHING) {
                libraryPreCacheStep(lib);
            }
            playerUpdate();
            playerUpdate();
        }

        uiSleepTimerTick();

        swiWaitForVBlank();
        if (renderFrame) {
            drawSwapBuffers();
        }
        frameCount++;

        settingsSaveTimer++;
        if (settingsSaveTimer >= SETTINGS_SAVE_INTERVAL) {
            settingsSaveTimer = 0;
            settingsCapture(&appSettings);
            strncpy(appSettings.lastDir, browser->path, sizeof(appSettings.lastDir) - 1);
            settingsSave(&appSettings);
        }
    }

    return 0;
}
