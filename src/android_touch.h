#ifndef SOE_ANDROID_TOUCH_H
#define SOE_ANDROID_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __ANDROID__

typedef struct SDL_Renderer SDL_Renderer;
typedef union SDL_Event SDL_Event;

/* Enable / disable the Android on-screen touch controller overlay (default: true). */
void SoeAndroidTouchSetEnabled(bool enabled);
bool SoeAndroidTouchIsEnabled(void);

/* Idle opacity percentage (10..100, default: 20 = 50% more transparent than ~40%). */
void SoeAndroidTouchSetOpacity(int percent);
int SoeAndroidTouchGetOpacity(void);

/* Notify overlay whether the slide-out settings menu is currently open
 * so taps on the menu drawer / YES-NO confirm dialog pass straight through. */
void SoeAndroidTouchSetMenuOpen(bool open);

/* Feed an SDL3 event (SDL_EVENT_FINGER_DOWN/MOTION/UP/CANCELED).
 * Returns true if the touch event was consumed by the overlay. */
bool SoeAndroidTouchHandleEvent(const SDL_Event *ev);

/* Current 12-bit SNES controller mask (SNES_PAD_* bits from snes_overlay_draw.h). */
uint32_t SoeAndroidTouchGetPadMask(void);

/* Returns true once per tap of LT (Slide-Out Menu) or swipe-when-overlay-off. */
bool SoeAndroidTouchConsumeMenuToggle(void);

/* Returns true once per tap of RT (Debug Overlay). */
bool SoeAndroidTouchConsumeDebugToggle(void);

/* Render the translucent Kenney CC0 touch controls onto the SDL3 renderer. */
void SoeAndroidTouchRender(SDL_Renderer *renderer);

/* Release cached SDL3 textures before renderer destruction. */
void SoeAndroidTouchShutdown(void);

#endif /* __ANDROID__ */
#endif /* SOE_ANDROID_TOUCH_H */
