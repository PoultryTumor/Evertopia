/*
 * slide_menu.h — In-game slide-out options menu, debug window, & widescreen
 * controller for Secret of Evermore (Evertopia).
 *
 * Provides:
 *  - Slide-out drawer menu toggled by Esc (or Tab/F2).
 *  - Halved 4x6 font size for compact, sharp typography.
 *  - Floating upper-right corner debug window with full money, all items,
 *    dog form cycling (via right arrow), and map warp select.
 *  - Game Enhancements / Famous Bugs fixes enabled by default.
 *  - Real scrolling map widescreen without mirrored/inverted side reflections.
 *  - 10-second fade-away "Menu [ESC]" button on screen startup.
 */

#ifndef SOE_SLIDE_MENU_H
#define SOE_SLIDE_MENU_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Widescreen modes supported */
typedef enum SoeWidescreenMode {
    SOE_WS_16_9 = 0,   /* 16:9 widescreen (342x224, +43px margins) - Default */
    SOE_WS_21_9 = 1,   /* 21:9 ultrawide (426x224, +85px margins) */
    SOE_WS_OFF = 2,    /* Authentic 4:3 aspect (256x224) */
    SOE_WS_COUNT
} SoeWidescreenMode;

/* HUD position modes */
typedef enum SoeHudPositionMode {
    SOE_HUD_MOVE_LEFT = 0, /* Reposition left-side HUD elements to far-left margin - Default */
    SOE_HUD_ORIGINAL = 1,  /* Original 4:3 centered anchor */
    SOE_HUD_COUNT
} SoeHudPositionMode;

/* Initialize slide menu, defaults, and input event filter */
void SoeSlideMenuInit(void);

/* Check if the slide menu or debug overlay is currently open */
bool SoeSlideMenuIsOpen(void);
void SoeSlideMenuToggle(void);

/* Settings accessors */
SoeWidescreenMode SoeGetWidescreenMode(void);
void SoeSetWidescreenMode(SoeWidescreenMode mode);
bool SoeIsWidescreenActive(void);
int SoeGetWidescreenExtra(void);

SoeHudPositionMode SoeGetHudPositionMode(void);
void SoeSetHudPositionMode(SoeHudPositionMode mode);

struct SnesNativeOverlay;
int SoeGetNativeOverlay(struct SnesNativeOverlay *out);

/* PPU frame hooks wired into SnesDesktopHostGame */
void SoePrepareFrame(int dw, int dh, int *w, int *h);
void SoeBeginSimFrame(unsigned number);
int SoeMenuPaused(void);
void SoeMenuPausedTick(void);
int SoeSaveStateSlot(int slot);
int SoeLoadStateSlot(int slot);
void SoeInitUiSfx(void);
void SoeEnsureLauncherMusicExtracted(void);
void SoeMixUiAudio(int16_t *pcm, int frames, int channels, int sample_rate);
void SoeCapturePostSimState(void);
void SoeProcessPendingJukeboxSong(void);
double SoePresentationHz(double display_refresh);
int SoeDrawFrame(uint8_t *dst, size_t pitch, const uint8_t *field,
                 int frame_w, int frame_h, double alpha);
int SoeWindowBaseWidth(int frame_w);
int SoeWindowBaseHeight(void);

/* Map Exporter API */
bool SoeExportCurrentMap(const char *out_path);
int SoeExportAllMaps(void);
int SoeExportAllMusic(void);

#ifdef __cplusplus
}
#endif

#endif /* SOE_SLIDE_MENU_H */
