/*
 * Android On-Screen Touch Controller Overlay for Evertopia
 *
 * Uses Kenney's Input Prompts 1.5A (CC0 1.0 Public Domain, www.kenney.nl)
 * rendered directly via SDL3 hardware textures with alpha modulation for a
 * translucent "ghostly float" appearance.
 *
 * Android-only (#ifdef __ANDROID__). Enabled by default on first boot and
 * toggleable via the in-game Settings -> Game tab ("Touch Overlay: On/Off").
 */
#ifdef __ANDROID__

#include <SDL3/SDL.h>
#include "android_touch.h"
#include "android_touch_assets.h"
#include "snes_overlay_draw.h"
#include "stb/stb_image.h"

#include <math.h>
#include <string.h>

enum {
    TOUCH_TEX_A = 0,
    TOUCH_TEX_B,
    TOUCH_TEX_X,
    TOUCH_TEX_Y,
    TOUCH_TEX_L,
    TOUCH_TEX_R,
    TOUCH_TEX_START,
    TOUCH_TEX_SELECT,
    TOUCH_TEX_MENU,
    TOUCH_TEX_STICK_BASE,
    TOUCH_TEX_STICK_KNOB,
    TOUCH_TEX_COUNT
};

typedef struct {
    float cx, cy;       /* Center in render output pixels */
    float size;         /* Drawn square width/height in pixels */
    float hit_radius;   /* Circular hit radius in pixels */
    uint32_t pad_bit;   /* SNES_PAD_* bit (0 for Menu button) */
    int tex_idx;        /* TOUCH_TEX_* index */
    uint8_t idle_alpha; /* Ghostly float alpha (0..255) */
    uint8_t press_alpha;/* Pressed feedback alpha (0..255) */
    bool is_menu;
} TouchButton;

typedef struct {
    float stick_cx, stick_cy;
    float stick_base_size;
    float stick_knob_size;
    float stick_radius;     /* Outer ring radius */
    float stick_max_travel; /* Max knob center displacement */
    TouchButton buttons[9];
    int button_count;
} TouchLayout;

#define MAX_TOUCH_FINGERS 10

typedef struct {
    bool active;
    SDL_FingerID id;
    float nx, ny; /* Normalized 0..1 coordinates */
    bool is_stick;
} TouchFinger;

static bool s_enabled = true; /* On by default on first boot */
static SDL_Renderer *s_tex_renderer = NULL;
static SDL_Texture *s_textures[TOUCH_TEX_COUNT];
static int s_out_w = 1280;
static int s_out_h = 720;

static TouchFinger s_fingers[MAX_TOUCH_FINGERS];
static uint32_t s_pad_mask = 0;
static uint32_t s_btn_pressed_mask = 0; /* Bit i = layout.buttons[i] pressed */
static bool s_stick_active = false;
static float s_stick_dx = 0.0f;
static float s_stick_dy = 0.0f;
static bool s_menu_toggle_pending = false;

static void ComputeLayout(int w, int h, TouchLayout *L)
{
    if (w <= 0) w = 1280;
    if (h <= 0) h = 720;
    float fw = (float)w;
    float fh = (float)h;
    float u = (fh < fw) ? fh : fw; /* Vertical reference in landscape */

    /* Virtual thumbstick: bottom-left */
    float stick_cx = fw * 0.135f;
    if (stick_cx < u * 0.21f) stick_cx = u * 0.21f;
    float stick_cy = fh * 0.72f;
    L->stick_cx = stick_cx;
    L->stick_cy = stick_cy;
    L->stick_base_size = u * 0.34f;
    L->stick_knob_size = u * 0.155f;
    L->stick_radius = L->stick_base_size * 0.5f;
    L->stick_max_travel = L->stick_radius * 0.62f;

    /* SNES face buttons diamond: bottom-right (X top, Y left, A right, B bottom) */
    float face_cx = fw * 0.865f;
    if (face_cx > fw - u * 0.21f) face_cx = fw - u * 0.21f;
    float face_cy = fh * 0.72f;
    float d = u * 0.125f;
    float btn_sz = u * 0.138f;
    float btn_hit = btn_sz * 0.62f;

    /* Shoulder L/R: top-left & top-right */
    float lr_sz = u * 0.135f;
    float l_cx = fw * 0.075f;
    if (l_cx < u * 0.095f) l_cx = u * 0.095f;
    float r_cx = fw - l_cx;
    float lr_cy = fh * 0.11f;

    /* Select / Start: bottom-center */
    float ss_sz = u * 0.095f;
    float ss_cy = fh * 0.91f;
    float ss_dx = u * 0.095f;

    /* Menu (Home): top-center */
    float menu_sz = u * 0.095f;
    float menu_cy = fh * 0.08f;

    int idx = 0;
    L->buttons[idx++] = (TouchButton){ face_cx + d, face_cy, btn_sz, btn_hit, SNES_PAD_A, TOUCH_TEX_A, 115, 220, false };
    L->buttons[idx++] = (TouchButton){ face_cx, face_cy + d, btn_sz, btn_hit, SNES_PAD_B, TOUCH_TEX_B, 115, 220, false };
    L->buttons[idx++] = (TouchButton){ face_cx, face_cy - d, btn_sz, btn_hit, SNES_PAD_X, TOUCH_TEX_X, 115, 220, false };
    L->buttons[idx++] = (TouchButton){ face_cx - d, face_cy, btn_sz, btn_hit, SNES_PAD_Y, TOUCH_TEX_Y, 115, 220, false };
    L->buttons[idx++] = (TouchButton){ l_cx, lr_cy, lr_sz, lr_sz * 0.68f, SNES_PAD_L, TOUCH_TEX_L, 105, 210, false };
    L->buttons[idx++] = (TouchButton){ r_cx, lr_cy, lr_sz, lr_sz * 0.68f, SNES_PAD_R, TOUCH_TEX_R, 105, 210, false };
    L->buttons[idx++] = (TouchButton){ fw * 0.5f - ss_dx, ss_cy, ss_sz, ss_sz * 0.72f, SNES_PAD_SELECT, TOUCH_TEX_SELECT, 100, 205, false };
    L->buttons[idx++] = (TouchButton){ fw * 0.5f + ss_dx, ss_cy, ss_sz, ss_sz * 0.72f, SNES_PAD_START, TOUCH_TEX_START, 100, 205, false };
    L->buttons[idx++] = (TouchButton){ fw * 0.5f, menu_cy, menu_sz, menu_sz * 0.75f, 0u, TOUCH_TEX_MENU, 95, 210, true };
    L->button_count = idx;
}

static void ClearAllTouches(void)
{
    memset(s_fingers, 0, sizeof(s_fingers));
    s_pad_mask = 0;
    s_btn_pressed_mask = 0;
    s_stick_active = false;
    s_stick_dx = 0.0f;
    s_stick_dy = 0.0f;
}

static void RecomputeState(void)
{
    TouchLayout L;
    ComputeLayout(s_out_w, s_out_h, &L);

    uint32_t pad = 0;
    uint32_t pressed_mask = 0;
    bool stick_found = false;
    float sdx = 0.0f, sdy = 0.0f;

    for (int f = 0; f < MAX_TOUCH_FINGERS; f++) {
        if (!s_fingers[f].active) continue;
        float px = s_fingers[f].nx * (float)s_out_w;
        float py = s_fingers[f].ny * (float)s_out_h;

        if (s_fingers[f].is_stick) {
            stick_found = true;
            float dx = px - L.stick_cx;
            float dy = py - L.stick_cy;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist > L.stick_max_travel && dist > 0.001f) {
                sdx = dx * (L.stick_max_travel / dist);
                sdy = dy * (L.stick_max_travel / dist);
            } else {
                sdx = dx;
                sdy = dy;
            }
            float deadzone = L.stick_radius * 0.20f;
            if (dist >= deadzone) {
                float ux = dx / dist;
                float uy = dy / dist;
                /* 8-way sector threshold: cos(67.5 deg) ~= 0.3827 */
                if (ux >  0.3827f) pad |= SNES_PAD_RIGHT;
                if (ux < -0.3827f) pad |= SNES_PAD_LEFT;
                if (uy >  0.3827f) pad |= SNES_PAD_DOWN;
                if (uy < -0.3827f) pad |= SNES_PAD_UP;
            }
            continue;
        }

        /* Non-stick finger: test against all buttons (allows multi-button thumb roll e.g. B+A) */
        for (int b = 0; b < L.button_count; b++) {
            const TouchButton *btn = &L.buttons[b];
            float dx = px - btn->cx;
            float dy = py - btn->cy;
            if (dx * dx + dy * dy <= btn->hit_radius * btn->hit_radius) {
                pressed_mask |= (1u << b);
                if (!btn->is_menu) {
                    pad |= btn->pad_bit;
                }
            }
        }
    }

    s_pad_mask = pad;
    s_btn_pressed_mask = pressed_mask;
    s_stick_active = stick_found;
    s_stick_dx = stick_found ? sdx : 0.0f;
    s_stick_dy = stick_found ? sdy : 0.0f;
}

void SoeAndroidTouchSetEnabled(bool enabled)
{
    s_enabled = enabled;
    if (!enabled) {
        ClearAllTouches();
    }
}

bool SoeAndroidTouchIsEnabled(void)
{
    return s_enabled;
}

uint32_t SoeAndroidTouchGetPadMask(void)
{
    return s_enabled ? s_pad_mask : 0u;
}

bool SoeAndroidTouchConsumeMenuToggle(void)
{
    if (s_menu_toggle_pending) {
        s_menu_toggle_pending = false;
        return true;
    }
    return false;
}

bool SoeAndroidTouchHandleEvent(const SDL_Event *ev)
{
    if (!s_enabled || !ev) return false;

    if (ev->type == SDL_EVENT_FINGER_DOWN) {
        SDL_FingerID fid = ev->tfinger.fingerID;
        float nx = ev->tfinger.x;
        float ny = ev->tfinger.y;
        float px = nx * (float)s_out_w;
        float py = ny * (float)s_out_h;

        TouchLayout L;
        ComputeLayout(s_out_w, s_out_h, &L);

        /* Check if this touch hits the virtual thumbstick */
        bool stick_in_use = false;
        for (int i = 0; i < MAX_TOUCH_FINGERS; i++) {
            if (s_fingers[i].active && s_fingers[i].is_stick) {
                stick_in_use = true;
                break;
            }
        }
        bool is_stick = false;
        if (!stick_in_use) {
            float dx = px - L.stick_cx;
            float dy = py - L.stick_cy;
            float capture_r = L.stick_radius * 1.35f;
            if (dx * dx + dy * dy <= capture_r * capture_r) {
                is_stick = true;
            }
        }

        /* Check if this touch hits any button (and fire Menu toggle on edge) */
        bool hit_any = is_stick;
        if (!is_stick) {
            for (int b = 0; b < L.button_count; b++) {
                const TouchButton *btn = &L.buttons[b];
                float dx = px - btn->cx;
                float dy = py - btn->cy;
                if (dx * dx + dy * dy <= btn->hit_radius * btn->hit_radius) {
                    hit_any = true;
                    if (btn->is_menu) {
                        s_menu_toggle_pending = true;
                    }
                }
            }
        }

        /* Always record finger in case it slides onto a face button, or only if it hit a control */
        if (hit_any) {
            int slot = -1;
            for (int i = 0; i < MAX_TOUCH_FINGERS; i++) {
                if (s_fingers[i].active && s_fingers[i].id == fid) { slot = i; break; }
                if (!s_fingers[i].active && slot < 0) slot = i;
            }
            if (slot >= 0) {
                s_fingers[slot].active = true;
                s_fingers[slot].id = fid;
                s_fingers[slot].nx = nx;
                s_fingers[slot].ny = ny;
                s_fingers[slot].is_stick = is_stick;
                RecomputeState();
            }
            return true;
        }
        return false;
    }

    if (ev->type == SDL_EVENT_FINGER_MOTION) {
        SDL_FingerID fid = ev->tfinger.fingerID;
        for (int i = 0; i < MAX_TOUCH_FINGERS; i++) {
            if (s_fingers[i].active && s_fingers[i].id == fid) {
                s_fingers[i].nx = ev->tfinger.x;
                s_fingers[i].ny = ev->tfinger.y;
                RecomputeState();
                return true;
            }
        }
        return false;
    }

    if (ev->type == SDL_EVENT_FINGER_UP || ev->type == SDL_EVENT_FINGER_CANCELED) {
        SDL_FingerID fid = ev->tfinger.fingerID;
        for (int i = 0; i < MAX_TOUCH_FINGERS; i++) {
            if (s_fingers[i].active && s_fingers[i].id == fid) {
                s_fingers[i].active = false;
                RecomputeState();
                return true;
            }
        }
        return false;
    }

    return false;
}

static SDL_Texture *CreatePngTexture(SDL_Renderer *renderer, const uint8_t *png_data, size_t png_len)
{
    int w = 0, h = 0, comp = 0;
    unsigned char *rgba = stbi_load_from_memory(png_data, (int)png_len, &w, &h, &comp, 4);
    if (!rgba || w <= 0 || h <= 0) return NULL;

    SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex) {
        SDL_UpdateTexture(tex, NULL, rgba, w * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    }
    stbi_image_free(rgba);
    return tex;
}

static void EnsureTextures(SDL_Renderer *renderer)
{
    if (!renderer) return;
    if (s_tex_renderer == renderer && s_textures[TOUCH_TEX_A] != NULL) return;

    SoeAndroidTouchShutdown();
    s_tex_renderer = renderer;

    static const struct {
        int idx;
        const uint8_t *data;
        size_t len;
    } kAssets[TOUCH_TEX_COUNT] = {
        { TOUCH_TEX_A,          kKenneyBtnA,      sizeof(kKenneyBtnA) },
        { TOUCH_TEX_B,          kKenneyBtnB,      sizeof(kKenneyBtnB) },
        { TOUCH_TEX_X,          kKenneyBtnX,      sizeof(kKenneyBtnX) },
        { TOUCH_TEX_Y,          kKenneyBtnY,      sizeof(kKenneyBtnY) },
        { TOUCH_TEX_L,          kKenneyBtnL,      sizeof(kKenneyBtnL) },
        { TOUCH_TEX_R,          kKenneyBtnR,      sizeof(kKenneyBtnR) },
        { TOUCH_TEX_START,      kKenneyBtnStart,  sizeof(kKenneyBtnStart) },
        { TOUCH_TEX_SELECT,     kKenneyBtnSelect, sizeof(kKenneyBtnSelect) },
        { TOUCH_TEX_MENU,       kKenneyBtnMenu,   sizeof(kKenneyBtnMenu) },
        { TOUCH_TEX_STICK_BASE, kKenneyStickBase, sizeof(kKenneyStickBase) },
        { TOUCH_TEX_STICK_KNOB, kKenneyStickKnob, sizeof(kKenneyStickKnob) },
    };

    for (int i = 0; i < TOUCH_TEX_COUNT; i++) {
        s_textures[kAssets[i].idx] = CreatePngTexture(renderer, kAssets[i].data, kAssets[i].len);
    }
}

void SoeAndroidTouchRender(SDL_Renderer *renderer)
{
    if (!s_enabled || !renderer) return;

    int rw = 0, rh = 0;
    if (SDL_GetRenderOutputSize(renderer, &rw, &rh) && rw > 0 && rh > 0) {
        s_out_w = rw;
        s_out_h = rh;
    }

    EnsureTextures(renderer);

    TouchLayout L;
    ComputeLayout(s_out_w, s_out_h, &L);

    /* 1. Draw virtual thumbstick base + movable knob */
    if (s_textures[TOUCH_TEX_STICK_BASE]) {
        SDL_FRect dst = {
            L.stick_cx - L.stick_base_size * 0.5f,
            L.stick_cy - L.stick_base_size * 0.5f,
            L.stick_base_size,
            L.stick_base_size
        };
        SDL_SetTextureAlphaMod(s_textures[TOUCH_TEX_STICK_BASE], s_stick_active ? 150 : 90);
        SDL_RenderTexture(renderer, s_textures[TOUCH_TEX_STICK_BASE], NULL, &dst);
    }
    if (s_textures[TOUCH_TEX_STICK_KNOB]) {
        float kx = L.stick_cx + s_stick_dx;
        float ky = L.stick_cy + s_stick_dy;
        SDL_FRect dst = {
            kx - L.stick_knob_size * 0.5f,
            ky - L.stick_knob_size * 0.5f,
            L.stick_knob_size,
            L.stick_knob_size
        };
        SDL_SetTextureAlphaMod(s_textures[TOUCH_TEX_STICK_KNOB], s_stick_active ? 215 : 120);
        SDL_RenderTexture(renderer, s_textures[TOUCH_TEX_STICK_KNOB], NULL, &dst);
    }

    /* 2. Draw face, shoulder, start/select, and menu buttons */
    for (int b = 0; b < L.button_count; b++) {
        const TouchButton *btn = &L.buttons[b];
        SDL_Texture *tex = s_textures[btn->tex_idx];
        if (!tex) continue;
        bool pressed = (s_btn_pressed_mask & (1u << b)) != 0;
        float scale = pressed ? 1.08f : 1.0f;
        float sz = btn->size * scale;
        SDL_FRect dst = {
            btn->cx - sz * 0.5f,
            btn->cy - sz * 0.5f,
            sz,
            sz
        };
        SDL_SetTextureAlphaMod(tex, pressed ? btn->press_alpha : btn->idle_alpha);
        SDL_RenderTexture(renderer, tex, NULL, &dst);
    }
}

void SoeAndroidTouchShutdown(void)
{
    for (int i = 0; i < TOUCH_TEX_COUNT; i++) {
        if (s_textures[i]) {
            SDL_DestroyTexture(s_textures[i]);
            s_textures[i] = NULL;
        }
    }
    s_tex_renderer = NULL;
}

#endif /* __ANDROID__ */
