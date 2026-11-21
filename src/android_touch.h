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

/* Feed an SDL3 event (SDL_EVENT_FINGER_DOWN/MOTION/UP/CANCELED).
 * Returns true if the touch event was consumed by the overlay. */
bool SoeAndroidTouchHandleEvent(const SDL_Event *ev);

/* Current 12-bit SNES controller mask (SNES_PAD_* bits from snes_overlay_draw.h). */
uint32_t SoeAndroidTouchGetPadMask(void);

/* Returns true once per tap of the top-center Menu/Home touch button. */
bool SoeAndroidTouchConsumeMenuToggle(void);

/* Render the translucent Kenney CC0 touch controls onto the SDL3 renderer. */
void SoeAndroidTouchRender(SDL_Renderer *renderer);

/* Release cached SDL3 textures before renderer destruction. */
void SoeAndroidTouchShutdown(void);

#endif /* __ANDROID__ */
#endif /* SOE_ANDROID_TOUCH_H */
