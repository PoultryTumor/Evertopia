/*
 * slide_menu.c — In-game slide-out options menu, debug window, & widescreen
 * controller for Secret of Evermore (Evertopia).
 *
 * Implements:
 *   1. Widescreen PPU & Sprite rendering policy:
 *      - 16:9 (342x224, extra=43) enabled by default.
 *      - Real scrolling map display on left/right margins from WRAM $7F:0000.
 *      - Widescreen event sprites, NPCs, and enemies ($80:9033 OAM metasprite
 *        builder and $8F:C436/$8F:C494/$8F:DE0A entity culling widened to
 *        [-extra .. 256 + extra] with 9-bit signed OAM X packing).
 *      - Start menu / title screen / intro square screen centered 4:3 pillarbox.
 *   2. HUD element repositioning option:
 *      - Automatically locates Secret of Evermore's status bar (scanlines 192-224)
 *        and shifts left-side HUD elements (Boy HP, gauge) into the far-left margin.
 *   3. Slide-out drawer menu toggled by F1, Esc, or unused Controller buttons
 *      (Share / L3 / R3 / L2 / R2 — never the Xbox Guide light-up button):
 *      - Authentic Secret of Evermore variable-width bitmap font loaded directly
 *        from ROM ($C4:0000 / $C4:0C02) with crisp drop-shadowed white typography.
 *      - Translucent royal blue drawer with glowing cyan accent edge.
 *      - Game Enhancements section with famous bug fixes (all enabled by default).
 *      - 10-second fade-away "Menu [F1/Pad]" screen button.
 *   4. Floating upper-right corner Debug Window:
 *      - Auto-hides the slide-out menu when activated so only the controller-driven
 *        Debug Menu remains on screen.
 *      - Displays the currently loaded Map ID in hex & decimal ($7E:0ADB).
 *      - Max money (Talons, Jewels, Gold Coins, Credits).
 *      - All items, weapons, magic, and 99 ingredients.
 *      - Dog form switcher with right-arrow (>) cycling.
 *      - Level and map warp select with instant warp trigger.
 */

#include "slide_menu.h"
#ifdef __ANDROID__
#include "android_touch.h"
#endif
#include "host_main.h"
#include "desktop/config.h"
#include "desktop/display_aspect.h"
#include "desktop/sdl_compat.h"
#include "snes/snes.h"
#include "snes/apu.h"
#include "snes/cart.h"
#include "snes/ppu.h"
#include "snes/interp_bridge.h"
#include "cpu_state.h"
#include "widescreen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif
#if !defined(_WIN32)
#include <dlfcn.h>
#define soe_dlsym(h, sym) dlsym((h), (sym))
#else
static inline void *soe_dlsym(void *h, const char *sym) { (void)h; (void)sym; return NULL; }
#endif

#ifdef _WIN32
#include <direct.h>
#define soe_mkdir(dir) _mkdir(dir)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define soe_mkdir(dir) mkdir(dir, 0777)
#endif

extern int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes);
extern int stbi_info(char const *filename, int *x, int *y, int *comp);
extern unsigned char *stbi_load(char const *filename, int *x, int *y, int *comp, int req_comp);
extern void stbi_image_free(void *retval_from_stbi_load);
void GameRunOneFrame(void);
extern void WatchdogFrameStart(void);
extern void WatchdogSetEnabled(int enabled);

/* External symbols from host framework */
extern Snes *g_snes;
extern SDL_Window *g_window;
extern uint8_t g_ram[0x20000];
extern Config g_config;
extern bool g_config_dirty;
extern char g_active_config_file[512];
extern void ChangeWindowScale(int scale_step);
extern int snesrecomp_get_window_scale(void);
extern void snesrecomp_set_window_scale(int scale);
extern void snesrecomp_set_fullscreen(int mode);
extern void snesrecomp_set_vsync(int mode);
extern int snesrecomp_get_vsync(void);
extern void snesrecomp_set_triple_buffering(int enabled);
extern int snesrecomp_get_triple_buffering(void);
extern void snesrecomp_apply_audio_settings(bool reopen_stream);
extern void snesrecomp_restore_desktop_mode_for_exit(void);
extern void snesrecomp_desktop_set_widescreen(int enabled);
extern void snesrecomp_desktop_request_clock_reset(void);
extern void snesrecomp_desktop_set_guest_input_suppressed(bool suppressed);
extern void WriteConfigFile(const char *path);
extern size_t RtlSaveSnapshotToMemory(void *data, size_t capacity);
extern bool RtlLoadSnapshotFromMemory(const void *data, size_t size);
extern void RtlEnsureSaveDir(void);
extern const char *RtlSaveRoot(void);
extern int snes_frame_counter;
extern uint64_t g_apu_last_sync_master;
extern uint32_t g_apu_pace_cycles_estimate;
extern uint32_t g_apu_last_sync_cycles;
extern void RtlApuLock(void);
extern void RtlApuUnlock(void);
extern void snes_refresh_exempt(void);
extern void snes_refresh_state_get(uint64_t *phase, uint64_t *charged_upto);
extern void snes_refresh_state_set(uint64_t phase, uint64_t charged_upto);
extern void snes_beam_frame_driver_get_position(uint32_t *resume_pc_out, uint8_t *waiting_out);
extern void snes_beam_frame_driver_set_position(uint32_t resume_pc, uint8_t waiting);
extern void WatchdogFrameStart(void);

/* ── Menu Option Identifiers ──────────────────────────────────────────────── */

/* Top-level Tabs */
typedef enum SoeMenuTab {
    TAB_GAME_OPTIONS = 0,
    TAB_VIDEO_OPTIONS = 1,
    TAB_AUDIO_OPTIONS = 2,
    TAB_FIXES = 3,
    TAB_CRT_OPTIONS = 4,
    TAB_BLOOM_OPTIONS = 5,
    TAB_INPUT_OPTIONS = 6,
    TAB_HELP_README = 7,
    TAB_COUNT = 8
} SoeMenuTab;

/* Tab 0: Game Options */
enum {
    GAME_OPT_SAVE_STATE_1 = 0, /* Save State Slot 1 */
    GAME_OPT_SAVE_STATE_2 = 1, /* Save State Slot 2 */
    GAME_OPT_SAVE_STATE_3 = 2, /* Save State Slot 3 */
    GAME_OPT_LOAD_STATE_1 = 3, /* Load State Slot 1 */
    GAME_OPT_LOAD_STATE_2 = 4, /* Load State Slot 2 */
    GAME_OPT_LOAD_STATE_3 = 5, /* Load State Slot 3 */
    GAME_OPT_DIALOG_THEME = 6, /* Dialog Box Theme: Blue Glass (Default), Original (Dither) */
    GAME_OPT_RING_MENU = 7,    /* Ring Menu: Enhanced (Smooth + Cached) or Original */
#ifdef __ANDROID__
    GAME_OPT_TOUCH_OVERLAY = 8,    /* On-screen Touch Controls Overlay: On (Default) / Off */
    GAME_OPT_RESET_ALL = 9,        /* Reset All Settings to Default */
    GAME_OPT_EXIT_TO_DESKTOP = 10, /* Save all settings and exit to Android launcher */
    GAME_OPT_COUNT = 11
#else
    GAME_OPT_RESET_ALL = 8,        /* Reset All Settings to Default */
    GAME_OPT_EXIT_TO_LAUNCHER = 9, /* Save all settings and return to launcher */
    GAME_OPT_EXIT_TO_DESKTOP = 10, /* Save all settings and exit to desktop */
    GAME_OPT_COUNT = 11
#endif
};

/* Tab 1: Gameplay Fixes */
enum {
    FIX_OPT_SHEATH = 0,   /* Silver Sheath sword boost fix */
    FIX_OPT_ATLAS = 1,    /* Atlas/Defend stat underflow clamp */
    FIX_OPT_DOG_LVL = 2,  /* Dog level 39+ curve fix */
    FIX_OPT_SOFTLOCK = 3, /* Boundary softlock prevention */
    FIX_OPT_COUNT = 4
};

/* Dialog Box Style & Shadow Options */
typedef enum {
    DIALOG_THEME_OMNITOPIA = 0,  /* Programmatic C Blue Glass (Default) */
    DIALOG_THEME_VOLCANO = 1,    /* Semi-muted Volcanic Orange Glass */
    DIALOG_THEME_SWAMP = 2,      /* Muted Swamp Emerald Green Glass */
    DIALOG_THEME_NOBILIA = 3,    /* Muted Nobilia Sandstone Gold Glass */
    DIALOG_THEME_EBON_KEEP = 4,  /* Moody Ebon Keep Amethyst Purple Glass */
    DIALOG_THEME_CRUSTACEA = 5,  /* Deep Crustacea Ocean Teal Glass */
    DIALOG_THEME_COLOSSEUM = 6,  /* Semi-muted Colosseum Crimson Glass */
    DIALOG_THEME_IVOR_TOWER = 7, /* Cool Ivor Tower Slate-Silver Glass */
    DIALOG_THEME_ORIGINAL = 8,   /* Authentic 1995 SNES window pattern */
    DIALOG_THEME_COUNT = 9
} SoeDialogTheme;
#define DIALOG_THEME_BLUE_GLASS DIALOG_THEME_OMNITOPIA
#define DIALOG_GLASS_COUNT 8

/* Tab 1: Video Options */
enum {
    VIDEO_OPT_WIDESCREEN = 0,   /* 16:9, 21:9, Off (4:3) */
    VIDEO_OPT_HUD_POSITION = 1, /* Move Left, Original */
    VIDEO_OPT_PILLARBOX = 2,    /* Enabled, Disabled */
    VIDEO_OPT_SOLID_DIALOG = 3, /* Solid Blue, Original */
    VIDEO_OPT_ASPECT_RATIO = 4, /* 4:3 CRT, 8:7 Square, 1:1 */
    VIDEO_OPT_WINDOW_SCALE = 5, /* 1x, 2x, 3x, 4x, Fullscreen */
    VIDEO_OPT_VSYNC = 6,        /* Off, On, Adaptive */
    VIDEO_OPT_TRIPLE_BUFFER = 7,/* OFF, ON */
    VIDEO_OPT_CRT_SCANLINES = 8,/* Quick Preset switcher */
    VIDEO_OPT_SPRITE_LIMITS = 9,/* ON (No Flicker), OFF (Standard 32) */
    VIDEO_OPT_LOCK_60FPS = 10,  /* High-Hz Subpixel Scroll, Locked 60 FPS */
    VIDEO_OPT_INTERPOLATE_SPRITES = 11, /* ON (Subpixel), OFF (60 Hz Snap) */
    VIDEO_OPT_ELIMINATE_LAG = 12, /* ON (Fast CPU/DMA), OFF (Original SNES) */
    VIDEO_OPT_RESET = 13,       /* [ Reset Video Settings to Default ] */
    VIDEO_OPT_COUNT = 14
};

/* Tab 2: Audio Options */
enum {
    AUDIO_OPT_ENABLE = 0,       /* ON, OFF */
    AUDIO_OPT_VOLUME = 1,       /* 0%..100% (5% steps) */
    AUDIO_OPT_FREQ = 2,         /* 22050, 32040, 44100, 48000 Hz */
    AUDIO_OPT_SAMPLES = 3,      /* 256, 512, 1024, 2048 */
    AUDIO_OPT_RESET = 4,        /* [ Reset Audio Settings to Default ] */
    AUDIO_OPT_COUNT = 5
};

static int s_av_scroll = 0;
static const int kAvVisibleRows = 12;

/* Tab 2: CRT Options */
typedef enum {
    CRT_OPT_PRESET = 0,         /* Presets: Just Scanlines, Fine 1px, Deep, Subtle, etc. */
    CRT_OPT_JUST_SCANLINES = 1, /* [ Apply "Just Scanlines" (Disable Masks) ] */
    CRT_OPT_DENSITY = 2,        /* Density/Spacing: Fine 1px, Standard 2px, Medium 3px, Wide 4px, Dense 2/3 */
    CRT_OPT_OPACITY = 3,        /* Scanline Darkness: 15%, 25%, 35%, 50%, 65%, 80%, 100% */
    CRT_OPT_THICKNESS = 4,      /* Beam Thickness: Thin (1-line), Thick (2-line) */
    CRT_OPT_MASK = 5,           /* Phosphor Mask: Off (Pure Lines), Vertical Stripes, Checkerboard, Grid */
    CRT_OPT_BLOOM = 6,          /* Luminance / Brightness Boost: Off, +10%, +20%, +30% */
    CRT_OPT_VIGNETTE = 7,       /* Curved Tube Vignette: Off, Subtle Glass, Classic CRT */
    CRT_OPT_RESET = 8,          /* [ Reset CRT Settings to Default ] */
    CRT_OPT_COUNT = 9
} SoeCrtOption;

/* CRT Presets */
typedef enum {
    CRT_PRESET_OFF = 0,
    CRT_PRESET_JUST_SCANLINES = 1,      /* Clean 2px horizontal scanlines (No masks or checkerboard) */
    CRT_PRESET_JUST_SCANLINES_FINE = 2, /* Dense 1px horizontal scanlines (No masks or checkerboard) */
    CRT_PRESET_JUST_SCANLINES_DEEP = 3, /* Deep 65% horizontal scanlines (No masks or checkerboard) */
    CRT_PRESET_SUBTLE = 4,              /* Subtle 2px scanlines, 25% opacity */
    CRT_PRESET_CLASSIC = 5,             /* Classic 2px scanlines, 50% opacity */
    CRT_PRESET_SOLID_RETRO = 6,         /* Solid 100% black scanline gaps */
    CRT_PRESET_TRINITRON = 7,           /* Sony Trinitron (With vertical RGB phosphor stripes) */
    CRT_PRESET_SHADOW_MASK = 8,         /* Shadow Mask (With dot triad checkerboard) */
    CRT_PRESET_ARCADE = 9,              /* Arcade Monitor (With slot grid) */
    CRT_PRESET_CUSTOM = 10,
    CRT_PRESET_COUNT = 11
} SoeCrtPreset;

/* Scanline Spacing / Density */
typedef enum {
    CRT_DENSITY_FINE_1PX = 0,      /* Fine 1px: Dense micro-scanlines across every row */
    CRT_DENSITY_STANDARD_2PX = 1,  /* Standard 2px: Alternating rows (classic 240p) */
    CRT_DENSITY_MEDIUM_3PX = 2,    /* Medium 3px: Every 3rd row */
    CRT_DENSITY_WIDE_4PX = 3,      /* Wide 4px: Every 4th row */
    CRT_DENSITY_DENSE_2_OF_3 = 4,  /* Dense 2 of 3 rows */
    CRT_DENSITY_COUNT = 5
} SoeCrtDensity;

/* Scanline Opacity / Strength */
typedef enum {
    CRT_OPACITY_15 = 0,     /* 15% - Factor 218 */
    CRT_OPACITY_25 = 1,     /* 25% - Factor 192 (Default Subtle) */
    CRT_OPACITY_35 = 2,     /* 35% - Factor 166 */
    CRT_OPACITY_50 = 3,     /* 50% - Factor 128 (Classic) */
    CRT_OPACITY_65 = 4,     /* 65% - Factor 90  (Deep) */
    CRT_OPACITY_80 = 5,     /* 80% - Factor 51  (Heavy) */
    CRT_OPACITY_100 = 6,    /* 100% - Factor 0  (Solid Black) */
    CRT_OPACITY_COUNT = 7
} SoeCrtOpacity;

/* Beam Thickness */
typedef enum {
    CRT_THICK_THIN = 0,     /* Thin 1-line beam gap */
    CRT_THICK_THICK = 1,    /* Thick 2-line beam gap */
    CRT_THICK_COUNT = 2
} SoeCrtThickness;

/* Phosphor Shadow Mask */
typedef enum {
    CRT_MASK_OFF = 0,
    CRT_MASK_APERTURE_GRILLE = 1, /* Sony Trinitron vertical RGB stripes */
    CRT_MASK_SHADOW_MASK = 2,    /* Dot Triad checkerboard pattern */
    CRT_MASK_SLOT_MASK = 3,      /* Arcade / PVM slot grid */
    CRT_MASK_COUNT = 4
} SoeCrtMask;

/* Brightness / Bloom Boost */
typedef enum {
    CRT_BLOOM_OFF = 0,     /* +0% */
    CRT_BLOOM_MILD = 1,    /* +10% */
    CRT_BLOOM_MEDIUM = 2,  /* +20% */
    CRT_BLOOM_BRIGHT = 3,  /* +30% */
    CRT_BLOOM_COUNT = 4
} SoeCrtBloom;

/* Curved Glass Tube Vignette */
typedef enum {
    CRT_VIGNETTE_OFF = 0,
    CRT_VIGNETTE_SUBTLE = 1,  /* Soft TV glass edge falloff */
    CRT_VIGNETTE_CLASSIC = 2, /* Classic curved CRT tube corner shadow */
    CRT_VIGNETTE_COUNT = 3
} SoeCrtVignette;

/* Tab 3: Bloom Filter Options */
typedef enum {
    BLOOM_OPT_PRESET = 0,     /* Presets: Subtle, Soft Focus, Warm Sun, Mystic Cyan, Neon, Intense, Custom, Off */
    BLOOM_OPT_INTENSITY = 1,  /* Intensity: Off, 25%, 50%, 75%, 100%, 130%, 170% */
    BLOOM_OPT_THRESHOLD = 2,  /* Luma Threshold: 210 Highlights, 160 Bright, 120 Mid-Bright, 80 Broad */
    BLOOM_OPT_RADIUS = 3,     /* Blur Spread Radius: 1px, 2px, 3px, 4px */
    BLOOM_OPT_BLEND = 4,      /* Blend Mode: Screen, Additive, Soft Light */
    BLOOM_OPT_TINT = 5,       /* Color Atmosphere Tint: Natural White, Golden Sunlight, Mystic Cyan, Rose */
    BLOOM_OPT_RESET = 6,      /* [ Reset Bloom Settings to Default ] */
    BLOOM_OPT_COUNT = 7
} SoeBloomOption;

/* Bloom Presets */
typedef enum {
    BLOOM_PRESET_OFF = 0,
    BLOOM_PRESET_SUBTLE = 1,     /* Subtle natural glow */
    BLOOM_PRESET_SOFT_FOCUS = 2, /* Dreamy soft focus */
    BLOOM_PRESET_WARM_SUN = 3,   /* Golden amber sunlight */
    BLOOM_PRESET_MYSTIC_CYAN = 4,/* Cool magical glow */
    BLOOM_PRESET_NEON = 5,       /* Vibrant additive neon */
    BLOOM_PRESET_INTENSE = 6,    /* Heavy ethereal bloom */
    BLOOM_PRESET_CUSTOM = 7,
    BLOOM_PRESET_DEFAULT = 8,    /* Default: 50% Mild, 80 Broad, 4px Spread, Additive, Natural */
    BLOOM_PRESET_COUNT = 9
} SoeBloomPreset;

/* Bloom Intensity */
typedef enum {
    BLOOM_INTENSITY_OFF = 0,
    BLOOM_INTENSITY_25 = 1,   /* 25% subtle */
    BLOOM_INTENSITY_50 = 2,   /* 50% mild */
    BLOOM_INTENSITY_75 = 3,   /* 75% warm */
    BLOOM_INTENSITY_100 = 4,  /* 100% normal */
    BLOOM_INTENSITY_130 = 5,  /* 130% strong */
    BLOOM_INTENSITY_170 = 6,  /* 170% intense */
    BLOOM_INTENSITY_COUNT = 7
} SoeBloomIntensity;

/* Bloom Threshold */
typedef enum {
    BLOOM_THRESH_HIGHLIGHTS_210 = 0, /* 210: Only super bright highlights glow */
    BLOOM_THRESH_BRIGHT_160 = 1,     /* 160: Bright elements and light sources */
    BLOOM_THRESH_MEDIUM_120 = 2,     /* 120: Mid-to-high tones glow */
    BLOOM_THRESH_WIDE_80 = 3,        /* 80: Soft overall luminous wash */
    BLOOM_THRESH_COUNT = 4
} SoeBloomThreshold;

/* Bloom Blur Radius / Spread */
typedef enum {
    BLOOM_RADIUS_NARROW_1PX = 0,
    BLOOM_RADIUS_MEDIUM_2PX = 1,
    BLOOM_RADIUS_WIDE_3PX = 2,
    BLOOM_RADIUS_BROAD_4PX = 3,
    BLOOM_RADIUS_COUNT = 4
} SoeBloomRadius;

/* Bloom Blend Mode */
typedef enum {
    BLOOM_BLEND_SCREEN = 0,    /* Screen: Soft luminous glow, no harsh clipping */
    BLOOM_BLEND_ADDITIVE = 1,  /* Additive: Vibrant punchy highlights */
    BLOOM_BLEND_SOFT_LIGHT = 2,/* Soft Light: Subtle ethereal diffusion */
    BLOOM_BLEND_COUNT = 3
} SoeBloomBlend;

/* Bloom Color Atmosphere Tint */
typedef enum {
    BLOOM_TINT_NATURAL = 0,    /* Natural white / source color */
    BLOOM_TINT_WARM_GOLD = 1,  /* Golden amber sunlight */
    BLOOM_TINT_MYSTIC_CYAN = 2,/* Cool blue/cyan ethereal aura */
    BLOOM_TINT_ROSE = 3,       /* Twilight magenta / rose */
    BLOOM_TINT_COUNT = 4
} SoeBloomTint;

/* Tab 4: Input Options */
enum {
    INPUT_OPT_VOLUME = 0,       /* Audio Volume 0%..100% */
    INPUT_OPT_CONTROLLER = 1,   /* Active Gamepad info */
    INPUT_OPT_KEYBINDS = 2,     /* Keybinds profile info */
    INPUT_OPT_HOTKEY_MENU = 3,  /* Menu shortcut info */
    INPUT_OPT_HOTKEY_DEBUG = 4, /* Debug shortcut info */
    INPUT_OPT_COUNT = 5
};

/* Tab 5: Help / ReadMe */
enum {
    HELP_OPT_PAGE = 0,          /* Section switcher / scroll */
    HELP_OPT_COUNT = 1
};

/* ── Debug Window Item Identifiers ────────────────────────────────────────── */

enum {
    DBG_GIVE_EVERYTHING = 0,/* Max currencies/stats, all weapons+Lv3, gear, alchemy, items, charms */
    DBG_INF_HP = 1,         /* Toggle infinite HP */
    DBG_INF_STAMINA = 2,    /* Toggle infinite stamina / 100% attack bar */
    DBG_ONE_HIT_KO = 3,     /* Toggle One-Hit KO (999 Attack) */
    DBG_WALK_WALLS = 4,     /* Toggle walk through walls (no-clip) */
    DBG_KEY_TRADE_ITEMS = 5,/* Open Key & Trade Items picker window to the left of Debug Tools */
    DBG_DOG_FORM = 6,       /* Cycle dog form (with right arrow) */
    DBG_JUKEBOX = 7,        /* Open Jukebox window to the left of Debug Tools */
    DBG_WARP = 8,           /* Warp to target map number */
    DBG_TOTAL_COUNT
};

/* ── State Storage & Defaults ─────────────────────────────────────────────── */

/* Defaults: 21:9 Ultrawide, 8:7 Pixel Aspect, 4x Scale, CRT Off, Bloom Default, Ivor Tower Dialog, Sprite Interp Off, Bug fixes ON, Cheats OFF */
static SoeWidescreenMode s_ws_mode = SOE_WS_21_9;
static SoeHudPositionMode s_hud_mode = SOE_HUD_MOVE_LEFT;
static int s_crt_mode = 0;
static SoeCrtPreset s_crt_preset = CRT_PRESET_OFF;
static SoeCrtDensity s_crt_density = (SoeCrtDensity)0;
static SoeCrtOpacity s_crt_opacity = (SoeCrtOpacity)0;
static SoeCrtThickness s_crt_thickness = (SoeCrtThickness)0;
static SoeCrtMask s_crt_mask = (SoeCrtMask)0;
static SoeCrtBloom s_crt_bloom = (SoeCrtBloom)0;
static SoeCrtVignette s_crt_vignette = (SoeCrtVignette)0;
static SoeBloomPreset s_bloom_preset = BLOOM_PRESET_DEFAULT;
static SoeBloomIntensity s_bloom_intensity = (SoeBloomIntensity)2;
static SoeBloomThreshold s_bloom_threshold = BLOOM_THRESH_WIDE_80;
static SoeBloomRadius s_bloom_radius = BLOOM_RADIUS_BROAD_4PX;
static SoeBloomBlend s_bloom_blend = BLOOM_BLEND_ADDITIVE;
static SoeBloomTint s_bloom_tint = BLOOM_TINT_NATURAL;
static char s_menu_toast[64] = {0};
static int s_menu_toast_timer = 0;
static bool s_pillarbox_enabled = true; /* Slideout toggle: Pillarboxes on/off */
static bool s_lock_60fps = false;       /* Slideout toggle: false = High-Hz Subpixel Scroll (Auto Display Hz) */
static bool s_interpolate_sprites = false; /* Slideout toggle: Subpixel sprite interpolation */
static bool s_enhanced_ring_menu = true;  /* Slideout toggle: Enhanced Ring Menu (smooth OAM + texel cache) */
#ifdef __ANDROID__
static bool s_android_touch_overlay = true; /* Android-only: On-screen Touch Controller Overlay (On on first boot) */
#endif

/* Enhanced Ring Menu opening curve: ~30% slower approach (~13 frames) than before
 * with the same 6-frame damped radial spring bounce (44 -> 34 -> 45 -> 44 -> 42) on open only. */
static bool s_ring_open_active = false;
static int s_ring_open_step = 0;
static uint8_t s_ring_final_angle = 0;

static void SoeRingHookStart(CpuState *cpu, uint32_t pc24)
{
    (void)cpu;
    (void)pc24;
    s_ring_open_active = true;
    s_ring_open_step = 0;
}

static void SoeRingHookLoopTop(CpuState *cpu, uint32_t pc24)
{
    (void)cpu;
    (void)pc24;
    if (!s_enhanced_ring_menu || !s_ring_open_active) return;

    static const uint8_t kRadii[19] = {
        238, 221, 204, 187, 170, 153, 136, 119, 103, 87, 72, 57, 44,
        36, 34, 37, 42, 45, 44
    };
    static const uint8_t kAngleOffsets[19] = {
        84, 77, 70, 63, 56, 49, 42, 36, 30, 24, 18, 13, 8,
        5, 3, 1, 0, 0, 0
    };

    if (s_ring_open_step == 0) {
        s_ring_final_angle = (uint8_t)((g_ram[0x061B] - 0x54u) & 0xFFu);
    }
    int idx = s_ring_open_step;
    if (idx < 0) idx = 0;
    if (idx >= 19) idx = 18;

    g_ram[0x0619] = kRadii[idx];
    g_ram[0x061A] = 0;
    g_ram[0x061B] = (uint8_t)((s_ring_final_angle + kAngleOffsets[idx]) & 0xFFu);
    g_ram[0x061C] = 0;
}

static void SoeRingHookLoopEnd(CpuState *cpu, uint32_t pc24)
{
    (void)cpu;
    (void)pc24;
    if (!s_enhanced_ring_menu || !s_ring_open_active) return;

    s_ring_open_step++;
    if (s_ring_open_step < 19) {
        interp_bridge_pre_opcode_redirect(0x8EB10Cu);
    } else {
        interp_bridge_pre_opcode_redirect(0x8EB146u);
    }
}

static void SoeRingHookFinalSettle(CpuState *cpu, uint32_t pc24)
{
    (void)cpu;
    (void)pc24;
    if (!s_ring_open_active) return;
    s_ring_open_active = false;
    if (s_enhanced_ring_menu) {
        g_ram[0x0619] = 0x2Au;
        g_ram[0x061A] = 0;
    }
}

static void SoeInstallEnhancedRingMenuHooks(void)
{
    interp_bridge_add_pre_opcode_hook(0x8EB0DCu, SoeRingHookStart);
    interp_bridge_add_pre_opcode_hook(0x8EB10Cu, SoeRingHookLoopTop);
    interp_bridge_add_pre_opcode_hook(0x8EB128u, SoeRingHookLoopEnd);
    interp_bridge_add_pre_opcode_hook(0x8EB178u, SoeRingHookFinalSettle);
}
static bool s_eliminate_lag_frames = true; /* Slideout toggle: Fast CPU/DMA to eliminate SNES lag frames */
static double s_active_pres_hz = 60.098811862;
extern void snes_beam_frame_driver_set_fast_cpu(bool enabled);
static SoeDialogTheme s_dialog_theme = DIALOG_THEME_OMNITOPIA;
static bool s_solid_dialog_bg = true;   /* Kept synced with s_dialog_theme != DIALOG_THEME_ORIGINAL */
static bool s_dialog_capture_active = false;
static int s_dialog_capture_h = 192;
static uint32_t s_bg3_overlay_buf[240 * 256];
static bool s_in_winprefs = false;
static int s_winprefs_idx = 1; /* Defaults to Omnitopia (Blue Glass) */
static uint16_t s_winprefs_expected_0b15 = 0x1994;
static bool s_debug_overlay = false;    /* Checkbox option: Debug overlay window in upper right */
static bool s_menu_open = false;
static bool s_menu_paused = false;
static float s_dbg_slide = 0.0f;       /* Debug stack slide: 0 (hidden off right edge) to 1 (open) */
static float s_warp_slide = 0.0f;      /* Warp takeover slide: 0 (Debug Tools visible) to 1 (Warp + Preview) */
static float s_anim_x = -256.0f;        /* Drawer X position: -256 (hidden) to 0 (open) */
static const float kMenuWidth = 252.0f; /* Drawer width in pixels */
static SoeMenuTab s_active_tab = TAB_GAME_OPTIONS; /* Active tab */

typedef enum {
    RESET_CONFIRM_NONE = 0,
    RESET_CONFIRM_ALL,
    RESET_CONFIRM_VIDEO,
    RESET_CONFIRM_CRT,
    RESET_CONFIRM_BLOOM,
    RESET_CONFIRM_EXIT_LAUNCHER,
    RESET_CONFIRM_EXIT_DESKTOP
} SoeResetConfirmKind;
static SoeResetConfirmKind s_reset_confirm = RESET_CONFIRM_NONE;
static int s_reset_confirm_choice = 1; /* 0 = Yes, 1 = No */
static int s_tab_selected_row[TAB_COUNT] = {0}; /* Per-tab selected row */
static int s_dbg_selected_row = 0;
static bool s_event_filter_installed = false;
static unsigned s_frame_counter = 0;
/* Pillarbox editing (badge + 0/1 keys) is active whenever the debug stack is shown */
#define PB_EDIT_ACTIVE (s_debug_overlay || s_dbg_slide > 0.0f)
static bool s_map_badge_focused = false;
static bool s_map_picker_open = false;  /* Number picker popup for map pillarbox mode */
static uint32_t s_last_mouse_move_ticks = 0;
static bool s_cursor_visible = true;
static bool s_prev_pad_debug = false;   /* RT edge detector */

/* Per-map pillarbox configuration (0 = Pillarboxed / Black Overlay, 1 = Widescreen, 2 = Custom/Future) */
static uint8_t s_map_pillarbox[256];

/* Bug Fix Toggles (all enabled by default under Game Enhancements) */
static bool s_fix_silver_sheath = true;
static bool s_fix_atlas_underflow = true;
static bool s_fix_dog_leveling = true;
static bool s_fix_softlocks = true;

/* Dog Forms: Wolf (Prehistoria), Greyhound (Antiqua), Poodle (Gothica), Toaster (Omnitopia), Spot (Podunk) */
static int s_dog_form = 0;
static const char *const kDogFormNames[5] = {
    "Wolf", "Greyhound", "Poodle", "Toaster", "Spot"
};
static const uint16_t kDogFormReqIds[5] = {
    0x0002, /* Wolf (Prehistoria)  -> $8F:9395 */
    0x0006, /* Greyhound (Antiqua) -> $8F:93E5 */
    0x0008, /* Poodle (Gothica)    -> $8F:940D */
    0x000C, /* Toaster (Omnitopia) -> $8F:9435 */
    0x000A  /* Spot (Podunk)       -> $8F:936D */
};

/* Map Warp Target Number (0..255) & Transition State */
static uint16_t s_warp_map_id = 0;
static uint16_t s_warp_target_map = 0;
static int s_warp_monitor_frames = 0;
static int s_warp_stuck_black_frames = 0;
static int s_post_warp_walk_timer = 0;
static bool s_walk_walls_active = false;
static bool s_inf_hp_active = false;
static bool s_inf_stamina_active = false;
static bool s_one_hit_ko_active = false;
static uint16_t s_orig_boy_atk = 0;
static uint16_t s_orig_dog_atk = 0;

/* Camera tracking for subpixel scroll interpolation across high-refresh (100 Hz+) displays */
static int16_t s_cam_x_prev = 0;
static int16_t s_cam_y_prev = 0;
static int16_t s_cam_x_curr = 0;
static int16_t s_cam_y_curr = 0;
static int16_t s_frame_left_prev = 0;
static int16_t s_frame_left_curr = 0;
static bool s_cam_initialized = false;
static int s_cam_dx_prev = 0;
static int s_cam_dy_prev = 0;
static double s_cam_res_x_prev = 0.0;
static double s_cam_res_y_prev = 0.0;
static double s_cam_res_x_curr = 0.0;
static double s_cam_res_y_curr = 0.0;

/* OAM sprite tracking for render-only subpixel entity/sprite interpolation */
typedef struct {
    int16_t x;
    int16_t y;
    uint16_t w0;
    uint16_t w1;
    uint8_t size;
    bool active;
} SoeOamSnap;

static SoeOamSnap s_oam_prev[128];
static SoeOamSnap s_oam_curr[128];
static bool s_oam_initialized = false;
static uint32_t s_oam_sim_gen = 1;
static uint8_t s_oam_suppressed_mask[16];

typedef struct {
    uint32_t gen;
    uint32_t vram_writes;
    uint16_t w1;
    int obj_adr;
    uint8_t size;
    uint8_t bright;
    uint32_t tex[64 * 64];
} SoeSpriteTexCache;
static SoeSpriteTexCache s_sprite_tex_cache[128];

/* Toast status message displayed in debug window */
static char s_debug_toast[64] = "";
static int s_debug_toast_timer = 0;

/* Full-frame screenshot capture state & 3-second cooldown */
static bool s_pending_screenshot = false;
static uint64_t s_screenshot_cooldown_until_ms = 0;

/* Confirmation dialog state for irreversible "Give Everything" action */
static bool s_give_all_confirm_open = false;
static int s_give_all_confirm_choice = 0; /* 0 = Cancel, 1 = Yes, Give All */

/* Jukebox sub-window state (opened from Debug Tools) */
static bool s_jukebox_open = false;
static uint8_t s_jukebox_songs[128];
static int s_jukebox_song_count = 0;
static bool s_jukebox_list_built = false;
static int s_jukebox_cursor = 0;
static int s_jukebox_scroll = 0;
static int s_pending_jukebox_song = -1;
static int s_pending_jukebox_sfx = -1;
static bool s_jukebox_synced_once = false;
static bool s_jukebox_lock_song = true;
static int  s_jukebox_locked_song_id = -1;
static bool s_jukebox_keep_focused = false;
static const int kJukeboxVisibleRows = 8;

/* Key & Trade Items sub-window state (opened from Debug Tools) */
typedef struct {
    const char *name;
    uint16_t addr;
    uint8_t mask; /* non-zero = bitmask Key Item in $2264; 0 = uint16 Trade Good count */
} SoeKeyTradeItemDef;

static const SoeKeyTradeItemDef kKeyTradeItems[] = {
    /* 6 Key Items ($2264 bitmasks per $CC:BBB0) */
    { "Diamond Eye",           0x2264, 0x01 },
    { "Diamond Eyes",          0x2264, 0x02 },
    { "Energy Core",           0x2264, 0x20 },
    { "Gauge",                 0x2264, 0x04 },
    { "Queen's Key",           0x2264, 0x10 },
    { "Wheel",                 0x2264, 0x08 },
    /* 13 Trading Goods ($2517..$2530 uint16 counts per $CC:BC00) */
    { "Annihilation Amulet",   0x2517, 0x00 },
    { "Bead",                  0x2519, 0x00 },
    { "Ceramic Pot",           0x251B, 0x00 },
    { "Chicken",               0x251D, 0x00 },
    { "Golden Jackal",         0x251F, 0x00 },
    { "Jeweled Scarab",        0x2521, 0x00 },
    { "Limestone Tablet",      0x2523, 0x00 },
    { "Perfume",               0x2525, 0x00 },
    { "Rice",                  0x2527, 0x00 },
    { "Spice",                 0x2529, 0x00 },
    { "Spoon",                 0x252B, 0x00 },
    { "Tapestry",              0x252D, 0x00 },
    { "Ticket for Exhibition", 0x252F, 0x00 }
};
static const int kKeyTradeItemCount = (int)(sizeof(kKeyTradeItems) / sizeof(kKeyTradeItems[0]));
static const int kKeyTradeVisibleRows = 9;
static bool s_key_trade_open = false;
static int s_key_trade_cursor = 0;
static int s_key_trade_scroll = 0;

/* Warp Locations sub-window state (opened from Debug Tools) */
typedef struct {
    uint16_t map_id;
    bool is_named;
} SoeWarpLocationDef;

static SoeWarpLocationDef s_warp_locations[128];
static int s_warp_location_count = 0;
static bool s_warp_list_built = false;
static bool s_warp_win_open = false;
static int s_warp_cursor = 0;
static int s_warp_scroll = 0;
static const int kWarpVisibleRows = 9;

/* Cached map preview metadata & PC-resolution native overlay for the selected Warp To entry */
static int32_t s_warp_preview_cached_id = -1;
static bool s_warp_preview_valid = false;
static int s_warp_preview_src_w = 0;
static int s_warp_preview_src_h = 0;
static char s_warp_preview_path[128];

static bool s_warp_native_ov_active = false;
static uint16_t s_warp_native_ov_map_id = 0;
static float s_warp_native_ov_x = 0.0f;
static float s_warp_native_ov_y = 0.0f;
static float s_warp_native_ov_w = 0.0f;
static float s_warp_native_ov_h = 0.0f;
static int s_warp_native_ov_fw = 0;
static int s_warp_native_ov_fh = 0;

int SoeGetNativeOverlay(struct SnesNativeOverlay *out)
{
    if (!out || !s_warp_native_ov_active || !s_warp_preview_valid) return 0;
    out->image_key = (uint64_t)s_warp_native_ov_map_id + 1u;
    out->image_path = s_warp_preview_path;
    out->snes_x = s_warp_native_ov_x;
    out->snes_y = s_warp_native_ov_y;
    out->snes_w = s_warp_native_ov_w;
    out->snes_h = s_warp_native_ov_h;
    out->frame_w = s_warp_native_ov_fw;
    out->frame_h = s_warp_native_ov_fh;
    out->preserve_aspect = 1;
    out->alpha = 255;
    return 1;
}

/* ── Fallback 5x7 Bitmap Font (used only if ROM font is unavailable) ──────── */

static const uint8_t kFont5x7[62][7] = {
    /* 32 ' ' */ {0, 0, 0, 0, 0, 0, 0},
    /* 33 '!' */ {8, 8, 8, 8, 0, 8, 0},
    /* 34 '"' */ {20, 20, 0, 0, 0, 0, 0},
    /* 35 '#' */ {10, 31, 10, 31, 10, 0, 0},
    /* 36 '$' */ {10, 30, 10, 15, 10, 0, 0},
    /* 37 '%' */ {25, 26, 2, 8, 11, 19, 0},
    /* 38 '&' */ {8, 20, 8, 21, 18, 13, 0},
    /* 39 ''' */ {8, 8, 0, 0, 0, 0, 0},
    /* 40 '(' */ {4, 8, 8, 8, 8, 4, 0},
    /* 41 ')' */ {8, 4, 4, 4, 4, 8, 0},
    /* 42 '*' */ {10, 4, 31, 4, 10, 0, 0},
    /* 43 '+' */ {0, 4, 4, 31, 4, 4, 0},
    /* 44 ',' */ {0, 0, 0, 0, 4, 4, 8},
    /* 45 '-' */ {0, 0, 0, 31, 0, 0, 0},
    /* 46 '.' */ {0, 0, 0, 0, 0, 12, 12},
    /* 47 '/' */ {1, 2, 4, 8, 16, 0, 0},
    /* 48 '0' */ {14, 17, 19, 21, 25, 17, 14},
    /* 49 '1' */ {4, 12, 4, 4, 4, 4, 14},
    /* 50 '2' */ {14, 17, 1, 6, 8, 16, 31},
    /* 51 '3' */ {14, 17, 1, 6, 1, 17, 14},
    /* 52 '4' */ {17, 17, 17, 31, 1, 1, 1},
    /* 53 '5' */ {31, 16, 30, 1, 1, 17, 14},
    /* 54 '6' */ {14, 16, 30, 17, 17, 17, 14},
    /* 55 '7' */ {31, 1, 2, 4, 8, 8, 8},
    /* 56 '8' */ {14, 17, 17, 14, 17, 17, 14},
    /* 57 '9' */ {14, 17, 17, 15, 1, 1, 14},
    /* 58 ':' */ {0, 12, 12, 0, 12, 12, 0},
    /* 59 ';' */ {0, 12, 12, 0, 12, 4, 8},
    /* 60 '<' */ {2, 4, 8, 16, 8, 4, 2},
    /* 61 '=' */ {0, 31, 0, 31, 0, 0, 0},
    /* 62 '>' */ {8, 4, 2, 1, 2, 4, 8},
    /* 63 '?' */ {14, 17, 1, 6, 4, 0, 4},
    /* 64 '@' */ {14, 17, 27, 21, 23, 16, 14},
    /* 65 'A' */ {14, 17, 17, 31, 17, 17, 17},
    /* 66 'B' */ {30, 17, 17, 30, 17, 17, 30},
    /* 67 'C' */ {14, 17, 16, 16, 16, 17, 14},
    /* 68 'D' */ {30, 17, 17, 17, 17, 17, 30},
    /* 69 'E' */ {31, 16, 16, 30, 16, 16, 31},
    /* 70 'F' */ {31, 16, 16, 30, 16, 16, 16},
    /* 71 'G' */ {14, 17, 16, 23, 17, 17, 14},
    /* 72 'H' */ {17, 17, 17, 31, 17, 17, 17},
    /* 73 'I' */ {14, 4, 4, 4, 4, 4, 14},
    /* 74 'J' */ {7, 1, 1, 1, 1, 17, 14},
    /* 75 'K' */ {17, 18, 20, 24, 20, 18, 17},
    /* 76 'L' */ {16, 16, 16, 16, 16, 16, 31},
    /* 77 'M' */ {17, 27, 21, 21, 17, 17, 17},
    /* 78 'N' */ {17, 25, 21, 19, 17, 17, 17},
    /* 79 'O' */ {14, 17, 17, 17, 17, 17, 14},
    /* 80 'P' */ {30, 17, 17, 30, 16, 16, 16},
    /* 81 'Q' */ {14, 17, 17, 17, 21, 18, 13},
    /* 82 'R' */ {30, 17, 17, 30, 20, 18, 17},
    /* 83 'S' */ {15, 16, 16, 14, 1, 1, 30},
    /* 84 'T' */ {31, 4, 4, 4, 4, 4, 4},
    /* 85 'U' */ {17, 17, 17, 17, 17, 17, 14},
    /* 86 'V' */ {17, 17, 17, 17, 10, 10, 4},
    /* 87 'W' */ {17, 17, 17, 21, 21, 27, 17},
    /* 88 'X' */ {17, 17, 10, 4, 10, 17, 17},
    /* 89 'Y' */ {17, 17, 10, 4, 4, 4, 4},
    /* 90 'Z' */ {31, 1, 2, 4, 8, 16, 31},
    /* 91 '[' */ {14, 8, 8, 8, 8, 8, 14},
    /* 92 '\' */ {16, 8, 4, 2, 1, 0, 0},
    /* 93 ']' */ {14, 2, 2, 2, 2, 2, 14},
};

/* ── Graphic Helper Primitives ────────────────────────────────────────────── */

/*
 * Blend a semi-transparent colored rectangle over an ARGB8888 framebuffer.
 * Uses integer fixed-point alpha blending: (color * a + bg * (255 - a)) / 255.
 */
static void DrawSemiTransparentRect(uint32_t *dst, int stride, int h_max,
                                    int x0, int y0, int w, int h,
                                    uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (!dst || stride <= 0 || h_max <= 0) return;
    int x_start = x0 < 0 ? 0 : x0;
    int y_start = y0 < 0 ? 0 : y0;
    int x_end = (x0 + w) > stride ? stride : (x0 + w);
    int y_end = (y0 + h) > h_max ? h_max : (y0 + h);
    if (x_start >= x_end || y_start >= y_end) return;

    const uint32_t inv_a = 255 - a;
    for (int y = y_start; y < y_end; y++) {
        uint32_t *row = &dst[y * stride];
        for (int x = x_start; x < x_end; x++) {
            uint32_t bg = row[x];
            uint32_t bg_r = (bg >> 16) & 0xFF;
            uint32_t bg_g = (bg >> 8) & 0xFF;
            uint32_t bg_b = bg & 0xFF;

            uint32_t out_r = ((uint32_t)r * a + bg_r * inv_a) / 255;
            uint32_t out_g = ((uint32_t)g * a + bg_g * inv_a) / 255;
            uint32_t out_b = ((uint32_t)b * a + bg_b * inv_a) / 255;

            row[x] = (0xFFu << 24) | (out_r << 16) | (out_g << 8) | out_b;
        }
    }
}

/*
 * Measure exact pixel width of text rendered with DrawSmallText.
 */
static int MeasureSmallText(const char *s)
{
    if (!s) return 0;
    const uint8_t *rom = (g_snes && g_snes->cart && g_snes->cart->rom &&
                          g_snes->cart->romSize >= 0x041904u)
                         ? g_snes->cart->rom : NULL;
    int cur_x = 0;
    if (rom && rom[0x040C82] == 12 && rom[0x040C83] == 2) {
        const uint32_t font_widths_base = 0x041884u;
        for (; *s; s++) {
            unsigned char c = (unsigned char)*s;
            if (c == ' ') {
                cur_x += 3;
                continue;
            }
            if (c < 0x21 || c > 0x7E) c = '?';
            uint32_t idx = (uint32_t)(c - 0x20u);
            int w_ch = (int)rom[font_widths_base + idx];
            if (w_ch <= 0 || w_ch > 12) w_ch = 4;
            cur_x += w_ch + 1;
        }
    } else {
        cur_x = (int)strlen(s) * 6;
    }
    return cur_x;
}

/*
 * Dynamic drawer width: in fullscreen mode, menu covers the entire screen width.
 */
static float GetMenuWidth(void)
{
    int frame_w = snesrecomp_desktop_frame_width();
    return (frame_w > 0) ? (float)frame_w : 342.0f;
}

/*
 * Draw a rectangular 1-pixel border.
 */
static void DrawRectOutline(uint32_t *dst, int stride, int h_max,
                            int x0, int y0, int w, int h,
                            uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    DrawSemiTransparentRect(dst, stride, h_max, x0, y0, w, 1, r, g, b, a);
    DrawSemiTransparentRect(dst, stride, h_max, x0, y0 + h - 1, w, 1, r, g, b, a);
    DrawSemiTransparentRect(dst, stride, h_max, x0, y0, 1, h, r, g, b, a);
    DrawSemiTransparentRect(dst, stride, h_max, x0 + w - 1, y0, 1, h, r, g, b, a);
}

/*
 * Render text using Secret of Evermore's authentic in-game bitmap font from ROM!
 *
 * In Secret of Evermore (USA), the game's variable-width bitmap fonts live at the
 * start of bank $C4 (initialized by $8C:9DBD and rendered by $8C:A5A2):
 *   - $C4:0000 (ROM offset 0x040000): Main 8px-tall proportional menu/dialog font
 *     (header: height=12, width_bytes=2, 24 bytes/glyph at 0x040002, per-glyph
 *     pixel width table at 0x040C02).
 *   - $C4:0C82 (ROM offset 0x040C82): Compact 6px-tall proportional HUD/ring font
 *     (header: height=12, width_bytes=2, 24 bytes/glyph at 0x040C84, per-glyph
 *     pixel width table at 0x041884).
 *
 * We render using the compact 6px-tall proportional font at $C4:0C82 (rows 1..10 of
 * the 12-row cell) with a 1px dark navy drop shadow (0xFF040C24), scaling down the
 * menu text cleanly by 25% (6px vs 8px) with crisp integer pixels.
 */
static void DrawSmallText(uint32_t *dst, int stride, int h_max,
                          int x0, int y0, const char *s, uint32_t col)
{
    if (!dst || !s || stride <= 0 || h_max <= 0) return;

    const uint8_t *rom = (g_snes && g_snes->cart && g_snes->cart->rom &&
                          g_snes->cart->romSize >= 0x041904u)
                         ? g_snes->cart->rom : NULL;

    /* Use the authentic Secret of Evermore compact 6px bitmap font from bank $C4 ($C4:0C82) */
    if (rom && rom[0x040C82] == 12 && rom[0x040C83] == 2) {
        const uint32_t font_glyphs_base = 0x040C84u;
        const uint32_t font_widths_base = 0x041884u;
        const uint32_t shadow_col = 0xFF040C24u;
        int cur_x = x0;

        for (; *s; s++) {
            unsigned char c = (unsigned char)*s;
            if (c == ' ') {
                cur_x += 3; /* Space advance (glyph 0x20 in $C4:0C82 is '1/2' symbol) */
                continue;
            }
            if (c < 0x21 || c > 0x7E) c = '?';

            uint32_t idx = (uint32_t)(c - 0x20u);
            int w_ch = (int)rom[font_widths_base + idx];
            if (w_ch <= 0 || w_ch > 12) w_ch = 4;

            uint32_t g_off = font_glyphs_base + idx * 24u;

            /* Pass 1: 1px drop shadow (down-right) for authentic SNES VWF contrast */
            for (int r = 1; r <= 10; r++) {
                uint16_t bits = ((uint16_t)rom[g_off + (uint32_t)r] << 8) |
                                 (uint16_t)rom[g_off + 12u + (uint32_t)r];
                if (!bits) continue;
                int py = y0 + (r - 1);
                if (py < 0 || py >= h_max) continue;
                uint32_t *sh_row = &dst[py * stride];
                for (int x = 0; x < w_ch; x++) {
                    if ((bits & (0x8000u >> x)) == 0) continue;
                    int px = cur_x + x + 1;
                    if (px >= 0 && px < stride) sh_row[px] = shadow_col;
                }
            }

            /* Pass 2: Crisp foreground glyph pixels */
            for (int r = 1; r <= 10; r++) {
                uint16_t bits = ((uint16_t)rom[g_off + (uint32_t)r] << 8) |
                                 (uint16_t)rom[g_off + 12u + (uint32_t)r];
                if (!bits) continue;
                int py = y0 + (r - 2);
                if (py < 0 || py >= h_max) continue;
                uint32_t *fg_row = &dst[py * stride];
                for (int x = 0; x < w_ch; x++) {
                    if ((bits & (0x8000u >> x)) == 0) continue;
                    int px = cur_x + x;
                    if (px >= 0 && px < stride) fg_row[px] = col;
                }
            }

            cur_x += w_ch + 1;
        }
        return;
    }

    /* Fallback 5x7 bitmap font if ROM is not yet mapped */
    int cur_x = x0;
    for (; *s; s++) {
        char c = *s;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c < 32 || c > 93) c = '?';
        const uint8_t *glyph = kFont5x7[(int)c - 32];

        for (int y = 0; y < 7; y++) {
            uint8_t row = glyph[y];
            int py = y0 + y;
            if (py < 0 || py >= h_max) continue;
            uint32_t *dst_row = &dst[py * stride];

            for (int x = 0; x < 5; x++) {
                if ((row & (16u >> x)) == 0) continue;
                int px = cur_x + x;
                if (px >= 0 && px < stride) dst_row[px] = col;
            }
        }
        cur_x += 6;
    }
}



/* ── CRT Emulation System ─────────────────────────────────────────────────── */

static const char *const kCrtPresetNames[CRT_PRESET_COUNT] = {
    "Off",
    "Just Scanlines (Standard 2px)",
    "Just Scanlines (Fine 1px)",
    "Just Scanlines (Deep 65%)",
    "Subtle CRT (Clean)",
    "Classic CRT (Clean)",
    "Solid Retro (Black Gaps)",
    "Trinitron PVM (Vertical Mask)",
    "Shadow Mask (Checkerboard)",
    "Arcade Monitor (Slot Grid)",
    "Custom"
};

static const char *const kCrtDensityNames[CRT_DENSITY_COUNT] = {
    "Fine (1px Dense)",
    "Standard (2px Native)",
    "Medium (3px)",
    "Wide (4px)",
    "Double Dense (2/3)"
};

static const char *const kCrtOpacityNames[CRT_OPACITY_COUNT] = {
    "15% (Ultra Light)",
    "25% (Light / Subtle)",
    "35% (Medium Subtle)",
    "50% (Medium Classic)",
    "65% (Heavy / Deep)",
    "80% (Dark)",
    "100% (Solid Black)"
};

static const char *const kCrtThicknessNames[CRT_THICK_COUNT] = {
    "Thin (1-Line Beam)",
    "Thick (2-Line Beam)"
};

static const char *const kCrtMaskNames[CRT_MASK_COUNT] = {
    "Off (Pure Horizontal)",
    "Aperture Grille (Stripes)",
    "Shadow Mask (Checkerboard)",
    "Slot Mask (Arcade Grid)"
};

static const char *const kCrtBloomNames[CRT_BLOOM_COUNT] = {
    "Off (+0%)",
    "Mild (+10%)",
    "Medium (+20%)",
    "Bright (+30%)"
};

static const char *const kCrtVignetteNames[CRT_VIGNETTE_COUNT] = {
    "Off",
    "Subtle Glass Curve",
    "Classic CRT Corner"
};

static const int kCrtOpacityFactors[CRT_OPACITY_COUNT] = {
    218, /* 15% */
    192, /* 25% */
    166, /* 35% */
    128, /* 50% */
    90,  /* 65% */
    51,  /* 80% */
    0    /* 100% (Solid Black) */
};

static const int kCrtBloomMultipliers[CRT_BLOOM_COUNT] = {
    256, /* +0% (1.0x) */
    282, /* +10% (1.1x) */
    307, /* +20% (1.2x) */
    333  /* +30% (1.3x) */
};

/*
 * Path to crt_config.json in current directory or next to executable.
 */
static const char *GetCrtConfigFilePath(void)
{
    static char s_path[512] = {0};
    if (s_path[0] == '\0') {
        FILE *f = fopen("Configs/crt_config.json", "r");
        if (f) {
            fclose(f);
            snprintf(s_path, sizeof(s_path), "Configs/crt_config.json");
            return s_path;
        }
        const char *base = SDL_GetBasePath();
        if (base) {
            snprintf(s_path, sizeof(s_path), "%sConfigs/crt_config.json", base);
            return s_path;
        }
        snprintf(s_path, sizeof(s_path), "Configs/crt_config.json");
    }
    return s_path;
}

static void SoeSaveCrtConfig(void)
{
    soe_mkdir("Configs");
    const char *path = GetCrtConfigFilePath();
    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "{\n");
    fprintf(f, "  \"preset\": %d,\n", (int)s_crt_preset);
    fprintf(f, "  \"density\": %d,\n", (int)s_crt_density);
    fprintf(f, "  \"opacity\": %d,\n", (int)s_crt_opacity);
    fprintf(f, "  \"thickness\": %d,\n", (int)s_crt_thickness);
    fprintf(f, "  \"mask\": %d,\n", (int)s_crt_mask);
    fprintf(f, "  \"bloom\": %d,\n", (int)s_crt_bloom);
    fprintf(f, "  \"vignette\": %d\n", (int)s_crt_vignette);
    fprintf(f, "}\n");
    fclose(f);
}

static void SoeApplyCrtPreset(SoeCrtPreset preset)
{
    s_crt_preset = preset;
    switch (preset) {
    case CRT_PRESET_OFF:
        s_crt_mode = 0;
        break;
    case CRT_PRESET_JUST_SCANLINES:
        s_crt_density = CRT_DENSITY_STANDARD_2PX;
        s_crt_opacity = CRT_OPACITY_25;
        s_crt_thickness = CRT_THICK_THIN;
        s_crt_mask = CRT_MASK_OFF;
        s_crt_bloom = CRT_BLOOM_OFF;
        s_crt_vignette = CRT_VIGNETTE_OFF;
        s_crt_mode = 1;
        break;
    case CRT_PRESET_JUST_SCANLINES_FINE:
        s_crt_density = CRT_DENSITY_FINE_1PX;
        s_crt_opacity = CRT_OPACITY_15;
        s_crt_thickness = CRT_THICK_THIN;
        s_crt_mask = CRT_MASK_OFF;
        s_crt_bloom = CRT_BLOOM_OFF;
        s_crt_vignette = CRT_VIGNETTE_OFF;
        s_crt_mode = 1;
        break;
    case CRT_PRESET_JUST_SCANLINES_DEEP:
        s_crt_density = CRT_DENSITY_STANDARD_2PX;
        s_crt_opacity = CRT_OPACITY_65;
        s_crt_thickness = CRT_THICK_THIN;
        s_crt_mask = CRT_MASK_OFF;
        s_crt_bloom = CRT_BLOOM_OFF;
        s_crt_vignette = CRT_VIGNETTE_OFF;
        s_crt_mode = 2;
        break;
    case CRT_PRESET_SUBTLE:
        s_crt_density = CRT_DENSITY_STANDARD_2PX;
        s_crt_opacity = CRT_OPACITY_25;
        s_crt_thickness = CRT_THICK_THIN;
        s_crt_mask = CRT_MASK_OFF;
        s_crt_bloom = CRT_BLOOM_OFF;
        s_crt_vignette = CRT_VIGNETTE_OFF;
        s_crt_mode = 1;
        break;
    case CRT_PRESET_CLASSIC:
        s_crt_density = CRT_DENSITY_STANDARD_2PX;
        s_crt_opacity = CRT_OPACITY_50;
        s_crt_thickness = CRT_THICK_THIN;
        s_crt_mask = CRT_MASK_OFF;
        s_crt_bloom = CRT_BLOOM_MILD;
        s_crt_vignette = CRT_VIGNETTE_SUBTLE;
        s_crt_mode = 2;
        break;
    case CRT_PRESET_SOLID_RETRO:
        s_crt_density = CRT_DENSITY_STANDARD_2PX;
        s_crt_opacity = CRT_OPACITY_100;
        s_crt_thickness = CRT_THICK_THIN;
        s_crt_mask = CRT_MASK_OFF;
        s_crt_bloom = CRT_BLOOM_BRIGHT;
        s_crt_vignette = CRT_VIGNETTE_OFF;
        s_crt_mode = 2;
        break;
    case CRT_PRESET_TRINITRON:
        s_crt_density = CRT_DENSITY_FINE_1PX;
        s_crt_opacity = CRT_OPACITY_35;
        s_crt_thickness = CRT_THICK_THIN;
        s_crt_mask = CRT_MASK_APERTURE_GRILLE;
        s_crt_bloom = CRT_BLOOM_MEDIUM;
        s_crt_vignette = CRT_VIGNETTE_SUBTLE;
        s_crt_mode = 1;
        break;
    case CRT_PRESET_SHADOW_MASK:
        s_crt_density = CRT_DENSITY_STANDARD_2PX;
        s_crt_opacity = CRT_OPACITY_35;
        s_crt_thickness = CRT_THICK_THIN;
        s_crt_mask = CRT_MASK_SHADOW_MASK;
        s_crt_bloom = CRT_BLOOM_MEDIUM;
        s_crt_vignette = CRT_VIGNETTE_SUBTLE;
        s_crt_mode = 2;
        break;
    case CRT_PRESET_ARCADE:
        s_crt_density = CRT_DENSITY_STANDARD_2PX;
        s_crt_opacity = CRT_OPACITY_50;
        s_crt_thickness = CRT_THICK_THICK;
        s_crt_mask = CRT_MASK_SLOT_MASK;
        s_crt_bloom = CRT_BLOOM_MEDIUM;
        s_crt_vignette = CRT_VIGNETTE_SUBTLE;
        s_crt_mode = 2;
        break;
    case CRT_PRESET_CUSTOM:
        s_crt_mode = 1;
        break;
    default:
        break;
    }
}

static void SoeApplyDefaultCrtSettings(void)
{
    /* Defaults: CRT Off by default, with user's custom sub-settings (0,0,0,0,0,0) */
    SoeApplyCrtPreset(CRT_PRESET_OFF);
    s_crt_density = (SoeCrtDensity)0;
    s_crt_opacity = (SoeCrtOpacity)0;
    s_crt_thickness = (SoeCrtThickness)0;
    s_crt_mask = (SoeCrtMask)0;
    s_crt_bloom = (SoeCrtBloom)0;
    s_crt_vignette = (SoeCrtVignette)0;
}

static void SoeLoadCrtConfig(void)
{
    SoeApplyDefaultCrtSettings();

    const char *path = GetCrtConfigFilePath();
    FILE *f = fopen(path, "r");
    if (!f) {
        SoeSaveCrtConfig();
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *q1 = strchr(line, '"');
        if (!q1) continue;
        char *q2 = strchr(q1 + 1, '"');
        if (!q2) continue;
        *q2 = '\0';
        char *colon = strchr(q2 + 1, ':');
        if (!colon) continue;
        int val = atoi(colon + 1);

        if (strcmp(q1 + 1, "preset") == 0) {
            if (val >= 0 && val < CRT_PRESET_COUNT) {
                SoeApplyCrtPreset((SoeCrtPreset)val);
            }
        } else if (strcmp(q1 + 1, "density") == 0) {
            if (val >= 0 && val < CRT_DENSITY_COUNT) s_crt_density = (SoeCrtDensity)val;
        } else if (strcmp(q1 + 1, "opacity") == 0) {
            if (val >= 0 && val < CRT_OPACITY_COUNT) s_crt_opacity = (SoeCrtOpacity)val;
        } else if (strcmp(q1 + 1, "thickness") == 0) {
            if (val >= 0 && val < CRT_THICK_COUNT) s_crt_thickness = (SoeCrtThickness)val;
        } else if (strcmp(q1 + 1, "mask") == 0) {
            if (val >= 0 && val < CRT_MASK_COUNT) s_crt_mask = (SoeCrtMask)val;
        } else if (strcmp(q1 + 1, "bloom") == 0) {
            if (val >= 0 && val < CRT_BLOOM_COUNT) s_crt_bloom = (SoeCrtBloom)val;
        } else if (strcmp(q1 + 1, "vignette") == 0) {
            if (val >= 0 && val < CRT_VIGNETTE_COUNT) s_crt_vignette = (SoeCrtVignette)val;
        }
    }
    fclose(f);
}

/*
 * Apply full-featured high-performance CRT filter:
 * Supports fine 1px scanlines (close together), custom spacing/thickness,
 * wide opacity range, Trinitron/Shadow/Slot masks, bloom boost, and vignette.
 */
static void ApplyCrtFilter(uint8_t *dst, size_t pitch, int w, int h,
                           SoeCrtPreset preset,
                           SoeCrtDensity density,
                           SoeCrtOpacity opacity,
                           SoeCrtThickness thickness,
                           SoeCrtMask mask,
                           SoeCrtBloom bloom,
                           SoeCrtVignette vignette)
{
    if (!dst || preset == CRT_PRESET_OFF) return;

    int base_factor = kCrtOpacityFactors[opacity % CRT_OPACITY_COUNT];
    int bloom_mult = kCrtBloomMultipliers[bloom % CRT_BLOOM_COUNT];

    int cx2 = w;
    int cy2 = h;
    int w2 = w * w;
    int h2 = h * h;
    if (w2 <= 0) w2 = 1;
    if (h2 <= 0) h2 = 1;

    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(dst + (size_t)y * pitch);

        int is_trough = 0;
        int is_secondary_trough = 0;

        switch (density) {
        case CRT_DENSITY_FINE_1PX:
            if ((y & 1) == 1) {
                is_trough = 1;
            } else {
                is_secondary_trough = 1;
            }
            break;
        case CRT_DENSITY_STANDARD_2PX:
            if ((y & 1) == 1) is_trough = 1;
            break;
        case CRT_DENSITY_MEDIUM_3PX:
            if (thickness == CRT_THICK_THICK) {
                if ((y % 3) != 0) is_trough = 1;
            } else {
                if ((y % 3) == 2) is_trough = 1;
            }
            break;
        case CRT_DENSITY_WIDE_4PX:
            if (thickness == CRT_THICK_THICK) {
                int mod = y % 4;
                if (mod == 2 || mod == 3) is_trough = 1;
            } else {
                if ((y % 4) == 3) is_trough = 1;
            }
            break;
        case CRT_DENSITY_DENSE_2_OF_3:
            if ((y % 3) != 0) is_trough = 1;
            break;
        default:
            if ((y & 1) == 1) is_trough = 1;
            break;
        }

        int scanline_factor = 256;
        if (is_trough) {
            scanline_factor = base_factor;
        } else if (is_secondary_trough) {
            if (thickness == CRT_THICK_THICK) {
                scanline_factor = base_factor;
            } else {
                scanline_factor = (base_factor + 256) / 2;
            }
        }

        int effective_row_factor = (scanline_factor * bloom_mult) >> 8;

        int dy = 2 * y - cy2;
        int y_term = (dy * dy * 256) / h2;
        int x = 0;

#if defined(__AVX2__)
        if (mask == CRT_MASK_OFF && vignette == 0) {
            const __m256i v_fac = _mm256_set1_epi16((short)effective_row_factor);
            const __m256i v_zero = _mm256_setzero_si256();
            const __m256i v_rgb_mask = _mm256_set1_epi32(0x00FFFFFF);
            const __m256i v_alpha_mask = _mm256_set1_epi32((int)0xFF000000u);
            for (; x + 8 <= w; x += 8) {
                __m256i p = _mm256_loadu_si256((const __m256i *)(row + x));
                __m256i p_lo = _mm256_unpacklo_epi8(p, v_zero);
                __m256i p_hi = _mm256_unpackhi_epi8(p, v_zero);
                p_lo = _mm256_srli_epi16(_mm256_mullo_epi16(p_lo, v_fac), 8);
                p_hi = _mm256_srli_epi16(_mm256_mullo_epi16(p_hi, v_fac), 8);
                __m256i scaled = _mm256_packus_epi16(p_lo, p_hi);
                __m256i out = _mm256_or_si256(_mm256_and_si256(p, v_alpha_mask),
                                              _mm256_and_si256(scaled, v_rgb_mask));
                _mm256_storeu_si256((__m256i *)(row + x), out);
            }
        }
#endif

        for (; x < w; x++) {
            uint32_t p = row[x];
            int r = (p >> 16) & 0xFF;
            int g = (p >> 8) & 0xFF;
            int b = p & 0xFF;

            int r_fac = effective_row_factor;
            int g_fac = effective_row_factor;
            int b_fac = effective_row_factor;

            if (mask == CRT_MASK_APERTURE_GRILLE) {
                int mod3 = x % 3;
                if (mod3 == 0) {
                    r_fac = (r_fac * 270) >> 8;
                    g_fac = (g_fac * 218) >> 8;
                    b_fac = (b_fac * 218) >> 8;
                } else if (mod3 == 1) {
                    r_fac = (r_fac * 218) >> 8;
                    g_fac = (g_fac * 270) >> 8;
                    b_fac = (b_fac * 218) >> 8;
                } else {
                    r_fac = (r_fac * 218) >> 8;
                    g_fac = (g_fac * 218) >> 8;
                    b_fac = (b_fac * 270) >> 8;
                }
            } else if (mask == CRT_MASK_SHADOW_MASK) {
                int dot = (x + (y & 1)) & 1;
                int dot_fac = dot ? 256 : 210;
                r_fac = (r_fac * dot_fac) >> 8;
                g_fac = (g_fac * dot_fac) >> 8;
                b_fac = (b_fac * dot_fac) >> 8;
            } else if (mask == CRT_MASK_SLOT_MASK) {
                int slot = ((x >> 1) + ((y >> 1) & 1)) & 1;
                int slot_fac = slot ? 256 : 210;
                r_fac = (r_fac * slot_fac) >> 8;
                g_fac = (g_fac * slot_fac) >> 8;
                b_fac = (b_fac * slot_fac) >> 8;
            }

            if (vignette > 0) {
                int dx = 2 * x - cx2;
                int r2 = (dx * dx * 256) / w2 + y_term;
                int thresh = (vignette == CRT_VIGNETTE_CLASSIC) ? 150 : 200;
                if (r2 > thresh) {
                    int max_r2 = 512;
                    int over = r2 - thresh;
                    int scale = (vignette == CRT_VIGNETTE_CLASSIC) ? 90 : 45;
                    int vig = 256 - (over * scale) / (max_r2 - thresh);
                    if (vig < 128) vig = 128;
                    r_fac = (r_fac * vig) >> 8;
                    g_fac = (g_fac * vig) >> 8;
                    b_fac = (b_fac * vig) >> 8;
                }
            }

            r = (r * r_fac) >> 8;
            g = (g * g_fac) >> 8;
            b = (b * b_fac) >> 8;

            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;

            row[x] = (p & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

static void ApplyCrtScanlines(uint8_t *dst, size_t pitch, int w, int h, int mode)
{
    (void)mode;
    ApplyCrtFilter(dst, pitch, w, h, s_crt_preset, s_crt_density, s_crt_opacity,
                   s_crt_thickness, s_crt_mask, s_crt_bloom, s_crt_vignette);
}



/* ── General Bloom Filter System ──────────────────────────────────────────── */

static const char *const kBloomPresetNames[BLOOM_PRESET_COUNT] = {
    "Off",
    "Subtle Glow",
    "Soft Focus",
    "Golden Sunlight",
    "Mystic Cyan",
    "Neon Radiance",
    "Intense Ethereal",
    "Custom",
    "Default Glow"
};

static const char *const kBloomIntensityNames[BLOOM_INTENSITY_COUNT] = {
    "Off (0%)",
    "25% (Subtle)",
    "50% (Mild)",
    "75% (Warm)",
    "100% (Normal)",
    "130% (Strong)",
    "170% (Intense)"
};

static const char *const kBloomThreshNames[BLOOM_THRESH_COUNT] = {
    "210 (Highlights Only)",
    "160 (Bright Elements)",
    "120 (Mid-Bright Tones)",
    "80 (Broad Luminous Wash)"
};

static const char *const kBloomRadiusNames[BLOOM_RADIUS_COUNT] = {
    "Narrow (1px Spread)",
    "Medium (2px Spread)",
    "Wide (3px Spread)",
    "Broad (4px Spread)"
};

static const char *const kBloomBlendNames[BLOOM_BLEND_COUNT] = {
    "Screen (Soft Glow)",
    "Additive (Neon Punch)",
    "Soft Light (Ethereal)"
};

static const char *const kBloomTintNames[BLOOM_TINT_COUNT] = {
    "Natural White",
    "Golden Sunlight",
    "Mystic Cyan Aura",
    "Twilight Rose"
};

static const int kBloomIntensityFactors[BLOOM_INTENSITY_COUNT] = {
    0, 64, 128, 192, 256, 333, 435
};

static const int kBloomThresholdValues[BLOOM_THRESH_COUNT] = {
    210, 160, 120, 80
};

/*
 * Path to bloom_config.json in current directory or next to executable.
 */
static const char *GetBloomConfigFilePath(void)
{
    static char s_path[512] = {0};
    if (s_path[0] == '\0') {
        FILE *f = fopen("Configs/bloom_config.json", "r");
        if (f) {
            fclose(f);
            snprintf(s_path, sizeof(s_path), "Configs/bloom_config.json");
            return s_path;
        }
        const char *base = SDL_GetBasePath();
        if (base) {
            snprintf(s_path, sizeof(s_path), "%sConfigs/bloom_config.json", base);
            return s_path;
        }
        snprintf(s_path, sizeof(s_path), "Configs/bloom_config.json");
    }
    return s_path;
}

static void SoeSaveBloomConfig(void)
{
    soe_mkdir("Configs");
    const char *path = GetBloomConfigFilePath();
    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "{\n");
    fprintf(f, "  \"preset\": %d,\n", (int)s_bloom_preset);
    fprintf(f, "  \"intensity\": %d,\n", (int)s_bloom_intensity);
    fprintf(f, "  \"threshold\": %d,\n", (int)s_bloom_threshold);
    fprintf(f, "  \"radius\": %d,\n", (int)s_bloom_radius);
    fprintf(f, "  \"blend\": %d,\n", (int)s_bloom_blend);
    fprintf(f, "  \"tint\": %d\n", (int)s_bloom_tint);
    fprintf(f, "}\n");
    fclose(f);
}

static void SoeApplyBloomPreset(SoeBloomPreset preset)
{
    s_bloom_preset = preset;
    switch (preset) {
    case BLOOM_PRESET_OFF:
        s_bloom_intensity = BLOOM_INTENSITY_OFF;
        break;
    case BLOOM_PRESET_SUBTLE:
        s_bloom_intensity = BLOOM_INTENSITY_50;
        s_bloom_threshold = BLOOM_THRESH_BRIGHT_160;
        s_bloom_radius = BLOOM_RADIUS_MEDIUM_2PX;
        s_bloom_blend = BLOOM_BLEND_SCREEN;
        s_bloom_tint = BLOOM_TINT_NATURAL;
        break;
    case BLOOM_PRESET_SOFT_FOCUS:
        s_bloom_intensity = BLOOM_INTENSITY_75;
        s_bloom_threshold = BLOOM_THRESH_MEDIUM_120;
        s_bloom_radius = BLOOM_RADIUS_WIDE_3PX;
        s_bloom_blend = BLOOM_BLEND_SCREEN;
        s_bloom_tint = BLOOM_TINT_NATURAL;
        break;
    case BLOOM_PRESET_WARM_SUN:
        s_bloom_intensity = BLOOM_INTENSITY_100;
        s_bloom_threshold = BLOOM_THRESH_MEDIUM_120;
        s_bloom_radius = BLOOM_RADIUS_WIDE_3PX;
        s_bloom_blend = BLOOM_BLEND_SCREEN;
        s_bloom_tint = BLOOM_TINT_WARM_GOLD;
        break;
    case BLOOM_PRESET_MYSTIC_CYAN:
        s_bloom_intensity = BLOOM_INTENSITY_100;
        s_bloom_threshold = BLOOM_THRESH_MEDIUM_120;
        s_bloom_radius = BLOOM_RADIUS_WIDE_3PX;
        s_bloom_blend = BLOOM_BLEND_SCREEN;
        s_bloom_tint = BLOOM_TINT_MYSTIC_CYAN;
        break;
    case BLOOM_PRESET_NEON:
        s_bloom_intensity = BLOOM_INTENSITY_130;
        s_bloom_threshold = BLOOM_THRESH_HIGHLIGHTS_210;
        s_bloom_radius = BLOOM_RADIUS_MEDIUM_2PX;
        s_bloom_blend = BLOOM_BLEND_ADDITIVE;
        s_bloom_tint = BLOOM_TINT_NATURAL;
        break;
    case BLOOM_PRESET_INTENSE:
        s_bloom_intensity = BLOOM_INTENSITY_170;
        s_bloom_threshold = BLOOM_THRESH_WIDE_80;
        s_bloom_radius = BLOOM_RADIUS_BROAD_4PX;
        s_bloom_blend = BLOOM_BLEND_ADDITIVE;
        s_bloom_tint = BLOOM_TINT_NATURAL;
        break;
    case BLOOM_PRESET_DEFAULT:
        s_bloom_intensity = BLOOM_INTENSITY_50;
        s_bloom_threshold = BLOOM_THRESH_WIDE_80;
        s_bloom_radius = BLOOM_RADIUS_BROAD_4PX;
        s_bloom_blend = BLOOM_BLEND_ADDITIVE;
        s_bloom_tint = BLOOM_TINT_NATURAL;
        break;
    case BLOOM_PRESET_CUSTOM:
        break;
    default:
        break;
    }
}

static void SoeApplyDefaultBloomSettings(void)
{
    SoeApplyBloomPreset(BLOOM_PRESET_DEFAULT);
    s_bloom_intensity = (SoeBloomIntensity)2;
    s_bloom_threshold = BLOOM_THRESH_WIDE_80;
    s_bloom_radius = BLOOM_RADIUS_BROAD_4PX;
    s_bloom_blend = BLOOM_BLEND_ADDITIVE;
    s_bloom_tint = BLOOM_TINT_NATURAL;
}

static void SoeLoadBloomConfig(void)
{
    SoeApplyDefaultBloomSettings();

    const char *path = GetBloomConfigFilePath();
    FILE *f = fopen(path, "r");
    if (!f) {
        SoeSaveBloomConfig();
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *q1 = strchr(line, '"');
        if (!q1) continue;
        char *q2 = strchr(q1 + 1, '"');
        if (!q2) continue;
        *q2 = '\0';
        char *colon = strchr(q2 + 1, ':');
        if (!colon) continue;
        int val = atoi(colon + 1);

        if (strcmp(q1 + 1, "preset") == 0) {
            if (val >= 0 && val < BLOOM_PRESET_COUNT) SoeApplyBloomPreset((SoeBloomPreset)val);
        } else if (strcmp(q1 + 1, "intensity") == 0) {
            if (val >= 0 && val < BLOOM_INTENSITY_COUNT) s_bloom_intensity = (SoeBloomIntensity)val;
        } else if (strcmp(q1 + 1, "threshold") == 0) {
            if (val >= 0 && val < BLOOM_THRESH_COUNT) s_bloom_threshold = (SoeBloomThreshold)val;
        } else if (strcmp(q1 + 1, "radius") == 0) {
            if (val >= 0 && val < BLOOM_RADIUS_COUNT) s_bloom_radius = (SoeBloomRadius)val;
        } else if (strcmp(q1 + 1, "blend") == 0) {
            if (val >= 0 && val < BLOOM_BLEND_COUNT) s_bloom_blend = (SoeBloomBlend)val;
        } else if (strcmp(q1 + 1, "tint") == 0) {
            if (val >= 0 && val < BLOOM_TINT_COUNT) s_bloom_tint = (SoeBloomTint)val;
        }
    }
    fclose(f);
}

/*
 * High-performance 2-pass separable Bloom Filter:
 * Extracts bright highlights above threshold with soft knee, blurs horizontally and vertically,
 * applies optional atmosphere tinting, and composites with Screen, Additive, or Soft Light.
 */
static void ApplyBloomFilter(uint8_t *dst, size_t pitch, int w, int h,
                            SoeBloomPreset preset,
                            SoeBloomIntensity intensity_idx,
                            SoeBloomThreshold thresh_idx,
                            SoeBloomRadius radius_idx,
                            SoeBloomBlend blend_idx,
                            SoeBloomTint tint_idx)
{
    if (!dst || preset == BLOOM_PRESET_OFF || intensity_idx == BLOOM_INTENSITY_OFF) return;

    int intensity = kBloomIntensityFactors[intensity_idx % BLOOM_INTENSITY_COUNT];
    int threshold = kBloomThresholdValues[thresh_idx % BLOOM_THRESH_COUNT];
    int radius = (int)radius_idx + 1;
    int blend = (int)blend_idx;
    int tint = (int)tint_idx;

    static uint32_t *s_bright_buf = NULL;
    static uint32_t *s_temp_buf = NULL;
    static int s_buf_capacity = 0;

    int total_pixels = w * h;
    if (total_pixels > s_buf_capacity) {
        free(s_bright_buf);
        free(s_temp_buf);
        s_bright_buf = (uint32_t *)malloc((size_t)total_pixels * sizeof(uint32_t));
        s_temp_buf = (uint32_t *)malloc((size_t)total_pixels * sizeof(uint32_t));
        s_buf_capacity = total_pixels;
    }
    if (!s_bright_buf || !s_temp_buf) return;

    int stride = (int)(pitch / 4);
    uint32_t *src_pixels = (uint32_t *)dst;

    /* Precompute soft-knee scale LUT for luma 0..255 */
    int32_t scale_lut[256];
    for (int l = 0; l < 256; l++) {
        if (l > threshold) {
            int excess = l - threshold;
            int num = excess * excess;
            int den = (255 - threshold) * l;
            if (den < 1) den = 1;
            int scale = (num * 256) / den;
            if (scale > 256) scale = 256;
            scale_lut[l] = scale;
        } else {
            scale_lut[l] = 0;
        }
    }

    int tint_r = 256, tint_g = 256, tint_b = 256;
    if (tint == BLOOM_TINT_WARM_GOLD) {
        tint_r = 280; tint_g = 230; tint_b = 150;
    } else if (tint == BLOOM_TINT_MYSTIC_CYAN) {
        tint_r = 150; tint_g = 240; tint_b = 280;
    } else if (tint == BLOOM_TINT_ROSE) {
        tint_r = 270; tint_g = 160; tint_b = 240;
    }

    /* 1. Threshold Bright-Pass Extraction with soft-knee */
    for (int y = 0; y < h; y++) {
        const uint32_t *src_row = src_pixels + y * stride;
        uint32_t *bright_row = s_bright_buf + y * w;
        int x = 0;

#if defined(__AVX2__)
        const __m256i v_ff = _mm256_set1_epi32(0xFF);
        const __m256i v_thresh = _mm256_set1_epi32(threshold);
        const __m256i v_tint_r = _mm256_set1_epi32(tint_r);
        const __m256i v_tint_g = _mm256_set1_epi32(tint_g);
        const __m256i v_tint_b = _mm256_set1_epi32(tint_b);

        for (; x + 8 <= w; x += 8) {
            __m256i p = _mm256_loadu_si256((const __m256i *)(src_row + x));
            __m256i r = _mm256_and_si256(_mm256_srli_epi32(p, 16), v_ff);
            __m256i g = _mm256_and_si256(_mm256_srli_epi32(p, 8), v_ff);
            __m256i b = _mm256_and_si256(p, v_ff);

            __m256i luma = _mm256_srli_epi32(
                _mm256_add_epi32(
                    _mm256_add_epi32(_mm256_mullo_epi32(r, _mm256_set1_epi32(54)),
                                     _mm256_mullo_epi32(g, _mm256_set1_epi32(183))),
                    _mm256_mullo_epi32(b, _mm256_set1_epi32(19))),
                8);

            __m256i above = _mm256_cmpgt_epi32(luma, v_thresh);
            if (_mm256_testz_si256(above, above)) {
                _mm256_storeu_si256((__m256i *)(bright_row + x), _mm256_setzero_si256());
                continue;
            }

            __m256i scale = _mm256_i32gather_epi32((const int *)scale_lut, luma, 4);
            __m256i br = _mm256_srli_epi32(_mm256_mullo_epi32(r, scale), 8);
            __m256i bg = _mm256_srli_epi32(_mm256_mullo_epi32(g, scale), 8);
            __m256i bb = _mm256_srli_epi32(_mm256_mullo_epi32(b, scale), 8);

            if (tint != BLOOM_TINT_NATURAL) {
                br = _mm256_srli_epi32(_mm256_mullo_epi32(br, v_tint_r), 8);
                bg = _mm256_srli_epi32(_mm256_mullo_epi32(bg, v_tint_g), 8);
                bb = _mm256_srli_epi32(_mm256_mullo_epi32(bb, v_tint_b), 8);
            }

            br = _mm256_min_epi32(br, v_ff);
            bg = _mm256_min_epi32(bg, v_ff);
            bb = _mm256_min_epi32(bb, v_ff);

            __m256i out = _mm256_or_si256(_mm256_or_si256(_mm256_slli_epi32(br, 16), _mm256_slli_epi32(bg, 8)), bb);
            out = _mm256_and_si256(out, above);
            _mm256_storeu_si256((__m256i *)(bright_row + x), out);
        }
#endif

        for (; x < w; x++) {
            uint32_t p = src_row[x];
            int r = (p >> 16) & 0xFF;
            int g = (p >> 8) & 0xFF;
            int b = p & 0xFF;

            int luma = (54 * r + 183 * g + 19 * b) >> 8;

            if (luma > threshold) {
                int scale = scale_lut[luma];

                int br = (r * scale) >> 8;
                int bg = (g * scale) >> 8;
                int bb = (b * scale) >> 8;

                if (tint != BLOOM_TINT_NATURAL) {
                    br = (br * tint_r) >> 8;
                    bg = (bg * tint_g) >> 8;
                    bb = (bb * tint_b) >> 8;
                }
                if (br > 255) br = 255;
                if (bg > 255) bg = 255;
                if (bb > 255) bb = 255;

                bright_row[x] = ((uint32_t)br << 16) | ((uint32_t)bg << 8) | (uint32_t)bb;
            } else {
                bright_row[x] = 0;
            }
        }
    }

    /* 2. Horizontal Blur Pass */
    int div = 2 * radius + 1;
#if defined(__AVX2__)
    const uint16_t inv_div_u16 = (uint16_t)((65536 + div - 1) / div);
    const __m256i v_inv_div = _mm256_set1_epi16((short)inv_div_u16);
    const __m256i v_zero = _mm256_setzero_si256();
#endif

    for (int y = 0; y < h; y++) {
        const uint32_t *in_row = s_bright_buf + y * w;
        uint32_t *out_row = s_temp_buf + y * w;

#if defined(__AVX2__)
        if (w >= 2 * radius + 8) {
            /* Left border [0 .. radius - 1] scalar */
            for (int x = 0; x < radius; x++) {
                int sum_r = 0, sum_g = 0, sum_b = 0;
                for (int k = -radius; k <= radius; k++) {
                    int cx = (x + k < 0) ? 0 : (x + k);
                    uint32_t p = in_row[cx];
                    sum_r += (p >> 16) & 0xFF;
                    sum_g += (p >> 8) & 0xFF;
                    sum_b += p & 0xFF;
                }
                out_row[x] = ((uint32_t)(sum_r / div) << 16) |
                             ((uint32_t)(sum_g / div) << 8) |
                             (uint32_t)(sum_b / div);
            }

            /* Interior 8-pixel-wide AVX2 loop */
            int x = radius;
            for (; x + 8 <= w - radius; x += 8) {
                __m256i sum_lo = v_zero;
                __m256i sum_hi = v_zero;
                for (int k = -radius; k <= radius; k++) {
                    __m256i pk = _mm256_loadu_si256((const __m256i *)(in_row + x + k));
                    sum_lo = _mm256_add_epi16(sum_lo, _mm256_unpacklo_epi8(pk, v_zero));
                    sum_hi = _mm256_add_epi16(sum_hi, _mm256_unpackhi_epi8(pk, v_zero));
                }
                __m256i avg_lo = _mm256_mulhi_epu16(sum_lo, v_inv_div);
                __m256i avg_hi = _mm256_mulhi_epu16(sum_hi, v_inv_div);
                __m256i out = _mm256_packus_epi16(avg_lo, avg_hi);
                _mm256_storeu_si256((__m256i *)(out_row + x), out);
            }

            /* Right tail & border scalar */
            for (; x < w; x++) {
                int sum_r = 0, sum_g = 0, sum_b = 0;
                for (int k = -radius; k <= radius; k++) {
                    int cx = (x + k >= w) ? (w - 1) : (x + k);
                    uint32_t p = in_row[cx];
                    sum_r += (p >> 16) & 0xFF;
                    sum_g += (p >> 8) & 0xFF;
                    sum_b += p & 0xFF;
                }
                out_row[x] = ((uint32_t)(sum_r / div) << 16) |
                             ((uint32_t)(sum_g / div) << 8) |
                             (uint32_t)(sum_b / div);
            }
            continue;
        }
#endif

        int sum_r = 0, sum_g = 0, sum_b = 0;
        for (int k = -radius; k <= radius; k++) {
            int cx = (k < 0) ? 0 : ((k >= w) ? (w - 1) : k);
            uint32_t p = in_row[cx];
            sum_r += (p >> 16) & 0xFF;
            sum_g += (p >> 8) & 0xFF;
            sum_b += p & 0xFF;
        }

        for (int x = 0; x < w; x++) {
            out_row[x] = ((uint32_t)(sum_r / div) << 16) |
                         ((uint32_t)(sum_g / div) << 8) |
                         (uint32_t)(sum_b / div);

            int left_idx = (x - radius < 0) ? 0 : (x - radius);
            int right_idx = (x + radius + 1 >= w) ? (w - 1) : (x + radius + 1);
            uint32_t p_left = in_row[left_idx];
            uint32_t p_right = in_row[right_idx];

            sum_r += ((p_right >> 16) & 0xFF) - ((p_left >> 16) & 0xFF);
            sum_g += ((p_right >> 8) & 0xFF) - ((p_left >> 8) & 0xFF);
            sum_b += (p_right & 0xFF) - (p_left & 0xFF);
        }
    }

    /* 3. Vertical Blur Pass & Composite (Row-Major for Cache Locality & AVX2) */
#if defined(__AVX2__)
    const __m256i v_intensity = _mm256_set1_epi16((short)intensity);
    const __m256i v_255_16 = _mm256_set1_epi16(255);
    const __m256i v_rgb_mask = _mm256_set1_epi32(0x00FFFFFF);
    const __m256i v_alpha_mask = _mm256_set1_epi32((int)0xFF000000u);
#endif
    const uint32_t *row_ptrs[9];

    for (int y = 0; y < h; y++) {
        for (int k = -radius; k <= radius; k++) {
            int cy = (y + k < 0) ? 0 : ((y + k >= h) ? (h - 1) : (y + k));
            row_ptrs[k + radius] = s_temp_buf + cy * w;
        }
        uint32_t *dst_row = src_pixels + y * stride;
        int x = 0;

#if defined(__AVX2__)
        for (; x + 8 <= w; x += 8) {
            __m256i sum_lo = v_zero;
            __m256i sum_hi = v_zero;
            for (int k = 0; k < div; k++) {
                __m256i pk = _mm256_loadu_si256((const __m256i *)(row_ptrs[k] + x));
                sum_lo = _mm256_add_epi16(sum_lo, _mm256_unpacklo_epi8(pk, v_zero));
                sum_hi = _mm256_add_epi16(sum_hi, _mm256_unpackhi_epi8(pk, v_zero));
            }

            if (blend != BLOOM_BLEND_SCREEN) {
                __m256i any_sum = _mm256_or_si256(sum_lo, sum_hi);
                if (_mm256_testz_si256(any_sum, any_sum)) {
                    continue;
                }
            }

            __m256i avg_lo = _mm256_mulhi_epu16(sum_lo, v_inv_div);
            __m256i avg_hi = _mm256_mulhi_epu16(sum_hi, v_inv_div);
            __m256i bl_lo = _mm256_srli_epi16(_mm256_mullo_epi16(avg_lo, v_intensity), 8);
            __m256i bl_hi = _mm256_srli_epi16(_mm256_mullo_epi16(avg_hi, v_intensity), 8);

            __m256i orig = _mm256_loadu_si256((const __m256i *)(dst_row + x));
            __m256i final_rgb;

            if (blend == BLOOM_BLEND_SCREEN) {
                bl_lo = _mm256_min_epi16(bl_lo, v_255_16);
                bl_hi = _mm256_min_epi16(bl_hi, v_255_16);
                __m256i or_lo = _mm256_unpacklo_epi8(orig, v_zero);
                __m256i or_hi = _mm256_unpackhi_epi8(orig, v_zero);
                __m256i inv_or_lo = _mm256_sub_epi16(v_255_16, or_lo);
                __m256i inv_or_hi = _mm256_sub_epi16(v_255_16, or_hi);
                __m256i inv_bl_lo = _mm256_sub_epi16(v_255_16, bl_lo);
                __m256i inv_bl_hi = _mm256_sub_epi16(v_255_16, bl_hi);
                __m256i prod_lo = _mm256_srli_epi16(_mm256_mullo_epi16(inv_or_lo, inv_bl_lo), 8);
                __m256i prod_hi = _mm256_srli_epi16(_mm256_mullo_epi16(inv_or_hi, inv_bl_hi), 8);
                __m256i res_lo = _mm256_sub_epi16(v_255_16, prod_lo);
                __m256i res_hi = _mm256_sub_epi16(v_255_16, prod_hi);
                final_rgb = _mm256_packus_epi16(res_lo, res_hi);
            } else if (blend == BLOOM_BLEND_SOFT_LIGHT) {
                __m256i or_lo = _mm256_unpacklo_epi8(orig, v_zero);
                __m256i or_hi = _mm256_unpackhi_epi8(orig, v_zero);
                __m256i d_lo = _mm256_srli_epi16(_mm256_mullo_epi16(bl_lo, or_lo), 8);
                __m256i d_hi = _mm256_srli_epi16(_mm256_mullo_epi16(bl_hi, or_hi), 8);
                __m256i d_8 = _mm256_packus_epi16(d_lo, d_hi);
                final_rgb = _mm256_adds_epu8(orig, d_8);
            } else {
                __m256i bl_8 = _mm256_packus_epi16(bl_lo, bl_hi);
                final_rgb = _mm256_adds_epu8(orig, bl_8);
            }

            __m256i out = _mm256_or_si256(_mm256_and_si256(orig, v_alpha_mask),
                                          _mm256_and_si256(final_rgb, v_rgb_mask));
            _mm256_storeu_si256((__m256i *)(dst_row + x), out);
        }
#endif

        for (; x < w; x++) {
            int sum_r = 0, sum_g = 0, sum_b = 0;
            for (int k = 0; k < div; k++) {
                uint32_t p = row_ptrs[k][x];
                sum_r += (p >> 16) & 0xFF;
                sum_g += (p >> 8) & 0xFF;
                sum_b += p & 0xFF;
            }

            int bl_r = (sum_r / div * intensity) >> 8;
            int bl_g = (sum_g / div * intensity) >> 8;
            int bl_b = (sum_b / div * intensity) >> 8;

            uint32_t orig = dst_row[x];
            int or = (orig >> 16) & 0xFF;
            int og = (orig >> 8) & 0xFF;
            int ob = orig & 0xFF;

            int final_r, final_g, final_b;

            if (blend == BLOOM_BLEND_SCREEN) {
                if (bl_r > 255) bl_r = 255;
                if (bl_g > 255) bl_g = 255;
                if (bl_b > 255) bl_b = 255;
                final_r = 255 - (((255 - or) * (255 - bl_r)) >> 8);
                final_g = 255 - (((255 - og) * (255 - bl_g)) >> 8);
                final_b = 255 - (((255 - ob) * (255 - bl_b)) >> 8);
            } else if (blend == BLOOM_BLEND_SOFT_LIGHT) {
                int d_r = (bl_r * or) >> 8;
                int d_g = (bl_g * og) >> 8;
                int d_b = (bl_b * ob) >> 8;
                final_r = or + d_r;
                final_g = og + d_g;
                final_b = ob + d_b;
            } else {
                final_r = or + bl_r;
                final_g = og + bl_g;
                final_b = ob + bl_b;
            }

            if (final_r > 255) final_r = 255;
            if (final_g > 255) final_g = 255;
            if (final_b > 255) final_b = 255;

            dst_row[x] = (orig & 0xFF000000u) | ((uint32_t)final_r << 16) | ((uint32_t)final_g << 8) | (uint32_t)final_b;
        }
    }
}



/* ── Programmatic Glass Dialog Box & Window Prefs System ──────────────────── */

typedef struct {
    const char *name;
    int top_r, top_g, top_b;
    int bot_r, bot_g, bot_b;
    int rim_top_r, rim_top_g, rim_top_b;
    int rim_side_r, rim_side_g, rim_side_b;
    int sparkle_r, sparkle_g, sparkle_b; /* out of 10 */
    int shadow_r, shadow_g, shadow_b;
    int alpha;
} SoeGlassPalette;

static const SoeGlassPalette kGlassPalettes[DIALOG_GLASS_COUNT] = {
    /* 0: Omnitopia (Signature Royal Blue Glass - Default) */
    { "Omnitopia",  26,  76, 182,  10, 32, 98,  20, 34, 50,  12, 22, 36,   7,  9, 10,   6, 10, 24, 184 },
    /* 1: Volcano (Semi-muted Burnt Amber / Volcanic Orange) */
    { "Volcano",   148,  72,  28,  72, 30, 12,  40, 24, 14,  28, 16, 10,  10,  7,  4,  18,  8,  4, 188 },
    /* 2: Swamp (Muted Moss / Emerald Green) */
    { "Swamp",      28,  98,  56,  12, 44, 24,  18, 38, 24,  12, 26, 16,   6, 10,  7,   6, 16, 10, 186 },
    /* 3: Nobilia (Muted Desert Sandstone Bronze-Gold) */
    { "Nobilia",   128,  92,  32,  58, 38, 12,  36, 28, 14,  24, 18, 10,  10,  8,  5,  16, 12,  6, 188 },
    /* 4: Ebon Keep (Moody Gothic Amethyst Purple) */
    { "Ebon Keep",  88,  42, 134,  36, 16, 62,  30, 18, 46,  20, 12, 32,   8,  6, 10,  12,  6, 20, 186 },
    /* 5: Crustacea (Deep Ocean Teal) */
    { "Crustacea",  20, 102, 126,   8, 44, 60,  16, 36, 44,  10, 24, 30,   5,  9, 10,   6, 14, 20, 186 },
    /* 6: Colosseum (Semi-muted Arena Crimson) */
    { "Colosseum", 124,  32,  46,  54, 12, 20,  40, 16, 20,  26, 10, 14,  10,  6,  7,  18,  6,  8, 188 },
    /* 7: Ivor Tower (Cool Silver-Slate Obsidian) */
    { "Ivor Tower", 68,  84, 114,  28, 36, 56,  28, 34, 46,  18, 22, 30,   8,  9, 10,   8, 10, 18, 188 }
};

static const char *const kDialogThemeNames[DIALOG_THEME_COUNT] = {
    "Omnitopia (Blue)",
    "Volcano (Orange)",
    "Swamp (Green)",
    "Nobilia (Gold)",
    "Ebon Keep (Purple)",
    "Crustacea (Teal)",
    "Colosseum (Red)",
    "Ivor Tower (Slate)",
    "Original (SNES)"
};

typedef struct {
    const char *name;
    uint16_t rom_off;
    SoeDialogTheme theme;
} SoeWinPrefsEntry;

/* In-game Window Prefs sequence: starts on original Checkerboard, followed immediately by Glass themes */
static const SoeWinPrefsEntry kWinPrefsEntries[] = {
    { "Checkerboard",  0x1944, DIALOG_THEME_ORIGINAL   },
    { "Omnitopia",     0x1994, DIALOG_THEME_OMNITOPIA  },
    { "Volcano",       0x1994, DIALOG_THEME_VOLCANO    },
    { "Swamp",         0x1994, DIALOG_THEME_SWAMP      },
    { "Nobilia",       0x1994, DIALOG_THEME_NOBILIA    },
    { "Ebon Keep",     0x1994, DIALOG_THEME_EBON_KEEP  },
    { "Crustacea",     0x1994, DIALOG_THEME_CRUSTACEA  },
    { "Colosseum",     0x1994, DIALOG_THEME_COLOSSEUM  },
    { "Ivor Tower",    0x1994, DIALOG_THEME_IVOR_TOWER },
    { "Fine Stripes",  0x1934, DIALOG_THEME_ORIGINAL   },
    { "2x2 Grid",      0x1954, DIALOG_THEME_ORIGINAL   },
    { "Starfield",     0x1964, DIALOG_THEME_ORIGINAL   },
    { "Diamonds",      0x1974, DIALOG_THEME_ORIGINAL   },
    { "Diagonal",      0x1984, DIALOG_THEME_ORIGINAL   },
    { "Solid Classic", 0x1994, DIALOG_THEME_ORIGINAL   },
    { "Pinstripe",     0x19A4, DIALOG_THEME_ORIGINAL   },
    { "Clear",         0x1924, DIALOG_THEME_ORIGINAL   }
};
#define WINPREFS_ENTRY_COUNT ((int)(sizeof(kWinPrefsEntries) / sizeof(kWinPrefsEntries[0])))


static const char *GetDialogConfigFilePath(void)
{
    static char s_path[512] = {0};
    if (s_path[0] == '\0') {
        FILE *f = fopen("Configs/dialog_config.json", "r");
        if (f) {
            fclose(f);
            snprintf(s_path, sizeof(s_path), "Configs/dialog_config.json");
            return s_path;
        }
        const char *base = SDL_GetBasePath();
        if (base) {
            snprintf(s_path, sizeof(s_path), "%sConfigs/dialog_config.json", base);
            return s_path;
        }
        snprintf(s_path, sizeof(s_path), "Configs/dialog_config.json");
    }
    return s_path;
}

static void SoeSaveDialogConfig(void)
{
    soe_mkdir("Configs");
    const char *path = GetDialogConfigFilePath();
    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "{\n");
    fprintf(f, "  \"theme\": %d\n", (int)s_dialog_theme);
    fprintf(f, "}\n");
    fclose(f);
}

static void SoeLoadDialogConfig(void)
{
    s_dialog_theme = DIALOG_THEME_OMNITOPIA;
    s_solid_dialog_bg = true;

    const char *path = GetDialogConfigFilePath();
    FILE *f = fopen(path, "r");
    if (!f) {
        SoeSaveDialogConfig();
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *q1 = strchr(line, '"');
        if (!q1) continue;
        char *q2 = strchr(q1 + 1, '"');
        if (!q2) continue;
        *q2 = '\0';
        char *colon = strchr(q2 + 1, ':');
        if (!colon) continue;
        int val = atoi(colon + 1);

        if (strcmp(q1 + 1, "theme") == 0) {
            if (val >= 0 && val < DIALOG_THEME_COUNT) {
                s_dialog_theme = (SoeDialogTheme)val;
            }
        }
    }
    fclose(f);
    s_solid_dialog_bg = (s_dialog_theme != DIALOG_THEME_ORIGINAL);
}

static const char *GetVideoConfigFilePath(void)
{
    static char s_path[512] = {0};
    if (s_path[0] == '\0') {
        FILE *f = fopen("Configs/video_config.json", "r");
        if (f) {
            fclose(f);
            snprintf(s_path, sizeof(s_path), "Configs/video_config.json");
            return s_path;
        }
        const char *base = SDL_GetBasePath();
        if (base) {
            snprintf(s_path, sizeof(s_path), "%sConfigs/video_config.json", base);
            return s_path;
        }
        snprintf(s_path, sizeof(s_path), "Configs/video_config.json");
    }
    return s_path;
}

static void SoeSaveVideoConfig(void)
{
    soe_mkdir("Configs");
    const char *path = GetVideoConfigFilePath();
    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "{\n");
    fprintf(f, "  \"ws_mode\": %d,\n", (int)s_ws_mode);
    fprintf(f, "  \"hud_mode\": %d,\n", (int)s_hud_mode);
    fprintf(f, "  \"pillarbox\": %d,\n", s_pillarbox_enabled ? 1 : 0);
    fprintf(f, "  \"lock_60fps\": %d,\n", s_lock_60fps ? 1 : 0);
    fprintf(f, "  \"interpolate_sprites\": %d,\n", s_interpolate_sprites ? 1 : 0);
    fprintf(f, "  \"enhanced_ring_menu\": %d,\n", s_enhanced_ring_menu ? 1 : 0);
    fprintf(f, "  \"eliminate_lag_frames\": %d,\n", s_eliminate_lag_frames ? 1 : 0);
#ifdef __ANDROID__
    fprintf(f, "  \"touch_overlay\": %d,\n", s_android_touch_overlay ? 1 : 0);
#endif
    fprintf(f, "  \"pillarbox_edit_mode\": %d\n", 0);
    fprintf(f, "}\n");
    fclose(f);
}

static void SoeLoadVideoConfig(void)
{
    s_ws_mode = g_config.widescreen ? SOE_WS_21_9 : SOE_WS_OFF;
    s_hud_mode = SOE_HUD_MOVE_LEFT;
    s_pillarbox_enabled = true;
    s_lock_60fps = false;
    s_interpolate_sprites = false;
    s_enhanced_ring_menu = true;
    s_eliminate_lag_frames = true;
#ifdef __ANDROID__
    s_android_touch_overlay = true;
    SoeAndroidTouchSetEnabled(true);
#endif
    snes_beam_frame_driver_set_fast_cpu(true);

    const char *path = GetVideoConfigFilePath();
    FILE *f = fopen(path, "r");
    if (!f) {
        g_config.disable_frame_delay = false;
        SoeSaveVideoConfig();
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *q1 = strchr(line, '"');
        if (!q1) continue;
        char *q2 = strchr(q1 + 1, '"');
        if (!q2) continue;
        *q2 = '\0';
        char *colon = strchr(q2 + 1, ':');
        if (!colon) continue;
        int val = atoi(colon + 1);

        if (strcmp(q1 + 1, "ws_mode") == 0) {
            if (val >= 0 && val < SOE_WS_COUNT) {
                s_ws_mode = (SoeWidescreenMode)val;
            }
        } else if (strcmp(q1 + 1, "hud_mode") == 0) {
            if (val >= 0 && val < SOE_HUD_COUNT) {
                s_hud_mode = (SoeHudPositionMode)val;
            }
        } else if (strcmp(q1 + 1, "pillarbox") == 0) {
            s_pillarbox_enabled = (val != 0);
        } else if (strcmp(q1 + 1, "lock_60fps") == 0) {
            s_lock_60fps = (val != 0);
        } else if (strcmp(q1 + 1, "interpolate_sprites") == 0) {
            s_interpolate_sprites = (val != 0);
        } else if (strcmp(q1 + 1, "enhanced_ring_menu") == 0) {
            s_enhanced_ring_menu = (val != 0);
        } else if (strcmp(q1 + 1, "eliminate_lag_frames") == 0) {
            s_eliminate_lag_frames = (val != 0);
#ifdef __ANDROID__
        } else if (strcmp(q1 + 1, "touch_overlay") == 0) {
            s_android_touch_overlay = (val != 0);
#endif
        } else if (strcmp(q1 + 1, "pillarbox_edit_mode") == 0) {
            (void)val; /* legacy key, ignored */
        }
    }
    fclose(f);

#ifdef __ANDROID__
    SoeAndroidTouchSetEnabled(s_android_touch_overlay);
#endif
    snes_beam_frame_driver_set_fast_cpu(s_eliminate_lag_frames);
    g_config.disable_frame_delay = false;

    /* Keep launcher/config.ini Widescreen flag authoritative if toggled externally */
    if (!g_config.widescreen) {
        s_ws_mode = SOE_WS_OFF;
    } else if (s_ws_mode == SOE_WS_OFF) {
        s_ws_mode = SOE_WS_21_9;
    }
}

static void SoeSaveDebugConfig(void)
{
    soe_mkdir("Configs");
    FILE *f = fopen("Configs/soe_debug.cfg", "w");
    if (!f) return;
    fprintf(f, "{\n");
    fprintf(f, "  \"inf_hp\": %d,\n", s_inf_hp_active ? 1 : 0);
    fprintf(f, "  \"inf_stamina\": %d,\n", s_inf_stamina_active ? 1 : 0);
    fprintf(f, "  \"one_hit_ko\": %d,\n", s_one_hit_ko_active ? 1 : 0);
    fprintf(f, "  \"walk_walls\": %d,\n", s_walk_walls_active ? 1 : 0);
    fprintf(f, "  \"fix_silver_sheath\": %d,\n", s_fix_silver_sheath ? 1 : 0);
    fprintf(f, "  \"fix_atlas_underflow\": %d,\n", s_fix_atlas_underflow ? 1 : 0);
    fprintf(f, "  \"fix_dog_leveling\": %d,\n", s_fix_dog_leveling ? 1 : 0);
    fprintf(f, "  \"fix_softlocks\": %d,\n", s_fix_softlocks ? 1 : 0);
    fprintf(f, "  \"jukebox_keep_song\": %d\n", s_jukebox_lock_song ? 1 : 0);
    fprintf(f, "}\n");
    fclose(f);
}

static void SoeLoadDebugConfig(void)
{
    FILE *f = fopen("Configs/soe_debug.cfg", "r");
    if (!f) {
        SoeSaveDebugConfig();
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int val = 0;
        if (sscanf(line, " \"inf_hp\": %d", &val) == 1) s_inf_hp_active = (val != 0);
        else if (sscanf(line, " \"inf_stamina\": %d", &val) == 1) s_inf_stamina_active = (val != 0);
        else if (sscanf(line, " \"one_hit_ko\": %d", &val) == 1) s_one_hit_ko_active = (val != 0);
        else if (sscanf(line, " \"walk_walls\": %d", &val) == 1) s_walk_walls_active = (val != 0);
        else if (sscanf(line, " \"fix_silver_sheath\": %d", &val) == 1) s_fix_silver_sheath = (val != 0);
        else if (sscanf(line, " \"fix_atlas_underflow\": %d", &val) == 1) s_fix_atlas_underflow = (val != 0);
        else if (sscanf(line, " \"fix_dog_leveling\": %d", &val) == 1) s_fix_dog_leveling = (val != 0);
        else if (sscanf(line, " \"fix_softlocks\": %d", &val) == 1) s_fix_softlocks = (val != 0);
        else if (sscanf(line, " \"jukebox_keep_song\": %d", &val) == 1) s_jukebox_lock_song = (val != 0);
    }
    fclose(f);
}

static void SoeApplyWidescreenMode(SoeWidescreenMode mode)
{
    if (mode < 0 || mode >= SOE_WS_COUNT) mode = SOE_WS_16_9;
    s_ws_mode = mode;
    g_config.widescreen = (mode != SOE_WS_OFF);
    g_config_dirty = true;
    snesrecomp_desktop_set_widescreen(g_config.widescreen ? 1 : 0);
    SoeSaveVideoConfig();
    snesrecomp_desktop_request_clock_reset();
}

typedef struct {
    int ix0, iy0, ix1, iy1; /* Interior fill bounding box */
    int bx0, by0, bx1, by1; /* Outer ROM border frame bounding box */
} SoeDialogBoxRect;

static inline uint32_t BlendRgbPixel(uint32_t bg, int fr, int fg, int fb, int alpha256)
{
    int br = (bg >> 16) & 0xFF;
    int bg_c = (bg >> 8) & 0xFF;
    int bb = bg & 0xFF;
    int inv = 256 - alpha256;
    int r = (fr * alpha256 + br * inv) >> 8;
    int g = (fg * alpha256 + bg_c * inv) >> 8;
    int b = (fb * alpha256 + bb * inv) >> 8;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (bg & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/*
 * Two-pass separable Gaussian blur (radius ~3, 7-tap [1, 6, 15, 20, 15, 6, 1] / 64)
 * applied to the background region under glass panels before compositing tint and sheen.
 */
static void BlurRegionGaussian(uint32_t *pixels, int stride, int frame_w, int frame_h,
                               int rx0, int ry0, int rx1, int ry1)
{
    if (!pixels) return;
    if (rx0 < 0) rx0 = 0;
    if (ry0 < 0) ry0 = 0;
    if (rx1 > frame_w) rx1 = frame_w;
    if (ry1 > frame_h) ry1 = frame_h;
    int rw = rx1 - rx0;
    int rh = ry1 - ry0;
    if (rw <= 2 || rh <= 2) return;

    static uint32_t *s_blur_tmp = NULL;
    static int s_blur_tmp_cap = 0;
    int need_cap = rw * rh;
    if (need_cap > s_blur_tmp_cap) {
        free(s_blur_tmp);
        s_blur_tmp = (uint32_t *)malloc((size_t)need_cap * sizeof(uint32_t));
        s_blur_tmp_cap = need_cap;
    }
    if (!s_blur_tmp) return;

    /* Pass 1: Horizontal Gaussian blur (7-tap: weights 1, 6, 15, 20, 15, 6, 1; sum = 64) */
    for (int y = ry0; y < ry1; y++) {
        const uint32_t *src_row = pixels + y * stride;
        uint32_t *tmp_row = s_blur_tmp + (y - ry0) * rw;

        for (int x = rx0; x < rx1; x++) {
            int acc_r = 0, acc_g = 0, acc_b = 0;

            for (int k = -3; k <= 3; k++) {
                int sx = x + k;
                if (sx < 0) sx = 0;
                else if (sx >= frame_w) sx = frame_w - 1;

                uint32_t p = src_row[sx];
                int weight;
                switch (k) {
                    case -3: case 3: weight = 1; break;
                    case -2: case 2: weight = 6; break;
                    case -1: case 1: weight = 15; break;
                    default:         weight = 20; break;
                }

                acc_r += ((p >> 16) & 0xFF) * weight;
                acc_g += ((p >> 8) & 0xFF) * weight;
                acc_b += (p & 0xFF) * weight;
            }

            tmp_row[x - rx0] = ((uint32_t)(acc_r >> 6) << 16) |
                               ((uint32_t)(acc_g >> 6) << 8) |
                               (uint32_t)(acc_b >> 6);
        }
    }

    /* Pass 2: Vertical Gaussian blur */
    for (int y = ry0; y < ry1; y++) {
        uint32_t *dst_row = pixels + y * stride;

        for (int x = rx0; x < rx1; x++) {
            int lx = x - rx0;
            int acc_r = 0, acc_g = 0, acc_b = 0;

            for (int k = -3; k <= 3; k++) {
                int sy = (y - ry0) + k;
                if (sy < 0) sy = 0;
                else if (sy >= rh) sy = rh - 1;

                uint32_t p = s_blur_tmp[sy * rw + lx];
                int weight;
                switch (k) {
                    case -3: case 3: weight = 1; break;
                    case -2: case 2: weight = 6; break;
                    case -1: case 1: weight = 15; break;
                    default:         weight = 20; break;
                }

                acc_r += ((p >> 16) & 0xFF) * weight;
                acc_g += ((p >> 8) & 0xFF) * weight;
                acc_b += (p & 0xFF) * weight;
            }

            dst_row[x] = (dst_row[x] & 0xFF000000u) |
                         ((uint32_t)(acc_r >> 6) << 16) |
                         ((uint32_t)(acc_g >> 6) << 8) |
                         (uint32_t)(acc_b >> 6);
        }
    }
}

static inline void AddSpecularPixel(uint32_t *p, int add_r, int add_g, int add_b)
{
    uint32_t orig = *p;
    int r = ((orig >> 16) & 0xFF) + add_r;
    int g = ((orig >> 8) & 0xFF) + add_g;
    int b = (orig & 0xFF) + add_b;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    *p = (orig & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline void DrawSparkleGlint(uint32_t *pixels, int stride, int frame_w, int frame_h,
                                    int cx, int cy, int intensity, const SoeGlassPalette *pal)
{
    if (intensity <= 0 || !pal) return;
    static const int dx[5] = { 0, -1, 1, 0, 0 };
    static const int dy[5] = { 0,  0, 0,-1, 1 };
    for (int k = 0; k < 5; k++) {
        int px = cx + dx[k];
        int py = cy + dy[k];
        if (px >= 0 && px < frame_w && py >= 0 && py < frame_h) {
            int mul = (k == 0) ? intensity : (intensity / 2);
            AddSpecularPixel(&pixels[py * stride + px],
                             (mul * pal->sparkle_r) / 10,
                             (mul * pal->sparkle_g) / 10,
                             (mul * pal->sparkle_b) / 10);
        }
    }
}

static uint16_t ReadLe16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void WriteLe16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static int ClampByte(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

typedef struct {
    int sel_r, sel_g, sel_b;
    int sel_brd_r, sel_brd_g, sel_brd_b;
    int inact_r, inact_g, inact_b;
    int inact_brd_r, inact_brd_g, inact_brd_b;
    int div_r, div_g, div_b;
    int accent_r, accent_g, accent_b;
    uint32_t subtext_col;
    uint32_t muted_col;
    uint32_t sel_text_col;
} SoeUiThemeColors;

static void GetUiThemeColorsFor(SoeDialogTheme theme, SoeUiThemeColors *out)
{
    if (!out) return;
    if (theme == DIALOG_THEME_ORIGINAL) {
        out->sel_r = 56; out->sel_g = 84; out->sel_b = 140;
        out->sel_brd_r = 210; out->sel_brd_g = 224; out->sel_brd_b = 248;
        out->inact_r = 24; out->inact_g = 34; out->inact_b = 62;
        out->inact_brd_r = 100; out->inact_brd_g = 120; out->inact_brd_b = 160;
        out->div_r = 150; out->div_g = 175; out->div_b = 215;
        out->accent_r = 175; out->accent_g = 200; out->accent_b = 240;
        out->subtext_col = 0xFFD8E4F8u;
        out->muted_col = 0xFFBCCCE8u;
        out->sel_text_col = 0xFFBCE0FFu;
        return;
    }

    int p_idx = (int)theme;
    if (p_idx < 0 || p_idx >= DIALOG_GLASS_COUNT) p_idx = 0;
    const SoeGlassPalette *pal = &kGlassPalettes[p_idx];

    out->sel_r = ClampByte((pal->top_r * 3) / 2 + 28);
    out->sel_g = ClampByte((pal->top_g * 3) / 2 + 28);
    out->sel_b = ClampByte((pal->top_b * 3) / 2 + 28);
    out->sel_brd_r = ClampByte(pal->top_r + 125);
    out->sel_brd_g = ClampByte(pal->top_g + 125);
    out->sel_brd_b = ClampByte(pal->top_b + 125);
    out->inact_r = pal->bot_r;
    out->inact_g = pal->bot_g;
    out->inact_b = pal->bot_b;
    out->inact_brd_r = ClampByte(pal->top_r + 50);
    out->inact_brd_g = ClampByte(pal->top_g + 50);
    out->inact_brd_b = ClampByte(pal->top_b + 50);
    out->div_r = ClampByte(pal->top_r + 105);
    out->div_g = ClampByte(pal->top_g + 105);
    out->div_b = ClampByte(pal->top_b + 105);
    out->accent_r = ClampByte(pal->top_r + 95);
    out->accent_g = ClampByte(pal->top_g + 95);
    out->accent_b = ClampByte(pal->top_b + 95);

    int sr = ClampByte(pal->top_r + 170);
    int sg = ClampByte(pal->top_g + 170);
    int sb = ClampByte(pal->top_b + 170);
    out->subtext_col = 0xFF000000u | ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | (uint32_t)sb;

    int mr = ClampByte(pal->top_r + 145);
    int mg = ClampByte(pal->top_g + 145);
    int mb = ClampByte(pal->top_b + 145);
    out->muted_col = 0xFF000000u | ((uint32_t)mr << 16) | ((uint32_t)mg << 8) | (uint32_t)mb;

    int hr = ClampByte(115 + (pal->top_r * 3) / 4);
    int hg = ClampByte(115 + (pal->top_g * 3) / 4);
    int hb = ClampByte(115 + (pal->top_b * 3) / 4);
    out->sel_text_col = 0xFF000000u | ((uint32_t)hr << 16) | ((uint32_t)hg << 8) | (uint32_t)hb;
}

static void GetUiThemeColors(SoeUiThemeColors *out)
{
    GetUiThemeColorsFor(s_dialog_theme, out);
}

static void DrawGlassGradientBox(uint32_t *pixels, int stride, int frame_w, int frame_h,
                                 int gx0, int gy0, int gx1, int gy1,
                                 int rim_x0, int rim_y0, int rim_x1, int rim_y1,
                                 int alpha, const SoeGlassPalette *pal,
                                 int mask_x_origin, const uint8_t skip_mask[224][256],
                                 bool round_corners, bool with_sparkle)
{
    int gw = gx1 - gx0;
    int gh = gy1 - gy0;
    if (!pixels || !pal || gw <= 2 || gh <= 2) return;

    /* Frosted glass effect: 3 cascaded Gaussian blur passes matching the slide-out menu */
    BlurRegionGaussian(pixels, stride, frame_w, frame_h, gx0, gy0, gx1, gy1);
    BlurRegionGaussian(pixels, stride, frame_w, frame_h, gx0, gy0, gx1, gy1);
    BlurRegionGaussian(pixels, stride, frame_w, frame_h, gx0, gy0, gx1, gy1);

    for (int ny = gy0; ny < gy1 && ny < frame_h; ny++) {
        if (ny < 0) continue;
        uint32_t *dst_row = pixels + ny * stride;
        int dy = ny - gy0;
        float ft = (gh > 1) ? ((float)dy / (float)(gh - 1)) : 0.0f;
        int grad_r = (int)((float)pal->top_r + ((float)pal->bot_r - (float)pal->top_r) * ft);
        int grad_g = (int)((float)pal->top_g + ((float)pal->bot_g - (float)pal->top_g) * ft);
        int grad_b = (int)((float)pal->top_b + ((float)pal->bot_b - (float)pal->top_b) * ft);

        for (int fx = gx0; fx < gx1 && fx < frame_w; fx++) {
            if (fx < 0) continue;
            int dx = fx - gx0;
            if (round_corners && (dy == 0 || dy == gh - 1) && (dx == 0 || dx == gw - 1)) continue;
            if (skip_mask) {
                int nx = fx - mask_x_origin;
                if (nx >= 0 && nx < 256 && ny < 224 && skip_mask[ny][nx]) continue;
            }

            int sheen_dist = dx + dy * 2;
            int cr = grad_r, cg = grad_g, cb = grad_b;
            if (sheen_dist < 56) {
                int s = (56 - sheen_dist) >> 2;
                cr += (s * (pal->top_r + 60)) / 180;
                cg += (s * (pal->top_g + 60)) / 180;
                cb += (s * (pal->top_b + 60)) / 180;
            }

            uint32_t px_out = BlendRgbPixel(dst_row[fx], cr, cg, cb, alpha);
            if (ny == rim_y0 && fx >= rim_x0 && fx < rim_x1) {
                AddSpecularPixel(&px_out, pal->rim_top_r, pal->rim_top_g, pal->rim_top_b);
            } else if (fx == rim_x0 && ny > rim_y0 && ny < rim_y1) {
                AddSpecularPixel(&px_out, pal->rim_side_r, pal->rim_side_g, pal->rim_side_b);
            }
            dst_row[fx] = px_out;
        }
    }

    if (with_sparkle) {
        int phase = (int)(s_frame_counter % 180u);
        int wave1 = (phase < 45) ? phase : ((phase < 90) ? (90 - phase) : 0);
        int wave2 = ((phase + 90) % 180 < 45) ? ((phase + 90) % 180)
                  : (((phase + 90) % 180 < 90) ? (90 - ((phase + 90) % 180)) : 0);
        int g1 = 35 + (wave1 * 70) / 45;
        int g2 = 30 + (wave2 * 65) / 45;

        DrawSparkleGlint(pixels, stride, frame_w, frame_h, rim_x0 + 3, rim_y0 + 2, g1, pal);
        if ((rim_x1 - rim_x0) >= 48) {
            DrawSparkleGlint(pixels, stride, frame_w, frame_h, rim_x1 - 4, rim_y0 + 2, g2, pal);
        }
    }
}

static uint16_t GetActiveClassicPatternOffset(void)
{
    uint16_t pat = ReadLe16(&g_ram[0x0B15]);
    if (pat >= 0x1924u && pat <= 0x19A4u && ((pat - 0x1924u) & 0x0Fu) == 0u) {
        return pat;
    }
    return 0x1944u; /* Checkerboard default */
}

static void DrawClassicPatternRect(uint32_t *pixels, int stride, int frame_w, int frame_h,
                                   int x0, int y0, int x1, int y1)
{
    if (!pixels || x1 <= x0 || y1 <= y0) return;
    uint16_t pat_off = GetActiveClassicPatternOffset();
    const uint8_t *tile = NULL;
    if (g_snes && g_snes->cart && g_snes->cart->rom && g_snes->cart->romSize >= 0x0419B4u) {
        tile = &g_snes->cart->rom[0x040000u + pat_off];
    }
    bool is_clear = (pat_off == 0x1924u);

    for (int ny = y0; ny < y1 && ny < frame_h; ny++) {
        if (ny < 0) continue;
        uint32_t *dst_row = pixels + ny * stride;
        int ty = (ny - y0) & 7;
        uint8_t b0 = tile ? tile[ty * 2] : 0x55u;
        uint8_t b1 = tile ? tile[ty * 2 + 1] : 0x55u;

        for (int fx = x0; fx < x1 && fx < frame_w; fx++) {
            if (fx < 0) continue;
            int bit = 7 - ((fx - x0) & 7);
            int col_idx = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
            if (col_idx != 0) {
                dst_row[fx] = BlendRgbPixel(dst_row[fx], 20, 28, 56, 224);
            } else {
                dst_row[fx] = BlendRgbPixel(dst_row[fx], 10, 14, 28, is_clear ? 170 : 148);
            }
        }
    }
}

/*
 * Draws a themed UI window backdrop (Settings Drawer or Debug Tools) matching
 * the active in-game window theme (all 8 Glass palettes or Classic SNES $7E:0B15 pattern).
 */
static void DrawThemedWindowPanelFor(SoeDialogTheme theme,
                                     uint32_t *pixels, int stride, int frame_w, int frame_h,
                                     int x, int y, int w, int h, int alpha_boost,
                                     int inset, bool draw_shadow, bool with_sparkle)
{
    int ix0 = x + inset;
    int iy0 = y + inset;
    int ix1 = x + w - inset;
    int iy1 = y + h - inset;

    if (theme == DIALOG_THEME_ORIGINAL) {
        DrawClassicPatternRect(pixels, stride, frame_w, frame_h, ix0, iy0, ix1, iy1);
        return;
    }

    int p_idx = (int)theme;
    if (p_idx < 0 || p_idx >= DIALOG_GLASS_COUNT) p_idx = 0;
    const SoeGlassPalette *pal = &kGlassPalettes[p_idx];
    int alpha = pal->alpha + alpha_boost;
    if (alpha > 234) alpha = 234;

    DrawGlassGradientBox(pixels, stride, frame_w, frame_h, ix0, iy0, ix1, iy1,
                         ix0, iy0, ix1, iy1, alpha, pal, 0, NULL, inset > 0, with_sparkle);
}

static void DrawThemedWindowPanel(uint32_t *pixels, int stride, int frame_w, int frame_h,
                                  int x, int y, int w, int h, int alpha_boost,
                                  int inset, bool draw_shadow, bool with_sparkle)
{
    DrawThemedWindowPanelFor(s_dialog_theme, pixels, stride, frame_w, frame_h,
                             x, y, w, h, alpha_boost, inset, draw_shadow, with_sparkle);
}

/*
 * Draws a Secret of Evermore ornate dialog border frame from ROM ($C4:19B4.. /
 * $8C:AC5E + palette $8C:AC74) onto a cols x rows 8x8 tile rectangle using the
 * exact $8C:B075 corner/edge symmetry layout.
 */
static void DrawRomDialogFrameForIndex(uint32_t *pixels, int stride, int frame_w, int frame_h,
                                       int x, int y, int cols, int rows, uint16_t frame_idx)
{
    if (!pixels || cols < 4 || rows < 4) return;
    if (!g_snes || !g_snes->cart || !g_snes->cart->rom || g_snes->cart->romSize < 0x0DAC90u) return;
    const uint8_t *rom = g_snes->cart->rom;

    frame_idx &= 0x1Eu;
    if (frame_idx > 20u) frame_idx = 0u;

    uint16_t tile_base = ReadLe16(&rom[0x0CAC5Eu + frame_idx]);
    uint16_t pal_off = ReadLe16(&rom[0x0CAC74u + frame_idx]);
    if ((uint32_t)tile_base + 7u * 16u > 0x10000u || (uint32_t)pal_off + 8u > 0x10000u) return;

    uint32_t pal_rgb[4] = { 0, 0, 0, 0 };
    for (int i = 1; i < 4; i++) {
        uint16_t c = ReadLe16(&rom[0x040000u + pal_off + (uint32_t)i * 2u]);
        int r = ((c & 0x1Fu) * 255) / 31;
        int g = (((c >> 5) & 0x1Fu) * 255) / 31;
        int b = (((c >> 10) & 0x1Fu) * 255) / 31;
        pal_rgb[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            bool is_top = (r == 0);
            bool is_bot = (r == rows - 1);
            bool is_left = (c == 0);
            bool is_right = (c == cols - 1);
            if (!is_top && !is_bot && !is_left && !is_right) continue;

            int tile_idx = 2;
            bool hflip = false;
            bool vflip = false;

            if (is_top || is_bot) {
                vflip = is_bot;
                if (c == 0) {
                    tile_idx = 0; hflip = false;
                } else if (c == cols - 1) {
                    tile_idx = 0; hflip = true;
                } else if (c == 1) {
                    tile_idx = 1; hflip = false;
                } else if (c == cols - 2) {
                    tile_idx = 1; hflip = true;
                } else if ((cols & 1) && c == cols / 2) {
                    tile_idx = 3; hflip = false;
                } else {
                    tile_idx = 2; hflip = (c >= cols / 2);
                }
            } else {
                hflip = is_right;
                if (r == 1) {
                    tile_idx = 4; vflip = false;
                } else if (r == rows - 2) {
                    tile_idx = 4; vflip = true;
                } else {
                    tile_idx = 5; vflip = false;
                }
            }

            const uint8_t *tptr = &rom[0x040000u + tile_base + (uint32_t)tile_idx * 16u];
            int dx0 = x + c * 8;
            int dy0 = y + r * 8;
            for (int py = 0; py < 8; py++) {
                int sy = vflip ? (7 - py) : py;
                int dy = dy0 + py;
                if (dy < 0 || dy >= frame_h) continue;
                uint8_t b0 = tptr[sy * 2];
                uint8_t b1 = tptr[sy * 2 + 1];
                uint32_t *dst_row = pixels + dy * stride;
                for (int px = 0; px < 8; px++) {
                    int sx = hflip ? px : (7 - px);
                    int col_idx = ((b0 >> sx) & 1) | (((b1 >> sx) & 1) << 1);
                    if (col_idx == 0) continue;
                    int dx = dx0 + px;
                    if (dx >= 0 && dx < frame_w) {
                        dst_row[dx] = pal_rgb[col_idx];
                    }
                }
            }
        }
    }
}

/* Draws the active in-game ornate dialog border frame ($7E:0B17). */
static void DrawRomDialogFrame(uint32_t *pixels, int stride, int frame_w, int frame_h,
                               int x, int y, int cols, int rows)
{
    uint16_t frame_idx = ReadLe16(&g_ram[0x0B17]) & 0x1Eu;
    DrawRomDialogFrameForIndex(pixels, stride, frame_w, frame_h, x, y, cols, rows, frame_idx);
}

static float GetDebugToolsSlideFactor(void)
{
    float warp_out = s_warp_slide * 2.0f;
    if (warp_out > 1.0f) warp_out = 1.0f;
    return s_dbg_slide * (1.0f - warp_out);
}

static float GetWarpToolsSlideFactor(void)
{
    float warp_in = (s_warp_slide - 0.5f) * 2.0f;
    if (warp_in < 0.0f) warp_in = 0.0f;
    if (warp_in > 1.0f) warp_in = 1.0f;
    return s_dbg_slide * warp_in;
}

static void GetDebugStackAnchorRect(int frame_w, int *out_x, int *out_y, int *out_w, int *out_h)
{
    int w = 168; /* 21 x 8px tiles */
    int h = 168; /* 21 x 8px tiles */
    /* Stack above the window: Music ID (15) + gap (2) + X/Y (15) + gap (2) + map badge (15) + gap (2) = 51px */
    int x = frame_w - w - 6 + (int)((1.0f - s_dbg_slide) * (float)(w + 12));
    int y = 3 + 51;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

static void GetDebugWindowRect(int frame_w, int *out_x, int *out_y, int *out_w, int *out_h)
{
    int w = 168; /* 21 x 8px tiles */
    int h = 168; /* 21 x 8px tiles */
    float dbg_t = GetDebugToolsSlideFactor();
    int x = frame_w - w - 6 + (int)((1.0f - dbg_t) * (float)(w + 12));
    int y = 3 + 51;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

static void GetGiveAllConfirmRect(int frame_w, int frame_h,
                                  int *out_x, int *out_y, int *out_w, int *out_h)
{
    int w = 232; /* 29 x 8px tiles */
    int h = 104; /* 13 x 8px tiles */
    int x = (frame_w - w) / 2;
    int y = (frame_h - h) / 2;
    if (x < 4) x = 4;
    if (y < 4) y = 4;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

/* Position the Jukebox window to the left of the Debug Tools window with matching frame height. */
static void GetJukeboxWindowRect(int frame_w, int *out_x, int *out_y, int *out_w, int *out_h)
{
    int dbg_x, dbg_y, dbg_w, dbg_h;
    GetDebugStackAnchorRect(frame_w, &dbg_x, &dbg_y, &dbg_w, &dbg_h);
    (void)dbg_w;
    int w = (dbg_x - 4 >= 164) ? 160 : 144; /* 20 or 18 x 8px tiles so it never overlaps Debug Tools */
    int h = dbg_h;
    int x = dbg_x - w - 4;
    if (x < 4) x = 4;
    int y = dbg_y;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

/* Position the Warp To window in Debug Tools' right-side slot while Debug Tools is slid away. */
static void GetWarpWindowRect(int frame_w, int *out_x, int *out_y, int *out_w, int *out_h)
{
    int w = (frame_w >= 400) ? 232 : ((frame_w >= 336) ? 192 : 144);
    int h = 168; /* 21 x 8px tiles */
    float warp_t = GetWarpToolsSlideFactor();
    int x = frame_w - w - 6 + (int)((1.0f - warp_t) * (float)(w + 12));
    int y = 3 + 51;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

/* Position the Warp Map Preview window to the left of Warp To using the expanded screen space. */
static bool GetWarpPreviewWindowRect(int frame_w, int wp_x, int wp_y, int wp_w, int wp_h,
                                     int *out_x, int *out_y, int *out_w, int *out_h)
{
    (void)wp_x;
    int steady_wp_x = frame_w - wp_w - 6;
    int avail_w = steady_wp_x - 8;
    if (avail_w < 48) {
        if (out_x) *out_x = 0;
        if (out_y) *out_y = 0;
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return false;
    }
    int pv_w = (avail_w / 8) * 8;
    if (pv_w > 240) pv_w = 240; /* 30 x 8px tiles max in 21:9 */
    int pv_h = 200;             /* 25 x 8px tiles: y = 22..222, clear of top-left badge & aligned at bottom */
    int pv_y = wp_y + wp_h - pv_h;
    if (pv_y < 4) pv_y = 4;
    int steady_pv_x = steady_wp_x - pv_w - 4;
    float warp_t = GetWarpToolsSlideFactor();
    int pv_x = steady_pv_x - (int)((1.0f - warp_t) * (float)(pv_w + 16));
    if (out_x) *out_x = pv_x;
    if (out_y) *out_y = pv_y;
    if (out_w) *out_w = pv_w;
    if (out_h) *out_h = pv_h;
    return true;
}

/* Probe Extract/MAPS/map_%03u.png dimensions when the selected map changes.
 * Full-resolution rendering happens at PC window resolution via SoeGetNativeOverlay. */
static void UpdateWarpMapPreview(uint16_t map_id)
{
    if ((int32_t)map_id == s_warp_preview_cached_id) {
        return;
    }

    s_warp_preview_cached_id = (int32_t)map_id;
    s_warp_preview_valid = false;
    s_warp_preview_src_w = 0;
    s_warp_preview_src_h = 0;

    snprintf(s_warp_preview_path, sizeof(s_warp_preview_path),
             "Extract/MAPS/map_%03u.png", (unsigned)map_id);
    int src_w = 0, src_h = 0, comp = 0;
    if (stbi_info(s_warp_preview_path, &src_w, &src_h, &comp) &&
        src_w > 0 && src_h > 0) {
        s_warp_preview_src_w = src_w;
        s_warp_preview_src_h = src_h;
        s_warp_preview_valid = true;
    }
}

/* Word-wrap and center short UI copy inside a bounding rectangle using DrawSmallText. */
static void DrawSmallTextWrappedCentered(uint32_t *pixels, int stride, int frame_h,
                                         int box_x, int box_y, int box_w, int box_h,
                                         const char *text, uint32_t col)
{
    if (!text || !text[0] || box_w <= 8 || box_h <= 7) return;
    char lines[12][64];
    int line_count = 0;
    const int max_line_w = box_w - 4;
    const char *p = text;
    while (*p && line_count < 12) {
        while (*p == ' ') p++;
        if (!*p) break;

        char cur[64];
        cur[0] = '\0';
        size_t cur_len = 0;
        while (*p) {
            const char *wstart = p;
            while (*p && *p != ' ') p++;
            size_t wlen = (size_t)(p - wstart);
            if (wlen > 0) {
                char cand[64];
                if (cur_len == 0) {
                    size_t n = (wlen < sizeof(cand) - 1) ? wlen : sizeof(cand) - 1;
                    memcpy(cand, wstart, n);
                    cand[n] = '\0';
                } else if (cur_len + 1 + wlen < sizeof(cand)) {
                    memcpy(cand, cur, cur_len);
                    cand[cur_len] = ' ';
                    memcpy(cand + cur_len + 1, wstart, wlen);
                    cand[cur_len + 1 + wlen] = '\0';
                } else {
                    p = wstart;
                    break;
                }
                if (cur_len == 0 || MeasureSmallText(cand) <= max_line_w) {
                    snprintf(cur, sizeof(cur), "%s", cand);
                    cur_len = strlen(cur);
                } else {
                    p = wstart;
                    break;
                }
            }
            while (*p == ' ') p++;
        }
        if (cur_len > 0) {
            snprintf(lines[line_count], sizeof(lines[line_count]), "%s", cur);
            line_count++;
        }
    }
    if (line_count == 0) return;

    const int line_h = 9;
    int total_h = line_count * line_h - 2;
    int start_y = box_y + (box_h - total_h) / 2;
    for (int i = 0; i < line_count; i++) {
        int lw = MeasureSmallText(lines[i]);
        DrawSmallText(pixels, stride, frame_h,
                      box_x + (box_w - lw) / 2, start_y + i * line_h,
                      lines[i], col);
    }
}

/* Compute exact geometry and centered label coordinates for the upper-right Map Pillarbox badge. */
static void GetMapBadgeLayout(int frame_w, uint16_t map_id, uint8_t pb_val,
                              int *out_x, int *out_y, int *out_w, int *out_h,
                              int *out_tx, int *out_ty,
                              char *out_buf, size_t out_cap)
{
    char local_buf[24];
    char *buf = (out_buf && out_cap > 0) ? out_buf : local_buf;
    size_t cap = (out_buf && out_cap > 0) ? out_cap : sizeof(local_buf);
    snprintf(buf, cap, "Map %u: [%d]", (unsigned)map_id, (int)pb_val);

    int tw = MeasureSmallText(buf);
    int w = 120;
    if (w < tw + 14) w = tw + 14;
    int h = 17;
    int x = frame_w - w - 6;
    int y = 26;
    int ty_off = 5;
    if (s_debug_overlay || s_dbg_slide > 0.0f) {
        int dx, dy, dw, dh;
        GetDebugStackAnchorRect(frame_w, &dx, &dy, &dw, &dh);
        (void)dh;
        w = dw; x = dx; h = 15; y = dy - 17; ty_off = 4;
    }

    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_tx) *out_tx = x + (w - tw) / 2;
    if (out_ty) *out_ty = y + ty_off;
}

static void DrawThemedHighlightRect(uint32_t *pixels, int stride, int frame_h,
                                    int x, int y, int w, int h,
                                    const SoeUiThemeColors *tc, bool active)
{
    if (!tc) return;
    if (active) {
        DrawSemiTransparentRect(pixels, stride, frame_h, x, y, w, h,
                                tc->sel_r, tc->sel_g, tc->sel_b, 232);
        DrawRectOutline(pixels, stride, frame_h, x, y, w, h,
                        tc->sel_brd_r, tc->sel_brd_g, tc->sel_brd_b, 220);
    } else {
        DrawSemiTransparentRect(pixels, stride, frame_h, x, y, w, h,
                                tc->inact_r, tc->inact_g, tc->inact_b, 185);
        DrawRectOutline(pixels, stride, frame_h, x, y, w, h,
                        tc->inact_brd_r, tc->inact_brd_g, tc->inact_brd_b, 165);
    }
}

/*
 * Shared Pass 1 + Pass 2 Glass Compositor (Drop Shadow + Translucent Gradient + Rim + Sparkles).
 * When skip_mask is non-NULL (224x256), skips glass pixels where skip_mask[ny][nx] != 0
 * so pre-rendered frame borders in Mode 4 Window Prefs remain untouched.
 */
static void DrawGlassDialogBoxEffects(uint32_t *pixels, int stride, int frame_w, int frame_h,
                                      int native_x0, const SoeDialogBoxRect *boxes, int box_count,
                                      SoeDialogTheme theme, const uint8_t skip_mask[224][256])
{
    if (!pixels || !boxes || box_count <= 0) return;
    int p_idx = (int)theme;
    if (p_idx < 0 || p_idx >= DIALOG_GLASS_COUNT) p_idx = 0;
    const SoeGlassPalette *pal = &kGlassPalettes[p_idx];

    /* Pass 2: Slick Translucent Glass Gradient + Specular Sheen & Subtle Sparkle */
    for (int b = 0; b < box_count; b++) {
        const SoeDialogBoxRect *r = &boxes[b];
        int gx0 = (r->bx0 + 4 < r->ix0) ? (r->bx0 + 4) : r->ix0;
        int gy0 = (r->by0 + 4 < r->iy0) ? (r->by0 + 4) : r->iy0;
        int gx1 = (r->bx1 - 4 > r->ix1) ? (r->bx1 - 4) : r->ix1;
        int gy1 = (r->by1 - 4 > r->iy1) ? (r->by1 - 4) : r->iy1;
        if (gx1 > 256) gx1 = 256;
        DrawGlassGradientBox(pixels, stride, frame_w, frame_h,
                             native_x0 + gx0, gy0, native_x0 + gx1, gy1,
                             native_x0 + r->ix0, r->iy0, native_x0 + r->ix1, r->iy1,
                             pal->alpha, pal, native_x0, skip_mask, true, true);
    }
}

/*
 * Programmatic C Dialog Box Renderer:
 * Decouples dialog box glass rendering from PPU per-scanline hacks.
 * Scans BG3 tilemap in VRAM to detect closed dialog/menu windows, draws a
 * soft drop shadow + slick translucent glass in the active theme color, and
 * composites the authentic ROM frame border tiles + original ROM VWF font on top.
 */
static void SoeRenderProgrammaticDialogs(uint32_t *pixels, int stride, int frame_w, int frame_h)
{
    if (!pixels || s_dialog_theme == DIALOG_THEME_ORIGINAL || !s_dialog_capture_active) return;
    if (!g_snes || !g_snes->ppu) return;

    Ppu *ppu = g_snes->ppu;
    if ((ppu->bgmode & 0x07) != 1 || (ppu->inidisp & 0x80) || !(ppu->screenEnabled[0] & 0x04)) {
        return;
    }

    int cap_h = s_dialog_capture_h;
    if (cap_h > frame_h) cap_h = frame_h;
    if (cap_h > 224) cap_h = 224;

    int native_x0 = (int)ppu->extraFrameLeft;
    if (native_x0 < 0) native_x0 = 0;

    static uint8_t s_bg_fill[224][256];
    static uint8_t s_bg_border[224][256];
    static uint8_t s_bg_col[224][256];
    static uint8_t s_tile_interior[28][32];
    static uint8_t s_tile_visited[28][32];
    static uint16_t s_tile_bfs_q[28 * 32];

    memset(s_bg_fill, 0, (size_t)cap_h * 256);
    memset(s_bg_border, 0, (size_t)cap_h * 256);
    memset(s_bg_col, 0, (size_t)cap_h * 256);
    memset(s_tile_interior, 0, sizeof(s_tile_interior));

    int tile_rows = (cap_h + 7) >> 3;
    if (tile_rows > 28) tile_rows = 28;
    uint8_t s_tile_fill_cnt[28][32];
    uint8_t s_tile_border_cnt[28][32];
    memset(s_tile_fill_cnt, 0, sizeof(s_tile_fill_cnt));
    memset(s_tile_border_cnt, 0, sizeof(s_tile_border_cnt));

    int map_base = PPU_bgTilemapAdr(ppu, 2);
    int tile_base = PPU_bgTileAdr(ppu, 2);
    bool wider = PPU_bgTilemapWider(ppu, 2) != 0;
    bool higher = PPU_bgTilemapHigher(ppu, 2) != 0;
    uint16_t hscr = ppu->hScroll[2];
    uint16_t vscr = ppu->vScroll[2];

    bool any_fill = false;
    bool any_fg = false;

    for (int ny = 0; ny < cap_h; ny++) {
        int sy = (ny + 1) + (int)vscr;
        int ty = (sy >> 3) & 0x1F;
        int py_base = sy & 7;
        int row_offs = map_base + (ty << 5);
        if ((sy & 0x100) && higher) {
            row_offs += wider ? 0x800 : 0x400;
        }
        const uint32_t *ov_row = &s_bg3_overlay_buf[ny * 256];
        int sty = ny >> 3;

        for (int nx = 0; nx < 256; nx++) {
            uint32_t ov_px = ov_row[nx];
            if (!ov_px) continue;

            int sx = nx + (int)hscr;
            int tx = (sx >> 3) & 0x1F;
            int addr = row_offs + tx;
            if ((sx & 0x100) && wider) {
                addr += 0x400;
            }

            uint16_t entry = ppu->vram[addr & 0x7FFF];
            int tile_num = entry & 0x3FF;
            int pal = (entry >> 10) & 7;
            int px = (entry & 0x4000) ? (7 - (sx & 7)) : (sx & 7);
            int py = (entry & 0x8000) ? (7 - py_base) : py_base;

            uint16_t tw = ppu->vram[(tile_base + tile_num * 8 + py) & 0x7FFF];
            int bit = 7 - px;
            int col_idx = ((tw >> bit) & 1) | (((tw >> (8 + bit)) & 1) << 1);
            if (col_idx == 0) continue;

            s_bg_col[ny][nx] = (uint8_t)col_idx;
            int stx = nx >> 3;
            if (pal == 0 && col_idx == 3) {
                s_bg_fill[ny][nx] = 1;
                if (s_tile_fill_cnt[sty][stx] < 255) s_tile_fill_cnt[sty][stx]++;
                any_fill = true;
            } else {
                if (pal != 0) {
                    s_bg_border[ny][nx] = 1;
                    if (s_tile_border_cnt[sty][stx] < 255) s_tile_border_cnt[sty][stx]++;
                }
                any_fg = true;
            }
        }
    }

    if (!any_fill && !any_fg) return;

    /* Mark 8x8 screen tiles belonging to a dialog/menu interior (works for ANY window pattern:
     * solid, checkerboard, vertical pinstripe, diagonal weave, and fractional scroll offsets) */
    int interior_tile_count = 0;
    for (int ty = 0; ty < tile_rows; ty++) {
        for (int tx = 0; tx < 32; tx++) {
            /* Interior tile has Palette 0 color 3 pattern pixels (allow minor 1-3px border overlap from scroll) */
            if (s_tile_fill_cnt[ty][tx] >= 4 && s_tile_border_cnt[ty][tx] <= s_tile_fill_cnt[ty][tx]) {
                s_tile_interior[ty][tx] = 1;
                interior_tile_count++;
            }
        }
    }

    /* Fast path: no dialog boxes detected -> pass through non-box BG3 pixels untouched */
    if (interior_tile_count == 0) {
        for (int ny = 0; ny < cap_h; ny++) {
            const uint32_t *ov_row = &s_bg3_overlay_buf[ny * 256];
            uint32_t *dst_row = pixels + ny * stride;
            for (int nx = 0; nx < 256; nx++) {
                int fx = native_x0 + nx;
                if (fx >= 0 && fx < frame_w && s_bg_col[ny][nx] && ov_row[nx]) {
                    dst_row[fx] = ov_row[nx];
                }
            }
        }
        return;
    }

    /* Connected-component BFS on the 32x28 tile grid to isolate distinct/nested boxes */
    SoeDialogBoxRect boxes[16];
    int box_count = 0;
    memset(s_tile_visited, 0, sizeof(s_tile_visited));

    for (int sty = 0; sty < tile_rows && box_count < 16; sty++) {
        for (int stx = 0; stx < 32 && box_count < 16; stx++) {
            if (!s_tile_interior[sty][stx] || s_tile_visited[sty][stx]) continue;

            int head = 0, tail = 0;
            s_tile_visited[sty][stx] = 1;
            s_tile_bfs_q[tail++] = (uint16_t)((sty << 5) | stx);

            int min_tx = stx, max_tx = stx;
            int min_ty = sty, max_ty = sty;

            while (head < tail) {
                uint16_t cur = s_tile_bfs_q[head++];
                int cy = cur >> 5;
                int cx = cur & 31;
                if (cx < min_tx) min_tx = cx;
                if (cx > max_tx) max_tx = cx;
                if (cy < min_ty) min_ty = cy;
                if (cy > max_ty) max_ty = cy;

                if (cx > 0 && s_tile_interior[cy][cx - 1] && !s_tile_visited[cy][cx - 1]) {
                    s_tile_visited[cy][cx - 1] = 1;
                    s_tile_bfs_q[tail++] = (uint16_t)((cy << 5) | (cx - 1));
                }
                if (cx < 31 && s_tile_interior[cy][cx + 1] && !s_tile_visited[cy][cx + 1]) {
                    s_tile_visited[cy][cx + 1] = 1;
                    s_tile_bfs_q[tail++] = (uint16_t)((cy << 5) | (cx + 1));
                }
                if (cy > 0 && s_tile_interior[cy - 1][cx] && !s_tile_visited[cy - 1][cx]) {
                    s_tile_visited[cy - 1][cx] = 1;
                    s_tile_bfs_q[tail++] = (uint16_t)(((cy - 1) << 5) | cx);
                }
                if (cy + 1 < tile_rows && s_tile_interior[cy + 1][cx] && !s_tile_visited[cy + 1][cx]) {
                    s_tile_visited[cy + 1][cx] = 1;
                    s_tile_bfs_q[tail++] = (uint16_t)(((cy + 1) << 5) | cx);
                }
            }

            int tx0 = min_tx * 8;
            int ty0 = min_ty * 8;
            int tx1 = (max_tx + 1) * 8;
            int ty1 = (max_ty + 1) * 8;
            if (ty1 > cap_h) ty1 = cap_h;

            /* Refine interior rect to exact pixel bounds of s_bg_fill (handles vScroll/hScroll offsets) */
            int ix0 = tx1, iy0 = ty1, ix1 = tx0, iy1 = ty0;
            for (int y = ty0; y < ty1; y++) {
                for (int x = tx0; x < tx1; x++) {
                    if (s_bg_fill[y][x]) {
                        if (x < ix0) ix0 = x;
                        if (x + 1 > ix1) ix1 = x + 1;
                        if (y < iy0) iy0 = y;
                        if (y + 1 > iy1) iy1 = y + 1;
                    }
                }
            }
            if ((ix1 - ix0) < 16 || (iy1 - iy0) < 8) continue;

            /* Locate exact outer ROM frame border bounds wrapping this interior */
            int bx0 = ix0, by0 = iy0, bx1 = ix1, by1 = iy1;

            for (int d = 1; d <= 8 && ix0 - d >= 0; d++) {
                bool found = false;
                for (int y = iy0; y < iy1; y++) {
                    if (s_bg_border[y][ix0 - d]) { found = true; break; }
                }
                if (found) bx0 = ix0 - d;
            }
            for (int d = 0; d < 8 && ix1 + d < 256; d++) {
                bool found = false;
                for (int y = iy0; y < iy1; y++) {
                    if (s_bg_border[y][ix1 + d]) { found = true; break; }
                }
                if (found) bx1 = ix1 + d + 1;
            }
            for (int d = 1; d <= 8 && iy0 - d >= 0; d++) {
                bool found = false;
                for (int x = ix0; x < ix1; x++) {
                    if (s_bg_border[iy0 - d][x]) { found = true; break; }
                }
                if (found) by0 = iy0 - d;
            }
            for (int d = 0; d < 8 && iy1 + d < cap_h; d++) {
                bool found = false;
                for (int x = ix0; x < ix1; x++) {
                    if (s_bg_border[iy1 + d][x]) { found = true; break; }
                }
                if (found) by1 = iy1 + d + 1;
            }

            boxes[box_count++] = (SoeDialogBoxRect){ ix0, iy0, ix1, iy1, bx0, by0, bx1, by1 };
        }
    }

    DrawGlassDialogBoxEffects(pixels, stride, frame_w, frame_h, native_x0,
                              boxes, box_count, s_dialog_theme, NULL);

    /* Pass 3: Composite Authentic ROM Frame Borders, BG3 Cursors, Original ROM VWF Font & Text Shadows */
    for (int ny = 0; ny < cap_h; ny++) {
        const uint32_t *ov_row = &s_bg3_overlay_buf[ny * 256];
        uint32_t *dst_row = pixels + ny * stride;
        for (int nx = 0; nx < 256; nx++) {
            uint32_t ov_px = ov_row[nx];
            if (!ov_px) continue;
            if (s_bg_fill[ny][nx]) {
                /* Keep isolated tiny noise outside boxes if any; suppress box fill */
                if (box_count > 0) {
                    bool in_box = false;
                    for (int b = 0; b < box_count; b++) {
                        if (nx >= boxes[b].ix0 && nx < boxes[b].ix1 &&
                            ny >= boxes[b].iy0 && ny < boxes[b].iy1) {
                            in_box = true;
                            break;
                        }
                    }
                    if (in_box) continue;
                }
            }
            if (s_bg_col[ny][nx]) {
                int fx = native_x0 + nx;
                if (fx >= 0 && fx < frame_w) dst_row[fx] = ov_px;
            }
        }
    }
}

static bool IsWindowPrefsActive(const Ppu *ppu)
{
    if (!ppu) return false;
    if ((ppu->bgmode & 0x07) != 4 || (ppu->inidisp & 0x80) || !(ppu->screenEnabled[0] & 0x02)) {
        return false;
    }
    uint16_t marker = (uint16_t)(g_ram[0x039F] | (g_ram[0x03A0] << 8));
    if (marker != 0x1924u) return false;
    int mb = PPU_bgTilemapAdr(ppu, 1);
    uint16_t center_entry = ppu->vram[(mb + (7 << 5) + 16) & 0x7FFF];
    return (center_entry & 0x3FFu) == 0x3EFu;
}

static void UploadWindowPrefsPreviewTile(Ppu *ppu, const SoeWinPrefsEntry *entry)
{
    if (!ppu || !entry) return;
    int tb = PPU_bgTileAdr(ppu, 1);
    int vram_base = tb + 0x3EF * 8;

    if (entry->theme != DIALOG_THEME_ORIGINAL) {
        /* Clear preview interior tile in VRAM so BG1 stone shows cleanly under programmatic glass */
        for (int w = 0; w < 8; w++) {
            ppu->vram[(vram_base + w) & 0x7FFF] = 0;
        }
        return;
    }

    if (g_snes && g_snes->cart && g_snes->cart->rom && g_snes->cart->romSize >= 0x0419B4u) {
        const uint8_t *src = &g_snes->cart->rom[0x040000u + entry->rom_off];
        for (int w = 0; w < 8; w++) {
            ppu->vram[(vram_base + w) & 0x7FFF] = (uint16_t)src[w * 2] | ((uint16_t)src[w * 2 + 1] << 8);
        }
    }
}

static int FindWinPrefsIndexForActiveState(void)
{
    if (s_dialog_theme != DIALOG_THEME_ORIGINAL) {
        for (int i = 0; i < WINPREFS_ENTRY_COUNT; i++) {
            if (kWinPrefsEntries[i].theme == s_dialog_theme) return i;
        }
        return 1;
    }
    uint16_t cur_0b15 = (uint16_t)(g_ram[0x0B15] | (g_ram[0x0B16] << 8));
    for (int i = 0; i < WINPREFS_ENTRY_COUNT; i++) {
        if (kWinPrefsEntries[i].theme == DIALOG_THEME_ORIGINAL &&
            kWinPrefsEntries[i].rom_off == cur_0b15) {
            return i;
        }
    }
    return 0; /* Checkerboard fallback */
}

static void SoeTickWindowPrefs(Ppu *ppu)
{
    bool now_wp = IsWindowPrefsActive(ppu);
    if (!now_wp) {
        if (s_in_winprefs) {
            s_in_winprefs = false;
        }
        return;
    }

    if (!s_in_winprefs) {
        s_in_winprefs = true;
        s_winprefs_idx = FindWinPrefsIndexForActiveState();
        const SoeWinPrefsEntry *e = &kWinPrefsEntries[s_winprefs_idx];
        s_winprefs_expected_0b15 = e->rom_off;
        g_ram[0x0B15] = (uint8_t)(s_winprefs_expected_0b15 & 0xFFu);
        g_ram[0x0B16] = (uint8_t)(s_winprefs_expected_0b15 >> 8);
        UploadWindowPrefsPreviewTile(ppu, e);
        return;
    }

    uint16_t cur_0b15 = (uint16_t)(g_ram[0x0B15] | (g_ram[0x0B16] << 8));
    if (cur_0b15 != s_winprefs_expected_0b15) {
        uint16_t left_next = (s_winprefs_expected_0b15 == 0x1924u) ? 0x19A4u : (uint16_t)(s_winprefs_expected_0b15 - 0x10u);
        uint16_t right_next = (s_winprefs_expected_0b15 == 0x19A4u) ? 0x1924u : (uint16_t)(s_winprefs_expected_0b15 + 0x10u);
        int dir = 0;
        if (cur_0b15 == left_next) dir = -1;
        else if (cur_0b15 == right_next) dir = 1;

        if (dir != 0) {
            s_winprefs_idx = (s_winprefs_idx + dir + WINPREFS_ENTRY_COUNT) % WINPREFS_ENTRY_COUNT;
            const SoeWinPrefsEntry *e = &kWinPrefsEntries[s_winprefs_idx];
            s_dialog_theme = e->theme;
            s_solid_dialog_bg = (s_dialog_theme != DIALOG_THEME_ORIGINAL);
            SoeSaveDialogConfig();
            s_winprefs_expected_0b15 = e->rom_off;
            g_ram[0x0B15] = (uint8_t)(s_winprefs_expected_0b15 & 0xFFu);
            g_ram[0x0B16] = (uint8_t)(s_winprefs_expected_0b15 >> 8);
        }
    }
    UploadWindowPrefsPreviewTile(ppu, &kWinPrefsEntries[s_winprefs_idx]);
}

static void SoeRenderWindowPrefsOverlay(uint32_t *pixels, int stride, int frame_w, int frame_h)
{
    if (!pixels || !g_snes || !g_snes->ppu) return;
    Ppu *ppu = g_snes->ppu;
    if (!IsWindowPrefsActive(ppu)) return;

    int native_x0 = (int)ppu->extraFrameLeft;
    if (native_x0 < 0) native_x0 = 0;

    static uint8_t s_wp_border_mask[224][256];
    memset(s_wp_border_mask, 0, sizeof(s_wp_border_mask));

    int mb = PPU_bgTilemapAdr(ppu, 1);
    int tb = PPU_bgTileAdr(ppu, 1);
    bool wider = PPU_bgTilemapWider(ppu, 1) != 0;
    bool higher = PPU_bgTilemapHigher(ppu, 1) != 0;
    uint16_t hscr = ppu->hScroll[1];
    uint16_t vscr = ppu->vScroll[1];

    int ix0 = 256, iy0 = 224, ix1 = 0, iy1 = 0;
    int bx0 = 256, by0 = 224, bx1 = 0, by1 = 0;

    /* Scan strictly across the Window Prefs preview frame band (y=48..83, x=16..239) */
    for (int ny = 48; ny < 84 && ny < frame_h; ny++) {
        int sy = (ny + 1) + (int)vscr;
        int ty = (sy >> 3) & 0x1F;
        int py_base = sy & 7;
        int row_offs = mb + (ty << 5);
        if ((sy & 0x100) && higher) row_offs += wider ? 0x800 : 0x400;

        for (int nx = 16; nx < 240; nx++) {
            int sx = nx + (int)hscr;
            int tx = (sx >> 3) & 0x1F;
            int addr = row_offs + tx + (((sx & 0x100) && wider) ? 0x400 : 0);
            uint16_t entry = ppu->vram[addr & 0x7FFF];
            int tile_num = entry & 0x3FF;
            if (tile_num == 0x3EF) {
                if (nx < ix0) ix0 = nx;
                if (nx + 1 > ix1) ix1 = nx + 1;
                if (ny < iy0) iy0 = ny;
                if (ny + 1 > iy1) iy1 = ny + 1;
                continue;
            }
            if (tile_num == 0) continue;

            int px = (entry & 0x4000) ? (7 - (sx & 7)) : (sx & 7);
            int py = (entry & 0x8000) ? (7 - py_base) : py_base;
            uint16_t tw = ppu->vram[(tb + tile_num * 8 + py) & 0x7FFF];
            int bit = 7 - px;
            int col_idx = ((tw >> bit) & 1) | (((tw >> (8 + bit)) & 1) << 1);
            if (col_idx != 0) {
                s_wp_border_mask[ny][nx] = 1;
                if (nx < bx0) bx0 = nx;
                if (nx + 1 > bx1) bx1 = nx + 1;
                if (ny < by0) by0 = ny;
                if (ny + 1 > by1) by1 = ny + 1;
            }
        }
    }

    if (ix1 > ix0 && iy1 > iy0 && s_dialog_theme != DIALOG_THEME_ORIGINAL) {
        if (bx0 >= bx1) { bx0 = ix0; by0 = iy0; bx1 = ix1; by1 = iy1; }
        SoeDialogBoxRect box = { ix0, iy0, ix1, iy1, bx0, by0, bx1, by1 };
        DrawGlassDialogBoxEffects(pixels, stride, frame_w, frame_h, native_x0,
                                  &box, 1, s_dialog_theme, s_wp_border_mask);
    }

    /* Display active pattern/theme name cleanly inside Window Prefs above the preview frame */
    const SoeWinPrefsEntry *cur_entry = &kWinPrefsEntries[s_winprefs_idx];
    char label[48];
    if (cur_entry->theme != DIALOG_THEME_ORIGINAL) {
        snprintf(label, sizeof(label), "< Glass: %s >", cur_entry->name);
    } else {
        snprintf(label, sizeof(label), "< Classic: %s >", cur_entry->name);
    }
    int lw = MeasureSmallText(label);
    int cx = native_x0 + 128 - lw / 2;
    int ly = (by0 >= 46 && by0 < 60) ? (by0 - 11) : 37;
    DrawSemiTransparentRect(pixels, stride, frame_h, cx - 5, ly - 2, lw + 10, 11, 10, 16, 32, 185);
    DrawSmallText(pixels, stride, frame_h, cx, ly, label,
                  (cur_entry->theme != DIALOG_THEME_ORIGINAL) ? 0xFFAEE2FFu : 0xFFFFFFFFu);
}

/* ── Debug Tools & Game Enhancements ──────────────────────────────────────── */

/*
 * Path to map_pillarbox.json in current directory or next to executable.
 */
static const char *GetMapPillarboxFilePath(void)
{
    static char s_path[512] = {0};
    if (s_path[0] == '\0') {
        FILE *f = fopen("Configs/map_pillarbox.json", "r");
        if (f) {
            fclose(f);
            snprintf(s_path, sizeof(s_path), "Configs/map_pillarbox.json");
            return s_path;
        }
        const char *base = SDL_GetBasePath();
        if (base) {
            snprintf(s_path, sizeof(s_path), "%sConfigs/map_pillarbox.json", base);
            return s_path;
        }
        snprintf(s_path, sizeof(s_path), "Configs/map_pillarbox.json");
    }
    return s_path;
}

/*
 * Save map pillarbox configuration to map_pillarbox.json.
 */
static void SoeSaveMapPillarboxConfig(void)
{
    soe_mkdir("Configs");
    const char *path = GetMapPillarboxFilePath();
    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "{\n");
    for (int i = 0; i < 256; i++) {
        fprintf(f, "  \"%d\": %d%s\n", i, (int)s_map_pillarbox[i], (i == 255) ? "" : ",");
    }
    fprintf(f, "}\n");
    fclose(f);
}

/*
 * Load map pillarbox configuration from map_pillarbox.json.
 * Defaults: Pillarboxed maps = { 0, 2, 29, 48, 50, 53, 70, 81, 83 } (0), all others = 1 (Widescreen).
 */
static void SoeLoadMapPillarboxConfig(void)
{
    for (int i = 0; i < 256; i++) {
        s_map_pillarbox[i] = 1;
    }
    static const uint8_t kDefaultPillarboxMaps[] = { 0, 2, 29, 48, 50, 53, 70, 81, 83 };
    for (size_t i = 0; i < sizeof(kDefaultPillarboxMaps); i++) {
        s_map_pillarbox[kDefaultPillarboxMaps[i]] = 0;
    }

    const char *path = GetMapPillarboxFilePath();
    FILE *f = fopen(path, "r");
    if (!f) {
        SoeSaveMapPillarboxConfig();
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *quote1 = strchr(line, '"');
        if (quote1) {
            char *quote2 = strchr(quote1 + 1, '"');
            if (quote2) {
                *quote2 = '\0';
                int key = atoi(quote1 + 1);
                char *colon = strchr(quote2 + 1, ':');
                if (colon) {
                    int val = atoi(colon + 1);
                    if (key >= 0 && key < 256 && val >= 0 && val <= 9) {
                        s_map_pillarbox[key] = (uint8_t)val;
                    }
                }
            }
        }
    }
    fclose(f);
}

/*
 * Return the currently loaded Map ID in Secret of Evermore:
 * $7E:0ADB is the authoritative room index read by $90:8E90 and $8C:CE99.
 * Falls back to $7E:0F9C if $7E:0ADB is 0.
 */
static uint16_t GetLoadedMapId(void)
{
    uint16_t map_id = (uint16_t)(g_ram[0x0ADB] | (g_ram[0x0ADC] << 8));
    if (map_id == 0) {
        map_id = (uint16_t)(g_ram[0x0F9C] | (g_ram[0x0F9D] << 8));
    }
    return map_id;
}

/*
 * Debug: Grant everything cleanly without touching active story/plot locks or Dog Form:
 * - Max currencies (9,999,999 Talons, Jewels, Gold Coins, Credits)
 * - Boy & Dog Level 99, max EXP, 999 HP, 999 Attack/Defense/M.Def
 * - All 13 Boy weapons + max Lv 3:00 skill levels + Dog Lv 3:00 + 99 Bazooka ammo
 * - All 12 Body Armors, 12 Helmets, 12 Bracers/Gloves/Rings, and 4 Dog Collars
 * - All 35 Alchemy formulas + 99 of all 22 ingredients (Wax $22FF .. Acorn $2314)
 * - Max consumables (6 each, Call Beads 99), all 14 Charms, and all 13 Trade Goods
 */
static void DebugGiveEverything(void)
{
    /* 1. Max money across all 4 historical eras (9,999,999 = 0x7F, 0x96, 0x98 LE) */
    g_ram[0x0AC6] = 0x7F; g_ram[0x0AC7] = 0x96; g_ram[0x0AC8] = 0x98; /* Talons */
    g_ram[0x0AC9] = 0x7F; g_ram[0x0ACA] = 0x96; g_ram[0x0ACB] = 0x98; /* Jewels */
    g_ram[0x0ACC] = 0x7F; g_ram[0x0ACD] = 0x96; g_ram[0x0ACE] = 0x98; /* Gold Coins */
    g_ram[0x0ACF] = 0x7F; g_ram[0x0AD0] = 0x96; g_ram[0x0AD1] = 0x98; /* Credits */

    /* 2. Boy Level 99, EXP, HP 999, Attack/Defense/M.Def 999 */
    g_ram[0x0A50] = 99;
    g_ram[0x0A49] = 0x7F; g_ram[0x0A4A] = 0x96; g_ram[0x0A4B] = 0x98; g_ram[0x0A4C] = 0x00;
    g_ram[0x0A35] = 999 & 0xFF; g_ram[0x0A36] = (999 >> 8) & 0xFF;
    g_ram[0x0A33] = 999 & 0xFF; g_ram[0x0A34] = (999 >> 8) & 0xFF;
    g_ram[0x4EB3] = 999 & 0xFF; g_ram[0x4EB4] = (999 >> 8) & 0xFF;
    g_ram[0x0A3F] = 999 & 0xFF; g_ram[0x0A40] = (999 >> 8) & 0xFF;
    g_ram[0x0A41] = 999 & 0xFF; g_ram[0x0A42] = (999 >> 8) & 0xFF;
    g_ram[0x0A43] = 999 & 0xFF; g_ram[0x0A44] = (999 >> 8) & 0xFF;

    /* 3. Dog Level 99, EXP, HP 999, Attack/Defense/M.Def 999 */
    g_ram[0x0A9A] = 99;
    g_ram[0x0A7F] = 999 & 0xFF; g_ram[0x0A80] = (999 >> 8) & 0xFF; /* Dog Max HP */
    g_ram[0x0A7D] = 999 & 0xFF; g_ram[0x0A7E] = (999 >> 8) & 0xFF; /* Dog HP */
    g_ram[0x4F61] = 999 & 0xFF; g_ram[0x4F62] = (999 >> 8) & 0xFF;
    g_ram[0x0A93] = 0x7F; g_ram[0x0A94] = 0x96; g_ram[0x0A95] = 0x98; g_ram[0x0A96] = 0x00;
    g_ram[0x0A8B] = 999 & 0xFF; g_ram[0x0A8C] = (999 >> 8) & 0xFF;
    g_ram[0x0A89] = 999 & 0xFF; g_ram[0x0A8A] = (999 >> 8) & 0xFF;
    g_ram[0x0A8D] = 999 & 0xFF; g_ram[0x0A8E] = (999 >> 8) & 0xFF;
    g_ram[0x0A91] = 999 & 0xFF; g_ram[0x0A92] = (999 >> 8) & 0xFF;
    g_ram[0x0A8F] = 999 & 0xFF; g_ram[0x0A90] = (999 >> 8) & 0xFF;

    /* 4. All 13 Weapons + Lv 3:00 Weapon Charge Levels ($0ADD..$0AF5) & Dog Attack ($0B07) */
    g_ram[0x22DA] |= 0xFE;
    g_ram[0x22DB] |= 0x3F;
    for (int addr = 0x0ADD; addr <= 0x0AF5; addr += 2) {
        WriteLe16(&g_ram[addr], 0x0300u);
    }
    WriteLe16(&g_ram[0x0B07], 0x0300u);

    /* 5. Bazooka Ammo x99 (Thunder Ball $2345, Particle Bomb $2346, Cryo-Blast $2347) */
    g_ram[0x2345] = 99;
    g_ram[0x2346] = 99;
    g_ram[0x2347] = 99;

    /* 6. All 40 Armor/Equipment pieces ($231D..$2344): Body Armor, Helmets, Bracers/Rings, Collars */
    for (int i = 0x231D; i <= 0x2344; i++) {
        g_ram[i] = 1;
    }

    /* 7. All 35 Alchemy Formulas ($2258..$225C) & 99 of all 22 Ingredients (Wax $22FF .. Acorn $2314) */
    for (int i = 0x2258; i <= 0x225B; i++) {
        g_ram[i] = 0xFF;
    }
    g_ram[0x225C] |= 0x07;
    /* Also equip Levitate (0x2A) and starter spells into any empty ($FF) Alchemy Ring slots ($0AD2..$0ADA)
     * without overwriting spells the player already has equipped. */
    {
        static const uint8_t kRingFillSpells[] = {
            0x2A, /* Levitate */
            0x24, /* Heal */
            0x0C, /* Cure */
            0x1E, /* Flash */
            0x0A, /* Crush */
            0x02, /* Atlas */
            0x0E, /* Defend */
            0x3E, /* Speed */
            0x14  /* Energize */
        };
        for (size_t s = 0; s < sizeof(kRingFillSpells); s++) {
            uint8_t sp = kRingFillSpells[s];
            bool already = false;
            int empty_idx = -1;
            for (int slot = 0; slot < 9; slot++) {
                uint8_t cur = g_ram[0x0AD2 + slot];
                if (cur == sp) {
                    already = true;
                    break;
                }
                if (cur == 0xFF && empty_idx < 0) {
                    empty_idx = slot;
                }
            }
            if (!already && empty_idx >= 0) {
                g_ram[0x0AD2 + empty_idx] = sp;
            }
        }
    }
    for (int i = 0x22FF; i <= 0x2314; i++) {
        g_ram[i] = 99;
    }

    /* 8. Consumables (Petals..Pixie Dust capped at 6) & Call Beads ($231C = 99) */
    for (int i = 0x2315; i <= 0x231B; i++) {
        g_ram[i] = 6;
    }
    g_ram[0x231C] = 99;

    /* 9. All 14 Charms ($2261..$2263 bitmasks per $CC:BBB0) without setting $2264 plot key bits */
    g_ram[0x2261] |= 0xE0; /* Armor Polish, Chocobo Egg, Insect Incense */
    g_ram[0x2262]  = 0xFF; /* Jade Disk .. Staff Of Life (incl. Jaguar Ring, Ruby Heart, Silver Sheath) */
    g_ram[0x2263] |= 0x07; /* Sun Stone, Thug's Cloak, Wizard's Coin */

    snprintf(s_debug_toast, sizeof(s_debug_toast), "Everything Given!");
    s_debug_toast_timer = 120;
}

static void SyncDogFormFromRam(void)
{
    uint16_t cur = ReadLe16(&g_ram[0x2363]);
    if (cur == 0x0006u)      s_dog_form = 1; /* Greyhound */
    else if (cur == 0x0008u) s_dog_form = 2; /* Poodle */
    else if (cur == 0x000Cu) s_dog_form = 3; /* Toaster */
    else if (cur == 0x000Au) s_dog_form = 4; /* Spot */
    else if (cur == 0x0002u || cur == 0x0004u) s_dog_form = 0; /* Wolf */
}

/* Native $8F:9916 sprite allocator freelist release */
static void FreeDogSpriteSlot(uint16_t addr)
{
    uint16_t slot = ReadLe16(&g_ram[addr]);
    if ((slot & 0x8000u) != 0 || slot >= 0x0400u) return;

    WriteLe16(&g_ram[0x3C14u + slot], 0x0000u);
    uint16_t head = ReadLe16(&g_ram[0x3BC7u]);
    WriteLe16(&g_ram[0x3BC9u + slot], head);
    WriteLe16(&g_ram[0x3BC7u], slot);
    WriteLe16(&g_ram[addr], 0xFFFFu);
}

/*
 * Extend $80:BF0E (HUD Dog Palette Table) and $80:C707 (HUD Dog Sprite Table)
 * from 6 entries ($00..$0A) to 7 entries ($00..$0C) in unused $80:8000..$80:801B
 * padding so Toaster ($2363 == 0x000C) outside Omnitopia ($0E70 != 4) never
 * indexes out of bounds into code bytes at $80:BF1A / $80:C713, and Spot ($0A)
 * displays a dog head portrait ($4322) rather than Toaster's curled sphere ($4522).
 */
static void EnsureToasterHudTablesPatched(uint8_t *rom)
{
    static const uint16_t kHudPalTable[7] = {
        0x0000u, 0xAE0Bu, 0xAE0Bu, 0xAE2Bu, 0xAE4Bu, 0xB54Bu, 0xAE6Bu
    };
    static const uint16_t kHudSprTable[7] = {
        0x0000u, 0x4322u, 0x4322u, 0x4402u, 0x4402u, 0x4322u, 0x4522u
    };
    for (int i = 0; i < 7; i++) {
        WriteLe16(&rom[0x008000u + (uint32_t)i * 2u], kHudPalTable[i]);
        WriteLe16(&rom[0x00800Eu + (uint32_t)i * 2u], kHudSprTable[i]);
    }
    WriteLe16(&rom[0x00BF02u], 0x8000u);
    WriteLe16(&rom[0x00C6FAu], 0x800Eu);
}

/*
 * Toaster Dog animations ($45B6 Sniff & $45E6 Idle Bark) invoke opcode 2E $3C,
 * mapping via $8C:8362[$3C] to SFX $0048 (Omnitopia sample pack $46). Outside
 * Omnitopia, SPC700 ARAM slot $48 is $0000 (NULL), causing the SPC driver to
 * interpret zero-page RAM as an SFX script — corrupting/looping SFX infinitely
 * and halting music across map transitions. Remap $8C:8362[$3C] to Toaster's
 * resident electronic chirp SFX ($0022) whenever slot $48 is unloaded in ARAM,
 * and restore $0048 whenever a pack containing slot $48 is active.
 */
static void SyncToasterDogSfx(uint8_t *rom, const Apu *apu)
{
    if (!rom || !apu || !g_snes || !g_snes->cart || g_snes->cart->romSize < 0x0C8500u) return;
    uint16_t sfx_tbl = (uint16_t)(apu->ram[0x1EEE] | ((uint16_t)apu->ram[0x1EEF] << 8));
    if (sfx_tbl < 0x0200u || (uint32_t)sfx_tbl + 0x5Au * 2u > 0x10000u) return;

    uint16_t sfx48_ptr = (uint16_t)(apu->ram[(sfx_tbl + 0x48u * 2u) & 0xFFFFu] |
                                    ((uint16_t)apu->ram[(sfx_tbl + 0x48u * 2u + 1u) & 0xFFFFu] << 8));
    uint16_t sfx22_ptr = (uint16_t)(apu->ram[(sfx_tbl + 0x22u * 2u) & 0xFFFFu] |
                                    ((uint16_t)apu->ram[(sfx_tbl + 0x22u * 2u + 1u) & 0xFFFFu] << 8));
    if (sfx22_ptr == 0u && sfx48_ptr == 0u) return;

    uint16_t desired = (sfx48_ptr != 0u) ? 0x0048u : 0x0022u;
    uint16_t cur = ReadLe16(&rom[0x0C8362u + 0x3Cu * 2u]);
    if (cur != desired) {
        WriteLe16(&rom[0x0C8362u + 0x3Cu * 2u], desired);
    }
}

/*
 * Read sequential bytes across HiROM upper-half ($80..$BF:$8000..$FFFF) bank
 * boundaries matching the sound driver's uploader ($8C:818D).
 */
static bool ReadRom24Bytes(const uint8_t *rom, size_t rom_size,
                           uint32_t ptr24, uint8_t *out, size_t len)
{
    if (!rom || !out) return false;
    uint8_t bank = (uint8_t)((ptr24 >> 16) & 0xFFu);
    uint32_t addr = (uint32_t)(ptr24 & 0xFFFFu);
    for (size_t i = 0; i < len; i++) {
        uint32_t off = ((uint32_t)(bank & 0x3Fu) * 0x10000u) + addr;
        if ((size_t)off >= rom_size) return false;
        out[i] = rom[off];
        addr++;
        if (addr >= 0x10000u) {
            bank++;
            addr = 0x8000u;
        }
    }
    return true;
}

/* Populate all valid playable music track IDs from the ROM's $81:9900 pack table. */
static void BuildJukeboxSongList(void)
{
    if (s_jukebox_list_built) return;
    if (!g_snes || !g_snes->cart || !g_snes->cart->rom ||
        g_snes->cart->romSize < 0x0C0000u) {
        return;
    }

    const uint8_t *rom = g_snes->cart->rom;
    size_t rom_size = g_snes->cart->romSize;
    const uint32_t tbl_base = 0x019900u;
    uint8_t n_packs = rom[tbl_base];
    uint8_t n_songs = rom[tbl_base + 1u];
    if (n_songs > 128u) n_songs = 128u;

    s_jukebox_song_count = 0;
    uint8_t chunk_buf[2u + 256u * 7u];
    uint8_t seq_buf[256];

    for (uint8_t s = 0; s < n_songs; s++) {
        uint32_t pack_idx_off = tbl_base + 3u + ((uint32_t)n_packs + 1u) * 3u + s;
        if ((size_t)pack_idx_off >= rom_size) break;
        uint8_t pk = rom[pack_idx_off];
        if (pk > n_packs) continue;

        uint32_t p_off = tbl_base + 3u + (uint32_t)pk * 3u;
        if ((size_t)p_off + 3u > rom_size) continue;
        uint32_t pack_ptr24 = (uint32_t)rom[p_off] |
                              ((uint32_t)rom[p_off + 1u] << 8) |
                              ((uint32_t)rom[p_off + 2u] << 16);

        uint8_t hdr2[2];
        if (!ReadRom24Bytes(rom, rom_size, pack_ptr24, hdr2, 2)) continue;
        uint16_t n_chunks = (uint16_t)(hdr2[0] | (hdr2[1] << 8));
        if (n_chunks == 0 || n_chunks > 256u) continue;
        if (!ReadRom24Bytes(rom, rom_size, pack_ptr24, chunk_buf, 2u + (size_t)n_chunks * 7u)) {
            continue;
        }

        uint16_t song_slot_dst = (uint16_t)(0x2EB8u + (uint16_t)s * 2u);
        uint16_t slot_aram = 0;
        bool has_slot = false;
        for (uint16_t c = 0; c < n_chunks; c++) {
            const uint8_t *ch = &chunk_buf[2u + (size_t)c * 7u];
            uint16_t clen = (uint16_t)(ch[0] | (ch[1] << 8));
            uint16_t cdst = (uint16_t)(ch[5] | (ch[6] << 8));
            if (cdst == song_slot_dst && clen >= 2u) {
                uint32_t cptr = (uint32_t)ch[2] | ((uint32_t)ch[3] << 8) | ((uint32_t)ch[4] << 16);
                uint8_t sbytes[2];
                if (ReadRom24Bytes(rom, rom_size, cptr, sbytes, 2)) {
                    slot_aram = (uint16_t)(sbytes[0] | (sbytes[1] << 8));
                    has_slot = (slot_aram != 0);
                }
                break;
            }
        }
        if (!has_slot) continue;

        /* Skip silent/halt-only dummy entries lacking instruments ($F5) or notes */
        bool has_inst = false;
        bool has_note = false;
        for (uint16_t c = 0; c < n_chunks; c++) {
            const uint8_t *ch = &chunk_buf[2u + (size_t)c * 7u];
            uint16_t clen = (uint16_t)(ch[0] | (ch[1] << 8));
            uint16_t cdst = (uint16_t)(ch[5] | (ch[6] << 8));
            if (cdst <= slot_aram || clen == 0) continue;
            uint32_t cptr = (uint32_t)ch[2] | ((uint32_t)ch[3] << 8) | ((uint32_t)ch[4] << 16);
            size_t probe = (clen < sizeof(seq_buf)) ? clen : sizeof(seq_buf);
            if (!ReadRom24Bytes(rom, rom_size, cptr, seq_buf, probe)) continue;
            for (size_t b = 0; b < probe; b++) {
                if (seq_buf[b] == 0xF5u) has_inst = true;
                if (seq_buf[b] < 0xC8u) has_note = true;
            }
            if (has_inst && has_note) break;
        }
        if (has_inst && has_note) {
            s_jukebox_songs[s_jukebox_song_count++] = s;
        }
    }

    if (s_jukebox_song_count == 0) {
        for (uint8_t s = 0; s < 70; s++) {
            if (s != 0x27u) s_jukebox_songs[s_jukebox_song_count++] = s;
        }
    }
    s_jukebox_list_built = true;
}

static void ClampJukeboxScroll(void)
{
    if (s_jukebox_song_count <= 0) {
        s_jukebox_cursor = 0;
        s_jukebox_scroll = 0;
        return;
    }
    if (s_jukebox_cursor < 0) s_jukebox_cursor = 0;
    if (s_jukebox_cursor >= s_jukebox_song_count) s_jukebox_cursor = s_jukebox_song_count - 1;
    if (s_jukebox_cursor < s_jukebox_scroll) {
        s_jukebox_scroll = s_jukebox_cursor;
    } else if (s_jukebox_cursor >= s_jukebox_scroll + kJukeboxVisibleRows) {
        s_jukebox_scroll = s_jukebox_cursor - (kJukeboxVisibleRows - 1);
    }
    int max_scroll = s_jukebox_song_count - kJukeboxVisibleRows;
    if (max_scroll < 0) max_scroll = 0;
    if (s_jukebox_scroll > max_scroll) s_jukebox_scroll = max_scroll;
    if (s_jukebox_scroll < 0) s_jukebox_scroll = 0;
}

static void SoeApplyJukeboxRomPatches(bool lock_active)
{
    if (!g_snes || !g_snes->cart || !g_snes->cart->rom || g_snes->cart->romSize < 0x100000u) {
        return;
    }
    uint8_t *rom = g_snes->cart->rom;

    /* 1. $8C:828F (Play Song / Load Pack entry): RTL when locked, REP #$30 when unlocked */
    rom[0x0C828Fu] = lock_active ? 0x6Bu : 0xC2u;

    /* 2. $8C:D6DD (inside room script opcode $8C:D6D6 Play Music, right after consuming
     *    the 1-byte song operand via LDA [$82]; INC $82; AND #$00FF):
     *    SEC; RTS when locked so it neither disables NMI ($4200=0) nor silences the SPC,
     *    PEA $7E80 (F4 80) when unlocked. */
    rom[0x0CD6DDu] = lock_active ? 0x38u : 0xF4u;
    rom[0x0CD6DEu] = lock_active ? 0x60u : 0x80u;

    /* 3. Ensure $8E:E592 entry instruction (LDA $0106 = AD 06) is always intact */
    rom[0x0EE592u] = 0xADu;
    rom[0x0EE593u] = 0x06u;

    /* 4. $8C:DC74 (Script Music Fade-Out opcode):
     *    SEC; RTS (38 60) when locked, SEP #$20 (E2 20) when unlocked. */
    rom[0x0CDC74u] = lock_active ? 0x38u : 0xE2u;
    rom[0x0CDC75u] = lock_active ? 0x60u : 0x20u;

    /* 5. $8C:8623 (Per-frame negative $9B music volume fade-out inside $8C:8619):
     *    BMI $8620 (30 FB -> REP #$20; RTL) when locked so fade-in (>0) still works
     *    while fade-out (<0) is ignored; BMI $863B (30 16) when unlocked. */
    rom[0x0C8623u] = 0x30u;
    rom[0x0C8624u] = lock_active ? 0xFBu : 0x16u;

    /* 6. Transition stop/fade JSL $8C81FD call sites (including the 3 inside $8E:E592):
     *    4x NOP (EA EA EA EA) when locked, JSL $8C81FD (22 FD 81 8C) when unlocked. */
    static const uint32_t kStopJslOffsets[] = {
        0x0EE5AAu, 0x0EE5CAu, 0x0EE5D4u,
        0x0EE4E9u, 0x0EE4F3u, 0x008207u, 0x008211u, 0x00DDB9u, 0x00DE41u
    };
    for (size_t i = 0; i < sizeof(kStopJslOffsets) / sizeof(kStopJslOffsets[0]); i++) {
        uint32_t off = kStopJslOffsets[i];
        if (lock_active) {
            rom[off + 0] = 0xEAu;
            rom[off + 1] = 0xEAu;
            rom[off + 2] = 0xEAu;
            rom[off + 3] = 0xEAu;
        } else {
            rom[off + 0] = 0x22u;
            rom[off + 1] = 0xFDu;
            rom[off + 2] = 0x81u;
            rom[off + 3] = 0x8Cu;
        }
    }

    if (lock_active && (int8_t)g_ram[0x009B] < 0) {
        g_ram[0x009B] = 0x00u;
        g_ram[0x009A] = 0xFFu;
    }
}

static void SoeSyncJukeboxRomPatches(void)
{
    bool lock_active = (s_jukebox_lock_song && s_jukebox_locked_song_id >= 0 && g_ram[0x0E4B] != 0);
    SoeApplyJukeboxRomPatches(lock_active);
}

static void ToggleJukeboxLockSong(void)
{
    s_jukebox_lock_song = !s_jukebox_lock_song;
    if (s_jukebox_lock_song) {
        if (s_pending_jukebox_song >= 0) {
            s_jukebox_locked_song_id = s_pending_jukebox_song;
        } else if (g_ram[0x0E4B] != 0) {
            s_jukebox_locked_song_id = (int)(ReadLe16(&g_ram[0x0E4D]) & 0xFFu);
        } else if (s_jukebox_song_count > 0 && s_jukebox_cursor >= 0 && s_jukebox_cursor < s_jukebox_song_count) {
            s_jukebox_locked_song_id = (int)s_jukebox_songs[s_jukebox_cursor];
        } else {
            s_jukebox_locked_song_id = -1;
        }
        snprintf(s_debug_toast, sizeof(s_debug_toast), "Jukebox: Keep Song ON");
    } else {
        s_jukebox_locked_song_id = -1;
        snprintf(s_debug_toast, sizeof(s_debug_toast), "Jukebox: Keep Song OFF");
    }
    SoeSyncJukeboxRomPatches();
    SoeSaveDebugConfig();
    s_debug_toast_timer = 90;
}

static void OpenJukeboxWindow(void)
{
    BuildJukeboxSongList();
    if (!s_jukebox_synced_once && s_jukebox_song_count > 0) {
        uint16_t cur = ReadLe16(&g_ram[0x0E4D]);
        for (int i = 0; i < s_jukebox_song_count; i++) {
            if ((uint16_t)s_jukebox_songs[i] == cur) {
                s_jukebox_cursor = i;
                break;
            }
        }
        s_jukebox_synced_once = true;
    }
    s_jukebox_keep_focused = false;
    ClampJukeboxScroll();
    s_key_trade_open = false;
    s_warp_win_open = false;
    s_jukebox_open = true;
}

static void ClampKeyTradeScroll(void)
{
    if (s_key_trade_cursor < 0) s_key_trade_cursor = 0;
    if (s_key_trade_cursor >= kKeyTradeItemCount) s_key_trade_cursor = kKeyTradeItemCount - 1;
    if (s_key_trade_cursor < s_key_trade_scroll) {
        s_key_trade_scroll = s_key_trade_cursor;
    } else if (s_key_trade_cursor >= s_key_trade_scroll + kKeyTradeVisibleRows) {
        s_key_trade_scroll = s_key_trade_cursor - kKeyTradeVisibleRows + 1;
    }
    int max_scroll = kKeyTradeItemCount - kKeyTradeVisibleRows;
    if (max_scroll < 0) max_scroll = 0;
    if (s_key_trade_scroll < 0) s_key_trade_scroll = 0;
    if (s_key_trade_scroll > max_scroll) s_key_trade_scroll = max_scroll;
}

static void OpenKeyTradeWindow(void)
{
    ClampKeyTradeScroll();
    s_jukebox_open = false;
    s_warp_win_open = false;
    s_key_trade_open = true;
}

static void SoePlayUiSfx(int clip_id);

static void StepKeyTradeCursor(int delta)
{
    if (kKeyTradeItemCount <= 0) return;
    if (delta == 1) {
        s_key_trade_cursor = (s_key_trade_cursor + 1) % kKeyTradeItemCount;
    } else if (delta == -1) {
        s_key_trade_cursor = (s_key_trade_cursor - 1 + kKeyTradeItemCount) % kKeyTradeItemCount;
    } else {
        s_key_trade_cursor += delta;
        if (s_key_trade_cursor < 0) s_key_trade_cursor = 0;
        if (s_key_trade_cursor >= kKeyTradeItemCount) s_key_trade_cursor = kKeyTradeItemCount - 1;
    }
    ClampKeyTradeScroll();
    SoePlayUiSfx(0);
}

static void ActivateKeyTradeSelection(int delta)
{
    if (s_key_trade_cursor < 0 || s_key_trade_cursor >= kKeyTradeItemCount) return;
    const SoeKeyTradeItemDef *it = &kKeyTradeItems[s_key_trade_cursor];
    if (it->mask != 0) {
        if (delta < 0) {
            g_ram[it->addr] &= (uint8_t)~it->mask;
        } else {
            g_ram[it->addr] ^= it->mask;
        }
        bool has = (g_ram[it->addr] & it->mask) != 0;
        snprintf(s_debug_toast, sizeof(s_debug_toast), "%s: %s", it->name, has ? "ON" : "OFF");
    } else {
        uint16_t cur = ReadLe16(&g_ram[it->addr]);
        int next = (int)cur + delta;
        if (next < 0) next = 0;
        if (next > 99) next = 99;
        WriteLe16(&g_ram[it->addr], (uint16_t)next);
        snprintf(s_debug_toast, sizeof(s_debug_toast), "%s: x%d", it->name, next);
    }
    s_debug_toast_timer = 90;
    SoePlayUiSfx(1);
}

static const char *GetMapName(uint16_t map_id);
static void DebugExecuteWarp(uint16_t target_map);

static void BuildWarpLocationList(void)
{
    if (s_warp_list_built) return;
    if (!g_snes || !g_snes->cart || !g_snes->cart->rom) return;

    const uint8_t *rom = g_snes->cart->rom;
    const uint32_t rom_size = g_snes->cart->romSize;
    const uint32_t off_table = 0x1F0000u + 0xFDE7u;

    s_warp_location_count = 0;

    /* Populate all valid Evermore maps 0x00..0x7E (127 maps max; exclude 0x7F crash pointer) */
    for (uint16_t m = 0; m <= 0x7Eu && s_warp_location_count < 128; m++) {
        uint32_t m_off = off_table + (uint32_t)m * 4u;
        if (m_off + 4u > rom_size) break;
        uint32_t rom_addr = (uint32_t)rom[m_off] |
                            ((uint32_t)rom[m_off + 1] << 8) |
                            ((uint32_t)rom[m_off + 2] << 16);
        if (rom_addr < 0x800000u || rom_addr >= 0xC00000u) continue;
        s_warp_locations[s_warp_location_count].map_id = m;
        s_warp_locations[s_warp_location_count].is_named = true;
        s_warp_location_count++;
    }

    s_warp_list_built = true;
}

static void ClampWarpScroll(void)
{
    if (s_warp_location_count <= 0) {
        s_warp_cursor = 0;
        s_warp_scroll = 0;
        return;
    }
    if (s_warp_cursor < 0) s_warp_cursor = 0;
    if (s_warp_cursor >= s_warp_location_count) s_warp_cursor = s_warp_location_count - 1;
    if (s_warp_cursor < s_warp_scroll) {
        s_warp_scroll = s_warp_cursor;
    } else if (s_warp_cursor >= s_warp_scroll + kWarpVisibleRows) {
        s_warp_scroll = s_warp_cursor - kWarpVisibleRows + 1;
    }
    int max_scroll = s_warp_location_count - kWarpVisibleRows;
    if (max_scroll < 0) max_scroll = 0;
    if (s_warp_scroll < 0) s_warp_scroll = 0;
    if (s_warp_scroll > max_scroll) s_warp_scroll = max_scroll;
}

static void SelectWarpMapId(uint16_t map_id)
{
    BuildWarpLocationList();
    for (int i = 0; i < s_warp_location_count; i++) {
        if (s_warp_locations[i].map_id == map_id) {
            s_warp_cursor = i;
            ClampWarpScroll();
            return;
        }
    }
    ClampWarpScroll();
}

static void OpenWarpWindow(void)
{
    SelectWarpMapId(GetLoadedMapId());
    s_jukebox_open = false;
    s_key_trade_open = false;
    s_warp_win_open = true;
}

static void StepWarpCursor(int delta)
{
    if (s_warp_location_count <= 0) return;
    if (delta == 1) {
        s_warp_cursor = (s_warp_cursor + 1) % s_warp_location_count;
    } else if (delta == -1) {
        s_warp_cursor = (s_warp_cursor - 1 + s_warp_location_count) % s_warp_location_count;
    } else {
        s_warp_cursor += delta;
        if (s_warp_cursor < 0) s_warp_cursor = 0;
        if (s_warp_cursor >= s_warp_location_count) s_warp_cursor = s_warp_location_count - 1;
    }
    ClampWarpScroll();
    SoePlayUiSfx(0);
}

static void ActivateWarpSelection(void)
{
    if (s_warp_cursor < 0 || s_warp_cursor >= s_warp_location_count) return;
    uint16_t target = s_warp_locations[s_warp_cursor].map_id;
    s_warp_map_id = target;
    DebugExecuteWarp(target);
    SoePlayUiSfx(1);
}

static void StepJukeboxCursor(int delta)
{
    BuildJukeboxSongList();
    if (s_jukebox_song_count <= 0) return;
    if (delta == 1) {
        if (s_jukebox_keep_focused) {
            s_jukebox_keep_focused = false;
            s_jukebox_cursor = 0;
        } else if (s_jukebox_cursor + 1 >= s_jukebox_song_count) {
            s_jukebox_keep_focused = true;
        } else {
            s_jukebox_cursor++;
        }
    } else if (delta == -1) {
        if (s_jukebox_keep_focused) {
            s_jukebox_keep_focused = false;
            s_jukebox_cursor = s_jukebox_song_count - 1;
        } else if (s_jukebox_cursor == 0) {
            s_jukebox_keep_focused = true;
        } else {
            s_jukebox_cursor--;
        }
    } else {
        if (s_jukebox_keep_focused) {
            ToggleJukeboxLockSong();
            return;
        }
        s_jukebox_cursor += delta;
        if (s_jukebox_cursor < 0) s_jukebox_cursor = 0;
        if (s_jukebox_cursor >= s_jukebox_song_count) s_jukebox_cursor = s_jukebox_song_count - 1;
    }
    ClampJukeboxScroll();
}

static void ActivateJukeboxSelection(void)
{
    if (s_jukebox_keep_focused) {
        ToggleJukeboxLockSong();
        return;
    }
    BuildJukeboxSongList();
    if (s_jukebox_song_count <= 0) return;
    ClampJukeboxScroll();
    uint8_t song_id = s_jukebox_songs[s_jukebox_cursor];
    s_pending_jukebox_song = (int)song_id;
    if (s_jukebox_lock_song) {
        s_jukebox_locked_song_id = (int)song_id;
    }
    snprintf(s_debug_toast, sizeof(s_debug_toast), "Jukebox -> Song $%02X", (unsigned)song_id);
    s_debug_toast_timer = 90;
}

/* Execute a single JSL subroutine inside bank $8C on the interpreter bridge. */
static void RunBank8CCall(uint32_t target_pc24, uint16_t a_val, uint16_t y_val)
{
    g_cpu.A = a_val;
    g_cpu.X = 0;
    g_cpu.Y = y_val;
    g_cpu.D = 0x0000;
    g_cpu.DB = 0x8C;
    g_cpu.PB = 0x8C;
    g_cpu.emulation = 0;
    g_cpu.m_flag = 0;
    g_cpu.x_flag = 0;
    g_cpu.P = (uint8_t)((g_cpu.P & ~0x30u) | 0x04u);
    cpu_p_to_mirrors(&g_cpu);
    g_cpu.host_return_valid = 0;

    /* JSL return frame ($8C:8000): RTL pops + exits interp_bridge_run past watermark */
    cpu_write8(&g_cpu, 0x00, g_cpu.S, 0x8C); g_cpu.S = (uint16_t)(g_cpu.S - 1);
    cpu_write8(&g_cpu, 0x00, g_cpu.S, 0x80); g_cpu.S = (uint16_t)(g_cpu.S - 1);
    cpu_write8(&g_cpu, 0x00, g_cpu.S, 0x00); g_cpu.S = (uint16_t)(g_cpu.S - 1);

    interp_bridge_run(&g_cpu, target_pc24);
}

void SoeProcessPendingJukeboxSong(void)
{
    SoeSyncJukeboxRomPatches();

    if (s_pending_jukebox_song < 0 && s_pending_jukebox_sfx < 0) return;
    if (snes_frame_counter <= 20 || !g_snes || !g_snes->apu ||
        g_cpu.emulation != 0 || g_ram[0x0E51] != 0) {
        return;
    }

    /* Hold the PPU/IRQ beam clock and save CPU, beam, refresh, and APU pacing state
     * so out-of-band SPC700 uploads (~50M master cycles inside interp_bridge_run)
     * never advance g_snes->beamMasterLast ahead of g_cpu.master_cycles or clobber
     * in-flight direct-page audio/DMA scratch registers. */
    WatchdogFrameStart();
    snes_beam_hold(1);

    uint32_t saved_resume_pc = 0;
    uint8_t saved_wai = 0;
    snes_beam_frame_driver_get_position(&saved_resume_pc, &saved_wai);

    uint64_t saved_ref_phase = 0;
    uint64_t saved_ref_upto = 0;
    snes_refresh_state_get(&saved_ref_phase, &saved_ref_upto);

    CpuState saved_cpu = g_cpu;
    uint64_t saved_sync_master = g_apu_last_sync_master;
    uint32_t saved_pace_est = g_apu_pace_cycles_estimate;
    uint32_t saved_sync_cycles = g_apu_last_sync_cycles;

    /* Preserve audio scratch DP ($7E:009C..$009E, $7E:00A0..$00A5) and temp vars
     * ($7E:0E47..$0E4A), while keeping $7E:009F (the live SPC handshake counter). */
    uint8_t saved_dp_9c[3];
    uint8_t saved_dp_a0[6];
    uint8_t saved_0e47[4];
    memcpy(saved_dp_9c, &g_ram[0x009C], sizeof(saved_dp_9c));
    memcpy(saved_dp_a0, &g_ram[0x00A0], sizeof(saved_dp_a0));
    memcpy(saved_0e47, &g_ram[0x0E47], sizeof(saved_0e47));

    RtlApuLock();
    uint64_t saved_port_clock = g_snes->apu->portClock;
    uint64_t saved_port_guest_anchor = g_snes->apu->portGuestAnchor;
    uint64_t saved_port_target_anchor = g_snes->apu->portTargetAnchor;
    uint64_t saved_port_last_guest = g_snes->apu->portLastGuest;
    uint64_t saved_port_last_target = g_snes->apu->portLastTarget;
    bool saved_port_time_valid = g_snes->apu->portTimeValid;
    uint64_t saved_catchup = g_snes->apuCatchupCycles;
    g_snes->apu->portTimeValid = false;
    g_snes->apu->portLastGuest = 0;
    g_snes->apu->portLastTarget = g_snes->apu->portClock;
    RtlApuUnlock();

    uint8_t saved_stack[64];
    for (int i = 0; i < 64; i++) {
        saved_stack[i] = cpu_read8(&g_cpu, 0x00, (uint16_t)(saved_cpu.S - (uint16_t)i));
    }

    if (s_pending_jukebox_sfx >= 0) {
        uint16_t sfx_id = (uint16_t)(s_pending_jukebox_sfx & 0xFF);
        s_pending_jukebox_sfx = -1;

        RunBank8CCall(0x8C82E3u, sfx_id, 0x0000u);

        memcpy(&g_ram[0x009C], saved_dp_9c, sizeof(saved_dp_9c));
        memcpy(&g_ram[0x00A0], saved_dp_a0, sizeof(saved_dp_a0));
        memcpy(&g_ram[0x0E47], saved_0e47, sizeof(saved_0e47));
        for (int i = 0; i < 64; i++) {
            cpu_write8(&g_cpu, 0x00, (uint16_t)(saved_cpu.S - (uint16_t)i), saved_stack[i]);
        }
        g_cpu = saved_cpu;
        snes_refresh_state_set(saved_ref_phase, saved_ref_upto);
        snes_beam_frame_driver_set_position(saved_resume_pc, saved_wai);
        snes_beam_hold(0);
        g_apu_last_sync_master = saved_sync_master;
        g_apu_pace_cycles_estimate = saved_pace_est;
        g_apu_last_sync_cycles = saved_sync_cycles;

        RtlApuLock();
        apu_runUntilPortQueueEmpty(g_snes->apu, 65536u);
        g_snes->apu->portClock = saved_port_clock;
        g_snes->apu->portGuestAnchor = saved_port_guest_anchor;
        g_snes->apu->portTargetAnchor = saved_port_target_anchor;
        g_snes->apu->portLastGuest = saved_port_last_guest;
        g_snes->apu->portLastTarget = saved_port_last_target;
        g_snes->apu->portTimeValid = saved_port_time_valid;
        g_snes->apuCatchupCycles = saved_catchup;
        RtlApuUnlock();
        return;
    }

    uint16_t song_id = (uint16_t)(s_pending_jukebox_song & 0xFF);
    s_pending_jukebox_song = -1;
    if (s_jukebox_lock_song) {
        s_jukebox_locked_song_id = (int)song_id;
    }

    /* Temporarily unpatch $8C:828F so our explicit Jukebox song load executes in full */
    SoeApplyJukeboxRomPatches(false);

    /* Cleanly silence/reset prior track state ($8C:D6E0 sequence) before loading & playing.
     * Note: $7E:009F is the persistent SPC handshake toggle counter and must NOT be restored. */
    RunBank8CCall(0x8C81FDu, 0x001Au, 0x0000u);
    RunBank8CCall(0x8C81FDu, 0x002Au, 0x00A0u);
    RunBank8CCall(0x8C81FDu, 0x002Cu, 0x0000u);
    RunBank8CCall(0x8C828Fu, song_id, 0x0000u);

    memcpy(&g_ram[0x009C], saved_dp_9c, sizeof(saved_dp_9c));
    memcpy(&g_ram[0x00A0], saved_dp_a0, sizeof(saved_dp_a0));
    memcpy(&g_ram[0x0E47], saved_0e47, sizeof(saved_0e47));
    g_ram[0x009A] = 0xFFu;
    g_ram[0x009B] = 0x00u;

    for (int i = 0; i < 64; i++) {
        cpu_write8(&g_cpu, 0x00, (uint16_t)(saved_cpu.S - (uint16_t)i), saved_stack[i]);
    }

    g_cpu = saved_cpu;
    snes_refresh_state_set(saved_ref_phase, saved_ref_upto);
    snes_beam_frame_driver_set_position(saved_resume_pc, saved_wai);
    snes_beam_hold(0);
    g_apu_last_sync_master = saved_sync_master;
    g_apu_pace_cycles_estimate = saved_pace_est;
    g_apu_last_sync_cycles = saved_sync_cycles;

    RtlApuLock();
    /* Drain final Play Song ($2140/$2142) port writes and wait for SPC700 ack */
    apu_runUntilPortQueueEmpty(g_snes->apu, 65536u);
    for (int i = 0; i < 256 && g_snes->apu->outPorts[2] != g_ram[0x009F]; i++) {
        apu_runCycles(g_snes->apu, 256u);
    }
    /* Discard silence generated during the multi-frame pack upload and prime 1 frame of music */
    if (g_snes->apu->dsp) {
        dsp_output_ring_set_write(g_snes->apu->dsp,
                                  __atomic_load_n(&g_snes->apu->dsp->sampleRead, __ATOMIC_ACQUIRE));
    }
    apu_runCycles(g_snes->apu, 17088u);

    g_snes->apu->portClock = saved_port_clock;
    g_snes->apu->portGuestAnchor = saved_port_guest_anchor;
    g_snes->apu->portTargetAnchor = saved_port_target_anchor;
    g_snes->apu->portLastGuest = saved_port_last_guest;
    g_snes->apu->portLastTarget = saved_port_last_target;
    g_snes->apu->portTimeValid = saved_port_time_valid;
    g_snes->apuCatchupCycles = saved_catchup;
    RtlApuUnlock();

    if (g_snes->cart && g_snes->cart->rom) {
        SyncToasterDogSfx(g_snes->cart->rom, g_snes->apu);
    }
    SoeSyncJukeboxRomPatches();
    snesrecomp_desktop_request_clock_reset();
}

/*
 * Apply Dog Form transformation safely during gameplay (matching $8F:946B,
 * $90:CF3A/$90:CFB8 palette DMA, and $8F:B3D4/$90:817B entity animation reset
 * on Dog $7E:4F37 without corrupting $4F37 + 0x60 entity AI type ID).
 */
static void ApplyDogFormInGame(uint16_t req_val)
{
    if (!g_snes || !g_snes->cart || !g_snes->cart->rom || g_snes->cart->romSize < 0x110000u) return;
    uint8_t *rom = g_snes->cart->rom;

    uint16_t prev_req = ReadLe16(&g_ram[0x2363]);
    uint16_t ptr = ReadLe16(&rom[0x0F945Du + req_val]);
    if (ptr < 0x8000u || (uint32_t)ptr + 40u > 0x10000u) return;
    const uint8_t *st = &rom[0x0F0000u + ptr];

    if (req_val == 0x000Cu || req_val == 0x000Au) {
        EnsureToasterHudTablesPatched(rom);
        SyncToasterDogSfx(rom, g_snes->apu);
        /* Ensure Spot ($8F:936D) keeps a valid Ring Menu sprite ($CB:1867) across map loads */
        if (rom[0x0F936Du] == 0 && rom[0x0F936Eu] == 0 && rom[0x0F936Fu] == 0) {
            rom[0x0F936Du] = 0x67u;
            rom[0x0F936Eu] = 0x18u;
            rom[0x0F936Fu] = 0xCBu;
        }
    }

    WriteLe16(&g_ram[0x2363], req_val);
    WriteLe16(&g_ram[0x2443], 0x0000u);

    /* Ring Menu animated dog sprite pointer ($7E:143E..$1440); fallback if $000000 (Spot) */
    if (st[0] == 0 && st[1] == 0 && st[2] == 0) {
        g_ram[0x143E] = 0x67;
        g_ram[0x143F] = 0x18;
        g_ram[0x1440] = 0xCB;
    } else {
        g_ram[0x143E] = st[0];
        g_ram[0x143F] = st[1];
        g_ram[0x1440] = st[2];
    }

    /* Dog entity HUD portrait metasprite ($7E:4FA5..$4FA7); $CC:3C8E (Spot) is an
     * unfinished off-center circle in ROM, so use $CC:3C72 with Spot's $B54B palette. */
    if (req_val == 0x000Au && st[3] == 0x8Eu && st[4] == 0x3Cu && st[5] == 0xCCu) {
        g_ram[0x4FA5] = 0x72u;
        g_ram[0x4FA6] = 0x3Cu;
        g_ram[0x4FA7] = 0xCCu;
    } else {
        g_ram[0x4FA5] = st[3];
        g_ram[0x4FA6] = st[4];
        g_ram[0x4FA7] = st[5];
    }

    /* Attack/ability stats, animation IDs, and bark/SFX entries ($8F:949A..$8F:9502) */
    static const uint16_t kFieldTargets[16] = {
        0x0A83, 0x0AA2, 0x0AA4, 0x0AA6, 0x0AB8, 0x0AB0, 0x0AA8, 0x0AAA,
        0x0AAC, 0x0AAE, 0x1443, 0x1445, 0x1447, 0x1449, 0x144B, 0x144D
    };
    for (int i = 0; i < 16; i++) {
        uint16_t v = ReadLe16(&st[8 + i * 2]);
        WriteLe16(&g_ram[kFieldTargets[i]], v);
    }

    WriteLe16(&g_ram[0x4FAB], 0x0E0Eu);
    WriteLe16(&g_ram[0x4F43], 0x000Eu);

    /* Palette pointer ($0A79 / $1286) & CGRAM palette 7 upload ($90:CF3A + $90:CFB8) */
    uint16_t tint = ReadLe16(&g_ram[0x23CF]);
    uint16_t pal_ptr = (tint != 0) ? (uint16_t)(tint + 0xA7CBu) : ReadLe16(&st[6]);
    WriteLe16(&g_ram[0x0A79], pal_ptr);
    WriteLe16(&g_ram[0x1286], pal_ptr);

    uint32_t pal_rom = 0x100000u + pal_ptr + 2u;
    if (pal_rom + 30u <= g_snes->cart->romSize) {
        memcpy(&g_ram[0x6369], &rom[pal_rom], 30u);
        if (g_snes->ppu) {
            for (int c = 0; c < 15; c++) {
                g_snes->ppu->cgram[0xF1 + c] = ReadLe16(&rom[pal_rom + (uint32_t)c * 2u]);
            }
        }
        uint16_t qidx = ReadLe16(&g_ram[0x128E]);
        if (qidx < 0x00E0u) {
            WriteLe16(&g_ram[0x1292u + qidx], (uint16_t)(pal_ptr + 2u));
            g_ram[0x1294u + qidx] = 0x90u;
            WriteLe16(&g_ram[0x1295u + qidx], 0x00F1u);
            WriteLe16(&g_ram[0x1297u + qidx], 0x001Eu);
            qidx = (uint16_t)(qidx + 7u);
            if (qidx >= 0x00E0u) qidx = 0u;
            WriteLe16(&g_ram[0x128E], qidx);
        }
    }

    /* Refresh Dog entity ($7E:4F37) visual animation & sprite slots ($90:817B)
     * without running the $8F:B3D4 room-load reset (which sets $4F47 bit $0020
     * and zeroes AI state, freezing the dog in place until room transition). */
    uint16_t anim_id = ReadLe16(&g_ram[0x0AA2]);
    uint32_t anim_off = 0x040000u + anim_id;
    if (anim_off + 3u <= g_snes->cart->romSize) {
        uint16_t ptr_lo = ReadLe16(&rom[anim_off]);
        WriteLe16(&g_ram[0x4F37 + 0x03], ptr_lo);
        WriteLe16(&g_ram[0x4F37 + 0x00], ptr_lo);
        WriteLe16(&g_ram[0x4F37 + 0x0E], 0x0000u);
        g_ram[0x4F37 + 0x02] = rom[anim_off + 2u];
        g_ram[0x4F37 + 0x05] = 0x01u;
    }

    WriteLe16(&g_ram[0x4F37 + 0x10], (uint16_t)(ReadLe16(&g_ram[0x4F37 + 0x10]) & ~0x0020u));
    FreeDogSpriteSlot(0x4F37 + 0x6A);
    FreeDogSpriteSlot(0x4F37 + 0x6C);

    /* Apply/remove Toaster stat bonus cleanly ($8F:866B) without clobbering Max Lv99 debug stats */
    if (prev_req != 0x000Cu && req_val == 0x000Cu) {
        uint16_t atk = ReadLe16(&g_ram[0x0A89]);
        uint16_t def = ReadLe16(&g_ram[0x0A8B]);
        if (atk < 900u) WriteLe16(&g_ram[0x0A89], (uint16_t)(atk + 0x0050u));
        if (def < 900u) WriteLe16(&g_ram[0x0A8B], (uint16_t)(def + 0x00FAu));
    } else if (prev_req == 0x000Cu && req_val != 0x000Cu) {
        uint16_t atk = ReadLe16(&g_ram[0x0A89]);
        uint16_t def = ReadLe16(&g_ram[0x0A8B]);
        if (atk > 0x0050u && atk < 900u) WriteLe16(&g_ram[0x0A89], (uint16_t)(atk - 0x0050u));
        if (def > 0x00FAu && def < 900u) WriteLe16(&g_ram[0x0A8B], (uint16_t)(def - 0x00FAu));
    }
}

/*
 * Debug: Cycle Dog Form:
 * 0: Wolf (Prehistoria)  -> $8F:945D[0x0002] ($8F:9395)
 * 1: Greyhound (Antiqua) -> $8F:945D[0x0006] ($8F:93E5)
 * 2: Poodle (Gothica)    -> $8F:945D[0x0008] ($8F:940D)
 * 3: Toaster (Omnitopia) -> $8F:945D[0x000C] ($8F:9435)
 * 4: Spot (Podunk)       -> $8F:945D[0x000A] ($8F:936D)
 */
static void DebugCycleDogForm(int direction)
{
    s_dog_form = (s_dog_form + direction + 5) % 5;
    uint16_t req_val = kDogFormReqIds[s_dog_form];

    if ((g_ram[0x0B29] & 0x03) == 0x03) {
        ApplyDogFormInGame(req_val);
    } else {
        WriteLe16(&g_ram[0x2443], req_val);
        WriteLe16(&g_ram[0x2363], req_val);
    }

    snprintf(s_debug_toast, sizeof(s_debug_toast), "Dog: %s", kDogFormNames[s_dog_form]);
    s_debug_toast_timer = 90;
}

static char s_custom_map_names[256][64];
static bool s_custom_map_names_loaded = false;

static void LoadCustomMapNames(void)
{
    if (s_custom_map_names_loaded) return;
    s_custom_map_names_loaded = true;
    memset(s_custom_map_names, 0, sizeof(s_custom_map_names));
    FILE *f = fopen("Configs/map_names.txt", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        int id = atoi(line);
        if (id < 0 || id >= 256) continue;
        char *val = eq + 1;
        size_t len = strlen(val);
        while (len > 0 && (val[len - 1] == '\n' || val[len - 1] == '\r')) {
            val[--len] = '\0';
        }
        snprintf(s_custom_map_names[id], sizeof(s_custom_map_names[id]), "%s", val);
    }
    fclose(f);
}

static void SaveCustomMapNames(void)
{
    soe_mkdir("Configs");
    FILE *f = fopen("Configs/map_names.txt", "w");
    if (!f) return;
    for (int i = 0; i < 256; i++) {
        if (s_custom_map_names[i][0] != '\0') {
            fprintf(f, "%d=%s\n", i, s_custom_map_names[i]);
        }
    }
    fclose(f);
}

/*
 * Debug Warp To Map Name Table (IDs 0x00..0x7E; 0x7F is excluded as an invalid Mode 7 pointer).
 *
 * Credits & Acknowledgments:
 * Map ID documentation, room mappings, and map name suggestions are based on work by the
 * Secret of Evermore reverse-engineering community:
 *   - black-sliver (Evermizer)
 *   - millanzarreta (soerominfo)
 *   - maluramichael (secret-of-evermore-re)
 *   - Data Crystal
 *   - The Cutting Room Floor (TCRF)
 */
static const char *GetMapName(uint16_t map_id)
{
    LoadCustomMapNames();
    if (map_id < 256 && s_custom_map_names[map_id][0] != '\0') {
        return s_custom_map_names[map_id];
    }
    switch (map_id) {
    case 0x00: return "Omnitopia - Guardbot Gauntlet";
    case 0x01: return "Crustacia - Outside Blimp's Hut";
    case 0x02: return "Podunk - The Mansion (1965)";
    case 0x03: return "Podunk - The Mansion (1995)";
    case 0x04: return "Prehistoria - Jungle East of Fire Eyes' Village";
    case 0x05: return "Antiqua - Crossroads (Horace's Camp / Collossia / Pyramid)";
    case 0x06: return "Antiqua - Great Pyramid (Exterior)";
    case 0x07: return "Antiqua - River (Dog Jump Bridge)";
    case 0x08: return "Antiqua - Sacred Dog Statue (A)";
    case 0x09: return "Podunk - The Mansion (1995 Intro)";
    case 0x0A: return "Antiqua - Nobilia Market";
    case 0x0B: return "Antiqua - Nobilia Fountain";
    case 0x0C: return "Antiqua - Nobilia Inn";
    case 0x0D: return "Antiqua - Sacred Dog Statue (B)";
    case 0x0E: return "Gothica - Ebon Keep Dining Hall";
    case 0x0F: return "Gothica - Ebon Keep (Naris's Room)";
    case 0x10: return "Prehistoria - Inside Big Bug - 3";
    case 0x11: return "Podunk - Intro Rooftop (Cutscene)";
    case 0x12: return "Gothica - Ebon Keep Great Hall";
    case 0x13: return "Gothica - Ebon Keep Hall (Alt)";
    case 0x14: return "Gothica - Tinker's Lab";
    case 0x15: return "Evermore Room";
    case 0x16: return "Prehistoria - Graveyard / Bone Dragon";
    case 0x17: return "Evermore Room";
    case 0x18: return "Prehistoria - Inside Big Bug - 1";
    case 0x19: return "Antiqua - Great Pyramid (Lower Floor)";
    case 0x1A: return "Evermore Room";
    case 0x1B: return "Gothica - Ebon Keep Sewers";
    case 0x1C: return "Evermore Room";
    case 0x1D: return "Evermore Room";
    case 0x1E: return "Antiqua - Nobilia Colosseum Plaza";
    case 0x1F: return "Evermore Room";
    case 0x20: return "Gothica - Dark Forest (Timberdrake Boss)";
    case 0x21: return "Gothica - Waterfall Before Dark Forest";
    case 0x22: return "Cutscene / Black Screen (Birds Audio)";
    case 0x23: return "Antiqua - Hall of Collosia (Three Pressure Plates)";
    case 0x24: return "Antiqua - Hall of Collosia (Timed Bridge Switch)";
    case 0x25: return "Prehistoria - Fire Eyes' Village";
    case 0x26: return "Prehistoria - Fire Eyes' Village (Secret Area)";
    case 0x27: return "Prehistoria - Mammoth Graveyard";
    case 0x28: return "Antiqua - Hall of Collosia (Collapsing Bridge)";
    case 0x29: return "Antiqua - Hall of Collosia (Entrance)";
    case 0x2A: return "Antiqua - Hall of Collosia (Megataur Boss)";
    case 0x2B: return "Antiqua - Hall of Collosia (Exterior)";
    case 0x2C: return "Antiqua - Hall of Collosia (Minitaur Boss)";
    case 0x2D: return "Antiqua - Hall of Collosia (Mad Monk Room)";
    case 0x2E: return "Antiqua - Blimp's Cave (Crustacia)";
    case 0x2F: return "Antiqua - Horace's Camp";
    case 0x30: return "Antiqua - Crustacia Item Shop";
    case 0x31: return "Podunk, USA (Title Card)";
    case 0x32: return "Podunk - Bijou Theater (1995)";
    case 0x33: return "Prehistoria - Strongheart's Hut (Exterior)";
    case 0x34: return "Prehistoria - Strongheart's Hut (Interior)";
    case 0x35: return "Prehistoria - Jaguar Ring Shop";
    case 0x36: return "Prehistoria - Windwalker Fire Pit (Flight BG)";
    case 0x37: return "Gothica - Gomi's Tower";
    case 0x38: return "Prehistoria - Jungle South of Fire Eyes' Village";
    case 0x39: return "Gothica - Tinker's Fire Pit";
    case 0x3A: return "Antiqua - Nobilia Fire Pit";
    case 0x3B: return "Prehistoria - Volcano (Levitate Room)";
    case 0x3C: return "Prehistoria - Volcano (Two Pots Room)";
    case 0x3D: return "Prehistoria - Volcano Pipe Maze";
    case 0x3E: return "Prehistoria - Volcano (Pre-Maze Pipes)";
    case 0x3F: return "Prehistoria - Volcano (Magmar Boss)";
    case 0x40: return "Gothica - Path Before Gomi's Tower";
    case 0x41: return "Prehistoria - Jungle North of Fire Eyes' Village";
    case 0x42: return "Omnitopia - Heat Coil Room";
    case 0x43: return "Omnitopia - Access Code Computer Room";
    case 0x44: return "Omnitopia - Greenhouse";
    case 0x45: return "Omnitopia - Faces Boss Room";
    case 0x46: return "Omnitopia - Professor Ruffleberg's Lab";
    case 0x47: return "Omnitopia - Supply Room";
    case 0x48: return "Omnitopia - Exterior / Overworld";
    case 0x49: return "Omnitopia - Landing Bay (First Room)";
    case 0x4A: return "Omnitopia - Carltron's Robot (Final Boss)";
    case 0x4B: return "Antiqua - Oglin Caves";
    case 0x4C: return "Antiqua - Nobilia Palace (Outside Front Gate)";
    case 0x4D: return "Antiqua - Sacred Dog Cutscene (Pompolonius)";
    case 0x4E: return "Gothica - Ivor Tower Market";
    case 0x4F: return "Antiqua - Outside Blimp's Cave (Crustacia)";
    case 0x50: return "Prehistoria - Volcano Crater Top (A)";
    case 0x51: return "Prehistoria - Fire Eyes' Village (Alchemist)";
    case 0x52: return "Prehistoria - Volcano Crater Top (B)";
    case 0x53: return "Antiqua - Blimp's Boat River Ride";
    case 0x54: return "Omnitopia - Inn / Rest Quarters";
    case 0x55: return "Antiqua - Great Pyramid (Ground Floor)";
    case 0x56: return "Antiqua - Great Pyramid (Middle Floor)";
    case 0x57: return "Antiqua - Tiny Boss Fight";
    case 0x58: return "Antiqua - Great Pyramid (Rimsala Boss)";
    case 0x59: return "Prehistoria - Quicksand Field";
    case 0x5A: return "Prehistoria - Acid Rain Alchemist";
    case 0x5B: return "Prehistoria - Jungle East of Fire Eyes' Village (B)";
    case 0x5C: return "Prehistoria - Raptor Attack";
    case 0x5D: return "Gothica - Ebon Keep Courtyard";
    case 0x5E: return "Gothica - Ebon Keep (Verminator Boss)";
    case 0x5F: return "Gothica - Ebon Keep Maze (Near Verminator)";
    case 0x60: return "Gothica - Treasure Room Below Verminator";
    case 0x61: return "Title Screen";
    case 0x62: return "Gothica - Perceval Plank's Exhibition (Exterior)";
    case 0x63: return "Gothica - Perceval Plank's Exhibition (Side Trailer)";
    case 0x64: return "Antiqua - Tiny's Oglin Maze (Entrance)";
    case 0x65: return "Prehistoria - Swamp Maze Before Salabog";
    case 0x66: return "Prehistoria - Room Before Swamp Maze";
    case 0x67: return "Prehistoria - Bugmuck (Outside Hard Ball Cave)";
    case 0x68: return "Antiqua - Crustacia Town Map";
    case 0x69: return "Prehistoria - Cliffside Before Volcano Crater";
    case 0x6A: return "Antiqua - Waterfall Drop to Oglin Cave";
    case 0x6B: return "Antiqua - Rainbow Waterfall (North of Pyramid)";
    case 0x6C: return "Gothica - Well South of Ivor Tower";
    case 0x6D: return "Antiqua - Aquagoth Boss";
    case 0x6E: return "Gothica - Ivor Tower (Stairway to Throne Room)";
    case 0x6F: return "Gothica - Ivor Tower Dining Hall";
    case 0x70: return "Evermore Room";
    case 0x71: return "Antiqua - Horace's Camp (No Water)";
    case 0x72: return "Gothica - Ivor Tower Dungeon";
    case 0x73: return "Evermore Room";
    case 0x74: return "Evermore Room";
    case 0x75: return "Evermore Room";
    case 0x76: return "Evermore Room";
    case 0x77: return "Evermore Room";
    case 0x78: return "Evermore Room";
    case 0x79: return "Evermore Room";
    case 0x7A: return "Evermore Room";
    case 0x7B: return "Evermore Room";
    case 0x7C: return "Evermore Room";
    case 0x7D: return "Antiqua - Colosseum";
    case 0x7E: return "Omnitopia - Interior Corridor / Guardbot Room";
    default:   return "Evermore Room";
    }
}



/*
 * Debug: Instantly warp player to target map number (0..255).
 * Uses native transition checklist & script opcode 0x22 ($8C:DB71) with
 * validated default entrance vectors from the map header.
 */
static void DebugExecuteWarpEx(uint16_t target_map, bool allow_cutscene_maps)
{
    if (!g_ram || !g_snes || !g_snes->cart || !g_snes->cart->rom) return;

    /* 1. Only allow in-game execution: Warping on title screen crashes entity-less loop */
    bool in_gameplay = (g_ram[0x0B29] & 0x03) == 0x03;
    if (!in_gameplay && !allow_cutscene_maps) {
        snprintf(s_debug_toast, sizeof(s_debug_toast), "Start Game First!");
        s_debug_toast_timer = 90;
        return;
    }
    if (!in_gameplay && allow_cutscene_maps) {
        g_ram[0x0B29] = 0x03;
    }

    /* 2. Guard against invalid Mode 7 crash pointer (0x7F+) if not in export mode */
    if (!allow_cutscene_maps && target_map >= 0x7Fu) {
        snprintf(s_debug_toast, sizeof(s_debug_toast), "Map %u: Invalid!", (unsigned)target_map);
        s_debug_toast_timer = 90;
        return;
    }

    if (!g_snes || !g_snes->cart || !g_snes->cart->rom) return;
    uint8_t *rom = g_snes->cart->rom;
    uint32_t off_table = 0x1F0000u + 0xFDE7u;
    uint32_t m_off = off_table + (target_map & 0xFFu) * 4u;
    if (m_off + 4u > g_snes->cart->romSize) {
        snprintf(s_debug_toast, sizeof(s_debug_toast), "Map %u: Invalid!", (unsigned)target_map);
        s_debug_toast_timer = 90;
        return;
    }
    uint32_t rom_addr = (uint32_t)rom[m_off] |
                        ((uint32_t)rom[m_off + 1] << 8) |
                        ((uint32_t)rom[m_off + 2] << 16);
    if (rom_addr < 0x800000u || rom_addr >= 0xC00000u) {
        snprintf(s_debug_toast, sizeof(s_debug_toast), "Map %u: No Data!", (unsigned)target_map);
        s_debug_toast_timer = 90;
        return;
    }

    /* 3. Read official entrance vector (spawn_x, spawn_y, room_w, room_h) */
    uint32_t file_off = rom_addr & 0x3FFFFFu;
    if (file_off + 4u > g_snes->cart->romSize) {
        snprintf(s_debug_toast, sizeof(s_debug_toast), "Map %u: OOB!", (unsigned)target_map);
        s_debug_toast_timer = 90;
        return;
    }
    uint8_t spawn_x = rom[file_off + 0];
    uint8_t spawn_y = rom[file_off + 1];
    uint8_t room_w  = rom[file_off + 2];
    uint8_t room_h  = rom[file_off + 3];

    if (room_w < 4) room_w = 4;
    if (room_h < 4) room_h = 4;

    /* If spawn coordinates are invalid or out of bounds, use safe room center */
    if (spawn_x < 2 || spawn_x >= room_w - 2) spawn_x = (uint8_t)(room_w / 2);
    if (spawn_y < 2 || spawn_y >= room_h - 2) spawn_y = (uint8_t)(room_h / 2);

    /* Opcode 0x22 ($8C:DB71) shifts byte left by 3 bits (x8); 1 tile = 16 pixels (2 * 8) */
    uint8_t byte_x = (uint8_t)(spawn_x * 2u);
    uint8_t byte_y = (uint8_t)(spawn_y * 2u);

    /* 4. Ensure Boy HP ($7E:0A33) and Dog HP ($7E:0A7D) are full and healthy */
    uint16_t boy_hp = (uint16_t)(g_ram[0x0A33] | (g_ram[0x0A34] << 8));
    if (boy_hp == 0) {
        g_ram[0x0A33] = 100; g_ram[0x0A34] = 0;
        g_ram[0x0A35] = 100; g_ram[0x0A36] = 0;
    }
    uint16_t dog_hp = (uint16_t)(g_ram[0x0A7D] | (g_ram[0x0A7E] << 8));
    if (dog_hp == 0) {
        g_ram[0x0A7D] = 100; g_ram[0x0A7E] = 0;
        g_ram[0x0A7F] = 100; g_ram[0x0A80] = 0;
    }

    /* 5. Clear stuck collision / physics flags */
    g_ram[0x0F55] = 0;

    /* 6. Inject native room-warp script: Opcode 0x22 (Warp) followed by Opcode 0x26 (Fade In).
     *    Placed in unused $81:98E8..$81:98EE 0xFF padding (right after the 24-byte Walk Through
     *    Walls stub at $81:98D0..$81:98E7) so we NEVER touch the $80:9479..$80:94D8 sprite/OAM
     *    attribute lookup table (read via ORA $809479,X at $80:92AB and $80:943D). */
    if (0x0198EFu > g_snes->cart->romSize) return;
    rom[0x0198E8u] = 0x22; /* Opcode 0x22 -> $8C:DB71 native room warp */
    rom[0x0198E9u] = byte_x;
    rom[0x0198EAu] = byte_y;
    rom[0x0198EBu] = (uint8_t)(target_map & 0xFFu);
    rom[0x0198ECu] = (uint8_t)(target_map >> 8);
    rom[0x0198EDu] = 0x26; /* Opcode 0x26 -> $8C:DC18 unblank and fade screen in */
    rom[0x0198EEu] = 0x00; /* Opcode 0x00 -> terminate script thread */

    /* 7. Find first available script thread slot */
    uint16_t thread_addr = 0;
    for (uint16_t cand = 0x28FC; cand < 0x2F28; cand += 0x4F) {
        if (g_ram[cand + 3] == 0) {
            thread_addr = cand;
            break;
        }
    }
    if (thread_addr == 0) {
        thread_addr = 0x2D9D;
    }

    g_ram[thread_addr + 0] = 0xE8;
    g_ram[thread_addr + 1] = 0x98;
    g_ram[thread_addr + 2] = 0x81; /* Bank $81 */
    g_ram[thread_addr + 3] = 0x02; /* Active thread state */
    g_ram[thread_addr + 4] = 0x00;
    g_ram[thread_addr + 5] = 0x00;
    g_ram[thread_addr + 6] = 0x00;
    g_ram[thread_addr + 7] = 0x00;

    /* 8. Register in active thread queue at $7E:2F28 */
    bool found = false;
    for (int i = 0; i < 16; i++) {
        uint16_t entry = (uint16_t)(g_ram[0x2F28 + i * 2] | (g_ram[0x2F29 + i * 2] << 8));
        if (entry == thread_addr) {
            found = true;
            break;
        }
        if (entry == 0) {
            g_ram[0x2F28 + i * 2] = (uint8_t)(thread_addr & 0xFF);
            g_ram[0x2F29 + i * 2] = (uint8_t)(thread_addr >> 8);
            if (i < 15) {
                g_ram[0x2F28 + (i + 1) * 2] = 0;
                g_ram[0x2F29 + (i + 1) * 2] = 0;
            }
            found = true;
            break;
        }
    }
    if (!found) {
        g_ram[0x2F28] = (uint8_t)(thread_addr & 0xFF);
        g_ram[0x2F29] = (uint8_t)(thread_addr >> 8);
        g_ram[0x2F2A] = 0;
        g_ram[0x2F2B] = 0;
    }

    /* 9. Monitor warp transition and auto-engage Walk Through Walls for 5 seconds */
    s_warp_target_map = target_map;
    s_warp_monitor_frames = 180;
    s_warp_stuck_black_frames = 0;
    s_post_warp_walk_timer = 300;

    snprintf(s_debug_toast, sizeof(s_debug_toast), "Warp -> Map %u (%s)",
             (unsigned)target_map, GetMapName(target_map));
    s_debug_toast_timer = 90;
}

static void DebugExecuteWarp(uint16_t target_map)
{
    DebugExecuteWarpEx(target_map, false);
}

/*
 * Map Exporter: Composites and exports the currently loaded room from WRAM bank $7F,
 * VRAM 4bpp tile graphics, and CGRAM palettes into a pristine full-resolution PNG image.
 */
bool SoeExportCurrentMap(const char *out_path)
{
    if (!g_snes || !g_snes->ppu || !g_snes->cart || !g_snes->cart->rom) return false;

    uint8_t width = g_ram[0x08EE];
    uint8_t height = g_ram[0x08F0];
    if (width == 0 || height == 0 || width > 160 || height > 160) {
        fprintf(stderr, "[Map Export] Invalid room dimensions: %u x %u\n", (unsigned)width, (unsigned)height);
        return false;
    }

    int img_w = (int)width * 16;
    int img_h = (int)height * 16;
    uint32_t *pixels = (uint32_t *)calloc((size_t)img_w * (size_t)img_h, sizeof(uint32_t));
    if (!pixels) return false;

    /* Decode CGRAM palettes: 16 colors per palette, 8 BG palettes (0..7) */
    uint32_t cgram_rgba[256];
    for (int i = 0; i < 256; i++) {
        uint16_t c = g_snes->ppu->cgram[i];
        uint8_t r = (uint8_t)(((c & 0x1F) * 255) / 31);
        uint8_t g = (uint8_t)((((c >> 5) & 0x1F) * 255) / 31);
        uint8_t b = (uint8_t)((((c >> 10) & 0x1F) * 255) / 31);
        /* RGBA byte layout for stbi_write_png: R in lowest byte, then G, B, A */
        cgram_rgba[i] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000u;
    }

    /* Initialize background to backdrop color */
    uint32_t backdrop = cgram_rgba[0];
    for (size_t p = 0; p < (size_t)(img_w * img_h); p++) {
        pixels[p] = backdrop;
    }

    /* VRAM char base from PPU registers for BG1 (layer 0) and BG2 (layer 1) */
    const uint16_t *vram = g_snes->ppu->vram;
    uint32_t char_bases[2];
    char_bases[0] = (uint32_t)PPU_bgTileAdr(g_snes->ppu, 0);
    char_bases[1] = (uint32_t)PPU_bgTileAdr(g_snes->ppu, 1);
    fprintf(stderr, "[Map Export] BG1 char base: 0x%X, BG2 char base: 0x%X\n",
            (unsigned)char_bases[0], (unsigned)char_bases[1]);

    for (int ty = 0; ty < (int)height; ty++) {
        for (int tx = 0; tx < (int)width; tx++) {
            uint32_t cell = (uint32_t)(ty * (int)width + tx);
            uint32_t ptr_idx = cell * 2u;
            if (ptr_idx + 2u > 0x10000u) continue;

            uint16_t ptr = (uint16_t)(g_ram[0x10000u + ptr_idx] |
                                     (g_ram[0x10000u + ptr_idx + 1u] << 8));
            if ((uint32_t)ptr + 4u > 0x10000u) continue;

            uint16_t bg1_word = (uint16_t)(g_ram[0x10000u + ptr + 0] |
                                          (g_ram[0x10000u + ptr + 1] << 8));
            uint16_t bg2_word = (uint16_t)(g_ram[0x10000u + ptr + 2] |
                                          (g_ram[0x10000u + ptr + 3] << 8));

            int cell_px = tx * 16;
            int cell_py = ty * 16;

            /* Render Mode 1 BG2 (ground) first, then BG1 (foreground) on top */
            uint16_t layers[2] = { bg2_word, bg1_word };
            for (int l = 0; l < 2; l++) {
                uint16_t word = layers[l];
                uint16_t base_tile = word & 0x03FFu;
                uint16_t pbase = ((word >> 10) & 7u) * 16u;
                bool hf = (word & 0x4000u) != 0;
                bool vf = (word & 0x8000u) != 0;
                uint32_t char_base = (l == 0) ? char_bases[1] : char_bases[0]; /* layers[0]=BG2, layers[1]=BG1 */

                for (int qy = 0; qy < 2; qy++) {
                    for (int qx = 0; qx < 2; qx++) {
                        int eff_qx = hf ? (1 - qx) : qx;
                        int eff_qy = vf ? (1 - qy) : qy;
                        uint16_t tile = (uint16_t)((base_tile + eff_qx + eff_qy * 16) & 0x03FFu);

                        /* Each 4bpp tile is 16 words (32 bytes); wrap within 32K-word VRAM */
                        uint32_t tile_addr = (char_base + (uint32_t)tile * 16u) & 0x7FFFu;
                        if (tile_addr + 16u > 0x8000u) continue;

                        int ox = cell_px + qx * 8;
                        int oy = cell_py + qy * 8;

                        for (int py = 0; py < 8; py++) {
                            int eff_py = vf ? (7 - py) : py;
                            uint16_t p01 = vram[tile_addr + eff_py];
                            uint16_t p23 = vram[tile_addr + 8 + eff_py];

                            int dy = oy + py;
                            if (dy < 0 || dy >= img_h) continue;

                            for (int px_x = 0; px_x < 8; px_x++) {
                                int eff_px = hf ? (7 - px_x) : px_x;
                                int bit = 7 - eff_px;

                                uint8_t c = (uint8_t)(((p01 >> bit) & 1) |
                                                     (((p01 >> (bit + 8)) & 1) << 1) |
                                                     (((p23 >> bit) & 1) << 2) |
                                                     (((p23 >> (bit + 8)) & 1) << 3));
                                if (c != 0) {
                                    int dx = ox + px_x;
                                    if (dx >= 0 && dx < img_w) {
                                        pixels[dy * img_w + dx] = cgram_rgba[pbase + c];
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    int ret = stbi_write_png(out_path, img_w, img_h, 4, pixels, img_w * 4);
    free(pixels);
    if (!ret) {
        fprintf(stderr, "[Map Export] Failed to write PNG to: %s\n", out_path);
        return false;
    }
    fprintf(stderr, "[Map Export] Successfully exported map (%ux%u -> %dx%d px) to %s\n",
            (unsigned)width, (unsigned)height, img_w, img_h, out_path);
    return true;
}

/*
 * Wait until a warped-to map finishes decompressing WRAM $7F, streaming VRAM
 * tilesets, populating CGRAM BG palettes, and unblanking at full brightness.
 * Large/complex maps (e.g. Great Pyramid Exterior, Oglin Caves, Gomi's Tower,
 * Swamp Maze) can take 140-200+ frames to finish loading; capping at 120 frames
 * previously exported them mid-load with stale/partial VRAM tiles.
 */
static void SoeWaitForMapReady(uint16_t target_map)
{
    int ready_frames = 0;
    for (int f = 0; f < 400; f++) {
        WatchdogFrameStart();
        GameRunOneFrame();
        if (f >= 60 && GetLoadedMapId() == target_map &&
            g_snes && g_snes->ppu &&
            !PPU_forcedBlank(g_snes->ppu) &&
            PPU_brightness(g_snes->ppu) >= 15) {
            uint8_t w = g_ram[0x08EE];
            uint8_t h = g_ram[0x08F0];
            uint16_t stride = (uint16_t)(g_ram[0x0F46] | (g_ram[0x0F47] << 8));
            int non_zero = 0;
            for (int ci = 16; ci < 128; ci++) {
                if (g_snes->ppu->cgram[ci] != 0) non_zero++;
            }
            if (w > 0 && h > 0 && stride == (uint16_t)w * 2u && non_zero >= 8) {
                ready_frames++;
                if (ready_frames >= 2) break;
            } else {
                ready_frames = 0;
            }
        } else {
            ready_frames = 0;
        }
    }
}

/*
 * Export All Maps: Creates ./MAPS/ folder and iterates through maps 0..126,
 * warping to each, waiting for decompression, and writing MAPS/map_%03u.png.
 */
#ifdef __ANDROID__
static SDL_Window   *s_android_ext_win = NULL;
static SDL_Renderer *s_android_ext_ren = NULL;
static SDL_Texture  *s_android_ext_tex = NULL;
static uint32_t      s_android_ext_fb[320 * 180];

static void SoeAndroidDrawExtractProgress(const char *subtitle, int pct)
{
    if (!s_android_ext_ren || !s_android_ext_tex) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    const int W = 320, H = 180;
    /* Dark Evertopia backdrop */
    for (int i = 0; i < W * H; i++) {
        s_android_ext_fb[i] = 0xFF0E131Cu;
    }

    /* Centered card panel */
    const int px = 28, py = 36, pw = 264, ph = 108;
    for (int y = py; y < py + ph; y++) {
        for (int x = px; x < px + pw; x++) {
            bool border = (x == px || x == px + pw - 1 || y == py || y == py + ph - 1);
            s_android_ext_fb[y * W + x] = border ? 0xFF5A78A8u : 0xFF182233u;
        }
    }

    const char *title = "ONE-TIME ASSET EXTRACTION";
    int tw = MeasureSmallText(title);
    DrawSmallText(s_android_ext_fb, W, H, (W - tw) / 2, py + 14, title, 0xFFFFFFFFu);

    const char *note = "Preparing sprites, tilesets & maps (first launch only)";
    int nw = MeasureSmallText(note);
    DrawSmallText(s_android_ext_fb, W, H, (W - nw) / 2, py + 28, note, 0xFF9EB4D4u);

    if (subtitle && subtitle[0]) {
        int sw = MeasureSmallText(subtitle);
        DrawSmallText(s_android_ext_fb, W, H, (W - sw) / 2, py + 48, subtitle, 0xFFD8E8FFu);
    }

    /* Progress bar */
    const int bx = px + 22, by = py + 64, bw = pw - 44, bh = 14;
    int fill_w = (bw - 2) * pct / 100;
    for (int y = by; y < by + bh; y++) {
        for (int x = bx; x < bx + bw; x++) {
            bool border = (x == bx || x == bx + bw - 1 || y == by || y == by + bh - 1);
            if (border) {
                s_android_ext_fb[y * W + x] = 0xFF7FA6DCu;
            } else if (x - (bx + 1) < fill_w) {
                s_android_ext_fb[y * W + x] = 0xFF3CC8FFu;
            } else {
                s_android_ext_fb[y * W + x] = 0xFF0B1018u;
            }
        }
    }

    char pct_buf[32];
    snprintf(pct_buf, sizeof(pct_buf), "%d%%", pct);
    int pct_w = MeasureSmallText(pct_buf);
    DrawSmallText(s_android_ext_fb, W, H, (W - pct_w) / 2, by + bh + 8, pct_buf, 0xFF70F0B8u);

    SDL_UpdateTexture(s_android_ext_tex, NULL, s_android_ext_fb, W * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(s_android_ext_ren, 14, 19, 28, 255);
    SDL_RenderClear(s_android_ext_ren);
    SDL_RenderTexture(s_android_ext_ren, s_android_ext_tex, NULL, NULL);
    SDL_RenderPresent(s_android_ext_ren);
    SDL_PumpEvents();
}
#endif

int SoeExportAllMaps(void)
{
    WatchdogSetEnabled(0);
    soe_mkdir("Extract");
    soe_mkdir("Extract/MAPS");
    FILE *init_pf = fopen("Extract/MAPS/progress.txt", "w");
    if (init_pf) {
        fprintf(init_pf, "0 127 0\n");
        fclose(init_pf);
    }

    /* Ensure gameplay state flags so engine processes warps cleanly */
    /* If booting directly into export before the game engine initializes (frame < 50),
     * simulate 360 frames so the engine finishes opening cinematics into attract mode */
    extern int snes_frame_counter;
    if (snes_frame_counter < 50) {
        for (int boot_f = 0; boot_f < 360; boot_f++) {
            WatchdogFrameStart();
            GameRunOneFrame();
        }
    }
    g_ram[0x0B29] = 0x03;
    if ((g_ram[0x0A33] | (g_ram[0x0A34] << 8)) == 0) {
        g_ram[0x0A33] = 100; g_ram[0x0A35] = 100;
        g_ram[0x0A7D] = 100; g_ram[0x0A7F] = 100;
    }

    uint16_t orig_map = GetLoadedMapId();
    uint16_t orig_boy_x = (uint16_t)(g_ram[0x0A06] | (g_ram[0x0A07] << 8));
    uint16_t orig_boy_y = (uint16_t)(g_ram[0x0A08] | (g_ram[0x0A09] << 8));

    /* Capture a clean gameplay baseline snapshot once we are in-game (not on title/Map 97) */
    size_t base_cap = RtlSaveSnapshotToMemory(NULL, 0);
    uint8_t *base_snap = base_cap ? (uint8_t *)malloc(base_cap) : NULL;
    size_t base_size = 0;
    if (base_snap && orig_map != 0 && orig_map != 97) {
        base_size = RtlSaveSnapshotToMemory(base_snap, base_cap);
    }

    int exported = 0;
    if (!g_snes || !g_snes->cart || !g_snes->cart->rom) {
        free(base_snap);
        return 0;
    }
    uint8_t *rom = g_snes->cart->rom;
    uint32_t off_table = 0x1F0000u + 0xFDE7u;

    for (uint16_t m = 0; m <= 126; m++) {
        uint32_t m_off = off_table + (m & 0xFFu) * 4u;
        if (m_off + 4u > g_snes->cart->romSize) continue;
        uint32_t rom_addr = (uint32_t)rom[m_off] |
                            ((uint32_t)rom[m_off + 1] << 8) |
                            ((uint32_t)rom[m_off + 2] << 16);
        if (rom_addr < 0x800000u || rom_addr >= 0xC00000u) continue;

        if (base_snap && base_size > 0) {
            RtlLoadSnapshotFromMemory(base_snap, base_size);
        }
        g_ram[0x0B29] = 0x03;
        DebugExecuteWarpEx(m, true);

        /* Advance simulation frames until target map is loaded, faded in, and BG palettes populated */
        SoeWaitForMapReady(m);

        uint16_t loaded_id = GetLoadedMapId();
        if (base_snap && base_size == 0 && loaded_id != 0 && loaded_id != 97) {
            base_size = RtlSaveSnapshotToMemory(base_snap, base_cap);
        }
        char path[128];
        snprintf(path, sizeof(path), "Extract/MAPS/map_%03u.png", (unsigned)m);
        if (loaded_id == m) {
            if (SoeExportCurrentMap(path)) {
                exported++;
            }
        } else {
            fprintf(stderr, "[Map Export] Skipped Map %u (loaded Map %u instead)\n",
                    (unsigned)m, (unsigned)loaded_id);
        }
        fprintf(stderr, "[MAP_PROGRESS] %d / 127 (Map %u)\n", exported, (unsigned)m);
        fflush(stderr);

        FILE *pf = fopen("Extract/MAPS/progress.txt", "w");
        if (pf) {
            fprintf(pf, "%u 127 %u\n", (unsigned)(m + 1), (unsigned)m);
            fclose(pf);
        }
#ifdef __ANDROID__
        {
            char prog_line[64];
            int pct = 10 + ((int)(m + 1) * 90) / 127;
            if (pct > 99) pct = 99;
            snprintf(prog_line, sizeof(prog_line), "Extracting Maps (%u / 127)...", (unsigned)(m + 1));
            SoeAndroidDrawExtractProgress(prog_line, pct);
        }
#endif
    }

    if (base_snap) {
        if (orig_map <= 126) {
            RtlLoadSnapshotFromMemory(base_snap, base_size);
        }
        free(base_snap);
    }

    /* Restore original player map and coordinates */
    if (orig_map <= 126) {
        DebugExecuteWarpEx(orig_map, true);
        for (int f = 0; f < 30; f++) {
            WatchdogFrameStart();
            GameRunOneFrame();
        }
        g_ram[0x0A06] = (uint8_t)(orig_boy_x & 0xFF);
        g_ram[0x0A07] = (uint8_t)(orig_boy_x >> 8);
        g_ram[0x0A08] = (uint8_t)(orig_boy_y & 0xFF);
        g_ram[0x0A09] = (uint8_t)(orig_boy_y >> 8);
    }

    WatchdogSetEnabled(1);

    FILE *done_pf = fopen("Extract/MAPS/progress.txt", "w");
    if (done_pf) {
        fprintf(done_pf, "127 127 126\n");
        fclose(done_pf);
    }

    snprintf(s_debug_toast, sizeof(s_debug_toast), "Exported %d maps to ./Extract/MAPS/!", exported);
    s_debug_toast_timer = 240;
    return exported;
}

#ifdef __ANDROID__
void SoeAndroidFirstBootExtract(void (*extract_cache_fn)(void))
{
    bool maps_ready = false;
    FILE *pf = fopen("Extract/MAPS/progress.txt", "r");
    if (pf) {
        int cur = 0, total = 0;
        if (fscanf(pf, "%d %d", &cur, &total) == 2 && cur >= 127 && total >= 127) {
            FILE *probe = fopen("Extract/MAPS/map_001.png", "rb");
            if (probe) {
                fclose(probe);
                maps_ready = true;
            }
        }
        fclose(pf);
    }

    if (maps_ready) {
        if (extract_cache_fn) extract_cache_fn();
        fprintf(stderr, "[Android] Extract/MAPS/ already present; skipping map rip.\n");
        return;
    }

    /* Open a temporary fullscreen progress window while extracting one-time assets */
    s_android_ext_win = SDL_CreateWindow("Evertopia - Asset Extraction", 0, 0, SDL_WINDOW_FULLSCREEN);
    if (s_android_ext_win) {
        s_android_ext_ren = SDL_CreateRenderer(s_android_ext_win, NULL);
        if (s_android_ext_ren) {
            s_android_ext_tex = SDL_CreateTexture(s_android_ext_ren, SDL_PIXELFORMAT_ARGB8888,
                                                  SDL_TEXTUREACCESS_STREAMING, 320, 180);
            if (s_android_ext_tex) {
                SDL_SetTextureScaleMode(s_android_ext_tex, SDL_SCALEMODE_NEAREST);
            }
        }
    }

    SoeAndroidDrawExtractProgress("Extracting Sprites & Tilesets...", 2);
    if (extract_cache_fn) extract_cache_fn();
    SoeAndroidDrawExtractProgress("Initializing Map Extractor...", 10);

    fprintf(stderr, "[Android] First-boot map extraction starting (127 maps)...\n");
    fflush(stderr);

    extern int snes_frame_counter;
    size_t boot_cap = RtlSaveSnapshotToMemory(NULL, 0);
    uint8_t *boot_snap = boot_cap ? (uint8_t *)malloc(boot_cap) : NULL;
    size_t boot_size = boot_snap ? RtlSaveSnapshotToMemory(boot_snap, boot_cap) : 0;

    int count = SoeExportAllMaps();
    SoeAndroidDrawExtractProgress("Extraction Complete! Starting Game...", 100);
    fprintf(stderr, "[Android] First-boot map extraction complete (%d maps).\n", count);
    fflush(stderr);

    if (boot_snap && boot_size > 0) {
        RtlLoadSnapshotFromMemory(boot_snap, boot_size);
    }
    free(boot_snap);
    snes_frame_counter = 0;
    s_debug_toast_timer = 0;

    if (s_android_ext_tex) { SDL_DestroyTexture(s_android_ext_tex); s_android_ext_tex = NULL; }
    if (s_android_ext_ren) { SDL_DestroyRenderer(s_android_ext_ren); s_android_ext_ren = NULL; }
    if (s_android_ext_win) { SDL_DestroyWindow(s_android_ext_win); s_android_ext_win = NULL; }
}
#endif


/* ── Music Dump ─────────────────────────────────────────────────────────
 * Mirrors SoeExportAllMaps: plays every jukebox song headlessly, captures
 * the S-DSP output stream and encodes it to Music/song_XX.mp3 with LAME
 * (loaded at runtime from libmp3lame; no build-time dependency).
 */
typedef void *(*lame_init_fn)(void);
typedef int (*lame_set_int_fn)(void *, int);
typedef int (*lame_init_params_fn)(void *);
typedef int (*lame_encode_il_fn)(void *, const short *, int, unsigned char *, int);
typedef int (*lame_flush_fn)(void *, unsigned char *, int);
typedef int (*lame_close_fn)(void *);

static void *SoeOpenLame(void)
{
#if defined(_WIN32)
    return NULL;
#else
    const char *names[] = { "libmp3lame.so.0", "libmp3lame.so", "libmp3lame.dylib", NULL };
    for (int i = 0; names[i]; i++) {
        void *h = dlopen(names[i], RTLD_NOW);
        if (h) return h;
    }
    return NULL;
#endif
}

int SoeExportAllMusic(void)
{
    WatchdogSetEnabled(0);
    soe_mkdir("Extract");
    soe_mkdir("Extract/MUSIC");
    FILE *init_pf = fopen("Extract/MUSIC/progress.txt", "w");
    if (init_pf) { fprintf(init_pf, "0 1 0\n"); fclose(init_pf); }

#if defined(_WIN32)
    fprintf(stderr, "[Music Exporter] MP3 encoding is not supported on this platform\n");
    return 0;
#else
    void *lib = SoeOpenLame();
    if (!lib) {
        fprintf(stderr, "[Music Exporter] libmp3lame not found (install libmp3lame0)\n");
        return 0;
    }
    lame_init_fn l_init = (lame_init_fn)soe_dlsym(lib, "lame_init");
    lame_set_int_fn l_set_rate_in = (lame_set_int_fn)soe_dlsym(lib, "lame_set_in_samplerate");
    lame_set_int_fn l_set_rate_out = (lame_set_int_fn)soe_dlsym(lib, "lame_set_out_samplerate");
    lame_set_int_fn l_set_ch = (lame_set_int_fn)soe_dlsym(lib, "lame_set_num_channels");
    lame_set_int_fn l_set_br = (lame_set_int_fn)soe_dlsym(lib, "lame_set_brate");
    lame_init_params_fn l_params = (lame_init_params_fn)soe_dlsym(lib, "lame_init_params");
    lame_encode_il_fn l_enc = (lame_encode_il_fn)soe_dlsym(lib, "lame_encode_buffer_interleaved");
    lame_flush_fn l_flush = (lame_flush_fn)soe_dlsym(lib, "lame_encode_flush");
    lame_close_fn l_close = (lame_close_fn)soe_dlsym(lib, "lame_close");
    if (!l_init || !l_set_rate_in || !l_set_rate_out || !l_set_ch || !l_set_br ||
        !l_params || !l_enc || !l_flush || !l_close) {
        fprintf(stderr, "[Music Exporter] libmp3lame is missing required symbols\n");
        return 0;
    }

    extern int snes_frame_counter;
    if (snes_frame_counter < 50) {
        for (int boot_f = 0; boot_f < 900; boot_f++) {
            WatchdogFrameStart();
            GameRunOneFrame();
        }
        if (snes_frame_counter <= 20) snes_frame_counter = 21;
    }
    g_ram[0x0B29] = 0x03;

    BuildJukeboxSongList();
    int total = s_jukebox_song_count;
    if (total <= 0 || !g_snes || !g_snes->apu || !g_snes->apu->dsp) return 0;

    int seconds = 90;
    const char *sec_env = getenv("SOE_MUSIC_SECONDS");
    if (sec_env && atoi(sec_env) >= 5) seconds = atoi(sec_env);
    const int fade_s = 3;
    const uint32_t rate = 32040;
    const uint32_t max_frames = (uint32_t)seconds * rate;
    const uint32_t fade_frames = (uint32_t)fade_s * rate;

    Dsp *dsp = g_snes->apu->dsp;
    int16_t *pcm = (int16_t *)malloc((size_t)max_frames * 2 * sizeof(int16_t));
    size_t mp3_cap = (size_t)max_frames * 5 / 4 + 7200 + 1024 * 64;
    unsigned char *mp3buf = (unsigned char *)malloc(mp3_cap);
    if (!pcm || !mp3buf) { free(pcm); free(mp3buf); return 0; }

    int exported = 0;
    for (int idx = 0; idx < total; idx++) {
        uint8_t song_id = s_jukebox_songs[idx];

        /* Queue the song and let the engine process it. */
        s_pending_jukebox_song = (int)song_id;
        for (int f = 0; f < 600 && s_pending_jukebox_song >= 0; f++) {
            WatchdogFrameStart();
            GameRunOneFrame();
        }
        s_pending_jukebox_song = -1;

        /* Capture the DSP output stream while the song plays. */
        /* No audio device is consuming the ring in export mode, so drain it
         * ourselves every frame or the DSP stops writing once it fills. */
        dsp->sampleRead = dsp->sampleWrite;
        uint32_t last = dsp->sampleWrite;
        uint32_t got = 0;
        uint32_t guard_frames = 0;
        const uint32_t max_sim_frames = (uint32_t)seconds * 120u + 600u;
        while (got < max_frames && guard_frames++ < max_sim_frames) {
            WatchdogFrameStart();
            GameRunOneFrame();
            uint32_t now = dsp->sampleWrite;
            uint32_t n = now - last;
            if (n > DSP_SAMPLE_RING) {            /* ring overrun: skip lost span */
                last = now - DSP_SAMPLE_RING;
                n = DSP_SAMPLE_RING;
            }
            for (uint32_t k = 0; k < n && got < max_frames; k++, got++) {
                uint32_t ri = ((last + k) & (DSP_SAMPLE_RING - 1)) * 2;
                pcm[(size_t)got * 2]     = dsp->sampleBuffer[ri];
                pcm[(size_t)got * 2 + 1] = dsp->sampleBuffer[ri + 1];
            }
            last = now;
            dsp->sampleRead = now;
        }

        /* Fade out the tail so the cut-off isn't abrupt. */
        uint32_t ff = fade_frames < got ? fade_frames : got;
        for (uint32_t k = 0; k < ff; k++) {
            uint32_t pos = got - ff + k;
            int32_t gain = (int32_t)(1024u - (1024u * k) / ff);
            pcm[(size_t)pos * 2]     = (int16_t)(((int32_t)pcm[(size_t)pos * 2] * gain) >> 10);
            pcm[(size_t)pos * 2 + 1] = (int16_t)(((int32_t)pcm[(size_t)pos * 2 + 1] * gain) >> 10);
        }

        void *gf = l_init();
        bool ok = false;
        if (gf) {
            l_set_rate_in(gf, (int)rate);
            l_set_rate_out(gf, 32000);  /* MPEG-1 rate; LAME resamples 32040 -> 32000 */
            l_set_ch(gf, 2);
            l_set_br(gf, 192);
            if (l_params(gf) >= 0) {
                int w = l_enc(gf, pcm, (int)got, mp3buf, (int)mp3_cap);
                if (w >= 0) {
                    int w2 = l_flush(gf, mp3buf + w, (int)(mp3_cap - (size_t)w));
                    if (w2 >= 0) {
                        char path[128];
                        snprintf(path, sizeof(path), "Extract/MUSIC/song_%02X.mp3", (unsigned)song_id);
                        FILE *mf = fopen(path, "wb");
                        if (mf) {
                            ok = fwrite(mp3buf, 1, (size_t)(w + w2), mf) == (size_t)(w + w2);
                            fclose(mf);
                        }
                    }
                }
            }
            l_close(gf);
        }
        if (ok) exported++;

        fprintf(stderr, "[MUSIC_PROGRESS] %d / %d (Song %u)\n", idx + 1, total, (unsigned)song_id);
        fflush(stderr);
        FILE *pf = fopen("Extract/MUSIC/progress.txt", "w");
        if (pf) { fprintf(pf, "%d %d %u\n", idx + 1, total, (unsigned)song_id); fclose(pf); }
    }

    free(pcm);
    free(mp3buf);
    WatchdogSetEnabled(1);
    snprintf(s_debug_toast, sizeof(s_debug_toast), "Exported %d songs to ./Extract/MUSIC/!", exported);
    s_debug_toast_timer = 240;
    return exported;
#endif
}

typedef struct {
    SaveLoadInfo base;
    uint8_t *buf;
    size_t pos;
    size_t cap;
    bool saving;
} SoeApuMemStream;

static void SoeApuMemFunc(SaveLoadInfo *info, void *data, size_t data_size)
{
    SoeApuMemStream *st = (SoeApuMemStream *)info;
    if (st->pos + data_size > st->cap) return;
    if (st->saving) {
        memcpy(st->buf + st->pos, data, data_size);
    } else {
        memcpy(data, st->buf + st->pos, data_size);
    }
    st->pos += data_size;
}

int SoeExportAllSpc(void)
{
    WatchdogSetEnabled(0);
    soe_mkdir("Extract");
    soe_mkdir("Extract/SPC");
    FILE *init_pf = fopen("Extract/SPC/progress.txt", "w");
    if (init_pf) { fprintf(init_pf, "0 1 0\n"); fclose(init_pf); }

    extern int snes_frame_counter;
    if (snes_frame_counter < 50) {
        for (int boot_f = 0; boot_f < 900; boot_f++) {
            WatchdogFrameStart();
            GameRunOneFrame();
        }
        if (snes_frame_counter <= 20) snes_frame_counter = 21;
    }
    g_ram[0x0B29] = 0x03;

    BuildJukeboxSongList();
    int total = s_jukebox_song_count;
    if (total <= 0 || !g_snes || !g_snes->apu || !g_snes->apu->dsp || !g_snes->apu->spc) {
        WatchdogSetEnabled(1);
        return 0;
    }

    uint8_t *spc_buf = (uint8_t *)malloc(66048u);
    uint8_t *clean_apu_buf = (uint8_t *)malloc(128u * 1024u);
    if (!spc_buf || !clean_apu_buf) {
        free(spc_buf);
        free(clean_apu_buf);
        WatchdogSetEnabled(1);
        return 0;
    }

    /* Save clean post-boot APU & audio driver WRAM state so an IPL-halted track cannot poison later tracks. */
    uint8_t clean_dp[12];
    uint8_t clean_0e47[16];
    memcpy(clean_dp, &g_ram[0x009A], sizeof(clean_dp));
    memcpy(clean_0e47, &g_ram[0x0E47], sizeof(clean_0e47));
    {
        SoeApuMemStream st;
        memset(&st, 0, sizeof(st));
        st.base.func = SoeApuMemFunc;
        st.buf = clean_apu_buf;
        st.cap = 128u * 1024u;
        st.saving = true;
        apu_saveload(g_snes->apu, &st.base);
    }

    int exported = 0;
    for (int idx = 0; idx < total; idx++) {
        uint8_t song_id = s_jukebox_songs[idx];

        if (g_snes->apu->spc->pc >= 0xFFC0u) {
            memcpy(&g_ram[0x009A], clean_dp, sizeof(clean_dp));
            memcpy(&g_ram[0x0E47], clean_0e47, sizeof(clean_0e47));
            SoeApuMemStream st;
            memset(&st, 0, sizeof(st));
            st.base.func = SoeApuMemFunc;
            st.buf = clean_apu_buf;
            st.cap = 128u * 1024u;
            st.saving = false;
            apu_saveload(g_snes->apu, &st.base);
            apu_clearPortQueue(g_snes->apu);
        }

        /* Queue the song and let the engine upload BRR samples/sequence and start playback. */
        s_pending_jukebox_song = (int)song_id;
        for (int f = 0; f < 600 && s_pending_jukebox_song >= 0; f++) {
            WatchdogFrameStart();
            GameRunOneFrame();
        }
        s_pending_jukebox_song = -1;

        /* Tick a few frames so the SPC700 driver enters its active playback loop. */
        Dsp *dsp = g_snes->apu->dsp;
        for (int f = 0; f < 30; f++) {
            WatchdogFrameStart();
            GameRunOneFrame();
            dsp->sampleRead = dsp->sampleWrite;
        }

        RtlApuLock();
        Apu *apu = g_snes->apu;
        Spc *spc = apu->spc;
        memset(spc_buf, 0, 66048u);

        /* 256-byte SPC700 header + ID666 metadata */
        static const char kSpcMagic[33] = "SNES-SPC700 Sound File Data v0.30";
        memcpy(spc_buf + 0x00, kSpcMagic, 33);
        spc_buf[0x21] = 0x1A;
        spc_buf[0x22] = 0x1A;
        spc_buf[0x23] = 0x1A; /* ID666 tag present */
        spc_buf[0x24] = 30;   /* Version 0.30 */
        spc_buf[0x25] = (uint8_t)(spc->pc & 0xFFu);
        spc_buf[0x26] = (uint8_t)((spc->pc >> 8) & 0xFFu);
        spc_buf[0x27] = spc->a;
        spc_buf[0x28] = spc->x;
        spc_buf[0x29] = spc->y;
        spc_buf[0x2A] = (uint8_t)(
            (spc->n ? 0x80u : 0u) |
            (spc->v ? 0x40u : 0u) |
            (spc->p ? 0x20u : 0u) |
            (spc->b ? 0x10u : 0u) |
            (spc->h ? 0x08u : 0u) |
            (spc->i ? 0x04u : 0u) |
            (spc->z ? 0x02u : 0u) |
            (spc->c ? 0x01u : 0u));
        spc_buf[0x2B] = spc->sp;

        char title[33];
        snprintf(title, sizeof(title), "Track %02d (ID $%02X)", idx + 1, (unsigned)song_id);
        size_t tlen = strlen(title);
        if (tlen > 32) tlen = 32;
        memcpy(spc_buf + 0x2E, title, tlen);

        const char *game_title = "Secret of Evermore";
        memcpy(spc_buf + 0x4E, game_title, strlen(game_title));

        const char *dumper = "Evertopia";
        memcpy(spc_buf + 0x6E, dumper, strlen(dumper));

        memcpy(spc_buf + 0xA9, "120", 3);
        memcpy(spc_buf + 0xAC, "5000", 4);

        const char *artist = "Jeremy Soule";
        memcpy(spc_buf + 0xB1, artist, strlen(artist));

        /* 64 KB ARAM dump */
        memcpy(spc_buf + 0x0100, apu->ram, 0x10000u);
        spc_buf[0x0100 + 0xF1] = (uint8_t)(
            (apu->romReadable ? 0x80u : 0u) |
            (apu->timer[0].enabled ? 0x01u : 0u) |
            (apu->timer[1].enabled ? 0x02u : 0u) |
            (apu->timer[2].enabled ? 0x04u : 0u));
        spc_buf[0x0100 + 0xF2] = apu->dspAdr;
        for (int p = 0; p < 4; p++) {
            spc_buf[0x0100 + 0xF4 + p] = apu->inPorts[p];
        }
        spc_buf[0x0100 + 0xF8] = apu->inPorts[4];
        spc_buf[0x0100 + 0xF9] = apu->inPorts[5];
        for (int t = 0; t < 3; t++) {
            spc_buf[0x0100 + 0xFA + t] = apu->timer[t].target;
        }

        /* 128 DSP registers */
        memcpy(spc_buf + 0x10100, apu->dsp->ram, 0x80u);
        RtlApuUnlock();

        char path[128];
        snprintf(path, sizeof(path), "Extract/SPC/Track %02d (ID $%02X).spc",
                 idx + 1, (unsigned)song_id);
        FILE *sf = fopen(path, "wb");
        if (sf) {
            if (fwrite(spc_buf, 1, 66048u, sf) == 66048u) {
                exported++;
            }
            fclose(sf);
        }

        fprintf(stderr, "[SPC_PROGRESS] %d / %d (Song %u)\n", idx + 1, total, (unsigned)song_id);
        fflush(stderr);
        FILE *pf = fopen("Extract/SPC/progress.txt", "w");
        if (pf) { fprintf(pf, "%d %d %u\n", idx + 1, total, (unsigned)song_id); fclose(pf); }
    }

    free(spc_buf);
    free(clean_apu_buf);
    WatchdogSetEnabled(1);
    snprintf(s_debug_toast, sizeof(s_debug_toast), "Exported %d tracks to ./Extract/SPC/!", exported);
    s_debug_toast_timer = 240;
    return exported;
}

static void SoeWriteWavHeader(FILE *f, uint32_t sample_rate, uint16_t channels, uint32_t num_frames)
{
    uint32_t data_bytes = num_frames * channels * 2u;
    uint32_t riff_size = 36u + data_bytes;
    uint32_t byte_rate = sample_rate * channels * 2u;
    uint16_t block_align = (uint16_t)(channels * 2u);
    uint16_t bits = 16u;
    uint16_t fmt_pcm = 1u;
    uint32_t fmt_size = 16u;

    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&fmt_pcm, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
}

int SoeExportAllSfx(void)
{
    WatchdogSetEnabled(0);
    soe_mkdir("Extract");
    soe_mkdir("Extract/SFX");
    FILE *init_pf = fopen("Extract/SFX/progress.txt", "w");
    if (init_pf) { fprintf(init_pf, "0 70 0\n"); fclose(init_pf); }

    extern int snes_frame_counter;
    if (snes_frame_counter < 50) {
        for (int boot_f = 0; boot_f < 900; boot_f++) {
            WatchdogFrameStart();
            GameRunOneFrame();
        }
        if (snes_frame_counter <= 20) snes_frame_counter = 21;
    }
    /* Wait until CPU is in native mode and not inside NMI/SPC busy */
    for (int f = 0; f < 120 && (g_cpu.emulation != 0 || g_ram[0x0E51] != 0); f++) {
        WatchdogFrameStart();
        GameRunOneFrame();
    }
    g_ram[0x0B29] = 0x03;

    if (!g_snes || !g_snes->apu || !g_snes->apu->dsp) {
        WatchdogSetEnabled(1);
        return 0;
    }

    Dsp *dsp = g_snes->apu->dsp;

    /* Helper macro-like inline block: safely call bank 8C without clobbering g_cpu/stack */
    #define SAFE_8C_CALL(pc24, aval, yval) do { \
        CpuState _scpu = g_cpu; \
        uint8_t _sstack[64]; \
        for (int _si = 0; _si < 64; _si++) \
            _sstack[_si] = cpu_read8(&g_cpu, 0x00, (uint16_t)(_scpu.S - (uint16_t)_si)); \
        RunBank8CCall((pc24), (aval), (yval)); \
        for (int _si = 0; _si < 64; _si++) \
            cpu_write8(&g_cpu, 0x00, (uint16_t)(_scpu.S - (uint16_t)_si), _sstack[_si]); \
        uint64_t _em = g_cpu.master_cycles; \
        uint32_t _ec = g_cpu.cycles; \
        uint64_t _ecp = g_cpu.coprocessor_master_cycles; \
        g_cpu = _scpu; \
        g_cpu.master_cycles = _em; \
        g_cpu.cycles = _ec; \
        g_cpu.coprocessor_master_cycles = _ecp; \
    } while (0)

    const int total = 70; /* $81:9901 = 0x46 (70 SFX) */
    const uint32_t rate = 32040;
    const uint32_t max_frames = rate * 2u;
    const uint32_t min_frames = rate / 4u;
    const uint32_t silence_limit = rate / 5u;
    int16_t *pcm = (int16_t *)malloc((size_t)max_frames * 2 * sizeof(int16_t));
    if (!pcm) {
        WatchdogSetEnabled(1);
        return 0;
    }

    /* Stop any intro music once before dumping SFX, while keeping base sample pack ($0E4B=01) loaded */
    SAFE_8C_CALL(0x8C81FDu, 0x001Au, 0x0000u);
    for (int f = 0; f < 10; f++) { WatchdogFrameStart(); GameRunOneFrame(); }
    SAFE_8C_CALL(0x8C81FDu, 0x002Au, 0x00A0u);
    for (int f = 0; f < 4; f++) { WatchdogFrameStart(); GameRunOneFrame(); }

    int exported = 0;
    for (int sfx_id = 0; sfx_id < total; sfx_id++) {
        /* Queue SFX and let SoeProcessPendingJukeboxSong() trigger $8C:82E3 cleanly */
        s_pending_jukebox_sfx = sfx_id;
        for (int f = 0; f < 600 && s_pending_jukebox_sfx >= 0; f++) {
            WatchdogFrameStart();
            GameRunOneFrame();
        }
        s_pending_jukebox_sfx = -1;

        dsp->sampleRead = dsp->sampleWrite;
        uint32_t last = dsp->sampleWrite;

        uint32_t captured = 0;
        uint32_t silent_frames = 0;
        for (int f = 0; f < 120 && captured < max_frames; f++) {
            WatchdogFrameStart();
            GameRunOneFrame();
            uint32_t now = dsp->sampleWrite;
            uint32_t n = now - last;
            if (n > DSP_SAMPLE_RING) {
                last = now - DSP_SAMPLE_RING;
                n = DSP_SAMPLE_RING;
            }
            for (uint32_t k = 0; k < n && captured < max_frames; k++, captured++) {
                uint32_t ri = ((last + k) & (DSP_SAMPLE_RING - 1)) * 2u;
                int16_t l = dsp->sampleBuffer[ri];
                int16_t r = dsp->sampleBuffer[ri + 1u];
                pcm[(size_t)captured * 2u] = l;
                pcm[(size_t)captured * 2u + 1u] = r;
                if (abs(l) < 8 && abs(r) < 8) {
                    silent_frames++;
                } else {
                    silent_frames = 0;
                }
            }
            last = now;
            dsp->sampleRead = now;
            if (captured >= min_frames && silent_frames >= silence_limit) {
                break;
            }
        }

        if (captured > 0) {
            char path[256];
            snprintf(path, sizeof(path), "Extract/SFX/sfx_%02X.wav", (unsigned)sfx_id);
            FILE *wf = fopen(path, "wb");
            if (wf) {
                SoeWriteWavHeader(wf, rate, 2, captured);
                fwrite(pcm, sizeof(int16_t), (size_t)captured * 2u, wf);
                fclose(wf);
                exported++;
            }
        }

        fprintf(stderr, "[SFX_PROGRESS] %d / %d (SFX %d)\n", sfx_id + 1, total, sfx_id);
        fflush(stderr);
        FILE *pf = fopen("Extract/SFX/progress.txt", "w");
        if (pf) { fprintf(pf, "%d %d %d\n", sfx_id + 1, total, sfx_id); fclose(pf); }
    }

    free(pcm);
    WatchdogSetEnabled(1);
    snprintf(s_debug_toast, sizeof(s_debug_toast), "Exported %d sound effects to ./Extract/SFX/!", exported);
    s_debug_toast_timer = 240;
    return exported;
}

/* ── First-Boot UI SFX Cache (0x00, 0x01, 0x02) & Host Audio Mixer ─────────── */

typedef struct {
    int16_t *samples;
    uint32_t frames;
    uint32_t channels;
    uint32_t sample_rate;
} SoeUiSfxClip;

typedef struct {
    bool active;
    int clip_id;
    double pos;
} SoeUiSfxVoice;

#define SOE_UI_SFX_COUNT 3
#define SOE_UI_SFX_VOICES 4

static SoeUiSfxClip s_ui_sfx[SOE_UI_SFX_COUNT];
static SoeUiSfxVoice s_ui_sfx_voices[SOE_UI_SFX_VOICES];
static SDL_Mutex *s_ui_sfx_mutex = NULL;
static bool s_ui_sfx_initialized = false;

static void SoePlayUiSfx(int clip_id)
{
    if (clip_id < 0 || clip_id >= SOE_UI_SFX_COUNT) return;
    if (!s_ui_sfx_initialized || !s_ui_sfx_mutex) return;
    if (!s_ui_sfx[clip_id].samples || s_ui_sfx[clip_id].frames == 0) return;

    if (!SDL_TryLockMutex(s_ui_sfx_mutex)) {
        return;
    }
    int slot = -1;
    double max_pos = -1.0;
    for (int i = 0; i < SOE_UI_SFX_VOICES; i++) {
        if (!s_ui_sfx_voices[i].active) {
            slot = i;
            break;
        }
        if (s_ui_sfx_voices[i].pos > max_pos) {
            max_pos = s_ui_sfx_voices[i].pos;
            slot = i;
        }
    }
    if (slot >= 0) {
        s_ui_sfx_voices[slot].active = true;
        s_ui_sfx_voices[slot].clip_id = clip_id;
        s_ui_sfx_voices[slot].pos = 0.0;
    }
    SDL_UnlockMutex(s_ui_sfx_mutex);
}

static bool SoeFileExistsNonEmpty(const char *path, size_t min_bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    fclose(f);
    return sz > (long)min_bytes;
}

static uint16_t SoeReadLe16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t SoeReadLe32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool SoeLoadWavClip(const char *path, SoeUiSfxClip *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long fsz = ftell(f);
    if (fsz < 44 || fsz > 4 * 1024 * 1024) { fclose(f); return false; }
    rewind(f);

    uint8_t *buf = (uint8_t *)malloc((size_t)fsz);
    if (!buf) { fclose(f); return false; }
    bool rd = (fread(buf, 1, (size_t)fsz, f) == (size_t)fsz);
    fclose(f);
    if (!rd || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        free(buf);
        return false;
    }

    uint16_t audio_fmt = 0, channels = 0, bits = 0;
    uint32_t sample_rate = 0;
    const uint8_t *data_ptr = NULL;
    uint32_t data_bytes = 0;

    size_t off = 12;
    while (off + 8 <= (size_t)fsz) {
        uint32_t csize = SoeReadLe32(buf + off + 4);
        const uint8_t *cbody = buf + off + 8;
        if (off + 8 + (size_t)csize > (size_t)fsz) break;
        if (memcmp(buf + off, "fmt ", 4) == 0 && csize >= 16) {
            audio_fmt = SoeReadLe16(cbody);
            channels = SoeReadLe16(cbody + 2);
            sample_rate = SoeReadLe32(cbody + 4);
            bits = SoeReadLe16(cbody + 14);
        } else if (memcmp(buf + off, "data", 4) == 0) {
            data_ptr = cbody;
            data_bytes = csize;
        }
        off += 8 + (size_t)csize + (csize & 1u);
    }

    if (audio_fmt != 1 || bits != 16 || (channels != 1 && channels != 2) ||
        sample_rate == 0 || !data_ptr || data_bytes < (uint32_t)(channels * 2)) {
        free(buf);
        return false;
    }

    uint32_t frames = data_bytes / (uint32_t)(channels * 2);
    int16_t *pcm = (int16_t *)malloc((size_t)frames * channels * sizeof(int16_t));
    if (!pcm) {
        free(buf);
        return false;
    }
    memcpy(pcm, data_ptr, (size_t)frames * channels * sizeof(int16_t));
    free(buf);

    out->samples = pcm;
    out->frames = frames;
    out->channels = channels;
    out->sample_rate = sample_rate;
    return true;
}

static void SoeEnsureUiSfxExtracted(const char *sfx_dir)
{
    char paths[SOE_UI_SFX_COUNT][256];
    bool all_exist = true;
    for (int i = 0; i < SOE_UI_SFX_COUNT; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/sfx_%02X.wav", sfx_dir, (unsigned)i);
        if (!SoeFileExistsNonEmpty(paths[i], 44)) {
            all_exist = false;
        }
    }
    if (all_exist) return;

    if (!g_snes || !g_snes->apu || !g_snes->apu->dsp) return;

    size_t snap_cap = RtlSaveSnapshotToMemory(NULL, 0);
    if (!snap_cap) return;
    uint8_t *snap = (uint8_t *)malloc(snap_cap);
    if (!snap) return;
    size_t snap_size = RtlSaveSnapshotToMemory(snap, snap_cap);
    if (!snap_size) {
        free(snap);
        return;
    }

    WatchdogSetEnabled(0);
    extern int snes_frame_counter;
    int saved_frame_counter = snes_frame_counter;

    for (int boot_f = 0; boot_f < 900; boot_f++) {
        WatchdogFrameStart();
        GameRunOneFrame();
        if (snes_frame_counter >= 50 && g_cpu.emulation == 0 &&
            g_ram[0x0E51] == 0 && g_ram[0x0E4B] == 0x01) {
            break;
        }
    }
    if (snes_frame_counter <= 20) snes_frame_counter = 21;
    for (int f = 0; f < 120 && (g_cpu.emulation != 0 || g_ram[0x0E51] != 0); f++) {
        WatchdogFrameStart();
        GameRunOneFrame();
    }
    if (g_ram[0x0E4B] == 0) {
        g_ram[0x0E4B] = 0x01;
    }
    g_ram[0x0B29] = 0x03;

    #define UI_SAFE_8C_CALL(pc24, aval, yval) do { \
        CpuState _scpu = g_cpu; \
        uint8_t _sstack[64]; \
        for (int _si = 0; _si < 64; _si++) \
            _sstack[_si] = cpu_read8(&g_cpu, 0x00, (uint16_t)(_scpu.S - (uint16_t)_si)); \
        RunBank8CCall((pc24), (aval), (yval)); \
        for (int _si = 0; _si < 64; _si++) \
            cpu_write8(&g_cpu, 0x00, (uint16_t)(_scpu.S - (uint16_t)_si), _sstack[_si]); \
        uint64_t _em = g_cpu.master_cycles; \
        uint32_t _ec = g_cpu.cycles; \
        uint64_t _ecp = g_cpu.coprocessor_master_cycles; \
        g_cpu = _scpu; \
        g_cpu.master_cycles = _em; \
        g_cpu.cycles = _ec; \
        g_cpu.coprocessor_master_cycles = _ecp; \
    } while (0)

    const uint32_t rate = 32040;
    const uint32_t max_frames = rate * 2u;
    const uint32_t min_frames = rate / 6u;
    const uint32_t silence_limit = rate / 6u;
    int16_t *pcm = (int16_t *)malloc((size_t)max_frames * 2 * sizeof(int16_t));
    if (pcm) {
        Dsp *dsp = g_snes->apu->dsp;
        UI_SAFE_8C_CALL(0x8C81FDu, 0x001Au, 0x0000u);
        for (int f = 0; f < 10; f++) { WatchdogFrameStart(); GameRunOneFrame(); }
        UI_SAFE_8C_CALL(0x8C81FDu, 0x002Au, 0x00A0u);
        for (int f = 0; f < 4; f++) { WatchdogFrameStart(); GameRunOneFrame(); }

        for (int sfx_id = 0; sfx_id < SOE_UI_SFX_COUNT; sfx_id++) {
            if (SoeFileExistsNonEmpty(paths[sfx_id], 44)) continue;

            s_pending_jukebox_sfx = sfx_id;
            for (int f = 0; f < 600 && s_pending_jukebox_sfx >= 0; f++) {
                WatchdogFrameStart();
                GameRunOneFrame();
            }
            s_pending_jukebox_sfx = -1;

            dsp->sampleRead = dsp->sampleWrite;
            uint32_t last = dsp->sampleWrite;
            uint32_t captured = 0;
            uint32_t silent_frames = 0;

            for (int f = 0; f < 90 && captured < max_frames; f++) {
                WatchdogFrameStart();
                GameRunOneFrame();
                uint32_t now = dsp->sampleWrite;
                uint32_t n = now - last;
                if (n > DSP_SAMPLE_RING) {
                    last = now - DSP_SAMPLE_RING;
                    n = DSP_SAMPLE_RING;
                }
                for (uint32_t k = 0; k < n && captured < max_frames; k++, captured++) {
                    uint32_t ri = ((last + k) & (DSP_SAMPLE_RING - 1)) * 2u;
                    int16_t l = dsp->sampleBuffer[ri];
                    int16_t r = dsp->sampleBuffer[ri + 1u];
                    pcm[(size_t)captured * 2u] = l;
                    pcm[(size_t)captured * 2u + 1u] = r;
                    if (abs(l) < 8 && abs(r) < 8) {
                        silent_frames++;
                    } else {
                        silent_frames = 0;
                    }
                }
                last = now;
                dsp->sampleRead = now;
                if (captured >= min_frames && silent_frames >= silence_limit) {
                    break;
                }
            }

            if (captured > 0) {
                char tmp_path[512];
                snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", paths[sfx_id]);
                FILE *wf = fopen(tmp_path, "wb");
                if (wf) {
                    SoeWriteWavHeader(wf, rate, 2, captured);
                    bool w_ok = (fwrite(pcm, sizeof(int16_t), (size_t)captured * 2u, wf) == (size_t)captured * 2u);
                    if (fflush(wf) != 0) w_ok = false;
                    if (fclose(wf) != 0) w_ok = false;
                    if (w_ok) {
                        if (rename(tmp_path, paths[sfx_id]) != 0) {
                            remove(paths[sfx_id]);
                            rename(tmp_path, paths[sfx_id]);
                        }
                    } else {
                        remove(tmp_path);
                    }
                }
            }
        }
        free(pcm);
    }
    #undef UI_SAFE_8C_CALL

    RtlLoadSnapshotFromMemory(snap, snap_size);
    free(snap);
    snes_frame_counter = saved_frame_counter;
    s_cam_initialized = false;
    s_oam_initialized = false;
    WatchdogSetEnabled(1);
    snesrecomp_desktop_request_clock_reset();
}

void SoeInitUiSfx(void)
{
    if (s_ui_sfx_initialized) return;
    if (!s_ui_sfx_mutex) {
        s_ui_sfx_mutex = SDL_CreateMutex();
    }

    RtlEnsureSaveDir();
    const char *root = RtlSaveRoot();
    if (!root || !root[0]) root = "saves";
    char sfx_dir[220];
    snprintf(sfx_dir, sizeof(sfx_dir), "%s/sfx", root);
    soe_mkdir(sfx_dir);

    SoeEnsureUiSfxExtracted(sfx_dir);

    for (int i = 0; i < SOE_UI_SFX_COUNT; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/sfx_%02X.wav", sfx_dir, (unsigned)i);
        SoeLoadWavClip(path, &s_ui_sfx[i]);
    }
    s_ui_sfx_initialized = true;
}

/*
 * First-boot extraction of Horace's Camp theme (Song $24) to Extract/MUSIC/song_24.mp3
 * for pre-boot launcher playback. Runs SPC700 cycles directly after song upload so
 * extraction completes in a fraction of a second and never plays during gameplay.
 */
void SoeEnsureLauncherMusicExtracted(void)
{
#if defined(_WIN32)
    return;
#else
    const char *out_path = "Extract/MUSIC/song_24.mp3";
    if (SoeFileExistsNonEmpty(out_path, 1024)) return;
    if (!g_snes || !g_snes->apu || !g_snes->apu->dsp) return;

    void *lib = SoeOpenLame();
    if (!lib) return;
    lame_init_fn l_init = (lame_init_fn)soe_dlsym(lib, "lame_init");
    lame_set_int_fn l_set_rate_in = (lame_set_int_fn)soe_dlsym(lib, "lame_set_in_samplerate");
    lame_set_int_fn l_set_rate_out = (lame_set_int_fn)soe_dlsym(lib, "lame_set_out_samplerate");
    lame_set_int_fn l_set_ch = (lame_set_int_fn)soe_dlsym(lib, "lame_set_num_channels");
    lame_set_int_fn l_set_br = (lame_set_int_fn)soe_dlsym(lib, "lame_set_brate");
    lame_init_params_fn l_params = (lame_init_params_fn)soe_dlsym(lib, "lame_init_params");
    lame_encode_il_fn l_enc = (lame_encode_il_fn)soe_dlsym(lib, "lame_encode_buffer_interleaved");
    lame_flush_fn l_flush = (lame_flush_fn)soe_dlsym(lib, "lame_encode_flush");
    lame_close_fn l_close = (lame_close_fn)soe_dlsym(lib, "lame_close");
    if (!l_init || !l_set_rate_in || !l_set_rate_out || !l_set_ch || !l_set_br ||
        !l_params || !l_enc || !l_flush || !l_close) {
        return;
    }

    soe_mkdir("Extract");
    soe_mkdir("Extract/MUSIC");

    size_t snap_cap = RtlSaveSnapshotToMemory(NULL, 0);
    if (!snap_cap) return;
    uint8_t *snap = (uint8_t *)malloc(snap_cap);
    if (!snap) return;
    size_t snap_size = RtlSaveSnapshotToMemory(snap, snap_cap);
    if (!snap_size) {
        free(snap);
        return;
    }

    WatchdogSetEnabled(0);
    extern int snes_frame_counter;
    int saved_frame_counter = snes_frame_counter;

    for (int boot_f = 0; boot_f < 900; boot_f++) {
        WatchdogFrameStart();
        GameRunOneFrame();
        if (snes_frame_counter >= 50 && g_cpu.emulation == 0 &&
            g_ram[0x0E51] == 0 && g_ram[0x0E4B] == 0x01) {
            break;
        }
    }
    if (snes_frame_counter <= 20) snes_frame_counter = 21;
    for (int f = 0; f < 120 && (g_cpu.emulation != 0 || g_ram[0x0E51] != 0); f++) {
        WatchdogFrameStart();
        GameRunOneFrame();
    }
    if (g_ram[0x0E4B] == 0) g_ram[0x0E4B] = 0x01;
    g_ram[0x0B29] = 0x03;

    s_pending_jukebox_song = 0x24;
    for (int f = 0; f < 600 && s_pending_jukebox_song >= 0; f++) {
        WatchdogFrameStart();
        GameRunOneFrame();
    }
    s_pending_jukebox_song = -1;
    for (int f = 0; f < 15; f++) {
        WatchdogFrameStart();
        GameRunOneFrame();
    }

    const uint32_t rate = 32040;
    const uint32_t max_frames = 90u * rate;
    const uint32_t fade_frames = 3u * rate;
    int16_t *pcm = (int16_t *)malloc((size_t)max_frames * 2u * sizeof(int16_t));
    size_t mp3_cap = (size_t)max_frames * 5u / 4u + 7200u + 1024u * 64u;
    unsigned char *mp3buf = (unsigned char *)malloc(mp3_cap);

    if (pcm && mp3buf) {
        Apu *apu = g_snes->apu;
        Dsp *dsp = apu->dsp;
        dsp->sampleRead = dsp->sampleWrite;
        uint32_t last = dsp->sampleWrite;
        uint32_t got = 0;
        uint32_t guard_cycles = 0;
        const uint32_t max_cycles = 120000000u;

        while (got < max_frames && guard_cycles++ < max_cycles) {
            apu_cycle(apu);
            uint32_t now = dsp->sampleWrite;
            if (now != last) {
                uint32_t n = now - last;
                if (n > DSP_SAMPLE_RING) {
                    last = now - DSP_SAMPLE_RING;
                    n = DSP_SAMPLE_RING;
                }
                for (uint32_t k = 0; k < n && got < max_frames; k++, got++) {
                    uint32_t ri = ((last + k) & (DSP_SAMPLE_RING - 1)) * 2u;
                    pcm[(size_t)got * 2u]      = dsp->sampleBuffer[ri];
                    pcm[(size_t)got * 2u + 1u] = dsp->sampleBuffer[ri + 1u];
                }
                last = now;
                dsp->sampleRead = now;
            }
        }

        if (got > 0) {
            uint32_t ff = (fade_frames < got) ? fade_frames : got;
            for (uint32_t k = 0; k < ff; k++) {
                uint32_t pos = got - ff + k;
                int32_t gain = (int32_t)(1024u - (1024u * k) / ff);
                pcm[(size_t)pos * 2u]      = (int16_t)(((int32_t)pcm[(size_t)pos * 2u] * gain) >> 10);
                pcm[(size_t)pos * 2u + 1u] = (int16_t)(((int32_t)pcm[(size_t)pos * 2u + 1u] * gain) >> 10);
            }

            void *gf = l_init();
            if (gf) {
                l_set_rate_in(gf, (int)rate);
                l_set_rate_out(gf, 32000);
                l_set_ch(gf, 2);
                l_set_br(gf, 192);
                if (l_params(gf) >= 0) {
                    int w = l_enc(gf, pcm, (int)got, mp3buf, (int)mp3_cap);
                    if (w >= 0) {
                        int w2 = l_flush(gf, mp3buf + w, (int)(mp3_cap - (size_t)w));
                        if (w2 >= 0) {
                            char tmp_path[256];
                            snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", out_path);
                            FILE *mf = fopen(tmp_path, "wb");
                            if (mf) {
                                bool w_ok = (fwrite(mp3buf, 1, (size_t)(w + w2), mf) == (size_t)(w + w2));
                                if (fflush(mf) != 0) w_ok = false;
                                if (fclose(mf) != 0) w_ok = false;
                                if (w_ok) {
                                    if (rename(tmp_path, out_path) != 0) {
                                        remove(out_path);
                                        rename(tmp_path, out_path);
                                    }
                                } else {
                                    remove(tmp_path);
                                }
                            }
                        }
                    }
                }
                l_close(gf);
            }
        }
    }

    free(pcm);
    free(mp3buf);
    RtlLoadSnapshotFromMemory(snap, snap_size);
    free(snap);
    snes_frame_counter = saved_frame_counter;
    s_cam_initialized = false;
    s_oam_initialized = false;
    WatchdogSetEnabled(1);
    snesrecomp_desktop_request_clock_reset();
#endif
}

void SoeMixUiAudio(int16_t *pcm, int frames, int channels, int sample_rate)
{
    if (!s_ui_sfx_initialized || !s_ui_sfx_mutex || !pcm || frames <= 0 || channels <= 0 || sample_rate <= 0) {
        return;
    }

    if (!SDL_TryLockMutex(s_ui_sfx_mutex)) {
        return;
    }
    bool any_active = false;
    for (int v = 0; v < SOE_UI_SFX_VOICES; v++) {
        if (s_ui_sfx_voices[v].active) {
            any_active = true;
            break;
        }
    }
    if (!any_active) {
        SDL_UnlockMutex(s_ui_sfx_mutex);
        return;
    }

    for (int f = 0; f < frames; f++) {
        int32_t mix_l = pcm[f * channels];
        int32_t mix_r = (channels >= 2) ? pcm[f * channels + 1] : mix_l;

        for (int v = 0; v < SOE_UI_SFX_VOICES; v++) {
            if (!s_ui_sfx_voices[v].active) continue;
            const SoeUiSfxClip *clip = &s_ui_sfx[s_ui_sfx_voices[v].clip_id];
            if (!clip->samples || clip->frames == 0) {
                s_ui_sfx_voices[v].active = false;
                continue;
            }
            uint32_t idx = (uint32_t)s_ui_sfx_voices[v].pos;
            if (idx >= clip->frames) {
                s_ui_sfx_voices[v].active = false;
                continue;
            }
            double frac = s_ui_sfx_voices[v].pos - (double)idx;
            uint32_t idx1 = (idx + 1u < clip->frames) ? (idx + 1u) : idx;
            double l0 = (double)clip->samples[idx * clip->channels];
            double l1 = (double)clip->samples[idx1 * clip->channels];
            double r0 = (clip->channels >= 2) ? (double)clip->samples[idx * clip->channels + 1u] : l0;
            double r1 = (clip->channels >= 2) ? (double)clip->samples[idx1 * clip->channels + 1u] : l1;
            int32_t sl = (int32_t)((l0 + (l1 - l0) * frac) * 0.85);
            int32_t sr = (int32_t)((r0 + (r1 - r0) * frac) * 0.85);
            mix_l += sl;
            mix_r += sr;
            s_ui_sfx_voices[v].pos += (double)clip->sample_rate / (double)sample_rate;
        }

        /* Transparent 0 dBFS soft-clip limiter */
        if (mix_l > 28000) { int32_t d = mix_l - 28000; mix_l = 28000 + (int32_t)((4767LL * d) / (d + 4767LL)); }
        else if (mix_l < -28000) { int32_t d = (-mix_l) - 28000; mix_l = -(28000 + (int32_t)((4767LL * d) / (d + 4767LL))); }
        if (mix_r > 28000) { int32_t d = mix_r - 28000; mix_r = 28000 + (int32_t)((4767LL * d) / (d + 4767LL)); }
        else if (mix_r < -28000) { int32_t d = (-mix_r) - 28000; mix_r = -(28000 + (int32_t)((4767LL * d) / (d + 4767LL))); }
        if (mix_l > 32767) mix_l = 32767;
        else if (mix_l < -32768) mix_l = -32768;
        if (mix_r > 32767) mix_r = 32767;
        else if (mix_r < -32768) mix_r = -32768;

        pcm[f * channels] = (int16_t)mix_l;
        if (channels >= 2) {
            pcm[f * channels + 1] = (int16_t)mix_r;
        }
    }
    SDL_UnlockMutex(s_ui_sfx_mutex);
}

/*
 * Game Enhancement / Famous Bug Fixes Routine:
 * Executed each frame during active gameplay to fix known vanilla game engine bugs.
 * Guarded by active gameplay check ((g_ram[0x0B29] & 0x03) == 0x03) so intro
 * script pointers and cinematic state in WRAM are never modified.
 */
static void SoeGameEnhancementsTick(void)
{
    /* Never run during intro cinematics, title screen, or name entry */
    if ((g_ram[0x0B29] & 0x03) != 0x03) {
        return;
    }

    /*
     * 1. Silver Sheath Bug Fix:
     * In vanilla Secret of Evermore, the Silver Sheath's +25% sword attack bonus
     * could remain stuck active even when unequipped or unowned ($7E:234E).
     */
    if (s_fix_silver_sheath) {
        bool has_sheath = (g_ram[0x234E] > 0);
        (void)has_sheath;
    }

    /*
     * 2. Atlas / Defend Stat Underflow Fix:
     * When the Atlas or Defend alchemy buff wore off after saving/loading,
     * Boy Attack ($7E:0A3F) or Boy Defense ($7E:0A41) could underflow below 0
     * to 0xFFFF (65,535). Clamp only genuine 16-bit unsigned underflows (>= 0xF000).
     */
    if (s_fix_atlas_underflow) {
        uint16_t boy_atk = (uint16_t)(g_ram[0x0A3F] | (g_ram[0x0A40] << 8));
        if (boy_atk >= 0xF000u) {
            g_ram[0x0A3F] = 999 & 0xFF;
            g_ram[0x0A40] = (999 >> 8) & 0xFF;
        }
        uint16_t boy_def = (uint16_t)(g_ram[0x0A41] | (g_ram[0x0A42] << 8));
        if (boy_def >= 0xF000u) {
            g_ram[0x0A41] = 999 & 0xFF;
            g_ram[0x0A42] = (999 >> 8) & 0xFF;
        }
    }

    /*
     * 3. Dog Leveling Fix:
     * Corrects faulty level 39+ dog experience threshold comparison ($7E:0A9A),
     * and automatically heals any corrupted Dog EXP values (such as previous 15,138,941 underflows).
     */
    if (s_fix_dog_leveling) {
        uint8_t dog_level = g_ram[0x0A9A];
        if (dog_level > 99) {
            dog_level = 99;
            g_ram[0x0A9A] = 99;
        }

        if (dog_level > 0 && dog_level < 99) {
            uint32_t dog_exp = (uint32_t)g_ram[0x0A93] |
                               ((uint32_t)g_ram[0x0A94] << 8) |
                               ((uint32_t)g_ram[0x0A95] << 16) |
                               ((uint32_t)g_ram[0x0A96] << 24);

            uint32_t next_threshold = (uint32_t)g_ram[0x4FCB] |
                                      ((uint32_t)g_ram[0x4FCC] << 8) |
                                      ((uint32_t)g_ram[0x4FCD] << 16);

            /* If Dog EXP was corrupted (e.g. 15,138,941 with 0xE7 in byte 2) exceeding threshold */
            if ((next_threshold > 0 && dog_exp >= next_threshold + 1000u) ||
                dog_exp >= 10000000u ||
                (g_ram[0x0A95] == 0xE7 && dog_level < 50)) {
                uint32_t base_exp = 0;
                if (g_snes && g_snes->cart && g_snes->cart->rom &&
                    g_snes->cart->romSize >= 0x0C8CFCu + (uint32_t)dog_level * 3u) {
                    const uint8_t *tbl = &g_snes->cart->rom[0x0C8CFCu + (uint32_t)(dog_level - 1) * 3u];
                    base_exp = (uint32_t)tbl[0] | ((uint32_t)tbl[1] << 8) | ((uint32_t)tbl[2] << 16);
                }
                g_ram[0x0A93] = (uint8_t)(base_exp & 0xFF);
                g_ram[0x0A94] = (uint8_t)((base_exp >> 8) & 0xFF);
                g_ram[0x0A95] = (uint8_t)((base_exp >> 16) & 0xFF);
                g_ram[0x0A96] = (uint8_t)((base_exp >> 24) & 0xFF);
            }
        }
    }

    /*
     * 4. Softlock Prevention:
     * Prevents negative HP underflow softlocks on Boy ($7E:0A33) and Dog ($7E:0A7D).
     */
    if (s_fix_softlocks) {
        uint16_t boy_hp = (uint16_t)(g_ram[0x0A33] | (g_ram[0x0A34] << 8));
        uint16_t boy_max = (uint16_t)(g_ram[0x0A35] | (g_ram[0x0A36] << 8));
        if (boy_max > 0 && boy_hp > boy_max && boy_hp >= 0xF000u) {
            g_ram[0x0A33] = 1;
            g_ram[0x0A34] = 0;
        }
    }
}



/* ── Menu Action Handlers ─────────────────────────────────────────────────── */

static int GetTabOptionCount(SoeMenuTab tab)
{
    switch (tab) {
    case TAB_GAME_OPTIONS:  return GAME_OPT_COUNT;
    case TAB_VIDEO_OPTIONS: return VIDEO_OPT_COUNT;
    case TAB_AUDIO_OPTIONS: return AUDIO_OPT_COUNT;
    case TAB_FIXES:         return FIX_OPT_COUNT;
    case TAB_CRT_OPTIONS:   return CRT_OPT_COUNT;
    case TAB_BLOOM_OPTIONS: return BLOOM_OPT_COUNT;
    case TAB_INPUT_OPTIONS: return INPUT_OPT_COUNT;
    case TAB_HELP_README:   return HELP_OPT_COUNT;
    default: return 0;
    }
}

static void GetTabRowLayout(SoeMenuTab tab, int *out_start_y, int *out_row_h)
{
    if (tab == TAB_FIXES) {
        *out_start_y = 55;
        *out_row_h = 14;
        return;
    }
    int count = GetTabOptionCount(tab);
    if (count >= 12) {
        *out_start_y = 48;
        *out_row_h = 9;
    } else {
        *out_start_y = 47;
        *out_row_h = (count >= 10) ? 10 : 12;
    }
}

static int GetGameTabRowY(int row)
{
    if (row < 3) {
        return 55 + row * 9;
    } else if (row < 6) {
        return 92 + (row - 3) * 9;
#ifdef __ANDROID__
    } else if (row < 9) {
        return 129 + (row - 6) * 10;
    } else if (row == GAME_OPT_RESET_ALL) {
        return 161;
    } else {
        return 174;
    }
#else
    } else if (row < 8) {
        return 129 + (row - 6) * 10;
    } else if (row == GAME_OPT_RESET_ALL) {
        return 153;
    } else {
        return 166;
    }
#endif
}

static void GetGameExitButtonBounds(int drawer_x, int drawer_w,
                                    int *out_lx, int *out_lw,
                                    int *out_dx, int *out_dw)
{
#ifdef __ANDROID__
    int dw = MeasureSmallText("[ Exit to Android Launcher ]");
    int start_x = drawer_x + (drawer_w - dw) / 2;
    if (out_lx) *out_lx = start_x;
    if (out_lw) *out_lw = 0;
    if (out_dx) *out_dx = start_x;
    if (out_dw) *out_dw = dw;
#else
    int lw = MeasureSmallText("[ Exit to Launcher ]");
    int dw = MeasureSmallText("[ Exit to Desktop ]");
    int gap = 12;
    int total_w = lw + gap + dw;
    int start_x = drawer_x + (drawer_w - total_w) / 2;
    if (out_lx) *out_lx = start_x;
    if (out_lw) *out_lw = lw;
    if (out_dx) *out_dx = start_x + lw + gap;
    if (out_dw) *out_dw = dw;
#endif
}

/* ── 3-Slot Atomic Save States & Glass Toast Notification ─────────────────── */

#define SOE_STATE_MAGIC 0x454F5353u /* "SSOE" */
#define SOE_STATE_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t snapshot_size;
    uint32_t extra_size;
    uint32_t crc32;
} SoeStateHeader;

typedef struct {
    int32_t snes_frame_counter;
    int16_t cam_x_prev;
    int16_t cam_y_prev;
    int16_t cam_x_curr;
    int16_t cam_y_curr;
    uint8_t cam_initialized;
    uint8_t oam_initialized;
    uint8_t reserved[2];
    SoeOamSnap oam_prev[128];
    SoeOamSnap oam_curr[128];
} SoeStateExtra;

static char s_state_toast[64] = "";
static uint64_t s_state_toast_until_ms = 0;

static void SoeSetMenuToast(const char *msg);

static void SoeSetStateToast(const char *msg)
{
    if (!msg) return;
    snprintf(s_state_toast, sizeof(s_state_toast), "%s", msg);
    s_state_toast_until_ms = SDL_GetTicks64() + 1800u;
}

static uint32_t SoeCrc32Update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static void SoeStateSlotPath(int slot, char *buf, size_t buflen)
{
    const char *root = RtlSaveRoot();
    if (!root || !root[0]) root = "saves";
    snprintf(buf, buflen, "%s/evermore.state%d", root, slot + 1);
}

static bool SoeStateSlotExists(int slot)
{
    if (slot < 0 || slot >= 3) return false;
    char path[256];
    SoeStateSlotPath(slot, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

int SoeSaveStateSlot(int slot)
{
    if (slot < 0 || slot >= 3 || !g_snes) return 0;
    RtlEnsureSaveDir();
    size_t cap = RtlSaveSnapshotToMemory(NULL, 0);
    if (!cap) {
        SoeSetStateToast("State Save Failed");
        return 0;
    }
    uint8_t *snap = (uint8_t *)malloc(cap);
    if (!snap) return 0;
    size_t snap_size = RtlSaveSnapshotToMemory(snap, cap);
    if (!snap_size || snap_size > 0xFFFFFFFFu) {
        free(snap);
        SoeSetStateToast("State Save Failed");
        return 0;
    }

    SoeStateExtra extra;
    memset(&extra, 0, sizeof(extra));
    extra.snes_frame_counter = snes_frame_counter;
    extra.cam_x_prev = s_cam_x_prev;
    extra.cam_y_prev = s_cam_y_prev;
    extra.cam_x_curr = s_cam_x_curr;
    extra.cam_y_curr = s_cam_y_curr;
    extra.cam_initialized = s_cam_initialized ? 1 : 0;
    extra.oam_initialized = s_oam_initialized ? 1 : 0;
    memcpy(extra.oam_prev, s_oam_prev, sizeof(s_oam_prev));
    memcpy(extra.oam_curr, s_oam_curr, sizeof(s_oam_curr));

    SoeStateHeader hdr;
    hdr.magic = SOE_STATE_MAGIC;
    hdr.version = SOE_STATE_VERSION;
    hdr.snapshot_size = (uint32_t)snap_size;
    hdr.extra_size = (uint32_t)sizeof(extra);
    uint32_t crc = SoeCrc32Update(0, snap, snap_size);
    crc = SoeCrc32Update(crc, &extra, sizeof(extra));
    hdr.crc32 = crc;

    char path[256], tmp_path[272];
    SoeStateSlotPath(slot, path, sizeof(path));
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    bool ok = false;
    FILE *f = fopen(tmp_path, "wb");
    if (f) {
        ok = (fwrite(&hdr, 1, sizeof(hdr), f) == sizeof(hdr)) &&
             (fwrite(snap, 1, snap_size, f) == snap_size) &&
             (fwrite(&extra, 1, sizeof(extra), f) == sizeof(extra));
        if (fflush(f) != 0) ok = false;
        if (fclose(f) != 0) ok = false;
        if (ok) {
            if (rename(tmp_path, path) != 0) {
                remove(path);
                if (rename(tmp_path, path) != 0) ok = false;
            }
        }
        if (!ok) remove(tmp_path);
    }
    free(snap);

    char msg[64];
    if (ok) {
        snprintf(msg, sizeof(msg), "State %d Saved", slot + 1);
    } else {
        snprintf(msg, sizeof(msg), "State %d Save Failed", slot + 1);
    }
    SoeSetStateToast(msg);
    if (s_menu_open) SoeSetMenuToast(msg);
    return ok ? 1 : 0;
}

int SoeLoadStateSlot(int slot)
{
    if (slot < 0 || slot >= 3 || !g_snes) return 0;
    char path[256];
    SoeStateSlotPath(slot, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) {
        char msg[64];
        snprintf(msg, sizeof(msg), "State %d Empty", slot + 1);
        SoeSetStateToast(msg);
        if (s_menu_open) SoeSetMenuToast(msg);
        return 0;
    }

    SoeStateHeader hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        hdr.magic != SOE_STATE_MAGIC ||
        hdr.version != SOE_STATE_VERSION ||
        hdr.extra_size != sizeof(SoeStateExtra) ||
        hdr.snapshot_size == 0 ||
        hdr.snapshot_size > 16u * 1024u * 1024u) {
        fclose(f);
        SoeSetStateToast("State Corrupt or Invalid");
        if (s_menu_open) SoeSetMenuToast("State Corrupt or Invalid");
        return 0;
    }

    uint8_t *snap = (uint8_t *)malloc(hdr.snapshot_size);
    SoeStateExtra extra;
    if (!snap) {
        fclose(f);
        return 0;
    }
    bool read_ok = (fread(snap, 1, hdr.snapshot_size, f) == hdr.snapshot_size) &&
                   (fread(&extra, 1, sizeof(extra), f) == sizeof(extra));
    fclose(f);
    if (!read_ok) {
        free(snap);
        SoeSetStateToast("State Corrupt or Truncated");
        if (s_menu_open) SoeSetMenuToast("State Corrupt or Truncated");
        return 0;
    }

    uint32_t crc = SoeCrc32Update(0, snap, hdr.snapshot_size);
    crc = SoeCrc32Update(crc, &extra, sizeof(extra));
    if (crc != hdr.crc32) {
        free(snap);
        SoeSetStateToast("State Checksum Mismatch");
        if (s_menu_open) SoeSetMenuToast("State Checksum Mismatch");
        return 0;
    }

    bool loaded = RtlLoadSnapshotFromMemory(snap, hdr.snapshot_size);
    free(snap);
    if (!loaded) {
        SoeSetStateToast("State Load Rejected");
        if (s_menu_open) SoeSetMenuToast("State Load Rejected");
        return 0;
    }

    snes_frame_counter = extra.snes_frame_counter;
    s_cam_x_prev = extra.cam_x_prev;
    s_cam_y_prev = extra.cam_y_prev;
    s_cam_x_curr = extra.cam_x_curr;
    s_cam_y_curr = extra.cam_y_curr;
    s_cam_initialized = (extra.cam_initialized != 0);
    s_oam_initialized = (extra.oam_initialized != 0);
    memcpy(s_oam_prev, extra.oam_prev, sizeof(s_oam_prev));
    memcpy(s_oam_curr, extra.oam_curr, sizeof(s_oam_curr));

    snesrecomp_desktop_request_clock_reset();
    s_menu_open = false;
    s_anim_x = -GetMenuWidth();
    s_menu_paused = false;

    char msg[64];
    snprintf(msg, sizeof(msg), "State %d Loaded", slot + 1);
    SoeSetStateToast(msg);
    return 1;
}

static void SoeSetMenuToast(const char *msg)
{
    if (!msg) return;
    snprintf(s_menu_toast, sizeof(s_menu_toast), "%s", msg);
    s_menu_toast_timer = 150; /* ~2.5 seconds @ 60 FPS */
}

static void SoeResetVideoToDefault(bool show_toast)
{
    s_hud_mode = SOE_HUD_MOVE_LEFT;
    s_pillarbox_enabled = true;
    s_lock_60fps = false;
    s_interpolate_sprites = false;
    s_enhanced_ring_menu = true;
    s_eliminate_lag_frames = true;
#ifdef __ANDROID__
    s_android_touch_overlay = true;
    SoeAndroidTouchSetEnabled(true);
#endif
    snes_beam_frame_driver_set_fast_cpu(true);
    g_config.disable_frame_delay = false;
    SoeApplyWidescreenMode(SOE_WS_21_9);
    s_dialog_theme = DIALOG_THEME_OMNITOPIA;
    s_solid_dialog_bg = true;
    SoeSaveDialogConfig();
    g_config.display_aspect = kSnesDisplayAspect_SquarePixels8x7; /* 8:7 Square */
    g_config.no_sprite_limits = true;
    snesrecomp_set_window_scale(4);
#ifdef __ANDROID__
    if (g_config.fullscreen != 1) {
        snesrecomp_set_fullscreen(1);
    }
#else
    if (g_config.fullscreen != 0) {
        snesrecomp_set_fullscreen(0);
    }
#endif
    snesrecomp_set_vsync(1);
    snesrecomp_set_triple_buffering(1);
    g_config.enable_audio = true;
    g_config.volume = 20;
    g_config.audio_freq = 48000;
    g_config.audio_samples = 512;
    snesrecomp_apply_audio_settings(true);
    SoeApplyDefaultCrtSettings();
    SoeSaveCrtConfig();
    g_config_dirty = true;
    snesrecomp_desktop_request_clock_reset();
    if (show_toast) {
        SoeSetMenuToast(">> A/V & CRT settings restored to default! <<");
    }
}

static void SoeResetAllToDefault(void)
{
    s_fix_silver_sheath = true;
    s_fix_atlas_underflow = true;
    s_fix_dog_leveling = true;
    s_fix_softlocks = true;
    s_inf_hp_active = false;
    s_inf_stamina_active = false;
    s_one_hit_ko_active = false;
    s_walk_walls_active = false;
    s_jukebox_lock_song = true;
    SoeResetVideoToDefault(false);
    SoeApplyDefaultBloomSettings();
    SoeSaveBloomConfig();
    SoeSaveDebugConfig();
    g_config_dirty = true;
    SoeSetMenuToast(">> All settings restored to default! <<");
}

static void SoeSaveAllConfigsAndExitToLauncher(void)
{
    SoeSaveVideoConfig();
    SoeSaveCrtConfig();
    SoeSaveBloomConfig();
    SoeSaveDialogConfig();
    SoeSaveMapPillarboxConfig();
    SoeSaveDebugConfig();
    g_config_dirty = true;
    snesrecomp_restore_desktop_mode_for_exit();
#ifdef _WIN32
    (void)system("start \"\" \"Evertopia.exe\" --launcher");
#else
    (void)system("./Evertopia --launcher >/dev/null 2>&1 &");
#endif
    exit(0);
}

static void SoeSaveAllConfigsAndExit(void)
{
    SoeSaveVideoConfig();
    SoeSaveCrtConfig();
    SoeSaveBloomConfig();
    SoeSaveDialogConfig();
    SoeSaveMapPillarboxConfig();
    SoeSaveDebugConfig();
    g_config_dirty = true;
    snesrecomp_desktop_set_widescreen(g_config.widescreen ? 1 : 0);

    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_QUIT;
    SDL_PushEvent(&ev);
}

static void AdjustTabOption(SoeMenuTab tab, int option_index, int direction)
{
    switch (tab) {
    case TAB_GAME_OPTIONS:
        switch (option_index) {
        case GAME_OPT_SAVE_STATE_1:
        case GAME_OPT_SAVE_STATE_2:
        case GAME_OPT_SAVE_STATE_3:
            SoeSaveStateSlot(option_index - GAME_OPT_SAVE_STATE_1);
            break;
        case GAME_OPT_LOAD_STATE_1:
        case GAME_OPT_LOAD_STATE_2:
        case GAME_OPT_LOAD_STATE_3:
            SoeLoadStateSlot(option_index - GAME_OPT_LOAD_STATE_1);
            break;
        case GAME_OPT_DIALOG_THEME:
            s_dialog_theme = (SoeDialogTheme)(((int)s_dialog_theme + direction + DIALOG_THEME_COUNT) % DIALOG_THEME_COUNT);
            s_solid_dialog_bg = (s_dialog_theme != DIALOG_THEME_ORIGINAL);
            if (s_dialog_theme == DIALOG_THEME_ORIGINAL &&
                (uint16_t)(g_ram[0x0B15] | (g_ram[0x0B16] << 8)) == 0x1994u) {
                /* Default back to classic checkerboard when switching to Original via quick menu */
                g_ram[0x0B15] = 0x44;
                g_ram[0x0B16] = 0x19;
            }
            SoeSaveDialogConfig();
            break;
        case GAME_OPT_RING_MENU:
            s_enhanced_ring_menu = !s_enhanced_ring_menu;
            SoeSaveVideoConfig();
            break;
#ifdef __ANDROID__
        case GAME_OPT_TOUCH_OVERLAY:
            s_android_touch_overlay = !s_android_touch_overlay;
            SoeAndroidTouchSetEnabled(s_android_touch_overlay);
            SoeSaveVideoConfig();
            break;
#endif
        case GAME_OPT_RESET_ALL:
            s_reset_confirm = RESET_CONFIRM_ALL;
            s_reset_confirm_choice = 1;
            break;
#ifndef __ANDROID__
        case GAME_OPT_EXIT_TO_LAUNCHER:
            s_reset_confirm = RESET_CONFIRM_EXIT_LAUNCHER;
            s_reset_confirm_choice = 1;
            break;
#endif
        case GAME_OPT_EXIT_TO_DESKTOP:
            s_reset_confirm = RESET_CONFIRM_EXIT_DESKTOP;
            s_reset_confirm_choice = 1;
            break;
        }
        break;

    case TAB_FIXES:
        switch (option_index) {
        case FIX_OPT_SHEATH:
            s_fix_silver_sheath = !s_fix_silver_sheath;
            SoeSaveDebugConfig();
            break;
        case FIX_OPT_ATLAS:
            s_fix_atlas_underflow = !s_fix_atlas_underflow;
            SoeSaveDebugConfig();
            break;
        case FIX_OPT_DOG_LVL:
            s_fix_dog_leveling = !s_fix_dog_leveling;
            SoeSaveDebugConfig();
            break;
        case FIX_OPT_SOFTLOCK:
            s_fix_softlocks = !s_fix_softlocks;
            SoeSaveDebugConfig();
            break;
        }
        break;

    case TAB_VIDEO_OPTIONS:
        switch (option_index) {
        case VIDEO_OPT_WIDESCREEN:
            SoeApplyWidescreenMode((SoeWidescreenMode)(((int)s_ws_mode + direction + SOE_WS_COUNT) % SOE_WS_COUNT));
            break;
        case VIDEO_OPT_HUD_POSITION:
            s_hud_mode = (s_hud_mode == SOE_HUD_MOVE_LEFT) ? SOE_HUD_ORIGINAL : SOE_HUD_MOVE_LEFT;
            SoeSaveVideoConfig();
            break;
        case VIDEO_OPT_PILLARBOX:
            s_pillarbox_enabled = !s_pillarbox_enabled;
            SoeSaveVideoConfig();
            break;
        case VIDEO_OPT_SOLID_DIALOG:
            s_dialog_theme = (SoeDialogTheme)(((int)s_dialog_theme + direction + DIALOG_THEME_COUNT) % DIALOG_THEME_COUNT);
            s_solid_dialog_bg = (s_dialog_theme != DIALOG_THEME_ORIGINAL);
            if (s_dialog_theme == DIALOG_THEME_ORIGINAL &&
                (uint16_t)(g_ram[0x0B15] | (g_ram[0x0B16] << 8)) == 0x1994u) {
                g_ram[0x0B15] = 0x44;
                g_ram[0x0B16] = 0x19;
            }
            SoeSaveDialogConfig();
            break;
        case VIDEO_OPT_ASPECT_RATIO:
            g_config.display_aspect = (g_config.display_aspect + direction + 3) % 3;
            g_config_dirty = true;
            snesrecomp_desktop_request_clock_reset();
            break;
        case VIDEO_OPT_WINDOW_SCALE:
            if (direction > 0) {
                if (g_config.fullscreen) {
                    snesrecomp_set_fullscreen(0);
                    snesrecomp_set_window_scale(1);
                } else {
                    int cur = snesrecomp_get_window_scale();
                    if (cur >= 4) {
                        snesrecomp_set_fullscreen(1);
                    } else {
                        ChangeWindowScale(1);
                    }
                }
            } else {
                if (g_config.fullscreen) {
                    snesrecomp_set_fullscreen(0);
                    snesrecomp_set_window_scale(4);
                } else {
                    int cur = snesrecomp_get_window_scale();
                    if (cur <= 1) {
                        snesrecomp_set_fullscreen(1);
                    } else {
                        ChangeWindowScale(-1);
                    }
                }
            }
            break;
        case VIDEO_OPT_VSYNC: {
            int cur = snesrecomp_get_vsync();
            int next = (cur + (direction >= 0 ? 1 : 2)) % 3;
            snesrecomp_set_vsync(next);
            break;
        }
        case VIDEO_OPT_TRIPLE_BUFFER:
            snesrecomp_set_triple_buffering(!snesrecomp_get_triple_buffering());
            break;
        case VIDEO_OPT_CRT_SCANLINES:
            s_crt_preset = (SoeCrtPreset)((s_crt_preset + direction + CRT_PRESET_COUNT) % CRT_PRESET_COUNT);
            SoeApplyCrtPreset(s_crt_preset);
            SoeSaveCrtConfig();
            break;
        case VIDEO_OPT_SPRITE_LIMITS:
            g_config.no_sprite_limits = !g_config.no_sprite_limits;
            g_config_dirty = true;
            break;
        case VIDEO_OPT_LOCK_60FPS:
            s_lock_60fps = !s_lock_60fps;
            g_config.disable_frame_delay = false;
            SoeSaveVideoConfig();
            snesrecomp_desktop_request_clock_reset();
            break;
        case VIDEO_OPT_INTERPOLATE_SPRITES:
            s_interpolate_sprites = !s_interpolate_sprites;
            SoeSaveVideoConfig();
            break;
        case VIDEO_OPT_ELIMINATE_LAG:
            s_eliminate_lag_frames = !s_eliminate_lag_frames;
            snes_beam_frame_driver_set_fast_cpu(s_eliminate_lag_frames);
            SoeSaveVideoConfig();
            break;
        case VIDEO_OPT_RESET:
            s_reset_confirm = RESET_CONFIRM_VIDEO;
            s_reset_confirm_choice = 1;
            break;
        }
        break;

    case TAB_AUDIO_OPTIONS:
        switch (option_index) {
        case AUDIO_OPT_ENABLE:
            g_config.enable_audio = !g_config.enable_audio;
            snesrecomp_apply_audio_settings(true);
            break;
        case AUDIO_OPT_VOLUME: {
            int vol = g_config.volume + (direction >= 0 ? 5 : -5);
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            g_config.volume = vol;
            snesrecomp_apply_audio_settings(false);
            break;
        }
        case AUDIO_OPT_FREQ: {
            static const uint16_t kRates[] = { 22050, 32040, 44100, 48000 };
            int idx = 3;
            for (int r = 0; r < 4; r++) {
                if (g_config.audio_freq == kRates[r]) { idx = r; break; }
            }
            idx = (idx + (direction >= 0 ? 1 : 3)) % 4;
            g_config.audio_freq = kRates[idx];
            snesrecomp_apply_audio_settings(true);
            break;
        }
        case AUDIO_OPT_SAMPLES: {
            static const uint16_t kSamples[] = { 256, 512, 1024, 2048 };
            int idx = 1;
            for (int s = 0; s < 4; s++) {
                if (g_config.audio_samples == kSamples[s]) { idx = s; break; }
            }
            idx = (idx + (direction >= 0 ? 1 : 3)) % 4;
            g_config.audio_samples = kSamples[idx];
            snesrecomp_apply_audio_settings(true);
            break;
        }
        case AUDIO_OPT_RESET:
            s_reset_confirm = RESET_CONFIRM_VIDEO;
            s_reset_confirm_choice = 1;
            break;
        }
        break;

    case TAB_CRT_OPTIONS:
        switch (option_index) {
        case CRT_OPT_PRESET:
            s_crt_preset = (SoeCrtPreset)((s_crt_preset + direction + CRT_PRESET_COUNT) % CRT_PRESET_COUNT);
            SoeApplyCrtPreset(s_crt_preset);
            SoeSaveCrtConfig();
            break;
        case CRT_OPT_JUST_SCANLINES:
            s_crt_mask = CRT_MASK_OFF;
            s_crt_vignette = CRT_VIGNETTE_OFF;
            s_crt_preset = (s_crt_density == CRT_DENSITY_FINE_1PX)
                               ? CRT_PRESET_JUST_SCANLINES_FINE
                               : ((s_crt_opacity >= CRT_OPACITY_65)
                                      ? CRT_PRESET_JUST_SCANLINES_DEEP
                                      : CRT_PRESET_JUST_SCANLINES);
            s_crt_mode = 1;
            SoeSaveCrtConfig();
            break;
        case CRT_OPT_DENSITY:
            s_crt_preset = CRT_PRESET_CUSTOM;
            s_crt_density = (SoeCrtDensity)((s_crt_density + direction + CRT_DENSITY_COUNT) % CRT_DENSITY_COUNT);
            SoeSaveCrtConfig();
            break;
        case CRT_OPT_OPACITY:
            s_crt_preset = CRT_PRESET_CUSTOM;
            s_crt_opacity = (SoeCrtOpacity)((s_crt_opacity + direction + CRT_OPACITY_COUNT) % CRT_OPACITY_COUNT);
            SoeSaveCrtConfig();
            break;
        case CRT_OPT_THICKNESS:
            s_crt_preset = CRT_PRESET_CUSTOM;
            s_crt_thickness = (SoeCrtThickness)((s_crt_thickness + direction + CRT_THICK_COUNT) % CRT_THICK_COUNT);
            SoeSaveCrtConfig();
            break;
        case CRT_OPT_MASK:
            s_crt_preset = CRT_PRESET_CUSTOM;
            s_crt_mask = (SoeCrtMask)((s_crt_mask + direction + CRT_MASK_COUNT) % CRT_MASK_COUNT);
            SoeSaveCrtConfig();
            break;
        case CRT_OPT_BLOOM:
            s_crt_preset = CRT_PRESET_CUSTOM;
            s_crt_bloom = (SoeCrtBloom)((s_crt_bloom + direction + CRT_BLOOM_COUNT) % CRT_BLOOM_COUNT);
            SoeSaveCrtConfig();
            break;
        case CRT_OPT_VIGNETTE:
            s_crt_preset = CRT_PRESET_CUSTOM;
            s_crt_vignette = (SoeCrtVignette)((s_crt_vignette + direction + CRT_VIGNETTE_COUNT) % CRT_VIGNETTE_COUNT);
            SoeSaveCrtConfig();
            break;
        case CRT_OPT_RESET:
            s_reset_confirm = RESET_CONFIRM_CRT;
            s_reset_confirm_choice = 1;
            break;
        }
        break;

    case TAB_BLOOM_OPTIONS:
        switch (option_index) {
        case BLOOM_OPT_PRESET:
            s_bloom_preset = (SoeBloomPreset)((s_bloom_preset + direction + BLOOM_PRESET_COUNT) % BLOOM_PRESET_COUNT);
            SoeApplyBloomPreset(s_bloom_preset);
            SoeSaveBloomConfig();
            break;
        case BLOOM_OPT_INTENSITY:
            s_bloom_preset = BLOOM_PRESET_CUSTOM;
            s_bloom_intensity = (SoeBloomIntensity)((s_bloom_intensity + direction + BLOOM_INTENSITY_COUNT) % BLOOM_INTENSITY_COUNT);
            SoeSaveBloomConfig();
            break;
        case BLOOM_OPT_THRESHOLD:
            s_bloom_preset = BLOOM_PRESET_CUSTOM;
            s_bloom_threshold = (SoeBloomThreshold)((s_bloom_threshold + direction + BLOOM_THRESH_COUNT) % BLOOM_THRESH_COUNT);
            SoeSaveBloomConfig();
            break;
        case BLOOM_OPT_RADIUS:
            s_bloom_preset = BLOOM_PRESET_CUSTOM;
            s_bloom_radius = (SoeBloomRadius)((s_bloom_radius + direction + BLOOM_RADIUS_COUNT) % BLOOM_RADIUS_COUNT);
            SoeSaveBloomConfig();
            break;
        case BLOOM_OPT_BLEND:
            s_bloom_preset = BLOOM_PRESET_CUSTOM;
            s_bloom_blend = (SoeBloomBlend)((s_bloom_blend + direction + BLOOM_BLEND_COUNT) % BLOOM_BLEND_COUNT);
            SoeSaveBloomConfig();
            break;
        case BLOOM_OPT_TINT:
            s_bloom_preset = BLOOM_PRESET_CUSTOM;
            s_bloom_tint = (SoeBloomTint)((s_bloom_tint + direction + BLOOM_TINT_COUNT) % BLOOM_TINT_COUNT);
            SoeSaveBloomConfig();
            break;
        case BLOOM_OPT_RESET:
            s_reset_confirm = RESET_CONFIRM_BLOOM;
            s_reset_confirm_choice = 1;
            break;
        }
        break;

    case TAB_INPUT_OPTIONS:
        switch (option_index) {
        case INPUT_OPT_VOLUME: {
            int cur = g_config.volume + direction * 20;
            if (cur > 100) cur = 0;
            if (cur < 0) cur = 100;
            g_config.volume = (uint8)cur;
            g_config_dirty = true;
            break;
        }
        case INPUT_OPT_CONTROLLER:
        case INPUT_OPT_KEYBINDS:
        case INPUT_OPT_HOTKEY_MENU:
        case INPUT_OPT_HOTKEY_DEBUG:
            break;
        }
        break;

    case TAB_HELP_README:
        break;

    default:
        break;
    }
}



static void StepDebugWarpMap(int delta)
{
    int next = (int)s_warp_map_id + delta;
    while (next < 0) next += 256;
    s_warp_map_id = (uint16_t)(next % 256);
    snprintf(s_debug_toast, sizeof(s_debug_toast), "Map %u: %s",
             (unsigned)s_warp_map_id, GetMapName(s_warp_map_id));
    s_debug_toast_timer = 90;
}

static void AdjustDebugOption(int option_index, int direction)
{
    switch (option_index) {
    case DBG_GIVE_EVERYTHING:
        /* Opening Give Everything requires explicit confirmation */
        s_give_all_confirm_choice = 0;
        s_give_all_confirm_open = true;
        break;
    case DBG_INF_HP:
        s_inf_hp_active = !s_inf_hp_active;
        SoeSaveDebugConfig();
        snprintf(s_debug_toast, sizeof(s_debug_toast), "Infinite HP: %s",
                 s_inf_hp_active ? "ON" : "OFF");
        s_debug_toast_timer = 90;
        break;
    case DBG_INF_STAMINA:
        s_inf_stamina_active = !s_inf_stamina_active;
        SoeSaveDebugConfig();
        snprintf(s_debug_toast, sizeof(s_debug_toast), "Infinite Stamina: %s",
                 s_inf_stamina_active ? "ON" : "OFF");
        s_debug_toast_timer = 90;
        break;
    case DBG_ONE_HIT_KO:
        s_one_hit_ko_active = !s_one_hit_ko_active;
        SoeSaveDebugConfig();
        if (s_one_hit_ko_active) {
            uint16_t cur_atk = (uint16_t)(g_ram[0x0A3F] | (g_ram[0x0A40] << 8));
            if (cur_atk < 900) s_orig_boy_atk = cur_atk;
            uint16_t cur_dog = (uint16_t)(g_ram[0x0A8D] | (g_ram[0x0A8E] << 8));
            if (cur_dog < 900) s_orig_dog_atk = cur_dog;
        } else {
            if (s_orig_boy_atk > 0) {
                g_ram[0x0A3F] = (uint8_t)(s_orig_boy_atk & 0xFF);
                g_ram[0x0A40] = (uint8_t)(s_orig_boy_atk >> 8);
            }
            if (s_orig_dog_atk > 0) {
                g_ram[0x0A8D] = (uint8_t)(s_orig_dog_atk & 0xFF);
                g_ram[0x0A8E] = (uint8_t)(s_orig_dog_atk >> 8);
            }
        }
        snprintf(s_debug_toast, sizeof(s_debug_toast), "One-Hit KO: %s",
                 s_one_hit_ko_active ? "ON" : "OFF");
        s_debug_toast_timer = 90;
        break;
    case DBG_WALK_WALLS:
        s_walk_walls_active = !s_walk_walls_active;
        SoeSaveDebugConfig();
        snprintf(s_debug_toast, sizeof(s_debug_toast), "Walk Walls: %s",
                 s_walk_walls_active ? "ON" : "OFF");
        s_debug_toast_timer = 90;
        break;
    case DBG_DOG_FORM:
        DebugCycleDogForm(direction);
        break;
    case DBG_WARP:
        if (direction >= 0) OpenWarpWindow();
        else s_warp_win_open = false;
        break;
    case DBG_JUKEBOX:
        if (direction >= 0) OpenJukeboxWindow();
        else s_jukebox_open = false;
        break;
    case DBG_KEY_TRADE_ITEMS:
        if (direction >= 0) OpenKeyTradeWindow();
        else s_key_trade_open = false;
        break;
    }
}



static void TriggerScreenshotCapture(void)
{
    uint64_t now_ms = (uint64_t)SDL_GetTicks();
    if (now_ms >= s_screenshot_cooldown_until_ms && !s_pending_screenshot) {
        s_pending_screenshot = true;
        s_screenshot_cooldown_until_ms = now_ms + 3000u;
    }
}

static void TriggerDebugAction(int option_index)
{
    switch (option_index) {
    case DBG_GIVE_EVERYTHING:
        /* Always confirm before irreversibly maxing currencies, stats, gear, and items */
        s_give_all_confirm_choice = 0;
        s_give_all_confirm_open = true;
        break;
    case DBG_INF_HP:
        AdjustDebugOption(DBG_INF_HP, 1);
        break;
    case DBG_INF_STAMINA:
        AdjustDebugOption(DBG_INF_STAMINA, 1);
        break;
    case DBG_ONE_HIT_KO:
        AdjustDebugOption(DBG_ONE_HIT_KO, 1);
        break;
    case DBG_WALK_WALLS:
        AdjustDebugOption(DBG_WALK_WALLS, 1);
        break;
    case DBG_DOG_FORM:
        DebugCycleDogForm(1);
        break;
    case DBG_WARP:
        OpenWarpWindow();
        break;
    case DBG_JUKEBOX:
        OpenJukeboxWindow();
        break;
    case DBG_KEY_TRADE_ITEMS:
        OpenKeyTradeWindow();
        break;
    }
}



/* ── SDL Event Filter & Controller / Keyboard Polling ─────────────────────── */

static bool s_pending_toggle_event = false;
static bool s_pending_up_event = false;
static bool s_pending_down_event = false;
static bool s_pending_left_event = false;
static bool s_pending_right_event = false;
static bool s_pending_act_event = false;
static bool s_pending_cancel_event = false;
static bool s_pending_fast_prev = false;
static bool s_pending_fast_next = false;

static bool s_prev_toggle = false;
static bool s_prev_up = false;
static bool s_prev_down = false;
static bool s_prev_left = false;
static bool s_prev_right = false;
static bool s_prev_act = false;
static bool s_prev_fast_prev = false;
static bool s_prev_fast_next = false;
static bool s_prev_key_lf = false;
static bool s_prev_key_rt = false;

/*
 * Convert SDL window mouse coordinates to 342x224 (or 256x224) SNES frame coordinates,
 * taking into account viewport centering, letterboxing/pillarboxing, and aspect ratio.
 */
static void WindowToFrameCoords(float mx, float my, int *out_fx, int *out_fy)
{
    int win_w = 0, win_h = 0;
    if (g_window) SDL_GetWindowSize(g_window, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) {
        *out_fx = -1; *out_fy = -1;
        return;
    }

    int frame_w = snesrecomp_desktop_frame_width();
    int frame_h = snesrecomp_desktop_frame_height();

    SnesDisplayViewport vp;
    SnesDisplayAspect_ComputeViewport(frame_w, frame_h, win_w, win_h,
                                      SnesDisplayAspect_Clamp(g_config.display_aspect),
                                      g_config.ignore_aspect_ratio, false, &vp);

    if (vp.width <= 0 || vp.height <= 0) {
        *out_fx = (int)(mx * frame_w / win_w);
        *out_fy = (int)(my * frame_h / win_h);
        return;
    }

    *out_fx = (int)((mx - (float)vp.x) * (float)frame_w / (float)vp.width);
    *out_fy = (int)((my - (float)vp.y) * (float)frame_h / (float)vp.height);
}

static bool SDLCALL SlideMenuEventFilter(void *userdata, SDL_Event *event)
{
    (void)userdata;
    if (!event || !g_window) return true;

    bool any_overlay_active = s_menu_open || s_debug_overlay;

    /* Keyboard events */
    if (event->type == SDL_EVENT_KEY_DOWN) {
        SDL_Keycode sym = SNESRECOMP_SDL_EVENT_KEY(*event);

        /* Alt+Enter: Toggle Fullscreen / Windowed from anywhere */
        if ((sym == SDLK_RETURN || sym == SDLK_KP_ENTER) &&
            ((SNESRECOMP_SDL_EVENT_MOD(*event) & KMOD_ALT) != 0 ||
             (SDL_GetModState() & KMOD_ALT) != 0)) {
            if (!event->key.repeat && g_window) {
                bool was_fs = (SDL_GetWindowFlags(g_window) &
                               (SNESRECOMP_SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_FULLSCREEN)) != 0;
                snesrecomp_set_fullscreen(was_fs ? 0 : 1);
            }
            return false;
        }

        /* F1 or Esc key */
        if (sym == SDLK_F1 || sym == SDLK_ESCAPE) {
            if (s_give_all_confirm_open) {
                s_give_all_confirm_open = false;
                return false;
            }
            if (s_map_picker_open) {
                s_map_picker_open = false;
                return false;
            }
            if (sym == SDLK_ESCAPE && s_debug_overlay && s_warp_win_open) {
                s_warp_win_open = false;
                SoePlayUiSfx(2);
                return false;
            }
            if (!event->key.repeat) {
                s_pending_toggle_event = true;
            }
            return false; /* Consume event: prevents host hotkeys / pause / exit */
        }

        /* F2 or Grave/Tilde: Toggle Debug Menu */
        if (sym == SDLK_F2 || sym == SDLK_GRAVE) {
            if (!event->key.repeat) {
                s_debug_overlay = !s_debug_overlay;
                if (s_debug_overlay) {
                    s_menu_open = false;
                    s_map_picker_open = false;
                    SyncDogFormFromRam();
                } else {
                    s_jukebox_open = false;
                    s_key_trade_open = false;
                    s_warp_win_open = false;
                    s_give_all_confirm_open = false;
                }
            }
            return false;
        }

        /* Ctrl+S: Capture full-frame PNG screenshot (keeps S free for SNES X and WASD Down) */
        if (sym == SDLK_s && ((SNESRECOMP_SDL_EVENT_MOD(*event) & KMOD_CTRL) != 0 ||
                              (SDL_GetModState() & KMOD_CTRL) != 0)) {
            if (!event->key.repeat) {
                TriggerScreenshotCapture();
            }
            return false;
        }

        /* Direct 0 / 1 keypress to set map pillarbox value (only when Pillarbox Edit Mode is active) */
        if (PB_EDIT_ACTIVE && (!s_menu_open || s_map_picker_open) &&
            (sym == SDLK_0 || sym == SDLK_KP_0 || sym == SDLK_1 || sym == SDLK_KP_1)) {
            uint8_t val = (sym == SDLK_0 || sym == SDLK_KP_0) ? 0 : 1;
            uint16_t map_id = GetLoadedMapId();
            s_map_pillarbox[map_id & 0xFF] = val;
            SoeSaveMapPillarboxConfig();
            s_map_picker_open = false;
            s_map_badge_focused = false;
            snprintf(s_debug_toast, sizeof(s_debug_toast), "Map %u -> [%d] %s",
                     (unsigned)map_id, (int)val, val == 0 ? "Pillarbox" : "Widescreen");
            s_debug_toast_timer = 90;
            return false;
        }

        /* If slide menu or debug menu is active, route keyboard navigation to it */
        if (any_overlay_active) {
            if (!event->key.repeat) {
                switch (sym) {
                case SDLK_UP:
                case SDLK_w:
                    s_pending_up_event = true;
                    break;
                case SDLK_DOWN:
                case SDLK_s:
                    s_pending_down_event = true;
                    break;
                case SDLK_LEFT:
                case SDLK_a:
                    s_pending_left_event = true;
                    break;
                case SDLK_RIGHT:
                case SDLK_d:
                    s_pending_right_event = true;
                    break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE:
                case SDLK_e:
                    s_pending_act_event = true;
                    break;
                case SDLK_x:
                    if (s_debug_overlay && s_jukebox_open) {
                        s_pending_act_event = true;
                    }
                    break;
                case SDLK_y:
                case SDLK_c:
                case SDLK_k:
                    if (s_debug_overlay && s_jukebox_open) {
                        ToggleJukeboxLockSong();
                    }
                    break;
                case SDLK_z:
                case SDLK_BACKSPACE:
                    s_pending_cancel_event = true;
                    break;
                case SDLK_LEFTBRACKET:
                case SDLK_PAGEUP:
                    s_pending_fast_prev = true;
                    break;
                case SDLK_RIGHTBRACKET:
                case SDLK_PAGEDOWN:
                    s_pending_fast_next = true;
                    break;
                default:
                    break;
                }
            }
            return false;
        }
    } else if (event->type == SDL_EVENT_KEY_UP) {
        SDL_Keycode sym = SNESRECOMP_SDL_EVENT_KEY(*event);
        if (sym == SDLK_F1 || sym == SDLK_ESCAPE || any_overlay_active ||
            (PB_EDIT_ACTIVE && s_map_picker_open)) {
            return false;
        }
    }

    /* Show mouse cursor on movement/clicks and reset auto-hide timer */
    if (event->type == SDL_EVENT_MOUSE_MOTION || event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        s_last_mouse_move_ticks = SDL_GetTicks();
        if (!s_cursor_visible) {
            snesrecomp_sdl_show_cursor(true);
            s_cursor_visible = true;
        }
    }

    /* Mouse click events */
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT) {
        int fx = -1, fy = -1;
        WindowToFrameCoords((float)event->button.x, (float)event->button.y, &fx, &fy);
        int frame_w = snesrecomp_desktop_frame_width();
        int frame_h = snesrecomp_desktop_frame_height();

        uint16_t cur_map_id = GetLoadedMapId();
        uint8_t cur_pb_val = s_map_pillarbox[cur_map_id & 0xFF];
        int badge_x, badge_y, badge_w, badge_h;
        GetMapBadgeLayout(frame_w, cur_map_id, cur_pb_val,
                          &badge_x, &badge_y, &badge_w, &badge_h,
                          NULL, NULL, NULL, 0);

        int picker_w = badge_w;
        int picker_h = 42;
        int picker_x = badge_x;
        int picker_y = badge_y + badge_h + 2;

        /* 1. If number picker popup is open, check option clicks or dismiss */
        if (PB_EDIT_ACTIVE && s_map_picker_open && !s_debug_overlay) {
            uint16_t map_id = cur_map_id;
            int r0_y = picker_y + 16;
            int r1_y = picker_y + 28;

            if (fx >= picker_x && fx <= picker_x + picker_w) {
                if (fy >= r0_y - 1 && fy < r0_y + 11) {
                    /* Clicked [0] 4:3 Pillarbox */
                    s_map_pillarbox[map_id & 0xFF] = 0;
                    SoeSaveMapPillarboxConfig();
                    s_map_picker_open = false;
                    snprintf(s_debug_toast, sizeof(s_debug_toast), "Map %u -> [0] Pillarbox", (unsigned)map_id);
                    s_debug_toast_timer = 90;
                    return false;
                }
                if (fy >= r1_y - 1 && fy < r1_y + 11) {
                    /* Clicked [1] Widescreen */
                    s_map_pillarbox[map_id & 0xFF] = 1;
                    SoeSaveMapPillarboxConfig();
                    s_map_picker_open = false;
                    snprintf(s_debug_toast, sizeof(s_debug_toast), "Map %u -> [1] Widescreen", (unsigned)map_id);
                    s_debug_toast_timer = 90;
                    return false;
                }
            }

            /* Clicking anywhere else (even letterbox bars or badge) closes picker */
            s_map_picker_open = false;
            return false;
        }

        if (fx >= 0 && fy >= 0 && fx < frame_w && fy < frame_h) {
            /* 2. Check Upper-Right Map Badge click (pops up number picker when Pillarbox Edit Mode is on) */
            if (PB_EDIT_ACTIVE && !s_debug_overlay && !s_menu_open &&
                fx >= badge_x && fx <= badge_x + badge_w && fy >= badge_y && fy <= badge_y + badge_h) {
                s_map_picker_open = true;
                return false;
            }

            /* 3. Check Give Everything confirmation dialog, Jukebox & Debug Window clicks */
            if (s_debug_overlay) {
                if (!s_give_all_confirm_open &&
                    fx >= badge_x && fx <= badge_x + badge_w && fy >= badge_y && fy <= badge_y + badge_h) {
                    uint8_t nv = s_map_pillarbox[cur_map_id & 0xFF] ? 0 : 1;
                    s_map_pillarbox[cur_map_id & 0xFF] = nv;
                    SoeSaveMapPillarboxConfig();
                    return false;
                }
                if (s_give_all_confirm_open) {
                    int dlg_x, dlg_y, dlg_w, dlg_h;
                    GetGiveAllConfirmRect(frame_w, frame_h, &dlg_x, &dlg_y, &dlg_w, &dlg_h);
                    if (fx >= dlg_x + 16 && fx < dlg_x + 104 && fy >= dlg_y + 64 && fy < dlg_y + 77) {
                        s_give_all_confirm_open = false;
                    } else if (fx >= dlg_x + dlg_w - 112 && fx < dlg_x + dlg_w - 16 &&
                               fy >= dlg_y + 64 && fy < dlg_y + 77) {
                        DebugGiveEverything();
                        s_give_all_confirm_open = false;
                    } else if (fx < dlg_x || fx >= dlg_x + dlg_w || fy < dlg_y || fy >= dlg_y + dlg_h) {
                        s_give_all_confirm_open = false;
                    }
                    return false;
                }

                if (s_warp_win_open) {
                    int wp_x, wp_y, wp_w, wp_h;
                    GetWarpWindowRect(frame_w, &wp_x, &wp_y, &wp_w, &wp_h);
                    int pv_x, pv_y, pv_w, pv_h;
                    if (GetWarpPreviewWindowRect(frame_w, wp_x, wp_y, wp_w, wp_h,
                                                 &pv_x, &pv_y, &pv_w, &pv_h) &&
                        fx >= pv_x && fx < pv_x + pv_w && fy >= pv_y && fy < pv_y + pv_h) {
                        return false;
                    }
                    if (fx >= wp_x && fx < wp_x + wp_w && fy >= wp_y && fy < wp_y + wp_h) {
                        BuildWarpLocationList();
                        if (fy >= wp_y + 30 && fy < wp_y + 31 + kWarpVisibleRows * 11) {
                            int r = (fy - (wp_y + 31)) / 11;
                            int idx = s_warp_scroll + r;
                            if (r >= 0 && r < kWarpVisibleRows && idx >= 0 && idx < s_warp_location_count) {
                                bool was_selected = (s_warp_cursor == idx);
                                s_warp_cursor = idx;
                                ClampWarpScroll();
                                if (was_selected) {
                                    ActivateWarpSelection();
                                }
                            }
                        } else if (fy >= wp_y + 135 && fy < wp_y + 158) {
                            if (fx >= wp_x + wp_w / 2) {
                                s_warp_win_open = false;
                            } else {
                                ActivateWarpSelection();
                            }
                        }
                        return false;
                    }
                }

                if (s_key_trade_open) {
                    int kt_x, kt_y, kt_w, kt_h;
                    GetJukeboxWindowRect(frame_w, &kt_x, &kt_y, &kt_w, &kt_h);
                    if (fx >= kt_x && fx < kt_x + kt_w && fy >= kt_y && fy < kt_y + kt_h) {
                        if (fy >= kt_y + 30 && fy < kt_y + 31 + kKeyTradeVisibleRows * 11) {
                            int r = (fy - (kt_y + 31)) / 11;
                            int idx = s_key_trade_scroll + r;
                            if (r >= 0 && r < kKeyTradeVisibleRows && idx >= 0 && idx < kKeyTradeItemCount) {
                                s_key_trade_cursor = idx;
                                ClampKeyTradeScroll();
                                ActivateKeyTradeSelection(1);
                            }
                        } else if (fy >= kt_y + 135 && fy < kt_y + 158) {
                            if (fx >= kt_x + kt_w / 2) {
                                s_key_trade_open = false;
                            } else {
                                ActivateKeyTradeSelection(1);
                            }
                        }
                        return false;
                    }
                }

                if (s_jukebox_open) {
                    int jb_x, jb_y, jb_w, jb_h;
                    GetJukeboxWindowRect(frame_w, &jb_x, &jb_y, &jb_w, &jb_h);
                    if (fx >= jb_x && fx < jb_x + jb_w && fy >= jb_y && fy < jb_y + jb_h) {
                        BuildJukeboxSongList();
                        if (fy >= jb_y + 29 && fy < jb_y + 42) {
                            /* Click on top '[X] Keep Song Playing' checkbox row */
                            s_jukebox_keep_focused = true;
                            ToggleJukeboxLockSong();
                        } else if (fy >= jb_y + 43 && fy < jb_y + 44 + kJukeboxVisibleRows * 11) {
                            int r = (fy - (jb_y + 44)) / 11;
                            int idx = s_jukebox_scroll + r;
                            if (r >= 0 && r < kJukeboxVisibleRows && idx >= 0 && idx < s_jukebox_song_count) {
                                s_jukebox_keep_focused = false;
                                s_jukebox_cursor = idx;
                                ClampJukeboxScroll();
                                ActivateJukeboxSelection();
                            }
                        } else if (fy >= jb_y + 135 && fy < jb_y + 146 && fx >= jb_x + jb_w / 2) {
                            /* Click on '[B] Close' half of footer */
                            s_jukebox_open = false;
                        } else if (fy >= jb_y + 135 && fy < jb_y + 146 && fx < jb_x + jb_w / 2) {
                            /* Click on '[A] Play' half of footer */
                            ActivateJukeboxSelection();
                        } else if (fy >= jb_y + 146 && fy < jb_y + 158) {
                            /* Click on '[Y] Keep Song' footer hint */
                            ToggleJukeboxLockSong();
                        }
                        return false;
                    }
                }

                int dbg_x, dbg_y, dbg_w, dbg_h;
                GetDebugWindowRect(frame_w, &dbg_x, &dbg_y, &dbg_w, &dbg_h);

                /* Click on map ID header line in debug window toggles 0 <-> 1 */
                if (fx >= dbg_x + 8 && fx <= dbg_x + dbg_w - 8 && fy >= dbg_y + 16 && fy <= dbg_y + 27) {
                    uint16_t map_id = GetLoadedMapId();
                    uint8_t cur_val = s_map_pillarbox[map_id & 0xFF];
                    uint8_t new_val = (cur_val == 0) ? 1 : 0;
                    s_map_pillarbox[map_id & 0xFF] = new_val;
                    SoeSaveMapPillarboxConfig();
                    snprintf(s_debug_toast, sizeof(s_debug_toast), "Map %u -> %d", (unsigned)map_id, (int)new_val);
                    s_debug_toast_timer = 90;
                    return false;
                }

                if (!s_warp_win_open && s_warp_slide <= 0.001f &&
                    fx >= dbg_x + 8 && fx < dbg_x + dbg_w - 8 && fy >= dbg_y + 29 && fy < dbg_y + dbg_h - 8) {
                    if (fy >= dbg_y + 131) {
                        /* Click on 'B: Close' half of footer closes Debug Tools */
                        if (fx >= dbg_x + dbg_w / 2) {
                            s_debug_overlay = false;
                            s_jukebox_open = false;
                            s_key_trade_open = false;
                            s_warp_win_open = false;
                            s_give_all_confirm_open = false;
                        } else {
                            TriggerDebugAction(s_dbg_selected_row);
                        }
                        return false;
                    }
                    int row = (fy - (dbg_y + 30)) / 11;
                    if (row >= 0 && row < DBG_TOTAL_COUNT) {
                        s_dbg_selected_row = row;
                        if (row == DBG_DOG_FORM && fx >= dbg_x + dbg_w - 26) {
                            AdjustDebugOption(DBG_DOG_FORM, 1);
                        } else {
                            TriggerDebugAction(row);
                        }
                        return false;
                    }
                }
            }

            /* 4. Check Slide Drawer clicks (Tabbed format) */
            if (s_menu_open) {
                int drawer_w = (int)GetMenuWidth();
                if (fx <= drawer_w) {
                    /* Check if a tab bar button was clicked (2 rows: y 17..28, y 30..41) */
                    if ((fy >= 17 && fy <= 28) || (fy >= 30 && fy <= 41)) {
                        int row_idx = (fy >= 30) ? 1 : 0;
                        int tab_bar_x = 6;
                        int tab_bar_w = drawer_w - 12;
                        int tab_w = tab_bar_w / 4;
                        int col_clicked = (fx - tab_bar_x) / tab_w;
                        if (col_clicked >= 0 && col_clicked < 4) {
                            int tab_clicked = row_idx * 4 + col_clicked;
                            if (tab_clicked >= 0 && tab_clicked < TAB_COUNT) {
                                if ((SoeMenuTab)tab_clicked != s_active_tab) {
                                    s_active_tab = (SoeMenuTab)tab_clicked;
                                    SoePlayUiSfx(0);
                                }
                                return false;
                            }
                        }
                    }

                    /* Check if an option row was clicked */
                    int start_y = 36;
                    int row_h = 16;
                    GetTabRowLayout(s_active_tab, &start_y, &row_h);
                    int count = GetTabOptionCount(s_active_tab);
                    int clicked_row = -1;
                    if (s_active_tab == TAB_GAME_OPTIONS) {
#ifdef __ANDROID__
                        int exit_y = GetGameTabRowY(GAME_OPT_EXIT_TO_DESKTOP);
                        if (fy >= exit_y && fy < exit_y + 9) {
                            clicked_row = GAME_OPT_EXIT_TO_DESKTOP;
                        } else {
                            for (int r = 0; r < GAME_OPT_EXIT_TO_DESKTOP; r++) {
                                int ry = GetGameTabRowY(r);
                                if (fy >= ry && fy < ry + 9) {
                                    clicked_row = r;
                                    break;
                                }
                            }
                        }
#else
                        int exit_y = GetGameTabRowY(GAME_OPT_EXIT_TO_LAUNCHER);
                        if (fy >= exit_y && fy < exit_y + 9) {
                            int lx = 0, lw = 0, dx = 0, dw = 0;
                            GetGameExitButtonBounds(0, drawer_w, &lx, &lw, &dx, &dw);
                            clicked_row = (fx >= dx - 6) ? GAME_OPT_EXIT_TO_DESKTOP : GAME_OPT_EXIT_TO_LAUNCHER;
                        } else {
                            for (int r = 0; r < GAME_OPT_EXIT_TO_LAUNCHER; r++) {
                                int ry = GetGameTabRowY(r);
                                if (fy >= ry && fy < ry + 9) {
                                    clicked_row = r;
                                    break;
                                }
                            }
                        }
#endif
                    } else if (s_active_tab == TAB_VIDEO_OPTIONS) {
                        int max_vis = (count < kAvVisibleRows) ? count : kAvVisibleRows;
                        if (fy >= start_y && fy < start_y + max_vis * row_h) {
                            clicked_row = s_av_scroll + (fy - start_y) / row_h;
                        }
                    } else if (fy >= start_y && fy < start_y + count * row_h) {
                        clicked_row = (fy - start_y) / row_h;
                    }
                    if (clicked_row >= 0 && clicked_row < count) {
                        int row = clicked_row;
                        s_tab_selected_row[s_active_tab] = row;
#ifdef __ANDROID__
                        bool is_exit_row = (s_active_tab == TAB_GAME_OPTIONS && row == GAME_OPT_EXIT_TO_DESKTOP);
#else
                        bool is_exit_row = (s_active_tab == TAB_GAME_OPTIONS &&
                                            (row == GAME_OPT_EXIT_TO_LAUNCHER || row == GAME_OPT_EXIT_TO_DESKTOP));
#endif
                        if (!is_exit_row || event->button.button == SDL_BUTTON_LEFT) {
                            SoePlayUiSfx(1);
                            AdjustTabOption(s_active_tab, row, (event->button.button == SDL_BUTTON_RIGHT) ? -1 : 1);
                        }
                        return false;
                    }
                } else {
                    s_menu_open = false;
                    SoePlayUiSfx(2);
                    return false;
                }
            } else {
                /* Check if top-left handle tab was clicked */
                if (fx <= 86 && fy >= 6 && fy <= 24) {
                    s_menu_open = true;
                    SoePlayUiSfx(1);
                    return false;
                }
            }
        }
    } else if (event->type == SDL_EVENT_MOUSE_MOTION && s_menu_open && g_window) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(g_window, &win_w, &win_h);
        if (win_w > 0 && win_h > 0) {
            int frame_w = snesrecomp_desktop_frame_width();
            int frame_h = snesrecomp_desktop_frame_height();
            if (frame_w <= 0) frame_w = 256;
            if (frame_h <= 0) frame_h = 224;
            int fx = (int)(event->motion.x * frame_w / win_w);
            int fy = (int)(event->motion.y * frame_h / win_h);
            int drawer_w = (int)GetMenuWidth();
            if (fx <= drawer_w) {
                int start_y = 36, row_h = 16;
                GetTabRowLayout(s_active_tab, &start_y, &row_h);
                int count = GetTabOptionCount(s_active_tab);
                int hover_row = -1;
                if (s_active_tab == TAB_GAME_OPTIONS) {
#ifdef __ANDROID__
                    int exit_y = GetGameTabRowY(GAME_OPT_EXIT_TO_DESKTOP);
                    if (fy >= exit_y && fy < exit_y + 9) {
                        hover_row = GAME_OPT_EXIT_TO_DESKTOP;
                    } else {
                        for (int r = 0; r < GAME_OPT_EXIT_TO_DESKTOP; r++) {
                            int ry = GetGameTabRowY(r);
                            if (fy >= ry && fy < ry + 9) {
                                hover_row = r;
                                break;
                            }
                        }
                    }
#else
                    int exit_y = GetGameTabRowY(GAME_OPT_EXIT_TO_LAUNCHER);
                    if (fy >= exit_y && fy < exit_y + 9) {
                        int lx = 0, lw = 0, dx = 0, dw = 0;
                        GetGameExitButtonBounds(0, drawer_w, &lx, &lw, &dx, &dw);
                        hover_row = (fx >= dx - 6) ? GAME_OPT_EXIT_TO_DESKTOP : GAME_OPT_EXIT_TO_LAUNCHER;
                    } else {
                        for (int r = 0; r < GAME_OPT_EXIT_TO_LAUNCHER; r++) {
                            int ry = GetGameTabRowY(r);
                            if (fy >= ry && fy < ry + 9) {
                                hover_row = r;
                                break;
                            }
                        }
                    }
#endif
                } else if (s_active_tab == TAB_VIDEO_OPTIONS) {
                    int max_vis = (count < kAvVisibleRows) ? count : kAvVisibleRows;
                    if (fy >= start_y && fy < start_y + max_vis * row_h) {
                        hover_row = s_av_scroll + (fy - start_y) / row_h;
                    }
                } else if (fy >= start_y && fy < start_y + count * row_h) {
                    hover_row = (fy - start_y) / row_h;
                }
                if (hover_row >= 0 && hover_row < count &&
                    hover_row != s_tab_selected_row[s_active_tab]) {
                    s_tab_selected_row[s_active_tab] = hover_row;
                    SoePlayUiSfx(0);
                }
            }
        }
    }

    return true;
}

/*
 * Direct per-frame keyboard & gamepad polling for the Slide Menu and Debug Menu:
 * - Toggles the Slide Menu via F1, Esc, or unused controller buttons:
 *     * Share button (SDL_GAMEPAD_BUTTON_MISC1)
 *     * L2 / R2 Analog Triggers (SDL_GAMEPAD_AXIS_LEFT_TRIGGER / RIGHT_TRIGGER)
 *     * (Explicitly NEVER uses SDL_GAMEPAD_BUTTON_GUIDE, the Xbox light-up button)
 * - Routes controller D-Pad, Left Stick, and A/B buttons to the Slide Menu when
 *   s_menu_open is true, or to the Debug Menu when s_debug_overlay is true!
 */
static void PollMenuKeyboardDirect(void)
{
    int num_keys = 0;
    const bool *ks = SDL_GetKeyboardState(&num_keys);

    bool key_toggle = (ks && num_keys > 0) &&
                      ((SDL_SCANCODE_F1 < num_keys && ks[SDL_SCANCODE_F1]) ||
                       (SDL_SCANCODE_ESCAPE < num_keys && ks[SDL_SCANCODE_ESCAPE]));
    bool pad_menu_toggle = false;
    bool pad_debug_toggle = false;
    bool pad_up = false;
    bool pad_dn = false;
    bool pad_lf = false;
    bool pad_rt = false;
    bool pad_act = false;
    bool pad_cancel = false;
    bool pad_keep_song = false;
    bool pad_fast_prev = false;
    bool pad_fast_next = false;

#if SNESRECOMP_SDL3
    int num_pads = 0;
    SDL_JoystickID *pad_ids = SDL_GetGamepads(&num_pads);
    if (pad_ids) {
        for (int i = 0; i < num_pads; i++) {
            SDL_Gamepad *gp = SDL_GetGamepadFromID(pad_ids[i]);
            if (!gp) continue;

            /* LT (Left Trigger), Share: Toggles the Slide Menu */
            if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_MISC1) ||
                SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) {
                pad_menu_toggle = true;
            }

            /* RT (Right Trigger), R3: Toggles the Debug Menu */
            if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_STICK) ||
                SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) {
                pad_debug_toggle = true;
            }

            if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_UP))    pad_up = true;
            if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  pad_dn = true;
            if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  pad_lf = true;
            if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) pad_rt = true;

            /* Controller A (South) triggers Action; B (East) dismisses Debug Menu; Y/X toggles Keep Song in Jukebox */
            if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH)) pad_act = true;
            if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST))  pad_cancel = true;
            if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_NORTH) ||
                SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_WEST))  pad_keep_song = true;

            /* LB / RB for fast stepping (-10 / +10) */
            if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  pad_fast_prev = true;
            if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) pad_fast_next = true;

            Sint16 ay = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY);
            Sint16 ax = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX);
            if (ay < -16000) pad_up = true;
            if (ay >  16000) pad_dn = true;
            if (ax < -16000) pad_lf = true;
            if (ax >  16000) pad_rt = true;
        }
        SDL_free(pad_ids);
    }
#endif

#ifdef __ANDROID__
    if (SoeAndroidTouchConsumeMenuToggle()) {
        s_pending_toggle_event = true;
    }
    if (s_menu_open || s_debug_overlay || s_jukebox_open || s_key_trade_open ||
        s_warp_win_open || s_give_all_confirm_open || s_map_picker_open) {
        uint32_t tpad = SoeAndroidTouchGetPadMask();
        if (tpad & (1u << 4))  pad_up = true;        /* SNES_PAD_UP */
        if (tpad & (1u << 5))  pad_dn = true;        /* SNES_PAD_DOWN */
        if (tpad & (1u << 6))  pad_lf = true;        /* SNES_PAD_LEFT */
        if (tpad & (1u << 7))  pad_rt = true;        /* SNES_PAD_RIGHT */
        if (tpad & (1u << 8))  pad_act = true;       /* SNES_PAD_A */
        if (tpad & (1u << 0))  pad_cancel = true;    /* SNES_PAD_B */
        if (tpad & ((1u << 1) | (1u << 9))) pad_keep_song = true; /* Y / X */
        if (tpad & (1u << 10)) pad_fast_prev = true; /* SNES_PAD_L */
        if (tpad & (1u << 11)) pad_fast_next = true; /* SNES_PAD_R */
    }
#endif

    bool menu_toggle_down = key_toggle || pad_menu_toggle || s_pending_toggle_event;
    s_pending_toggle_event = false;

    /* LT / F1 / Esc toggles the Slide Menu */
    if (menu_toggle_down && !s_prev_toggle) {
        s_menu_open = !s_menu_open;
        SoePlayUiSfx(s_menu_open ? 1 : 2);
        if (s_menu_open) {
            s_debug_overlay = false;
            s_jukebox_open = false;
            s_key_trade_open = false;
            s_warp_win_open = false;
            s_give_all_confirm_open = false;
            s_map_picker_open = false;
        }
    }
    s_prev_toggle = menu_toggle_down;

    /* RT toggles the Debug Menu (hold 3s to open, tap to close) */
    {
        static uint64_t s_rt_hold_start_ms = 0;
        static bool s_rt_hold_triggered = false;
        if (pad_debug_toggle && !s_prev_pad_debug) {
            if (s_debug_overlay) {
                s_debug_overlay = false;
                s_jukebox_open = false;
                s_key_trade_open = false;
                s_warp_win_open = false;
                s_give_all_confirm_open = false;
                s_rt_hold_triggered = true;
            } else if (GetLoadedMapId() == 9) {
                s_debug_overlay = true;
                s_menu_open = false;
                s_map_picker_open = false;
                SyncDogFormFromRam();
                s_rt_hold_triggered = true;
            } else {
                s_rt_hold_start_ms = SDL_GetTicks();
                s_rt_hold_triggered = false;
            }
        } else if (pad_debug_toggle && !s_debug_overlay && !s_rt_hold_triggered) {
            if (SDL_GetTicks() - s_rt_hold_start_ms >= 2000u) {
                s_debug_overlay = true;
                s_menu_open = false;
                s_map_picker_open = false;
                SyncDogFormFromRam();
                s_rt_hold_triggered = true;
            }
        } else if (!pad_debug_toggle) {
            s_rt_hold_triggered = false;
        }
    }
    s_prev_pad_debug = pad_debug_toggle;

    /* Suppress guest controller input while either Slide Menu or Debug Menu is active */
    bool any_overlay_active = s_menu_open || s_debug_overlay;
    snesrecomp_desktop_set_guest_input_suppressed(any_overlay_active);

    static bool s_prev_pad_cancel = false;

    if (!any_overlay_active) {
        s_pending_up_event = false;
        s_pending_down_event = false;
        s_pending_left_event = false;
        s_pending_right_event = false;
        s_pending_act_event = false;
        s_pending_cancel_event = false;
        s_pending_fast_prev = false;
        s_pending_fast_next = false;
        s_prev_up = false;
        s_prev_down = false;
        s_prev_left = false;
        s_prev_right = false;
        s_prev_key_lf = false;
        s_prev_key_rt = false;
        s_prev_act = false;
        s_prev_pad_cancel = pad_cancel;
        s_prev_fast_prev = false;
        s_prev_fast_next = false;
        return;
    }

    bool ctrl_held = (SDL_GetModState() & KMOD_CTRL) != 0;
    bool up_down = pad_up || s_pending_up_event ||
                   ((ks && num_keys > 0) &&
                    ((SDL_SCANCODE_UP < num_keys && ks[SDL_SCANCODE_UP]) ||
                     (SDL_SCANCODE_W < num_keys && ks[SDL_SCANCODE_W])));
    bool dn_down = pad_dn || s_pending_down_event ||
                   ((ks && num_keys > 0) &&
                    ((SDL_SCANCODE_DOWN < num_keys && ks[SDL_SCANCODE_DOWN]) ||
                     (!ctrl_held && SDL_SCANCODE_S < num_keys && ks[SDL_SCANCODE_S])));

    bool key_lf = s_pending_left_event ||
                  ((ks && num_keys > 0) &&
                   ((SDL_SCANCODE_LEFT < num_keys && ks[SDL_SCANCODE_LEFT]) ||
                    (SDL_SCANCODE_A < num_keys && ks[SDL_SCANCODE_A])));
    bool key_rt = s_pending_right_event ||
                  ((ks && num_keys > 0) &&
                   ((SDL_SCANCODE_RIGHT < num_keys && ks[SDL_SCANCODE_RIGHT]) ||
                    (SDL_SCANCODE_D < num_keys && ks[SDL_SCANCODE_D])));

    bool lf_down = pad_lf || key_lf;
    bool rt_down = pad_rt || key_rt;

    bool alt_held = (SDL_GetModState() & KMOD_ALT) != 0;
    bool act_down = pad_act || s_pending_act_event ||
                    ((ks && num_keys > 0) &&
                     ((!alt_held && SDL_SCANCODE_RETURN < num_keys && ks[SDL_SCANCODE_RETURN]) ||
                      (SDL_SCANCODE_SPACE < num_keys && ks[SDL_SCANCODE_SPACE]) ||
                      (SDL_SCANCODE_E < num_keys && ks[SDL_SCANCODE_E])));
    bool cancel_down = pad_cancel || s_pending_cancel_event;
    bool fast_prev_down = pad_fast_prev || s_pending_fast_prev ||
                          ((ks && num_keys > 0) &&
                           ((SDL_SCANCODE_LEFTBRACKET < num_keys && ks[SDL_SCANCODE_LEFTBRACKET]) ||
                            (SDL_SCANCODE_PAGEUP < num_keys && ks[SDL_SCANCODE_PAGEUP])));
    bool fast_next_down = pad_fast_next || s_pending_fast_next ||
                          ((ks && num_keys > 0) &&
                           ((SDL_SCANCODE_RIGHTBRACKET < num_keys && ks[SDL_SCANCODE_RIGHTBRACKET]) ||
                            (SDL_SCANCODE_PAGEDOWN < num_keys && ks[SDL_SCANCODE_PAGEDOWN])));

    s_pending_up_event = false;
    s_pending_down_event = false;
    s_pending_left_event = false;
    s_pending_right_event = false;
    s_pending_act_event = false;
    s_pending_cancel_event = false;
    s_pending_fast_prev = false;
    s_pending_fast_next = false;

    if (s_menu_open) {
        /* Route controller & keyboard navigation to the Tabbed Slide-Out Menu */
        int cur_row = s_tab_selected_row[s_active_tab];
        int count = GetTabOptionCount(s_active_tab);

        if (s_reset_confirm != RESET_CONFIRM_NONE) {
            if (cancel_down && !s_prev_pad_cancel) {
                s_reset_confirm = RESET_CONFIRM_NONE;
                SoePlayUiSfx(2);
            } else if ((lf_down && !s_prev_left) || (rt_down && !s_prev_right) ||
                       (up_down && !s_prev_up) || (dn_down && !s_prev_down)) {
                s_reset_confirm_choice ^= 1;
                SoePlayUiSfx(0);
            } else if (act_down && !s_prev_act) {
                SoePlayUiSfx(1);
                if (s_reset_confirm_choice == 0) {
                    switch (s_reset_confirm) {
                        case RESET_CONFIRM_ALL:
                            SoeResetAllToDefault();
                            break;
                        case RESET_CONFIRM_VIDEO:
                            SoeResetVideoToDefault(true);
                            break;
                        case RESET_CONFIRM_CRT:
                            SoeApplyDefaultCrtSettings();
                            SoeSaveCrtConfig();
                            SoeSetMenuToast(">> CRT settings restored to default! <<");
                            break;
                        case RESET_CONFIRM_BLOOM:
                            SoeApplyDefaultBloomSettings();
                            SoeSaveBloomConfig();
                            SoeSetMenuToast(">> Bloom settings restored to default! <<");
                            break;
                        case RESET_CONFIRM_EXIT_LAUNCHER:
                            SoeSaveAllConfigsAndExitToLauncher();
                            break;
                        case RESET_CONFIRM_EXIT_DESKTOP:
                            SoeSaveAllConfigsAndExit();
                            break;
                        default:
                            break;
                    }
                }
                s_reset_confirm = RESET_CONFIRM_NONE;
            }
        } else if (cancel_down && !s_prev_pad_cancel) {
            /* B (East) / Z / Backspace closes the Settings slide-out menu */
            s_menu_open = false;
            SoePlayUiSfx(2);
        } else if (fast_prev_down && !s_prev_fast_prev) {
            /* LB / [: Previous Tab */
            s_active_tab = (SoeMenuTab)(((int)s_active_tab - 1 + TAB_COUNT) % TAB_COUNT);
            SoePlayUiSfx(0);
        } else if (fast_next_down && !s_prev_fast_next) {
            /* RB / ]: Next Tab */
            s_active_tab = (SoeMenuTab)(((int)s_active_tab + 1) % TAB_COUNT);
            SoePlayUiSfx(0);
        } else if (up_down && !s_prev_up) {
            /* Joystick / D-Pad Up: Move selection cursor up */
            if (count > 0) {
                cur_row = (cur_row - 1 + count) % count;
                s_tab_selected_row[s_active_tab] = cur_row;
                SoePlayUiSfx(0);
            }
        } else if (dn_down && !s_prev_down) {
            /* Joystick / D-Pad Down: Move selection cursor down */
            if (count > 0) {
                cur_row = (cur_row + 1) % count;
                s_tab_selected_row[s_active_tab] = cur_row;
                SoePlayUiSfx(0);
            }
        } else if (act_down && !s_prev_act) {
            SoePlayUiSfx(1);
            AdjustTabOption(s_active_tab, cur_row, 1);
        } else if (key_lf && !s_prev_key_lf) {
            /* Keyboard Left Arrow: step option backward (or switch Exit buttons) */
#ifndef __ANDROID__
            if (s_active_tab == TAB_GAME_OPTIONS && (cur_row == GAME_OPT_EXIT_TO_DESKTOP || cur_row == GAME_OPT_EXIT_TO_LAUNCHER)) {
                if (cur_row == GAME_OPT_EXIT_TO_DESKTOP) {
                    s_tab_selected_row[s_active_tab] = GAME_OPT_EXIT_TO_LAUNCHER;
                    SoePlayUiSfx(0);
                }
            } else
#endif
            {
                SoePlayUiSfx(1);
                AdjustTabOption(s_active_tab, cur_row, -1);
            }
        } else if (key_rt && !s_prev_key_rt) {
            /* Keyboard Right Arrow: step option forward (or switch Exit buttons) */
#ifndef __ANDROID__
            if (s_active_tab == TAB_GAME_OPTIONS && (cur_row == GAME_OPT_EXIT_TO_DESKTOP || cur_row == GAME_OPT_EXIT_TO_LAUNCHER)) {
                if (cur_row == GAME_OPT_EXIT_TO_LAUNCHER) {
                    s_tab_selected_row[s_active_tab] = GAME_OPT_EXIT_TO_DESKTOP;
                    SoePlayUiSfx(0);
                }
            } else
#endif
            {
                SoePlayUiSfx(1);
                AdjustTabOption(s_active_tab, cur_row, 1);
            }
        }
        /* Note: Controller joystick (ax) and D-pad Left/Right are intentionally ignored here
         * so scrolling down or up never accidentally modifies settings! */
    } else if (s_debug_overlay && s_give_all_confirm_open) {
        /* Route modal input to the "Give Everything" confirmation dialog */
        if (cancel_down && !s_prev_pad_cancel) {
            s_give_all_confirm_open = false;
        } else if ((lf_down && !s_prev_left) || (rt_down && !s_prev_right) ||
                   (up_down && !s_prev_up) || (dn_down && !s_prev_down)) {
            s_give_all_confirm_choice ^= 1;
        } else if (act_down && !s_prev_act) {
            if (s_give_all_confirm_choice == 1) {
                DebugGiveEverything();
            }
            s_give_all_confirm_open = false;
        }
    } else if (s_debug_overlay && s_warp_win_open) {
        /* Route navigation to Warp Locations sub-window; B closes Warp window only */
        if (cancel_down && !s_prev_pad_cancel) {
            s_warp_win_open = false;
        } else if (up_down && !s_prev_up) {
            StepWarpCursor(-1);
        } else if (dn_down && !s_prev_down) {
            StepWarpCursor(1);
        } else if ((lf_down && !s_prev_left) || (fast_prev_down && !s_prev_fast_prev)) {
            StepWarpCursor(-10);
        } else if ((rt_down && !s_prev_right) || (fast_next_down && !s_prev_fast_next)) {
            StepWarpCursor(10);
        } else if (act_down && !s_prev_act) {
            ActivateWarpSelection();
        }
    } else if (s_debug_overlay && s_key_trade_open) {
        /* Route navigation to Key & Trade Items sub-window; B closes sub-window only */
        if (cancel_down && !s_prev_pad_cancel) {
            s_key_trade_open = false;
        } else if (up_down && !s_prev_up) {
            StepKeyTradeCursor(-1);
        } else if (dn_down && !s_prev_down) {
            StepKeyTradeCursor(1);
        } else if (fast_prev_down && !s_prev_fast_prev) {
            StepKeyTradeCursor(-5);
        } else if (fast_next_down && !s_prev_fast_next) {
            StepKeyTradeCursor(5);
        } else if (lf_down && !s_prev_left) {
            ActivateKeyTradeSelection(-1);
        } else if ((rt_down && !s_prev_right) || (act_down && !s_prev_act)) {
            ActivateKeyTradeSelection(1);
        }
    } else if (s_debug_overlay && s_jukebox_open) {
        /* Route navigation to Jukebox sub-window; B closes Jukebox only */
        static bool s_prev_pad_keep = false;
        if (cancel_down && !s_prev_pad_cancel) {
            s_jukebox_open = false;
        } else if (pad_keep_song && !s_prev_pad_keep) {
            ToggleJukeboxLockSong();
        } else if (up_down && !s_prev_up) {
            StepJukeboxCursor(-1);
        } else if (dn_down && !s_prev_down) {
            StepJukeboxCursor(1);
        } else if ((lf_down && !s_prev_left) || (fast_prev_down && !s_prev_fast_prev)) {
            StepJukeboxCursor(-10);
        } else if ((rt_down && !s_prev_right) || (fast_next_down && !s_prev_fast_next)) {
            StepJukeboxCursor(10);
        } else if (act_down && !s_prev_act) {
            ActivateJukeboxSelection();
        }
        s_prev_pad_keep = pad_keep_song;
    } else if (s_debug_overlay) {
        /* Controller B (East) button dismisses the Debug Window */
        if (cancel_down && !s_prev_pad_cancel) {
            s_debug_overlay = false;
            s_jukebox_open = false;
            s_key_trade_open = false;
            s_warp_win_open = false;
            s_give_all_confirm_open = false;
        } else if (up_down && !s_prev_up) {
            s_dbg_selected_row = (s_dbg_selected_row - 1 + DBG_TOTAL_COUNT) % DBG_TOTAL_COUNT;
        } else if (dn_down && !s_prev_down) {
            s_dbg_selected_row = (s_dbg_selected_row + 1) % DBG_TOTAL_COUNT;
        } else if (act_down && !s_prev_act) {
            TriggerDebugAction(s_dbg_selected_row);
        }
        /* Controller D-pad and stick Left/Right are ignored so scrolling never cycles or checks items; A activates/toggles, B closes. */
    }

    s_prev_up = up_down;
    s_prev_down = dn_down;
    s_prev_left = lf_down;
    s_prev_right = rt_down;
    s_prev_key_lf = key_lf;
    s_prev_key_rt = key_rt;
    s_prev_act = act_down;
    s_prev_pad_cancel = cancel_down;
    s_prev_fast_prev = fast_prev_down;
    s_prev_fast_next = fast_next_down;
}

/* ── Widescreen Sprite & Entity Culling Patcher ($80:9033 & Bank $8F) ─────── */

/*
 * Dynamically patches Secret of Evermore's OAM metasprite builder ($80:9033)
 * and entity active-window / spawner horizontal checks ($8F:C436, $8F:C494,
 * $8F:DE0A) in ROM so that event sprites, NPCs, and enemies adhere to the
 * active widescreen aspect ratio ([-extra .. 256 + extra]) instead of 4:3.
 * Restores original 4:3 bounds when extra == 0.
 */
static void SoePatchWidescreenSprites(int left_extra, int right_extra)
{
    static int s_last_patched_left = -1;
    static int s_last_patched_right = -1;
    if (left_extra < 0) left_extra = 0;
    if (right_extra < 0) right_extra = 0;
    if (left_extra == s_last_patched_left && right_extra == s_last_patched_right) return;
    if (!g_snes || !g_snes->cart || !g_snes->cart->rom || g_snes->cart->romSize < 0x100000u) return;

    uint8_t *rom = g_snes->cart->rom;
    bool any_extra = (left_extra > 0 || right_extra > 0);

    /*
     * 1. Metasprite OAM Builder ($80:9033 / $80:9040):
     *    - $80:9053: When extra > 0, NOP out `BCS $9058` (b0 03 -> ea ea) so all
     *      metasprites use the 16-bit signed X/Y sub-sprite builder ($80:90F8)
     *      instead of the 8-bit [0..255] fast path ($80:92EB) or 4:3 bbox cull.
     *    - $80:912F / $80:9134 (8x8 sub-sprites): Widen X range from [-7 .. 256)
     *      to [-7 - extra .. 256 + extra).
     *    - $80:920E / $80:9213 (16x16 sub-sprites): Widen X range from [-15 .. 256)
     *      to [-15 - extra .. 256 + extra).
     *    - $80:91E1 (8x8) & $80:92CB (16x16): Replace `BIT $17; BPL` (24 17 10 xx,
     *      which only tested bit 7 of high byte $17 for X < 0) with `LSR $17; BCC`
     *      (46 17 90 xx), testing bit 0 of $17 so both X < 0 ($17 = 0xFF) and
     *      256 <= X < 256 + extra ($17 = 0x01) set the 9th OAM X bit in highOam.
     */
    if (any_extra) {
        rom[0x009053] = 0xEA; rom[0x009054] = 0xEA; /* NOP; NOP */
        rom[0x0091E1] = 0x46; rom[0x0091E2] = 0x17; rom[0x0091E3] = 0x90; /* LSR $17; BCC */
        rom[0x0092CB] = 0x46; rom[0x0092CC] = 0x17; rom[0x0092CD] = 0x90; /* LSR $17; BCC */
    } else {
        rom[0x009053] = 0xB0; rom[0x009054] = 0x03; /* BCS $9058 */
        rom[0x0091E1] = 0x24; rom[0x0091E2] = 0x17; rom[0x0091E3] = 0x10; /* BIT $17; BPL */
        rom[0x0092CB] = 0x24; rom[0x0092CC] = 0x17; rom[0x0092CD] = 0x10; /* BIT $17; BPL */
    }

    WriteLe16(&rom[0x009130], (uint16_t)(int16_t)(-7 - left_extra));
    WriteLe16(&rom[0x009135], (uint16_t)(256 + right_extra));
    WriteLe16(&rom[0x00920F], (uint16_t)(int16_t)(-15 - left_extra));
    WriteLe16(&rom[0x009214], (uint16_t)(256 + right_extra));

    /*
     * 2. Entity Active-Window & Spawner Horizontal Culling ($8F:C436, $8F:C494, $8F:DE0A):
     *    Widen the active entity and spawn X thresholds around camera X ($0F59)
     *    from [-48 .. 304] / [-16 .. 272] to [-48 - left_extra .. 304 + right_extra] and
     *    [-16 - left_extra .. 272 + right_extra] so enemies, NPCs, and event sprites remain
     *    active and spawned across the full widescreen field of view.
     */
    WriteLe16(&rom[0x0FC437], (uint16_t)(int16_t)(-48 - left_extra));
    WriteLe16(&rom[0x0FC43C], (uint16_t)(304 + right_extra));
    WriteLe16(&rom[0x0FC495], (uint16_t)(int16_t)(-16 - left_extra));
    WriteLe16(&rom[0x0FC49A], (uint16_t)(272 + right_extra));
    WriteLe16(&rom[0x0FDE0B], (uint16_t)(int16_t)(-48 - left_extra));
    WriteLe16(&rom[0x0FDE10], (uint16_t)(304 + right_extra));
    WriteLe16(&rom[0x0FDE32], (uint16_t)(int16_t)(-16 - left_extra));
    WriteLe16(&rom[0x0FDE37], (uint16_t)(272 + right_extra));

    s_last_patched_left = left_extra;
    s_last_patched_right = right_extra;
}

/*
 * 3. Widescreen Enemy / Ally Targeting Bounds ($CC:8658):
 *    Widens the screen-relative X cull (`-8 .. 264`) in IsEntityTargetable
 *    to match the actual visible widescreen viewport `[-8 - left .. 264 + right)`
 *    so Ring Menu Targeting and Alchemy spells target every visible enemy.
 */
static void SoePatchWidescreenTargeting(int left_extra, int right_extra)
{
    static int s_last_left = -1;
    static int s_last_right = -1;
    if (left_extra < 0) left_extra = 0;
    if (right_extra < 0) right_extra = 0;
    if (left_extra == s_last_left && right_extra == s_last_right) return;
    if (!g_snes || !g_snes->cart || !g_snes->cart->rom || g_snes->cart->romSize < 0x0D0000u) return;

    uint8_t *rom = g_snes->cart->rom;
    WriteLe16(&rom[0x0C8667], (uint16_t)(int16_t)(-8 - left_extra));
    WriteLe16(&rom[0x0C866C], (uint16_t)(264 + right_extra));
    s_last_left = left_extra;
    s_last_right = right_extra;
}

/* ── Widescreen Room Tilemap Streamer ($7F:0000 -> VRAM $0000..$07FF) ─────── */

/*
 * Secret of Evermore's native tilemap engine ($90:98FA..$90:9A7F) only streams
 * 17 columns ($0FAC = 0x11, 272px wide) into the 32x32 BG1 ($0000..$03FF) and
 * BG2 ($0400..$07FF) VRAM tilemaps, leaving the left/right widescreen margin
 * columns empty (black) or containing stale tiles from previous vertical scroll
 * positions.
 *
 * Meanwhile, the entire decompressed room tilemap is resident in WRAM bank $7F:
 *   - $7E:08EE (uint8_t)  = Room width in 16x16 tiles
 *   - $7E:08F0 (uint8_t)  = Room height in 16x16 tiles
 *   - $7E:0F46 (uint16_t) = Row stride in bytes (width * 2)
 *   - $7F:0000 + (ty * width + tx) * 2 holds a 16-bit pointer `ptr` into $7F:
 *       - *(uint16_t *)&g_ram[0x10000 + ptr]     = BG1 16x16 tilemap word
 *       - *(uint16_t *)&g_ram[0x10000 + ptr + 2] = BG2 16x16 tilemap word
 *
 * By populating the left/right widescreen columns of BG1 and BG2 in ppu->vram
 * directly from the room map in $7F every frame, the left and right widescreen
 * sections scroll 100% in lockstep sync with the center without stale tiles,
 * black bars, or mirroring artifacts.
 */
static int s_cur_left_space = 0;
static int s_cur_right_space = 0;

static inline bool IsNobiliaDesertMap(uint16_t map_id)
{
    return (map_id == 0x001Bu || map_id == 0x0003u);
}

/*
 * Patches large black void rectangles (bg1=0xA800, bg2=0xA800) in specific rooms
 * with the surrounding flat gray/semi-black block so widescreen scrolling never
 * exposes an empty black cutout:
 *   - Map 13  ($000D, Ebon Keep interior, 41x61): tx=0..21, ty=0..27 -> (22, 0)
 *   - Map 45  ($002D, Hall of Collosia Mad Monk Room, 105x96): tx=68..104, ty=0..31 -> (68, 32)
 *   - Map 110 ($006E, Ivor Tower exterior, 38x61): tx=19..37, ty=0..27 -> (19, 28)
 */
static void SoePatchMapVoidTilemaps(uint16_t room_id, uint8_t width, uint8_t height)
{
    typedef struct {
        uint16_t room_id;
        uint8_t width, height;
        uint8_t x0, x1, y0, y1;
        uint8_t src_x, src_y;
    } VoidPatchDef;

    static const VoidPatchDef kPatches[] = {
        { 0x000Du,  41u, 61u,  0u,  22u, 0u, 28u, 22u,  0u },
        { 0x002Du, 105u, 96u, 68u, 105u, 0u, 32u, 68u, 32u },
        { 0x006Eu,  38u, 61u, 19u,  38u, 0u, 28u, 19u, 28u },
    };

    const VoidPatchDef *p = NULL;
    for (size_t i = 0; i < sizeof(kPatches) / sizeof(kPatches[0]); i++) {
        if (kPatches[i].room_id == room_id &&
            kPatches[i].width == width &&
            kPatches[i].height == height) {
            p = &kPatches[i];
            break;
        }
    }
    if (!p) return;

    uint32_t gray_idx = ((uint32_t)p->src_y * width + p->src_x) * 2u;
    uint32_t check_idx = ((uint32_t)p->y0 * width + p->x0) * 2u;
    uint16_t gray_ptr = (uint16_t)(g_ram[0x10000u + gray_idx] |
                                   (g_ram[0x10000u + gray_idx + 1u] << 8));
    uint16_t check_ptr = (uint16_t)(g_ram[0x10000u + check_idx] |
                                    (g_ram[0x10000u + check_idx + 1u] << 8));
    if ((uint32_t)gray_ptr + 4u > 0x10000u || (uint32_t)check_ptr + 4u > 0x10000u) return;

    uint16_t gray_bg1 = (uint16_t)(g_ram[0x10000u + gray_ptr] |
                                   (g_ram[0x10000u + gray_ptr + 1u] << 8));
    uint16_t gray_bg2 = (uint16_t)(g_ram[0x10000u + gray_ptr + 2u] |
                                   (g_ram[0x10000u + gray_ptr + 3u] << 8));
    uint16_t check_bg1 = (uint16_t)(g_ram[0x10000u + check_ptr] |
                                    (g_ram[0x10000u + check_ptr + 1u] << 8));
    uint16_t check_bg2 = (uint16_t)(g_ram[0x10000u + check_ptr + 2u] |
                                    (g_ram[0x10000u + check_ptr + 3u] << 8));
    if ((gray_bg1 == 0xA800u && gray_bg2 == 0xA800u) ||
        check_bg1 != 0xA800u || check_bg2 != 0xA800u) return;

    uint8_t lo = (uint8_t)(gray_ptr & 0xFFu);
    uint8_t hi = (uint8_t)((gray_ptr >> 8) & 0xFFu);
    for (uint32_t ty = p->y0; ty < p->y1; ty++) {
        for (uint32_t tx = p->x0; tx < p->x1; tx++) {
            uint32_t idx = (ty * width + tx) * 2u;
            uint16_t cur_ptr = (uint16_t)(g_ram[0x10000u + idx] |
                                          (g_ram[0x10000u + idx + 1u] << 8));
            if ((uint32_t)cur_ptr + 4u > 0x10000u) continue;
            uint16_t cur_bg1 = (uint16_t)(g_ram[0x10000u + cur_ptr] |
                                          (g_ram[0x10000u + cur_ptr + 1u] << 8));
            uint16_t cur_bg2 = (uint16_t)(g_ram[0x10000u + cur_ptr + 2u] |
                                          (g_ram[0x10000u + cur_ptr + 3u] << 8));
            if (cur_bg1 == 0xA800u && cur_bg2 == 0xA800u) {
                g_ram[0x10000u + idx] = lo;
                g_ram[0x10000u + idx + 1u] = hi;
            }
        }
    }
}

static void SoePopulateWidescreenTilemaps(Ppu *ppu)
{
    if (!ppu) return;

    uint8_t width = g_ram[0x08EE];
    uint8_t height = g_ram[0x08F0];
    uint16_t stride_bytes = (uint16_t)(g_ram[0x0F46] | (g_ram[0x0F47] << 8));

    /* Validate active room map dimensions in $7E:08EE..$7E:0F46 */
    if (width == 0 || height == 0) return;
    if (stride_bytes != (uint16_t)width * 2u) return;

    uint16_t room_id = GetLoadedMapId();
    SoePatchMapVoidTilemaps(room_id, width, height);
    bool is_desert = IsNobiliaDesertMap(room_id);
    bool wrap_horizontal = is_desert || ((room_id == 0x0011u || room_id == 0x0061u) && width == 48u);
    int r_min = is_desert ? -2 : -1;
    int r_max = is_desert ? 16 : 15;

    /* Populate BG1 (layer 0) and BG2 (layer 1) */
    for (int layer = 0; layer < 2; layer++) {
        uint16_t vram_base = (uint16_t)PPU_bgTilemapAdr(ppu, (uint)layer);
        uint16_t tile_off  = (layer == 0) ? 0u : 2u;

        /*
         * Read the full 16-bit camera scroll from WRAM ($7E:010E/$0110 for BG1,
         * $7E:0112/$0114 for BG2) exactly as the game's tilemap streamer does at
         * $90:98FA / $90:994A.
         */
        uint16_t hscroll_addr = (layer == 0) ? 0x010Eu : 0x0112u;
        uint16_t vscroll_addr = (layer == 0) ? 0x0110u : 0x0114u;
        uint16_t hscroll = (uint16_t)(g_ram[hscroll_addr] | (g_ram[hscroll_addr + 1u] << 8));
        uint16_t vscroll = (uint16_t)(g_ram[vscroll_addr] | (g_ram[vscroll_addr + 1u] << 8));
        int bg_x = (int)(hscroll >> 4);
        int bg_y = (int)((uint16_t)(vscroll + 1u) >> 4);
        int fine_x = (int)(hscroll & 15u);
        int c_min = (s_cur_left_space > fine_x) ? -((s_cur_left_space - fine_x + 15) / 16) : 0;
        int c_max = (255 + s_cur_right_space + fine_x) / 16;
        if (c_max < 15) c_max = 15;
        if (c_max - c_min > 31) c_max = c_min + 31;

        int c_count = c_max - c_min + 1;
        if (c_count > 32) c_count = 32;
        uint16_t col_tx[32];
        uint16_t col_vc[32];
        for (int ci = 0; ci < c_count; ci++) {
            int raw_tx = bg_x + c_min + ci;
            int tx = raw_tx;

            if (is_desert && room_id == 0x001Bu && width == 97u) {
                /* Map 27 loops horizontally with a 62-tile period between tx=2..79,
                 * while tx=0 and tx>=80 contain black border padding. */
                while (tx <= 0) tx += 62;
                while (tx >= 80) tx -= 62;
            } else if (wrap_horizontal) {
                /* Room 0x0011 (Intro Pipe Shafts & Title Rooftop) wraps mod 48 */
                tx = ((tx % (int)width) + (int)width) % (int)width;
            } else if (tx < 0) {
                /* Preserve 32x32 metatile even/odd phase on outer room border */
                tx = (width >= 2u) ? (raw_tx & 1) : 0;
            } else if (tx >= (int)width) {
                tx = (width >= 2u) ? ((int)width - 2 + (raw_tx & 1)) : ((int)width - 1);
            }
            col_tx[ci] = (uint16_t)tx;
            col_vc[ci] = (uint16_t)(raw_tx & 0x1F);
        }

        for (int r = r_min; r <= r_max; r++) {
            int ty = bg_y + r;
            if (is_desert) {
                ty = ((ty % (int)height) + (int)height) % (int)height;
            } else if (ty < 0) {
                ty = 0;
            } else if (ty >= (int)height) {
                ty = (int)height - 1;
            }

            uint16_t vr = (uint16_t)((bg_y + r) & 0x1F);
            uint32_t row_idx_base = (uint32_t)(ty * (int)width) * 2u;
            uint16_t *vram_row = &ppu->vram[vram_base | (vr << 5)];

            for (int ci = 0; ci < c_count; ci++) {
                uint32_t idx = row_idx_base + (uint32_t)col_tx[ci] * 2u;
                if (idx + 2u > 0x10000u) continue;

                uint16_t ptr = (uint16_t)(g_ram[0x10000u + idx] |
                                         (g_ram[0x10000u + idx + 1u] << 8));
                if ((uint32_t)ptr + tile_off + 2u > 0x10000u) continue;

                uint16_t tile_word = (uint16_t)(g_ram[0x10000u + ptr + tile_off] |
                                               (g_ram[0x10000u + ptr + tile_off + 1u] << 8));

                vram_row[col_vc[ci]] = tile_word;
            }
        }
    }
}

/*
 * Returns true when PPU is displaying one of the Mode 4 8bpp stone-mosaic menu
 * screens (Save Select, Load, Name Entry, Level, Stat, Act, Edit, Alchemy).
 * These Mode 4 menu screens always widen cleanly across the side margins even if
 * Map 97's Mode 1 Title Screen is pillarboxed.
 */
static bool IsMode4MenuMosaicActive(const Ppu *ppu)
{
    if (!ppu) return false;
    if ((ppu->bgmode & 0x07u) != 4u) return false;
    if ((ppu->screenEnabled[0] & 0x03u) != 0x03u) return false;
    if (ppu->screenWindowed[0] != 0 || ppu->screenWindowed[1] != 0) return false;

    int mapadr = PPU_bgTilemapAdr(ppu, 0);
    uint16_t t00 = ppu->vram[mapadr & 0x7FFF] & 0x3FFu;
    uint16_t t01 = ppu->vram[(mapadr + 1) & 0x7FFF] & 0x3FFu;
    uint16_t t02 = ppu->vram[(mapadr + 2) & 0x7FFF] & 0x3FFu;
    uint16_t t10 = ppu->vram[(mapadr + 32) & 0x7FFF] & 0x3FFu;
    return (t01 == t00 + 1u) && (t02 == t00 + 2u) && (t10 == t00 + 8u);
}

/*
 * Fill the widescreen side margins of Mode 4 stone-mosaic menus (Save/Load/Name
 * and Ring Menu submenus: Level, Stat, Act, Edit, Edit, Alchemy) using BG1 tiles.
 */
static void DrawMode4MenuSideMosaic(const Ppu *ppu, uint32_t *pixels, int stride,
                                    int frame_w, int frame_h, int extra)
{
    if (!ppu || !pixels || extra <= 0 || PPU_forcedBlank(ppu)) return;
    if ((ppu->bgmode & 0x07u) != 4u || (ppu->screenEnabled[0] & 0x01u) == 0u) return;

    uint8_t bright = PPU_brightness(ppu);
    uint32_t pal_rgb[256];
    for (int i = 0; i < 256; i++) {
        uint16_t c = ppu->cgram[i];
        uint8_t r = ppu_brightness_level((unsigned)(c & 0x1Fu), bright);
        uint8_t g = ppu_brightness_level((unsigned)((c >> 5) & 0x1Fu), bright);
        uint8_t b = ppu_brightness_level((unsigned)((c >> 10) & 0x1Fu), bright);
        pal_rgb[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    int mapadr = PPU_bgTilemapAdr(ppu, 0);
    int tileadr = PPU_bgTileAdr(ppu, 0);
    int hscr = (int)ppu->hScroll[0];
    int vscr = (int)ppu->vScroll[0];

    for (int y = 0; y < frame_h; y++) {
        int sy = (y + 1 + vscr) & 0x3FF;
        int tile_y = (sy >> 3) & 31;
        uint32_t *dst_row = pixels + y * stride;

        for (int side = 0; side < 2; side++) {
            int fx_start = (side == 0) ? 0 : (extra + 256);
            int fx_end = (side == 0) ? extra : frame_w;
            if (fx_start < 0) fx_start = 0;
            if (fx_end > frame_w) fx_end = frame_w;
            int span = fx_end - fx_start;
            if (span <= 0) continue;

            for (int fx = fx_start; fx < fx_end; fx++) {
                int sx = (fx - extra + hscr) & 0xFF;
                int tile_x = (sx >> 3) & 31;
                uint16_t tile = ppu->vram[(mapadr + (tile_y << 5) + tile_x) & 0x7FFF];

                unsigned px = (unsigned)(sx & 7);
                unsigned py = (unsigned)(sy & 7);
                if (tile & 0x4000u) px = 7u - px;
                if (tile & 0x8000u) py = 7u - py;

                unsigned bit = 7u - px;
                unsigned addr = (unsigned)((tileadr + (int)(tile & 0x3FFu) * 32 + (int)py) & 0x7FFF);
                uint16_t p01 = ppu->vram[addr];
                uint16_t p23 = ppu->vram[(addr + 8u) & 0x7FFF];
                uint16_t p45 = ppu->vram[(addr + 16u) & 0x7FFF];
                uint16_t p67 = ppu->vram[(addr + 24u) & 0x7FFF];

                unsigned pixel =
                    ((p01 >> bit) & 1u) | (((p01 >> (bit + 8u)) & 1u) << 1) |
                    (((p23 >> bit) & 1u) << 2) | (((p23 >> (bit + 8u)) & 1u) << 3) |
                    (((p45 >> bit) & 1u) << 4) | (((p45 >> (bit + 8u)) & 1u) << 5) |
                    (((p67 >> bit) & 1u) << 6) | (((p67 >> (bit + 8u)) & 1u) << 7);

                /* Graceful black-to-transparent edge gradient across outer 25% of side margins */
                int dist = (side == 0) ? (fx - fx_start) : (fx_end - 1 - fx);
                float grad_span = (span > 1) ? ((float)(span - 1) * 0.25f) : 1.0f;
                float u = (grad_span > 0.0f) ? ((float)dist / grad_span) : 1.0f;
                if (u > 1.0f) u = 1.0f;
                float inv = 1.0f - u;
                float ease = 1.0f - inv * inv * inv;
                int gain = (int)(ease * 256.0f + 0.5f);
                if (gain > 256) gain = 256;
                uint32_t col = pal_rgb[pixel];
                uint32_t r = (((col >> 16) & 0xFFu) * (uint32_t)gain) >> 8;
                uint32_t g = (((col >> 8) & 0xFFu) * (uint32_t)gain) >> 8;
                uint32_t b = ((col & 0xFFu) * (uint32_t)gain) >> 8;
                dst_row[fx] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
}



/* ── Public API Implementation ────────────────────────────────────────────── */

void SoeSlideMenuInit(void)
{
    /* Set defaults from config + per-subsystem configs */
    SoeLoadVideoConfig();
    SoeLoadMapPillarboxConfig();
    SoeLoadCrtConfig();
    SoeLoadBloomConfig();
    SoeLoadDialogConfig();
    g_config.enable_gamepad[0] = true;
    if (g_config.player_src[0] == 0) {
        g_config.player_src[0] = 2; /* Default Player 1 to Gamepad (+ Keyboard in host_main.c) */
    }
    s_fix_silver_sheath = true;
    s_fix_atlas_underflow = true;
    s_fix_dog_leveling = true;
    s_fix_softlocks = true;
    SoeLoadDebugConfig();

    if (getenv("SOE_OPEN_MENU")) {
        s_menu_open = true;
        s_anim_x = 0.0f;
        const char *tab_env = getenv("SOE_INITIAL_TAB_TEST");
        if (tab_env) {
            s_active_tab = (SoeMenuTab)(atoi(tab_env) % TAB_COUNT);
        }
        const char *row_env = getenv("SOE_INITIAL_ROW_TEST");
        if (row_env) {
            int r = atoi(row_env);
            if (r >= 0) s_tab_selected_row[s_active_tab] = r;
        }
    }
    if (getenv("SOE_EXPORT_ALL_MAPS") || getenv("SOE_EXPORT_ALL_MUSIC") || getenv("SOE_EXPORT_ALL_SFX")) {
        g_config.enable_audio = false;
    }
    if (getenv("SOE_DEBUG_OVERLAY")) {
        s_debug_overlay = true;
    }
    if (getenv("SOE_MAP_PICKER")) {
        s_map_picker_open = true;
    }
    if (getenv("SOE_WALK_WALLS")) {
        s_walk_walls_active = true;
        s_dbg_selected_row = DBG_WALK_WALLS;
    }
    if (getenv("SOE_INF_HP")) {
        s_inf_hp_active = true;
    }
    if (getenv("SOE_INF_STAMINA")) {
        s_inf_stamina_active = true;
    }
    if (getenv("SOE_ONE_HIT_KO")) {
        s_one_hit_ko_active = true;
    }
    if (getenv("SOE_HUD_ORIGINAL")) {
        s_hud_mode = SOE_HUD_ORIGINAL;
    }

    SoeInstallEnhancedRingMenuHooks();
}



bool SoeSlideMenuIsOpen(void)
{
    return s_menu_open || s_debug_overlay;
}

void SoeSlideMenuToggle(void)
{
    s_menu_open = !s_menu_open;
    SoePlayUiSfx(s_menu_open ? 1 : 2);
}

SoeWidescreenMode SoeGetWidescreenMode(void)
{
    return s_ws_mode;
}

void SoeSetWidescreenMode(SoeWidescreenMode mode)
{
    SoeApplyWidescreenMode(mode);
}

bool SoeIsWidescreenActive(void)
{
    return s_ws_mode != SOE_WS_OFF;
}

int SoeGetWidescreenExtra(void)
{
    switch (s_ws_mode) {
    case SOE_WS_16_9: return 43; /* 256 + 2*43 = 342 px */
    case SOE_WS_21_9: return 85; /* 256 + 2*85 = 426 px */
    default: return 0;
    }
}



SoeHudPositionMode SoeGetHudPositionMode(void)
{
    return s_hud_mode;
}

void SoeSetHudPositionMode(SoeHudPositionMode mode)
{
    if (mode < 0 || mode >= SOE_HUD_COUNT) mode = SOE_HUD_MOVE_LEFT;
    s_hud_mode = mode;
    SoeSaveVideoConfig();
}

void SoePrepareFrame(int dw, int dh, int *w, int *h)
{
    (void)dw; (void)dh;
    *h = 224;

    switch (s_ws_mode) {
    case SOE_WS_16_9:
        *w = 342;
        break;
    case SOE_WS_21_9:
        *w = 426;
        break;
    case SOE_WS_OFF:
    default:
        *w = 256;
        break;
    }
}



/*
 * Called before PPU rasterization:
 * - Re-asserts SDL event filter and keyboard/controller input.
 * - Runs guarded per-frame bug fixes during active gameplay.
 * - Centers bounded 256px screens (Mode 4 Name Entry, 1965 square-screen intro,
 *   hardware-windowed transitions) at 4:3 with clean pillarboxes.
 * - Patches OAM metasprite & entity horizontal culling for widescreen sprites/enemies.
 * - Populates BG1/BG2 widescreen margin columns directly from the room map in
 *   WRAM $7F:0000 so left and right map sections scroll in sync with the middle.
 * - Anchors BG3 HUD elements (and HUD OAM icons) to the outer edges when active.
 */
/*
 * Dialog Window Capture Setup:
 * Leaves original ROM pattern tiles untouched (so authentic 1995 checkerboard and other
 * patterns in Window Prefs remain pure) and captures Mode-1 BG3 tiles into s_bg3_overlay_buf
 * when any Glass theme is active so SoeRenderProgrammaticDialogs can composite them in C.
 */
static void PatchDialogWindowBackground(Ppu *ppu, bool glass_active)
{
    if (ppu) {
        PpuSetSemiTransparentDialogBg(ppu, false);

        uint16_t cur_map = GetLoadedMapId();
        bool is_mode1 = ((ppu->bgmode & 0x07) == 1);
        bool hud_vis = ((g_ram[0x0B29] & 0x03) != 0);
        int cap_h = hud_vis ? 192 : 224;

        if (glass_active && is_mode1 && s_frame_counter >= 60 && cur_map != 0 && cur_map != 97 &&
            !(ppu->inidisp & 0x80) && (ppu->screenEnabled[0] & 0x04)) {
            s_dialog_capture_active = true;
            s_dialog_capture_h = cap_h;
            PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg3,
                                  (uint8_t *)s_bg3_overlay_buf, 256 * sizeof(uint32_t));
            PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg3,
                                 0, 0, 256, cap_h, kPpuOverlayFlag_RemoveFromGame);
            PpuBindOverlaySurface(ppu, kPpuOverlaySource_Obj, NULL, 0);
        } else {
            s_dialog_capture_active = false;
            PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg3, NULL, 0);
            PpuBindOverlaySurface(ppu, kPpuOverlaySource_Obj, NULL, 0);
        }
    }
}



int SoeMenuPaused(void)
{
    float menu_w = GetMenuWidth();
    s_menu_paused = s_menu_open || (s_anim_x > -menu_w + 0.5f) || s_debug_overlay || s_jukebox_open || s_key_trade_open || s_warp_win_open;
    return s_menu_paused ? 1 : 0;
}

void SoeMenuPausedTick(void)
{
    PollMenuKeyboardDirect();
    SoeProcessPendingJukeboxSong();
}

typedef struct {
    int8_t steps[4];
    int count;
    int idx;
    double res;
} SoeCamAxisState;

static SoeCamAxisState s_cam_axis_x = {{0, 0, 0, 0}, 0, 0, 0.0};
static SoeCamAxisState s_cam_axis_y = {{0, 0, 0, 0}, 0, 0, 0.0};

static void SoeCamAxisReset(SoeCamAxisState *st)
{
    memset(st, 0, sizeof(*st));
}

/* Smooth periodic fractional-pixel scroll cadences (1.5 px/frame = 2,1,2,1 and
 * 1.75 px/frame = 2,2,2,1) using causal trailing subpixel residuals without
 * overshooting integer stop positions or map edges. */
static double SoeCamAxisStep(SoeCamAxisState *st, int d_curr)
{
    if (d_curr == 0 || abs(d_curr) > 2) {
        SoeCamAxisReset(st);
        return 0.0;
    }
    int prev = (st->count > 0) ? st->steps[(st->idx + 3) & 3] : 0;
    if (prev != 0 && (prev * d_curr) <= 0) {
        SoeCamAxisReset(st);
    }

    st->steps[st->idx] = (int8_t)d_curr;
    st->idx = (st->idx + 1) & 3;
    if (st->count < 4) st->count++;

    double v = (double)d_curr;
    if (st->count == 4) {
        int sum = st->steps[0] + st->steps[1] + st->steps[2] + st->steps[3];
        v = (double)sum / 4.0;
    } else if (st->count >= 2) {
        int p1 = st->steps[(st->idx + 3) & 3];
        int p2 = st->steps[(st->idx + 2) & 3];
        v = (double)(p1 + p2) / 2.0;
    }

    double prev_res = st->res;
    double next_res = prev_res - (double)d_curr + v;
    if (d_curr > 0) {
        if (next_res > 0.0) next_res = 0.0;
        else if (next_res < -0.85) next_res = -0.85;
        if ((double)d_curr + (next_res - prev_res) < 0.0) next_res = prev_res - (double)d_curr;
    } else {
        if (next_res < 0.0) next_res = 0.0;
        else if (next_res > 0.85) next_res = 0.85;
        if ((double)d_curr + (next_res - prev_res) > 0.0) next_res = prev_res - (double)d_curr;
    }
    st->res = next_res;
    return next_res;
}

static int16_t SoeGetEffectivePpuScrollAxis(uint16_t ram_addr, uint16_t ppu_scroll10)
{
    int16_t ram_s = (int16_t)(uint16_t)(g_ram[ram_addr] | (g_ram[ram_addr + 1] << 8));
    int diff = (int)(int16_t)(ppu_scroll10 & 0x03FFu) - (int)(ram_s & 0x03FF);
    if (diff > 512) diff -= 1024;
    if (diff < -512) diff += 1024;
    return (abs(diff) <= 32) ? (int16_t)((int)ram_s + diff) : ram_s;
}

static int SoeGetWorldCamScrollLayer(const Ppu *ppu, bool vertical)
{
    if (!ppu || (ppu->screenEnabled[0] & 0x03u) != 0x03u) return 0;
    if ((g_ram[0x0B29] & 0x03u) != 0x03u) return 0;

    uint16_t a1 = vertical ? 0x0110u : 0x010Eu;
    uint16_t a2 = vertical ? 0x0114u : 0x0112u;
    int16_t s1 = (int16_t)(uint16_t)(g_ram[a1] | (g_ram[a1 + 1u] << 8));
    int16_t s2 = (int16_t)(uint16_t)(g_ram[a2] | (g_ram[a2 + 1u] << 8));
    if (s1 == s2) return 0;

    /* Secret of Evermore's true world camera lives at $7E:0059 (X) / $7E:005B (Y).
     * On parallax maps (e.g. Map 56 & Map 65 jungle canopy), the engine writes $0059/$005B
     * directly to the playable world layer (BG2) and computes the other layer (BG1) at a
     * parallax ratio (e.g. 1.25x). Matching $0059/$005B directly never flips layers mid-walk. */
    uint16_t cam_addr = vertical ? 0x005Bu : 0x0059u;
    int16_t cam = (int16_t)(uint16_t)(g_ram[cam_addr] | (g_ram[cam_addr + 1u] << 8));
    int d1 = abs((int)s1 - (int)cam);
    int d2 = abs((int)s2 - (int)cam);
    if (d2 != d1) {
        return (d2 < d1) ? 1 : 0;
    }

    uint16_t ent = (uint16_t)(g_ram[0x0F3Eu] | (g_ram[0x0F3Fu] << 8));
    if (ent < 0x0100u || ent >= 0xF000u) {
        ent = (uint16_t)(g_ram[0x0F40u] | (g_ram[0x0F41u] << 8));
    }
    if (ent < 0x0100u || ent >= 0xF000u) return 0;

    int coord = (int)(int16_t)(uint16_t)(g_ram[ent + (vertical ? 0x1Cu : 0x1Au)] |
                                          (g_ram[ent + (vertical ? 0x1Du : 0x1Bu)] << 8));
    int span = vertical ? 224 : 256;
    bool on1 = (coord >= (int)s1 && coord < (int)s1 + span);
    bool on2 = (coord >= (int)s2 && coord < (int)s2 + span);
    if (on2 && !on1) return 1;
    if (!on2 && on1) return 0;
    if (on2 && abs(coord - ((int)s2 + span / 2)) < abs(coord - ((int)s1 + span / 2))) {
        return 1;
    }
    return 0;
}

static int16_t SoeGetEffectivePpuCamScroll(const Ppu *ppu, bool vertical)
{
    int layer = SoeGetWorldCamScrollLayer(ppu, vertical);
    uint16_t addr = vertical ? ((layer == 1) ? 0x0114u : 0x0110u)
                             : ((layer == 1) ? 0x0112u : 0x010Eu);
    if (!ppu) return (int16_t)(uint16_t)(g_ram[addr] | (g_ram[addr + 1u] << 8));
    uint16_t ppu_scroll = vertical ? ppu->vScroll[layer] : ppu->hScroll[layer];
    return SoeGetEffectivePpuScrollAxis(addr, ppu_scroll);
}

static int16_t SoeGetEffectivePpuHScroll(const Ppu *ppu)
{
    return SoeGetEffectivePpuCamScroll(ppu, false);
}

static int16_t SoeGetEffectivePpuVScroll(const Ppu *ppu)
{
    return SoeGetEffectivePpuCamScroll(ppu, true);
}

static bool SoeIsTileUniformVoid(const Ppu *ppu, int layer, uint16_t word, bool allow_dark_solid)
{
    if (word == 0xA800u) return true;
    if (!ppu) return false;

    uint32_t char_base = (uint32_t)PPU_bgTileAdr(ppu, (uint)layer);
    uint16_t base_tile = word & 0x03FFu;
    const uint16_t *vram = ppu->vram;

    uint16_t ref01 = 0, ref23 = 0;
    for (int qy = 0; qy < 2; qy++) {
        for (int qx = 0; qx < 2; qx++) {
            uint16_t tile = (uint16_t)((base_tile + qx + qy * 16) & 0x03FFu);
            uint32_t tile_addr = (char_base + (uint32_t)tile * 16u) & 0x7FFFu;
            if (tile_addr + 16u > 0x8000u) return false;

            uint16_t q01 = vram[tile_addr + 0];
            uint16_t q23 = vram[tile_addr + 8];
            if (qx == 0 && qy == 0) {
                uint8_t b0 = (uint8_t)(q01 & 0xFFu);
                uint8_t b1 = (uint8_t)(q01 >> 8);
                uint8_t b2 = (uint8_t)(q23 & 0xFFu);
                uint8_t b3 = (uint8_t)(q23 >> 8);
                if ((b0 != 0x00u && b0 != 0xFFu) ||
                    (b1 != 0x00u && b1 != 0xFFu) ||
                    (b2 != 0x00u && b2 != 0xFFu) ||
                    (b3 != 0x00u && b3 != 0xFFu)) {
                    return false;
                }
                ref01 = q01;
                ref23 = q23;
            } else if (q01 != ref01 || q23 != ref23) {
                return false;
            }

            for (int py = 1; py < 8; py++) {
                if (vram[tile_addr + py] != ref01 || vram[tile_addr + 8 + py] != ref23) {
                    return false;
                }
            }
        }
    }

    uint8_t c = (uint8_t)(((ref01 & 1u) ? 1u : 0u) |
                          ((ref01 & 0x0100u) ? 2u : 0u) |
                          ((ref23 & 1u) ? 4u : 0u) |
                          ((ref23 & 0x0100u) ? 8u : 0u));
    if (c == 0) {
        if (!allow_dark_solid) return true;
        uint16_t bg_rgb = ppu->cgram[0];
        return ((bg_rgb & 0x1Fu) <= 4u &&
                ((bg_rgb >> 5) & 0x1Fu) <= 4u &&
                ((bg_rgb >> 10) & 0x1Fu) <= 4u);
    }
    if (!allow_dark_solid) return false;

    uint16_t pbase = (uint16_t)(((word >> 10) & 7u) * 16u);
    uint16_t rgb15 = ppu->cgram[pbase + c];
    return ((rgb15 & 0x1Fu) <= 4u &&
            ((rgb15 >> 5) & 0x1Fu) <= 4u &&
            ((rgb15 >> 10) & 0x1Fu) <= 4u);
}

static bool SoeRoomColHasContent(uint8_t width, uint8_t height, int tx, int ty0, int ty1, uint8_t layer_mask)
{
    if (tx < 0 || tx >= (int)width) return false;
    if (ty0 < 0) ty0 = 0;
    if (ty1 >= (int)height) ty1 = (int)height - 1;
    const Ppu *ppu = (g_snes && g_snes->ppu) ? g_snes->ppu : NULL;
    bool bg2_only = (layer_mask == 0x02u);
    bool dark_backdrop = true;
    if (bg2_only && ppu) {
        uint16_t bg_rgb = ppu->cgram[0];
        dark_backdrop = ((bg_rgb & 0x1Fu) <= 4u &&
                         ((bg_rgb >> 5) & 0x1Fu) <= 4u &&
                         ((bg_rgb >> 10) & 0x1Fu) <= 4u);
    }
    for (int ty = ty0; ty <= ty1; ty++) {
        uint32_t idx = ((uint32_t)ty * width + (uint32_t)tx) * 2u;
        if (idx + 2u > 0x10000u) continue;
        uint16_t ptr = (uint16_t)(g_ram[0x10000u + idx] |
                                  (g_ram[0x10000u + idx + 1u] << 8));
        if ((uint32_t)ptr + 4u > 0x10000u) continue;
        if (layer_mask & 0x01u) {
            uint16_t bg1 = (uint16_t)(g_ram[0x10000u + ptr] |
                                      (g_ram[0x10000u + ptr + 1u] << 8));
            if (!SoeIsTileUniformVoid(ppu, 0, bg1, false)) return true;
        }
        if (layer_mask & 0x02u) {
            uint16_t bg2 = (uint16_t)(g_ram[0x10000u + ptr + 2u] |
                                      (g_ram[0x10000u + ptr + 3u] << 8));
            if (bg2 == 0xA800u && bg2_only && !dark_backdrop) return true;
            if (!SoeIsTileUniformVoid(ppu, 1, bg2, true)) return true;
        }
    }
    return false;
}

static void SoeComputeRoomContentBoundsX(uint8_t width, uint8_t height,
                                         int16_t hscroll_cam, int16_t vscroll_cam,
                                         int span, uint8_t layer_mask,
                                         int *out_left_px, int *out_right_px)
{
    *out_left_px = 0;
    *out_right_px = (int)width * 16;

    int bg_y = (int)((uint16_t)(vscroll_cam + 1) >> 4);
    int ty0 = (bg_y < 0) ? 0 : bg_y;
    int ty1 = bg_y + 14;
    if (ty1 >= (int)height) ty1 = (int)height - 1;
    if (ty0 > ty1) return;

    int view_l = (int)hscroll_cam >> 4;
    int view_r = ((int)hscroll_cam + 255) >> 4;
    if (view_l < 0) view_l = 0;
    if (view_r >= (int)width) view_r = (int)width - 1;
    if (view_l > view_r) return;

    int seed_l = -1, seed_r = -1;
    for (int tx = view_l; tx <= view_r; tx++) {
        if (SoeRoomColHasContent(width, height, tx, ty0, ty1, layer_mask)) {
            seed_l = tx;
            break;
        }
    }
    if (seed_l < 0) return;
    for (int tx = view_r; tx >= view_l; tx--) {
        if (SoeRoomColHasContent(width, height, tx, ty0, ty1, layer_mask)) {
            seed_r = tx;
            break;
        }
    }

    int max_probe = (span + 15) / 16 + 2;

    int content_l = seed_l;
    int stop_l = seed_l - max_probe;
    if (stop_l < 0) stop_l = 0;
    for (int tx = seed_l - 1; tx >= stop_l; tx--) {
        if (SoeRoomColHasContent(width, height, tx, ty0, ty1, layer_mask)) {
            content_l = tx;
        } else if (tx - 1 >= 0 && SoeRoomColHasContent(width, height, tx - 1, ty0, ty1, layer_mask)) {
            content_l = tx;
        } else {
            *out_left_px = content_l * 16;
            break;
        }
    }

    int content_r = seed_r;
    int stop_r = seed_r + max_probe;
    if (stop_r >= (int)width) stop_r = (int)width - 1;
    for (int tx = seed_r + 1; tx <= stop_r; tx++) {
        if (SoeRoomColHasContent(width, height, tx, ty0, ty1, layer_mask)) {
            content_r = tx;
        } else if (tx + 1 < (int)width && SoeRoomColHasContent(width, height, tx + 1, ty0, ty1, layer_mask)) {
            content_r = tx;
        } else {
            *out_right_px = (content_r + 1) * 16;
            break;
        }
    }
}

static void SoeComputeAnchoredWidescreenMargins(int extra, int16_t hscroll_cam, int16_t vscroll_cam,
                                                int *out_left, int *out_right)
{
    uint8_t room_w_tiles = g_ram[0x08EE];
    uint8_t room_h_tiles = g_ram[0x08F0];
    uint16_t stride_bytes = (uint16_t)(g_ram[0x0F46] | (g_ram[0x0F47] << 8));
    int span = 2 * extra;
    int left_space = extra;
    int right_space = extra;

    uint16_t room_id = GetLoadedMapId();
    bool wrap_horizontal = IsNobiliaDesertMap(room_id) ||
                           ((room_id == 0x0011u || room_id == 0x0061u) && room_w_tiles == 48u);
    /* Map 49 ($0031, Podunk 1965 street): dual parallax (street + foreground foliage)
     * during intro cutscene; skip horizontal screen clamping so both layers scroll freely. */
    bool disable_clamping = wrap_horizontal || (room_id == 0x0031u);
    if (!disable_clamping && room_w_tiles > 0 && room_h_tiles > 0 &&
        stride_bytes == (uint16_t)room_w_tiles * 2u) {
        int eff_left_px = 0;
        int eff_right_px = (int)room_w_tiles * 16;
        uint8_t layer_mask = 0x03u;
        int16_t h1 = (int16_t)(uint16_t)(g_ram[0x010Eu] | (g_ram[0x010Fu] << 8));
        int16_t h2 = (int16_t)(uint16_t)(g_ram[0x0112u] | (g_ram[0x0113u] << 8));
        int16_t v1 = (int16_t)(uint16_t)(g_ram[0x0110u] | (g_ram[0x0111u] << 8));
        int16_t v2 = (int16_t)(uint16_t)(g_ram[0x0114u] | (g_ram[0x0115u] << 8));
        if (h1 != h2 || v1 != v2) {
            const Ppu *ppu = (g_snes && g_snes->ppu) ? g_snes->ppu : NULL;
            int cam_layer = SoeGetWorldCamScrollLayer(ppu, false);
            layer_mask = (cam_layer == 1) ? 0x02u : 0x01u;
        }
        SoePatchMapVoidTilemaps(room_id, room_w_tiles, room_h_tiles);
        SoeComputeRoomContentBoundsX(room_w_tiles, room_h_tiles, hscroll_cam, vscroll_cam,
                                     span, layer_mask, &eff_left_px, &eff_right_px);
        int eff_room_px_w = eff_right_px - eff_left_px;
        if (eff_room_px_w < 256) eff_room_px_w = 256;

        int avail_left = (int)hscroll_cam - eff_left_px;
        if (avail_left < 0) avail_left = 0;
        int avail_right = eff_right_px - ((int)hscroll_cam + 256);
        if (avail_right < 0) avail_right = 0;

        if (eff_room_px_w >= 256 + span) {
            left_space = (avail_left < extra) ? avail_left : extra;
            right_space = (avail_right < extra) ? avail_right : extra;
            if (left_space < extra && avail_right > right_space) {
                int add = span - left_space - right_space;
                int max_add = avail_right - right_space;
                right_space += (add < max_add) ? add : max_add;
            }
            if (right_space < extra && avail_left > left_space) {
                int add = span - left_space - right_space;
                int max_add = avail_left - left_space;
                left_space += (add < max_add) ? add : max_add;
            }
        } else {
            left_space = (avail_left < span) ? avail_left : span;
            right_space = span - left_space;
            if (right_space > avail_right) right_space = avail_right;
        }
    }

    *out_left = left_space;
    *out_right = right_space;
}

static void SoeApplyHudWidescreenConfig(Ppu *ppu, int left_space, int right_space)
{
    (void)left_space;
    (void)right_space;
    if (!ppu) return;

    /*
     * HUD Element Repositioning:
     * Only active when the in-game status bar is displayed ((g_ram[0x0B29] & 0x03) != 0)
     * and widescreen margins are enabled (ppu->extraLeftRight > 0).
     *
     * Two invariant layouts, never shifting with map edges or player position:
     *  1) Dual (Boy + Dog) in SOE_HUD_MOVE_LEFT: left half pinned to physical
     *     [0..128), right half pinned to physical [frame_w - 128 .. frame_w).
     *  2) Single-character HUD (or SOE_HUD_ORIGINAL): entire HUD pinned to
     *     physical center [extraLeftRight .. extraLeftRight + 256).
     */
    bool hud_visible = ((g_ram[0x0B29] & 0x03) != 0);
    if (!hud_visible || ppu->extraLeftRight == 0) {
        PpuSetWidescreenHudAlwaysVisible(ppu, false);
        PpuSetWidescreenBg3Widen(ppu, 0);
        PpuSetWidescreenLayerElasticBand(ppu, 2 /* BG3 layer */, 0, 0, NULL, 0);
        PpuSetWsHudOamBand(ppu, 0, 0, 0);
        PpuSetWsHudOamShiftRange(ppu, 0, 0);
        PpuSetWsHudOamShiftRange2(ppu, 0, 0);
        return;
    }

    /*
     * Only split the bottom BG3 HUD (at x = 128) and shift side OAM portraits when
     * BOTH Boy and Dog HUDs are active side-by-side ($051D=0x0003, $0561=0x0013).
     * When separated (e.g. Nobilia solo segments or Podunk/Prehistoria solo), the game
     * sets $0B29 bit 2/3 (0x04/0x08), $2261 bit 0/1 (0x01/0x02), clears $0F3E/$0F40,
     * and centers the lone character's HUD at column 0x000B — splitting at x = 128
     * would tear that centered graphic in half, so keep single-character HUD centered.
     */
    uint16_t boy_hud_col = (uint16_t)g_ram[0x051D] | ((uint16_t)g_ram[0x051E] << 8);
    uint16_t dog_hud_col = (uint16_t)g_ram[0x0561] | ((uint16_t)g_ram[0x0562] << 8);
    uint16_t boy_ent = (uint16_t)g_ram[0x0F3E] | ((uint16_t)g_ram[0x0F3F] << 8);
    uint16_t dog_ent = (uint16_t)g_ram[0x0F40] | ((uint16_t)g_ram[0x0F41] << 8);
    bool single_char_hud = ((g_ram[0x0B29] & 0x0C) != 0) ||
                           ((g_ram[0x2261] & 0x03) != 0) ||
                           (boy_hud_col == 0x000Bu) ||
                           (dog_hud_col == 0x000Bu) ||
                           (boy_ent == 0u) ||
                           (dog_ent == 0u);
    bool dual_edge_hud = (s_hud_mode == SOE_HUD_MOVE_LEFT && !single_char_hud);

    PpuSetWidescreenHudAlwaysVisible(ppu, true);
    PpuSetWidescreenBg3Widen(ppu, 192);

    if (dual_edge_hud) {
        int fl = (int)ppu->extraFrameLeft;
        int fr = (int)ppu->extraFrameRight;
        const PpuWsElasticSeg hud_segs[2] = {
            { 0,   128, (int16_t)(-fl),     (int16_t)(128 - fl) },
            { 128, 256, (int16_t)(128 + fr), (int16_t)(256 + fr) }
        };
        PpuSetWidescreenLayerElasticBand(ppu, 2 /* BG3 layer */, 192, 224, hud_segs, 2);
    } else {
        int center_shift = (int)ppu->extraLeftRight - (int)ppu->extraFrameLeft;
        const PpuWsElasticSeg center_seg[1] = {
            { 0, 256, (int16_t)center_shift, (int16_t)(256 + center_shift) }
        };
        PpuSetWidescreenLayerElasticBand(ppu, 2 /* BG3 layer */, 192, 224, center_seg, 1);
    }

    /* Scan OAM slots 0..127 so HUD portraits stay anchored even when Ring Menu offsets their slots */
    uint8_t r1_first = 0, r1_cnt = 0;
    uint8_t r2_first = 0, r2_cnt = 0;
    for (int sl = 0; sl < 128; sl++) {
        uint16_t w0 = ppu->oam[sl * 2];
        uint16_t w1 = ppu->oam[sl * 2 + 1];
        uint8_t hi = (uint8_t)((ppu->highOam[sl >> 2] >> ((sl & 3) * 2)) & 0x03u);
        int oy = (w0 >> 8) & 0xFF;
        /* HUD character portraits are 16x16 (size=1), prio=3, at y=201 */
        if (oy < 198 || oy > 204) continue;
        if ((hi & 0x02u) == 0 || ((w1 >> 12) & 3u) != 3u) continue;
        int ox = (w0 & 0xFF) | (((int)hi & 1) << 8);
        if (ox >= 256) ox -= 512;

        if (single_char_hud) {
            if (ox >= 68 && ox <= 76) {
                r1_first = (uint8_t)sl;
                r1_cnt = 1;
                break;
            }
        } else {
            if (ox >= 4 && ox <= 16) {
                if (r1_cnt == 0) {
                    r1_first = (uint8_t)sl;
                    r1_cnt = 1;
                }
            } else if (ox >= 224 && ox <= 240) {
                if (r2_cnt == 0) {
                    r2_first = (uint8_t)sl;
                    r2_cnt = 1;
                }
            }
            if (r1_cnt > 0 && r2_cnt > 0) break;
        }
    }
    if (r1_cnt > 0 || r2_cnt > 0) {
        PpuSetWsHudOamBand(ppu, 224, dual_edge_hud ? 112 : 1, dual_edge_hud ? 144 : 255);
        PpuSetWsHudOamShiftRange(ppu, r1_first, r1_cnt);
        PpuSetWsHudOamShiftRange2(ppu, r2_first, r2_cnt);
    } else {
        PpuSetWsHudOamBand(ppu, 0, 0, 0);
        PpuSetWsHudOamShiftRange(ppu, 0, 0);
        PpuSetWsHudOamShiftRange2(ppu, 0, 0);
    }
}

void SoeCapturePostSimState(void)
{
    bool in_gameplay = ((g_ram[0x0B29] & 0x03) == 0x03);
    int dx_cam = 0, dy_cam = 0;
    bool cam_snapped = false;

    /* Sync view-anchored widescreen margins, HUD elastic band, and tilemaps to the
     * POST-sim PPU scroll right before snes_beam_frame_driver_draw_ppu_frame() renders.
     * Note: Do NOT call PpuSetExtraSpace() here because it calls PpuResetLayerPolicies(),
     * which would wipe wsElastic and wsWindowExpand. */
    if (g_snes && g_snes->ppu) {
        Ppu *ppu = g_snes->ppu;
        int extra = SoeGetWidescreenExtra();
        uint16_t current_map = GetLoadedMapId();
        bool map_pillarboxed = (s_map_pillarbox[current_map & 0xFF] == 0);
        bool is_mode1_16x16 = ((ppu->bgmode & 0x07) == 1) && ((ppu->bgmode & 0x30) == 0x30);
        bool has_window_mask = (ppu->screenWindowed[0] != 0) || (ppu->screenWindowed[1] != 0);
        if (extra > 0 && is_mode1_16x16 && !has_window_mask && s_frame_counter >= 60) {
            if (!(s_pillarbox_enabled && map_pillarboxed)) {
                int16_t eff_h = SoeGetEffectivePpuHScroll(ppu);
                int16_t eff_v = SoeGetEffectivePpuVScroll(ppu);
                int left_space = extra, right_space = extra;
                SoeComputeAnchoredWidescreenMargins(extra, eff_h, eff_v, &left_space, &right_space);
                s_cur_left_space = left_space;
                s_cur_right_space = right_space;
                PpuSetWidescreenViewAnchor(ppu, true);
                PpuSetExtraSideSpace(ppu, left_space, right_space, 0);
                SoeApplyHudWidescreenConfig(ppu, left_space, right_space);
                SoePopulateWidescreenTilemaps(ppu);
            } else {
                SoeApplyHudWidescreenConfig(ppu, 0, 0);
            }
        }
    }

    /* Track 60 Hz effective viewport coordinates AFTER GameRunOneFrame() and widescreen
     * view-anchoring so anchored edges report zero camera velocity (no judder). */
    if (in_gameplay) {
        int raw_cam_x = (int)SoeGetEffectivePpuHScroll(g_snes ? g_snes->ppu : NULL);
        int frame_left = (g_snes && g_snes->ppu) ? (int)g_snes->ppu->extraFrameLeft : 0;
        int cur_cam_x = raw_cam_x - frame_left;
        int cur_cam_y = (int)SoeGetEffectivePpuVScroll(g_snes ? g_snes->ppu : NULL);

        if (!s_cam_initialized) {
            s_cam_x_prev = cur_cam_x;
            s_cam_y_prev = cur_cam_y;
            s_cam_x_curr = cur_cam_x;
            s_cam_y_curr = cur_cam_y;
            s_frame_left_prev = (int16_t)frame_left;
            s_frame_left_curr = (int16_t)frame_left;
            s_cam_dx_prev = 0;
            s_cam_dy_prev = 0;
            SoeCamAxisReset(&s_cam_axis_x);
            SoeCamAxisReset(&s_cam_axis_y);
            s_cam_res_x_prev = 0.0;
            s_cam_res_y_prev = 0.0;
            s_cam_res_x_curr = 0.0;
            s_cam_res_y_curr = 0.0;
            s_cam_initialized = true;
            cam_snapped = true;
        } else {
            dx_cam = (int)cur_cam_x - (int)s_cam_x_curr;
            dy_cam = (int)cur_cam_y - (int)s_cam_y_curr;
            /* If a screen warp, teleport, or room transition occurs, snap without interpolating */
            if (abs(dx_cam) > 16 || abs(dy_cam) > 16) {
                s_cam_x_prev = cur_cam_x;
                s_cam_y_prev = cur_cam_y;
                s_cam_x_curr = cur_cam_x;
                s_cam_y_curr = cur_cam_y;
                s_frame_left_prev = (int16_t)frame_left;
                s_frame_left_curr = (int16_t)frame_left;
                s_cam_dx_prev = 0;
                s_cam_dy_prev = 0;
                SoeCamAxisReset(&s_cam_axis_x);
                SoeCamAxisReset(&s_cam_axis_y);
                s_cam_res_x_prev = 0.0;
                s_cam_res_y_prev = 0.0;
                s_cam_res_x_curr = 0.0;
                s_cam_res_y_curr = 0.0;
                cam_snapped = true;
            } else {
                s_cam_x_prev = s_cam_x_curr;
                s_cam_y_prev = s_cam_y_curr;
                s_cam_x_curr = cur_cam_x;
                s_cam_y_curr = cur_cam_y;
                s_frame_left_prev = s_frame_left_curr;
                s_frame_left_curr = (int16_t)frame_left;
                s_cam_res_x_prev = s_cam_res_x_curr;
                s_cam_res_y_prev = s_cam_res_y_curr;
                s_cam_res_x_curr = SoeCamAxisStep(&s_cam_axis_x, dx_cam);
                s_cam_res_y_curr = SoeCamAxisStep(&s_cam_axis_y, dy_cam);
                s_cam_dx_prev = dx_cam;
                s_cam_dy_prev = dy_cam;
            }
        }
    } else {
        s_cam_initialized = false;
        s_frame_left_prev = 0;
        s_frame_left_curr = 0;
        s_cam_dx_prev = 0;
        s_cam_dy_prev = 0;
        SoeCamAxisReset(&s_cam_axis_x);
        SoeCamAxisReset(&s_cam_axis_y);
        s_cam_res_x_prev = 0.0;
        s_cam_res_y_prev = 0.0;
        s_cam_res_x_curr = 0.0;
        s_cam_res_y_curr = 0.0;
    }

    /* Track 60 Hz OAM sprite positions for render-only subpixel sprite interpolation */
    memset(s_oam_suppressed_mask, 0, sizeof(s_oam_suppressed_mask));
    if (in_gameplay && g_snes && g_snes->ppu) {
        Ppu *ppu = g_snes->ppu;
        static const uint8_t kSpriteSizes[8][2] = {
            {8, 16}, {8, 32}, {8, 64}, {16, 32},
            {16, 64}, {32, 64}, {16, 32}, {16, 32}
        };
        int obj_size_mode = PPU_objSize(ppu);
        int extra_margin = SoeGetWidescreenExtra();
        bool menu_mosaic = (extra_margin > 0 && IsMode4MenuMosaicActive(ppu));
        bool high_hz_active = (!s_lock_60fps && s_active_pres_hz > 65.0);
        bool allow_suppress = s_interpolate_sprites && high_hz_active && !menu_mosaic;

        SoeOamSnap old_curr[128];
        if (s_oam_initialized) {
            memcpy(old_curr, s_oam_curr, sizeof(old_curr));
        }
        s_oam_sim_gen++;
        if (s_oam_sim_gen == 0) s_oam_sim_gen = 1;

        const int same_tile_max_d = s_enhanced_ring_menu ? 28 : 8;
        const int rot_tile_max_d = s_enhanced_ring_menu ? 28 : 6;

        for (int sl = 0; sl < 128; sl++) {
            uint16_t w0 = ppu->oam[sl * 2];
            uint16_t w1 = ppu->oam[sl * 2 + 1];
            uint8_t hi = (uint8_t)((ppu->highOam[sl >> 2] >> ((sl & 3) * 2)) & 0x03u);

            int raw_x = (int)(w0 & 0xFFu) | (((int)hi & 1) << 8);
            if (raw_x >= 256 + (int)ppu->extraRightCur) raw_x -= 512;
            int raw_y = (int)((w0 >> 8) & 0xFFu);
            int sy = (raw_y >= 224) ? (raw_y - 256) : raw_y;
            uint8_t sz = kSpriteSizes[obj_size_mode][(hi >> 1) & 1];

            /* Exclude bottom HUD (sy >= 192) and offscreen/hidden slots */
            int left_bound = (extra_margin > 0) ? (int)ppu->extraLeftCur : 0;
            int right_bound = (extra_margin > 0) ? (int)ppu->extraRightCur : 0;
            bool is_world_active = (sy + (int)sz > 0) && (sy < 192) &&
                                   (raw_x + (int)sz > -left_bound) &&
                                   (raw_x < 256 + right_bound);

            SoeOamSnap cur;
            cur.x = (int16_t)raw_x;
            cur.y = (int16_t)sy;
            cur.w0 = w0;
            cur.w1 = w1;
            cur.size = sz;
            cur.active = is_world_active;

            SoeOamSnap matched = cur;
            bool found_match = false;
            if (s_oam_initialized && cur.active) {
                /* Include H/V flip bits (0xC000) and palette (0x0E00) so mirrored or animated
                 * halves never falsely match each other across OAM slot shuffles. */
                uint16_t cur_flip_pal = (uint16_t)(cur.w1 & 0xCE00u);
                uint16_t cur_identity = (uint16_t)(cur.w1 & 0xCFFFu);
                if (old_curr[sl].active && old_curr[sl].size == cur.size) {
                    int adx = abs((int)cur.x - (int)old_curr[sl].x);
                    int ady = abs((int)cur.y - (int)old_curr[sl].y);
                    if (adx <= 4 && ady <= 4 &&
                        (old_curr[sl].w1 & 0xCE00u) == cur_flip_pal) {
                        matched = old_curr[sl];
                        found_match = true;
                    } else if (adx <= same_tile_max_d && ady <= same_tile_max_d &&
                               (old_curr[sl].w1 & 0xCFFFu) == cur_identity) {
                        matched = old_curr[sl];
                        found_match = true;
                    }
                }
                /* Fallback for SNES OAM slot rotation: strict match by (size, tile+attr+flip) */
                if (!found_match) {
                    int best_dist = 999;
                    for (int j = 0; j < 128; j++) {
                        if (!old_curr[j].active || old_curr[j].size != cur.size) continue;
                        if ((old_curr[j].w1 & 0xCFFFu) != cur_identity) continue;
                        int adx = abs((int)cur.x - (int)old_curr[j].x);
                        int ady = abs((int)cur.y - (int)old_curr[j].y);
                        if (adx <= rot_tile_max_d && ady <= rot_tile_max_d && (adx + ady) < best_dist) {
                            best_dist = adx + ady;
                            matched = old_curr[j];
                            found_match = true;
                            if (best_dist == 0) break;
                        }
                    }
                }
            }
            s_oam_prev[sl] = matched;
            s_oam_curr[sl] = cur;

            if (allow_suppress && cur.active) {
                s_oam_suppressed_mask[sl >> 3] |= (uint8_t)(1u << (sl & 7));
            }
        }
        s_oam_initialized = true;
        PpuSetWsOamSuppressMask(ppu, allow_suppress ? s_oam_suppressed_mask : NULL);
    } else {
        s_oam_initialized = false;
        if (g_snes && g_snes->ppu) {
            PpuSetWsOamSuppressMask(g_snes->ppu, NULL);
        }
    }
}

void SoeBeginSimFrame(unsigned number)
{
    s_frame_counter = number;

    /* Install event filter only after gameplay window exists (never during pre-boot launcher) */
    if (g_window) {
        if (!s_event_filter_installed) {
            SDL_SetEventFilter(&SlideMenuEventFilter, NULL);
            s_event_filter_installed = true;
        }
        PollMenuKeyboardDirect();
    }

    SoeGameEnhancementsTick();
    SoeSyncJukeboxRomPatches();

    bool in_gameplay = ((g_ram[0x0B29] & 0x03) == 0x03);

    static uint16_t s_last_logged_map = 0xFFFF;
    uint16_t cur_logged_map = GetLoadedMapId();
    bool map_changed_now = (cur_logged_map != s_last_logged_map);
    if (map_changed_now) {
        fprintf(stderr, "[Map Change] Frame %u: %u -> %u (in_gameplay=%d)\n",
                number, (unsigned)s_last_logged_map, (unsigned)cur_logged_map, (int)in_gameplay);
        s_last_logged_map = cur_logged_map;
    }

    /*
     * 1. Warp Monitor & Failsafe Unblank:
     * When a map warp occurs, opcode 0x22 fades out the screen ($0106 = $8F).
     * If opcode 0x26 did not run or the script yielded, unblank the screen ($0106 = 0, $0B83 = $0F)
     * as soon as the target map is loaded (or upon timeout) so the destination is immediately visible.
     */
    static int s_gameplay_frames = 0;
    static bool s_auto_warp_done = false;
    static bool s_env_cached = false;
    static const char *s_open_menu_env = NULL;
    static const char *s_initial_tab_env = NULL;
    static const char *s_open_debug_env = NULL;
    static const char *s_auto_warp_env = NULL;
    static const char *s_cli_export_maps_env = NULL;
    static const char *s_cli_export_music_env = NULL;
    static const char *s_cli_export_sfx_env = NULL;
    static const char *s_cli_export_spc_env = NULL;
    if (!s_env_cached) {
        s_env_cached = true;
        s_open_menu_env = getenv("SOE_OPEN_MENU_TEST");
        s_initial_tab_env = getenv("SOE_INITIAL_TAB_TEST");
        s_open_debug_env = getenv("SOE_OPEN_DEBUG_TEST");
        s_auto_warp_env = getenv("SOE_AUTO_WARP_TEST");
        s_cli_export_maps_env = getenv("SOE_EXPORT_ALL_MAPS");
        s_cli_export_music_env = getenv("SOE_EXPORT_ALL_MUSIC");
        s_cli_export_sfx_env = getenv("SOE_EXPORT_ALL_SFX");
        s_cli_export_spc_env = getenv("SOE_EXPORT_ALL_SPC");
    }
    if (in_gameplay) {
        s_gameplay_frames++;
        if (s_open_menu_env && s_gameplay_frames >= 20) {
            s_menu_open = true;
            s_anim_x = 0.0f;
            if (s_initial_tab_env) {
                s_active_tab = (SoeMenuTab)(atoi(s_initial_tab_env) % TAB_COUNT);
            }
        }
        if (s_open_debug_env && s_gameplay_frames >= 20) {
            if (!s_debug_overlay) SyncDogFormFromRam();
            s_debug_overlay = true;
            s_dbg_slide = 1.0f;
            s_menu_open = false;
            if (strcmp(s_open_debug_env, "warp") == 0) {
                s_dbg_selected_row = DBG_WARP;
                s_warp_slide = 1.0f;
                OpenWarpWindow();
                const char *wc = getenv("SOE_WARP_TEST_CURSOR");
                if (wc) {
                    s_warp_cursor = atoi(wc);
                    ClampWarpScroll();
                }
            }
        }
        if (s_auto_warp_env && !s_auto_warp_done && s_gameplay_frames == 90) {
            uint16_t tw = (uint16_t)atoi(s_auto_warp_env);
            s_auto_warp_done = true;
            fprintf(stderr, "[Auto Warp Test] Triggering warp to Map %u at frame %u (cur map: %u)\n",
                    (unsigned)tw, number, (unsigned)GetLoadedMapId());
            DebugExecuteWarpEx(tw, true);
        }
    } else {
        if (s_auto_warp_env && (number == 700 || number == 730)) {
            uint16_t tw = (number == 700) ? 1u : (uint16_t)atoi(s_auto_warp_env);
            g_ram[0x0B29] = 0x03;
            g_ram[0x0A33] = 100; g_ram[0x0A35] = 100;
            g_ram[0x0A89] = 100; g_ram[0x0A8B] = 100;
            if (number == 730) s_auto_warp_done = true;
            fprintf(stderr, "[Auto Warp Test] Booting directly into Map %u at frame %u\n",
                    (unsigned)tw, number);
            DebugExecuteWarpEx(tw, true);
        }
    }

    if (s_cli_export_maps_env) {
        if (g_window) {
            SDL_HideWindow(g_window);
        }
        if (number == 10) {
            fprintf(stderr, "[Map Exporter] CLI --export-maps requested: exporting all 127 maps...\n");
            int count = SoeExportAllMaps();
            fprintf(stderr, "[Map Exporter] Complete: %d maps successfully exported to ./Extract/MAPS/\n", count);
            exit(0);
        }
    }

    const char *one_map_env = getenv("SOE_EXPORT_MAP");
    if (one_map_env && one_map_env[0] && number == 10) {
        uint16_t m = (uint16_t)atoi(one_map_env);
        WatchdogSetEnabled(0);
        soe_mkdir("Extract");
        soe_mkdir("Extract/MAPS");
        for (int boot_f = 0; boot_f < 360; boot_f++) {
            WatchdogFrameStart();
            GameRunOneFrame();
        }
        g_ram[0x0B29] = 0x03;
        if ((g_ram[0x0A33] | (g_ram[0x0A34] << 8)) == 0) {
            g_ram[0x0A33] = 100; g_ram[0x0A35] = 100;
            g_ram[0x0A7D] = 100; g_ram[0x0A7F] = 100;
        }
        DebugExecuteWarpEx(m, true);
        SoeWaitForMapReady(m);
        uint16_t loaded = GetLoadedMapId();
        char path[128];
        snprintf(path, sizeof(path), "Extract/MAPS/map_%03u.png", (unsigned)m);
        bool ok = SoeExportCurrentMap(path);
        fprintf(stderr, "[Map Exporter] Single map %u: loaded=%u ok=%d bright=%u cgram0=0x%04X\n",
                (unsigned)m, (unsigned)loaded, ok ? 1 : 0,
                (unsigned)(g_snes && g_snes->ppu ? PPU_brightness(g_snes->ppu) : 0),
                (unsigned)(g_snes && g_snes->ppu ? g_snes->ppu->cgram[0] : 0));
        exit(0);
    }

    if (s_cli_export_music_env) {
        if (g_window) {
            SDL_HideWindow(g_window);
        }
        if (number == 10) {
            fprintf(stderr, "[Music Exporter] CLI --export-music requested...\n");
            int count = SoeExportAllMusic();
            fprintf(stderr, "[Music Exporter] Complete: %d songs exported to ./Extract/MUSIC/\n", count);
            exit(0);
        }
    }

    if (s_cli_export_sfx_env) {
        if (g_window) {
            SDL_HideWindow(g_window);
        }
        if (number == 10) {
            fprintf(stderr, "[SFX Exporter] CLI --export-sfx requested...\n");
            int count = SoeExportAllSfx();
            fprintf(stderr, "[SFX Exporter] Complete: %d sound effects exported to ./Extract/SFX/\n", count);
            exit(0);
        }
    }

    if (s_cli_export_spc_env) {
        if (g_window) {
            SDL_HideWindow(g_window);
        }
        if (number == 10) {
            fprintf(stderr, "[SPC Exporter] CLI --export-spc requested...\n");
            int count = SoeExportAllSpc();
            fprintf(stderr, "[SPC Exporter] Complete: %d tracks exported to ./Extract/SPC/\n", count);
            exit(0);
        }
    }

    if (s_warp_monitor_frames > 0) {
        s_warp_monitor_frames--;
        uint16_t cur_map = GetLoadedMapId();
        uint8_t inidisp = g_ram[0x0106];
        uint16_t fade_target = (uint16_t)g_ram[0x0B83] | ((uint16_t)g_ram[0x0B84] << 8);
        bool full_bright = ((inidisp & 0x80u) == 0) && ((inidisp & 0x0Fu) == 0x0Fu);
        bool fading_in = ((fade_target & 0x8000u) != 0) && ((fade_target & 0x000Fu) > 0);

        if (cur_map == s_warp_target_map && full_bright) {
            /* Native Opcode 0x22 + 0x26 completed its full VRAM upload and fade-in cleanly. */
            s_warp_monitor_frames = 0;
            s_warp_stuck_black_frames = 0;
        } else {
            /* Never clear forced blank ($0106 bit 7) while the room's VRAM/CGRAM/tilemap/sprite
             * DMA queues are still draining or while a native fade-in is already running; doing
             * so drops mid-load BG tilemap DMAs and leaves the initial viewport black. */
            bool vram_dma_empty  = (g_ram[0x013B] == g_ram[0x013D]) && (g_ram[0x013C] == g_ram[0x013E]);
            bool cgram_dma_empty = (g_ram[0x1290] == g_ram[0x128E]) && (g_ram[0x1291] == g_ram[0x128F]);
            bool tilemap_empty   = (g_ram[0x1170] == g_ram[0x1172]) && (g_ram[0x1171] == g_ram[0x1173]);
            bool spr16_empty     = (g_ram[0x15D6] == 0 && g_ram[0x15D7] == 0);
            bool spr32_empty     = (g_ram[0x163C] == 0 && g_ram[0x163D] == 0);
            bool queues_empty    = vram_dma_empty && cgram_dma_empty && tilemap_empty &&
                                   spr16_empty && spr32_empty;

            if (cur_map == s_warp_target_map && queues_empty && !fading_in && !full_bright) {
                s_warp_stuck_black_frames++;
            } else {
                s_warp_stuck_black_frames = 0;
            }

            if (s_warp_stuck_black_frames >= 30 ||
                (s_warp_monitor_frames == 0 && queues_empty && !full_bright && !fading_in)) {
                fprintf(stderr, "[Warp Monitor] Failsafe fade-in at frame %u! Cur map: %u, target: %u\n",
                        number, (unsigned)cur_map, (unsigned)s_warp_target_map);
                g_ram[0x0106] = 0x00;
                g_ram[0x0B83] = 0x0F;
                g_ram[0x0B84] = 0x80; /* Request native fade-in so SoeRoomTransitionDrainIfReady runs */
                s_warp_monitor_frames = 0;
                s_warp_stuck_black_frames = 0;
            }
        }
    }

    if (s_post_warp_walk_timer > 0) {
        s_post_warp_walk_timer--;
    }

    /*
     * 2. Walk Through Walls (No-Clip):
     * Active if manually enabled in Debug Menu ([X] Walk Thru Walls) or automatically
     * for 300 frames (~5s) post-warp to prevent getting stuck in walls.
     *
     * Scopes collision bypass strictly to Boy ($7E:0F3E) and Dog ($7E:0F40) via a
     * 24-byte stub in unused $81:98D0..$81:98E7 0xFF padding (never touching the
     * $80:9479..$80:94D8 sprite/OAM table), leaving NPCs, bosses, cutscene scripts,
     * $8F:AE70 (never jump to BRK/STP), and $8F:AF0E ($0012,Y moved bit) intact.
     */
    bool walk_walls = s_walk_walls_active || (s_post_warp_walk_timer > 0);
    uint8_t *rom = (g_snes && g_snes->cart) ? g_snes->cart->rom : NULL;
    if (rom && g_snes->cart->romSize >= 0x0FAF50u) {
        /* Always keep 0x0FA458, 0x0FAF0E, and 0x0FAE70 at their authentic ROM bytes */
        if (rom[0x0FA458] != 0xAA) {
            rom[0x0FA458] = 0xAA; /* TAX */
            rom[0x0FA459] = 0x7B; /* TDC */
            rom[0x0FA45A] = 0x8A; /* TXA */
            rom[0x0FA45B] = 0xAA; /* TAX */
            rom[0x0FA45C] = 0xA5; /* LDA $67 */
        }
        rom[0x0FAF0E] = 0xB9; /* LDA $0012,Y (must stay intact so movement completion bits work) */
        rom[0x0FAE70] = 0x03; /* BEQ +3 (must stay 0x03; 0x70 branches mid-insn to BRK -> STP!) */

        if (walk_walls) {
            /*
             * $8F:AF48 = "can entity Y step to ($46,$48)?" Returns CARRY SET = passable
             * (caller then sets $12|=2 and commits the move via $8F:C2E4), CARRY CLEAR =
             * blocked. The engine already has a native no-clip path at $8F:AF67 (taken
             * when entity flag $10 & $0800 is set): it looks up the destination tile
             * ($90:9815), refreshes the entity's height/layer in $44 ($8F:A914) and
             * returns SEC. Route Boy/Dog through that path so they pass walls while
             * keeping correct layer/priority (no sprite glitching) and real movement.
             */
            static const uint8_t kWalkStub[24] = {
                0xB9, 0x18, 0x00,       /* LDA $0018,Y  (displaced) */
                0x85, 0x44,             /* STA $44      (displaced) */
                0x98,                   /* TYA                      */
                0xCD, 0x3E, 0x0F,       /* CMP $0F3E (Boy)          */
                0xF0, 0x09,             /* BEQ noclip               */
                0xCD, 0x40, 0x0F,       /* CMP $0F40 (Dog)          */
                0xF0, 0x04,             /* BEQ noclip               */
                0x5C, 0x4D, 0xAF, 0x8F, /* JML $8FAF4D (vanilla)    */
                /* noclip: */
                0x5C, 0x67, 0xAF, 0x8F  /* JML $8FAF67 (native no-clip, SEC) */
            };
            memcpy(&rom[0x0198D0u], kWalkStub, sizeof(kWalkStub));
            rom[0x0FAF48] = 0x5C; /* JML $8198D0 */
            rom[0x0FAF49] = 0xD0;
            rom[0x0FAF4A] = 0x98;
            rom[0x0FAF4B] = 0x81;
            rom[0x0FAF4C] = 0xEA; /* NOP */
        } else {
            rom[0x0FAF48] = 0xB9; /* LDA $0018,Y */
            rom[0x0FAF49] = 0x18;
            rom[0x0FAF4A] = 0x00;
            rom[0x0FAF4B] = 0x85; /* STA $44 */
            rom[0x0FAF4C] = 0x44;
            memset(&rom[0x0198D0u], 0xFF, 24u);
        }
    }

    /*
     * 3. Infinite HP, Infinite Stamina, and One-Hit KO Cheats:
     */
    if (in_gameplay) {
        /* Infinite HP is implemented as a ROM hook on the damage routine ($8F:C237); no RAM writes. */

        if (s_inf_stamina_active) {
            /* Keep Boy & Dog stamina at exactly 0x0400 (100% ready).
             * 0x0400 is neutral/ready (low byte 0x00, high byte 0x04).
             * Values >= 0x0401 indicate a weapon charge attack in progress ($8F:CE66),
             * so never write a non-zero low byte when clamping to 100%, and preserve
             * values > 0x0400 so holding Attack to charge Level 1-3 attacks still works. */
            uint16_t boy_stam = (uint16_t)g_ram[0x4EB7] | ((uint16_t)g_ram[0x4EB8] << 8);
            if (boy_stam < 0x0400u) {
                g_ram[0x4EB7] = 0x00;
                g_ram[0x4EB8] = 0x04;
                boy_stam = 0x0400u;
            }
            if (boy_stam == 0x0400u) {
                /* Clear the 100% HUD linger timer ($0B4D) so the bar stays blank */
                g_ram[0x0B4D] = 0x00;
                g_ram[0x0B4E] = 0x00;
            }
            uint16_t dog_stam = (uint16_t)g_ram[0x4F65] | ((uint16_t)g_ram[0x4F66] << 8);
            if (dog_stam < 0x0400u) {
                g_ram[0x4F65] = 0x00;
                g_ram[0x4F66] = 0x04;
                dog_stam = 0x0400u;
            }
            if (dog_stam == 0x0400u) {
                /* Clear the Dog 100% HUD linger timer ($0B4F) so the bar stays blank */
                g_ram[0x0B4F] = 0x00;
                g_ram[0x0B50] = 0x00;
            }
        }

        if (s_one_hit_ko_active) {
            /* Max physical attack power (999) for instantaneous one-hit kills */
            g_ram[0x0A3F] = 999 & 0xFF;
            g_ram[0x0A40] = (999 >> 8) & 0xFF;
            g_ram[0x0A8D] = 999 & 0xFF;
            g_ram[0x0A8E] = (999 >> 8) & 0xFF;

            /* Auto-heal Dog EXP if it was previously corrupted */
            uint32_t dog_exp = (uint32_t)g_ram[0x0A93] |
                               ((uint32_t)g_ram[0x0A94] << 8) |
                               ((uint32_t)g_ram[0x0A95] << 16) |
                               ((uint32_t)g_ram[0x0A96] << 24);
            if (g_ram[0x0A9A] < 99 && (dog_exp >= 10000000u || g_ram[0x0A95] == 0xE7)) {
                uint8_t lvl = g_ram[0x0A9A];
                uint32_t base_exp = 0;
                if (lvl > 0 && g_snes && g_snes->cart && g_snes->cart->rom &&
                    g_snes->cart->romSize >= 0x0C8CFCu + (uint32_t)lvl * 3u) {
                    const uint8_t *tbl = &g_snes->cart->rom[0x0C8CFCu + (uint32_t)(lvl - 1) * 3u];
                    base_exp = (uint32_t)tbl[0] | ((uint32_t)tbl[1] << 8) | ((uint32_t)tbl[2] << 16);
                }
                g_ram[0x0A93] = (uint8_t)(base_exp & 0xFF);
                g_ram[0x0A94] = (uint8_t)((base_exp >> 8) & 0xFF);
                g_ram[0x0A95] = (uint8_t)((base_exp >> 16) & 0xFF);
                g_ram[0x0A96] = (uint8_t)((base_exp >> 24) & 0xFF);
            }
        }
    }


    /*
     * Infinite HP: hook the shared "HP -= damage" routine at $8F:C237.
     *   Original: LDA $002A,Y / SEC / SBC $0076,Y / BPL +1 / TDC / STA $002A,Y
     * When enabled, a stub at $8F:C000 (unused padding) checks whether entity Y is the Boy
     * ($0F3E) or Dog ($0F40). If so it skips the subtraction entirely (HP untouched);
     * every other entity (enemies) runs the original code unchanged. When disabled, the
     * original bytes are restored. No RAM is written, so nothing else can be disturbed.
     */
    if (rom && g_snes->cart->romSize >= 0x0FC040u) {
        static const uint8_t kOrigDmg[13] = {
            0xB9, 0x2A, 0x00, 0x38, 0xF9, 0x76, 0x00, 0x10, 0x01, 0x7B, 0x99, 0x2A, 0x00
        };
        static const uint8_t kHpStub[36] = {
            0x98,                   /* TYA                       */
            0xCD, 0x3E, 0x0F,       /* CMP $0F3E (Boy)           */
            0xF0, 0x16,             /* BEQ skip                  */
            0xCD, 0x40, 0x0F,       /* CMP $0F40 (Dog)           */
            0xF0, 0x11,             /* BEQ skip                  */
            0xB9, 0x2A, 0x00,       /* original routine (13 B)   */
            0x38,
            0xF9, 0x76, 0x00,
            0x10, 0x01,
            0x7B,
            0x99, 0x2A, 0x00,
            0x5C, 0x44, 0xC2, 0x8F, /* JML $8FC244               */
            /* skip: */
            0xB9, 0x2A, 0x00,       /* LDA $002A,Y (no damage)   */
            0x38,                   /* SEC (as if no borrow)     */
            0x5C, 0x44, 0xC2, 0x8F  /* JML $8FC244               */
        };
        if (s_inf_hp_active) {
            memcpy(&rom[0x0FC000], kHpStub, sizeof(kHpStub));
            rom[0x0FC237] = 0x5C; rom[0x0FC238] = 0x00; rom[0x0FC239] = 0xC0; rom[0x0FC23A] = 0x8F;
            memset(&rom[0x0FC23B], 0xEA, 9);
        } else {
            memcpy(&rom[0x0FC237], kOrigDmg, sizeof(kOrigDmg));
        }
    }

    /* Running Stamina Patch in ROM ($90:82B2 -> ROM offset 0x1082B2).
     * Original: LDA $0016,Y (B9 16 00) followed by stamina drain and JMP $8279.
     * Patched:  JMP $8279   (4C 79 82) to skip stamina drain completely while running,
     * keeping $002E,Y at 0x0400 so the stamina bar stays completely blank. */
    if (rom && g_snes->cart->romSize >= 0x1082B5u) {
        if (s_inf_stamina_active) {
            rom[0x1082B2] = 0x4C; /* JMP $8279 */
            rom[0x1082B3] = 0x79;
            rom[0x1082B4] = 0x82;
        } else {
            rom[0x1082B2] = 0xB9; /* LDA $0016,Y (authentic running stamina drain) */
            rom[0x1082B3] = 0x16;
            rom[0x1082B4] = 0x00;
        }
    }

    /* Sync Toaster Dog's area-dependent SFX ($8C:8362[$3C]) with active SPC700 ARAM */
    if (rom && g_snes && g_snes->apu) {
        SyncToasterDogSfx(rom, g_snes->apu);
    }

    if (!g_snes || !g_snes->ppu) return;
    Ppu *ppu = g_snes->ppu;

    /* Sync in-game Window Prefs entries & apply Glass Dialog capture when active */
    SoeTickWindowPrefs(ppu);
    PatchDialogWindowBackground(ppu, s_dialog_theme != DIALOG_THEME_ORIGINAL);

    int extra = SoeGetWidescreenExtra();
    if (extra <= 0) {
        SoePatchWidescreenSprites(0, 0);
        SoePatchWidescreenTargeting(0, 0);
        PpuSetWidescreenViewAnchor(ppu, false);
        PpuSetWidescreenHudAlwaysVisible(ppu, false);
        PpuSetWidescreenBg3Widen(ppu, 0);
        PpuSetWidescreenLayerElasticBand(ppu, 2 /* BG3 layer */, 0, 0, NULL, 0);
        PpuSetWsHudOamBand(ppu, 0, 0, 0);
        PpuSetWsHudOamShiftRange(ppu, 0, 0);
        PpuSetWsHudOamShiftRange2(ppu, 0, 0);
        return;
    }

    /*
     * Center 4:3 Pillarboxed Screens:
     * - Maps marked as 0 in map_pillarbox.json when pillarboxing is enabled.
     * - Non-Mode-1 screens or screens without 16x16 BG1/BG2 tiles (e.g. Mode 4
     *   Name Entry screen, boot credits, menus).
     * - Screens using hardware window clipping ($212E/$212F screenWindowed != 0,
     *   such as the 1965 Podunk square-screen cinema/lab intro).
     */
    uint16_t current_map = GetLoadedMapId();
    bool map_pillarboxed = (s_map_pillarbox[current_map & 0xFF] == 0);

    bool is_mode1_16x16 = ((ppu->bgmode & 0x07) == 1) && ((ppu->bgmode & 0x30) == 0x30);
    bool has_window_mask = (ppu->screenWindowed[0] != 0) || (ppu->screenWindowed[1] != 0);

    if ((s_pillarbox_enabled && map_pillarboxed) || !is_mode1_16x16 || has_window_mask || number < 60) {
        if (number >= 60 && IsMode4MenuMosaicActive(ppu)) {
            s_cur_left_space = extra;
            s_cur_right_space = extra;
        } else {
            s_cur_left_space = 0;
            s_cur_right_space = 0;
        }
        SoePatchWidescreenSprites(0, 0);
        SoePatchWidescreenTargeting(0, 0);
        PpuSetWidescreenViewAnchor(ppu, false);
        PpuSetExtraSpaceCentered(ppu, (uint16_t)extra);
        if (is_mode1_16x16 && !has_window_mask && number >= 60) {
            SoeApplyHudWidescreenConfig(ppu, 0, 0);
        } else {
            PpuSetWidescreenHudAlwaysVisible(ppu, false);
            PpuSetWidescreenBg3Widen(ppu, 0);
            PpuSetWidescreenLayerElasticBand(ppu, 2 /* BG3 layer */, 0, 0, NULL, 0);
            PpuSetWsHudOamBand(ppu, 0, 0, 0);
            PpuSetWsHudOamShiftRange(ppu, 0, 0);
            PpuSetWsHudOamShiftRange2(ppu, 0, 0);
        }
        return;
    }

    int left_space = extra;
    int right_space = extra;
    int cam_h_layer = SoeGetWorldCamScrollLayer(ppu, false);
    int cam_v_layer = SoeGetWorldCamScrollLayer(ppu, true);
    uint16_t h_addr = (cam_h_layer == 1) ? 0x0112u : 0x010Eu;
    uint16_t v_addr = (cam_v_layer == 1) ? 0x0114u : 0x0110u;
    int16_t hscroll_cam = (int16_t)(uint16_t)(g_ram[h_addr] | (g_ram[h_addr + 1u] << 8));
    int16_t vscroll_cam = (int16_t)(uint16_t)(g_ram[v_addr] | (g_ram[v_addr + 1u] << 8));
    SoeComputeAnchoredWidescreenMargins(extra, hscroll_cam, vscroll_cam, &left_space, &right_space);

    s_cur_left_space = left_space;
    s_cur_right_space = right_space;
    SoePatchWidescreenSprites(left_space, right_space);
    PpuSetExtraSpace(ppu, (uint16_t)extra);
    PpuSetWidescreenViewAnchor(ppu, true);
    SoePatchWidescreenTargeting(left_space, right_space);
    PpuSetExtraSideSpace(ppu, left_space, right_space, 0);

    if (IsNobiliaDesertMap(current_map) && !map_pillarboxed && (left_space > 0 || right_space > 0)) {
        PpuSetWidescreenWindowExpansion(ppu, 0x1F, 0x03);
    } else {
        PpuSetWidescreenWindowExpansion(ppu, 0, 0);
    }

    SoeApplyHudWidescreenConfig(ppu, left_space, right_space);
}



/* Write a BGRA/XRGB8888 framebuffer into a standard RGBA PNG file. */
static bool SaveFramebufferPng(const char *path, const uint32_t *pixels, int stride, int frame_w, int frame_h)
{
    if (!path || !pixels || frame_w <= 0 || frame_h <= 0 || stride < frame_w)
        return false;

    uint32_t *rgba = (uint32_t *)malloc((size_t)frame_w * (size_t)frame_h * sizeof(uint32_t));
    if (!rgba)
        return false;

    for (int y = 0; y < frame_h; y++) {
        const uint32_t *src_row = pixels + y * stride;
        uint32_t *dst_row = rgba + y * frame_w;
        for (int x = 0; x < frame_w; x++) {
            uint32_t px = src_row[x];
            uint8_t r = (px >> 16) & 0xFF;
            uint8_t g = (px >> 8) & 0xFF;
            uint8_t b = px & 0xFF;
            uint8_t a = (px >> 24) & 0xFF;
            if (a == 0) a = 0xFF;
            dst_row[x] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
        }
    }

    int ok = stbi_write_png(path, frame_w, frame_h, 4, rgba, frame_w * 4);
    free(rgba);
    return ok != 0;
}

/* Capture the fully composited output frame into Screenshots/ as a PNG. */
static void CaptureFullScreenshot(const uint32_t *pixels, int stride, int frame_w, int frame_h)
{
    soe_mkdir("Screenshots");

    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char base_name[64];
    if (tm_now) {
        snprintf(base_name, sizeof(base_name), "Screenshots/screenshot_%04d%02d%02d_%02d%02d%02d",
                 tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday,
                 tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec);
    } else {
        snprintf(base_name, sizeof(base_name), "Screenshots/screenshot_%llu",
                 (unsigned long long)SDL_GetTicks());
    }

    char out_path[96];
    snprintf(out_path, sizeof(out_path), "%s.png", base_name);
    for (int seq = 1; seq < 100; seq++) {
        FILE *probe = fopen(out_path, "rb");
        if (!probe) break;
        fclose(probe);
        snprintf(out_path, sizeof(out_path), "%s_%02d.png", base_name, seq);
    }

    if (!SaveFramebufferPng(out_path, pixels, stride, frame_w, frame_h)) {
        s_screenshot_cooldown_until_ms = 0;
        if (s_debug_overlay) {
            snprintf(s_debug_toast, sizeof(s_debug_toast), "Screenshot failed!");
            s_debug_toast_timer = 150;
        }
    }
}

/*
 * Presentation rate hook wired into SnesDesktopHostGame:
 * When locked to 60 FPS, presents at standard 60 Hz lockstep.
 * When unlocked, decouples presentation to the display's native refresh rate (e.g. 100 Hz, 120 Hz, 144 Hz)
 * while the SNES simulation advances at exact authentic 60 Hz.
 */
double SoePresentationHz(double display_refresh)
{
    /* Match SNES_HOST_NTSC_HZ (60.098811862) exactly whenever locked to 60 FPS OR running
     * on a ~60 Hz display so alpha stays 1.0 (1:1 lockstep with zero drift/blur/judder). */
    const double kNtscSimHz = 60.098811862;
    if (s_lock_60fps || display_refresh <= 65.0) {
        s_active_pres_hz = kNtscSimHz;
        return kNtscSimHz;
    }
    s_active_pres_hz = display_refresh;
    return display_refresh;
}

/*
 * High-performance 2D bilinear subpixel camera resampler:
 * Shifts the game world viewport (scanlines 0 .. 191) by subpixel displacement (shift_x, shift_y)
 * according to the presentation accumulator fraction alpha, achieving ultra-smooth 100 Hz scrolling
 * with zero 3:2 pulldown judder while leaving the bottom status bar (192 .. 223) pixel-perfect and static.
 */
static void ApplySubpixelCameraScroll(uint32_t *pixels, int stride, int frame_w, int frame_h,
                                      double shift_x, double shift_y)
{
    if (!pixels || frame_w <= 2 || frame_h <= 2) return;
    if (fabs(shift_x) < 0.001 && fabs(shift_y) < 0.001) return;

    /* Clamp subpixel shift within reasonable bounds */
    if (shift_x < -4.0) shift_x = -4.0;
    else if (shift_x > 4.0) shift_x = 4.0;
    if (shift_y < -4.0) shift_y = -4.0;
    else if (shift_y > 4.0) shift_y = 4.0;

    int world_h = frame_h > 192 ? 192 : frame_h;

    static uint32_t *s_subpix_tmp = NULL;
    static int s_subpix_tmp_cap = 0;
    int total_world_px = frame_w * world_h;
    if (total_world_px > s_subpix_tmp_cap) {
        free(s_subpix_tmp);
        s_subpix_tmp = (uint32_t *)malloc((size_t)total_world_px * sizeof(uint32_t));
        s_subpix_tmp_cap = total_world_px;
    }
    if (!s_subpix_tmp) return;

    /* Copy current game world pixels to temporary buffer */
    for (int y = 0; y < world_h; y++) {
        memcpy(s_subpix_tmp + y * frame_w, pixels + y * stride, (size_t)frame_w * sizeof(uint32_t));
    }

    int sx_int = (int)floor(shift_x);
    int sy_int = (int)floor(shift_y);
    int frac_x = (int)((shift_x - (double)sx_int) * 256.0);
    int frac_y = (int)((shift_y - (double)sy_int) * 256.0);
    if (frac_x < 0) frac_x = 0; else if (frac_x > 256) frac_x = 256;
    if (frac_y < 0) frac_y = 0; else if (frac_y > 256) frac_y = 256;

    int w00 = (256 - frac_x) * (256 - frac_y);
    int w10 = frac_x * (256 - frac_y);
    int w01 = (256 - frac_x) * frac_y;
    int w11 = frac_x * frac_y;

#if defined(__AVX2__)
    const __m256i v_w00 = _mm256_set1_epi32(w00);
    const __m256i v_w10 = _mm256_set1_epi32(w10);
    const __m256i v_w01 = _mm256_set1_epi32(w01);
    const __m256i v_w11 = _mm256_set1_epi32(w11);
    const __m256i v_ff = _mm256_set1_epi32(0xFF);
    const __m256i v_alpha = _mm256_set1_epi32((int)0xFF000000u);
    int simd_x_start = (sx_int < 0) ? -sx_int : 0;
    int simd_x_end = frame_w - 1 - sx_int;
    if (simd_x_end > frame_w) simd_x_end = frame_w;
#endif

    for (int y = 0; y < world_h; y++) {
        uint32_t *dst_row = pixels + y * stride;
        int y0 = y + sy_int;
        int y1 = y0 + 1;
        if (y0 < 0) y0 = 0; else if (y0 >= world_h) y0 = world_h - 1;
        if (y1 < 0) y1 = 0; else if (y1 >= world_h) y1 = world_h - 1;

        const uint32_t *row0 = s_subpix_tmp + y0 * frame_w;
        const uint32_t *row1 = s_subpix_tmp + y1 * frame_w;
        int x = 0;

#if defined(__AVX2__)
        for (; x < simd_x_start && x < frame_w; x++) {
            int x0 = x + sx_int;
            int x1 = x0 + 1;
            if (x0 < 0) x0 = 0; else if (x0 >= frame_w) x0 = frame_w - 1;
            if (x1 < 0) x1 = 0; else if (x1 >= frame_w) x1 = frame_w - 1;

            uint32_t p00 = row0[x0], p10 = row0[x1], p01 = row1[x0], p11 = row1[x1];
            int r = (((p00 >> 16) & 0xFF) * w00 + ((p10 >> 16) & 0xFF) * w10 +
                     ((p01 >> 16) & 0xFF) * w01 + ((p11 >> 16) & 0xFF) * w11) >> 16;
            int g = (((p00 >> 8) & 0xFF) * w00 + ((p10 >> 8) & 0xFF) * w10 +
                     ((p01 >> 8) & 0xFF) * w01 + ((p11 >> 8) & 0xFF) * w11) >> 16;
            int b = ((p00 & 0xFF) * w00 + (p10 & 0xFF) * w10 +
                     (p01 & 0xFF) * w01 + (p11 & 0xFF) * w11) >> 16;
            if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
            dst_row[x] = (p00 & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
        for (; x + 8 <= simd_x_end; x += 8) {
            int x0 = x + sx_int;
            __m256i p00 = _mm256_loadu_si256((const __m256i *)(row0 + x0));
            __m256i p10 = _mm256_loadu_si256((const __m256i *)(row0 + x0 + 1));
            __m256i p01 = _mm256_loadu_si256((const __m256i *)(row1 + x0));
            __m256i p11 = _mm256_loadu_si256((const __m256i *)(row1 + x0 + 1));

            __m256i r = _mm256_srli_epi32(_mm256_add_epi32(
                _mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(p00, 16), v_ff), v_w00),
                                 _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(p10, 16), v_ff), v_w10)),
                _mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(p01, 16), v_ff), v_w01),
                                 _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(p11, 16), v_ff), v_w11))), 16);
            __m256i g = _mm256_srli_epi32(_mm256_add_epi32(
                _mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(p00, 8), v_ff), v_w00),
                                 _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(p10, 8), v_ff), v_w10)),
                _mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(p01, 8), v_ff), v_w01),
                                 _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(p11, 8), v_ff), v_w11))), 16);
            __m256i b = _mm256_srli_epi32(_mm256_add_epi32(
                _mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(p00, v_ff), v_w00),
                                 _mm256_mullo_epi32(_mm256_and_si256(p10, v_ff), v_w10)),
                _mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(p01, v_ff), v_w01),
                                 _mm256_mullo_epi32(_mm256_and_si256(p11, v_ff), v_w11))), 16);

            r = _mm256_min_epi32(r, v_ff);
            g = _mm256_min_epi32(g, v_ff);
            b = _mm256_min_epi32(b, v_ff);

            __m256i out = _mm256_or_si256(_mm256_and_si256(p00, v_alpha),
                          _mm256_or_si256(_mm256_slli_epi32(r, 16),
                          _mm256_or_si256(_mm256_slli_epi32(g, 8), b)));
            _mm256_storeu_si256((__m256i *)(dst_row + x), out);
        }
#endif

        for (; x < frame_w; x++) {
            int x0 = x + sx_int;
            int x1 = x0 + 1;
            if (x0 < 0) x0 = 0; else if (x0 >= frame_w) x0 = frame_w - 1;
            if (x1 < 0) x1 = 0; else if (x1 >= frame_w) x1 = frame_w - 1;

            uint32_t p00 = row0[x0];
            uint32_t p10 = row0[x1];
            uint32_t p01 = row1[x0];
            uint32_t p11 = row1[x1];

            int r = (((p00 >> 16) & 0xFF) * w00 +
                     ((p10 >> 16) & 0xFF) * w10 +
                     ((p01 >> 16) & 0xFF) * w01 +
                     ((p11 >> 16) & 0xFF) * w11) >> 16;

            int g = (((p00 >> 8) & 0xFF) * w00 +
                     ((p10 >> 8) & 0xFF) * w10 +
                     ((p01 >> 8) & 0xFF) * w01 +
                     ((p11 >> 8) & 0xFF) * w11) >> 16;

            int b = ((p00 & 0xFF) * w00 +
                     (p10 & 0xFF) * w10 +
                     (p01 & 0xFF) * w01 +
                     (p11 & 0xFF) * w11) >> 16;

            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;

            dst_row[x] = (p00 & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

/*
 * Render suppressed world OAM sprites at subpixel-interpolated positions (prev + (curr - prev) * alpha)
 * onto the world viewport (scanlines 0..191), excluding HUD/dialogue/menu sprites.
 */
static void RenderInterpolatedSprites(uint32_t *pixels, int stride, int frame_w, int frame_h,
                                      int extra, double alpha)
{
    if (!pixels || !g_snes || !g_snes->ppu || !s_oam_initialized) return;
    const Ppu *ppu = g_snes->ppu;
    if (PPU_forcedBlank(ppu)) return;
    int world_h = frame_h;
    if (world_h <= 0 || frame_w <= 0) return;

    /* Precompute OBJ palette RGB table (CGRAM 128..255) with current PPU brightness */
    uint32_t obj_pal[128];
    uint8_t bright = PPU_brightness(ppu);
    for (int i = 0; i < 128; i++) {
        uint16_t c = ppu->cgram[128 + i];
        uint8_t r = ppu_brightness_level((unsigned)(c & 0x1Fu), bright);
        uint8_t g = ppu_brightness_level((unsigned)((c >> 5) & 0x1Fu), bright);
        uint8_t b = ppu_brightness_level((unsigned)((c >> 10) & 0x1Fu), bright);
        obj_pal[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    int adr1 = PPU_objTileAdr1(ppu);
    int adr2 = PPU_objTileAdr2(ppu);
    bool in_gameplay = ((g_ram[0x0B29] & 0x03) == 0x03);
    bool cam_shifted = (in_gameplay && !s_lock_60fps && alpha >= 0.0 && alpha < 1.0 && s_cam_initialized);
    bool can_interp = s_interpolate_sprites && !s_lock_60fps && alpha >= 0.0 && alpha < 1.0;

    /* Match ApplySubpixelCameraScroll's BG shift so OAM sprites remain locked
     * to the subpixel camera grid even when scrolling horizontally/vertically. */
    double cam_res_shift_x = 0.0;
    double cam_res_shift_y = 0.0;
    double bg_cam_shift_x = 0.0;
    double bg_cam_shift_y = 0.0;
    if (cam_shifted) {
        int cam_dx = (int)s_cam_x_curr - (int)s_cam_x_prev;
        int cam_dy = (int)s_cam_y_curr - (int)s_cam_y_prev;
        if (abs(cam_dx) <= 16 && abs(cam_dy) <= 16) {
            cam_res_shift_x = -(s_cam_res_x_prev + (s_cam_res_x_curr - s_cam_res_x_prev) * alpha);
            cam_res_shift_y = -(s_cam_res_y_prev + (s_cam_res_y_curr - s_cam_res_y_prev) * alpha);
            double px_prev = (double)s_cam_x_prev + s_cam_res_x_prev;
            double px_curr = (double)s_cam_x_curr + s_cam_res_x_curr;
            double py_prev = (double)s_cam_y_prev + s_cam_res_y_prev;
            double py_curr = (double)s_cam_y_curr + s_cam_res_y_curr;
            bg_cam_shift_x = -((px_prev + (px_curr - px_prev) * alpha) - (double)s_cam_x_curr);
            bg_cam_shift_y = -((py_prev + (py_curr - py_prev) * alpha) - (double)s_cam_y_curr);
        }
    }

    /* Draw in reverse OAM order (127 -> 0) so lower OAM slots appear in front */
    for (int sl = 127; sl >= 0; sl--) {
        if (!(s_oam_suppressed_mask[sl >> 3] & (1u << (sl & 7)))) continue;

        const SoeOamSnap *cur = &s_oam_curr[sl];
        const SoeOamSnap *prv = &s_oam_prev[sl];
        if (!cur->active) continue;

        int frame_left = (extra > 0) ? (int)ppu->extraFrameLeft : 0;
        double draw_x = (double)cur->x + (double)frame_left + bg_cam_shift_x;
        double draw_y = (double)cur->y + bg_cam_shift_y;
        if (can_interp && prv->active && prv->size == cur->size) {
            int prev_fb_x = (int)prv->x + (int)s_frame_left_prev;
            int curr_fb_x = (int)cur->x + (int)s_frame_left_curr;
            int dx = curr_fb_x - prev_fb_x;
            int dy = (int)cur->y - (int)prv->y;
            int max_delta = (s_enhanced_ring_menu && ((prv->w1 & 0xCFFFu) == (cur->w1 & 0xCFFFu))) ? 28 : 12;
            if (abs(dx) <= max_delta && abs(dy) <= max_delta) {
                draw_x = (double)prev_fb_x + (double)dx * alpha + cam_res_shift_x;
                draw_y = (double)prv->y + (double)dy * alpha + cam_res_shift_y;
            }
        }

        int sz = (int)cur->size;
        uint16_t oam1 = cur->w1;
        int obj_adr = (oam1 & 0x100u) ? adr2 : adr1;
        int pal_off = 16 * ((int)(oam1 & 0xE00u) >> 9);
        bool hflip = (oam1 & 0x4000u) != 0;
        bool vflip = (oam1 & 0x8000u) != 0;

        /* Decode sprite texels (cached per 60 Hz sim frame & VRAM write count when Enhanced) */
        uint32_t tex_local[64 * 64];
        uint32_t *tex = tex_local;
        bool need_decode = true;
        if (s_enhanced_ring_menu) {
            SoeSpriteTexCache *cache = &s_sprite_tex_cache[sl];
            tex = cache->tex;
            if (cache->gen == s_oam_sim_gen &&
                cache->vram_writes == ppu->vramWriteCount &&
                cache->w1 == oam1 &&
                cache->obj_adr == obj_adr &&
                cache->size == (uint8_t)sz &&
                cache->bright == bright) {
                need_decode = false;
            } else {
                cache->gen = s_oam_sim_gen;
                cache->vram_writes = ppu->vramWriteCount;
                cache->w1 = oam1;
                cache->obj_adr = obj_adr;
                cache->size = (uint8_t)sz;
                cache->bright = bright;
            }
        }
        if (need_decode) {
            for (int row = 0; row < sz; row++) {
                int used_row = vflip ? (sz - 1 - row) : row;
                for (int col = 0; col < sz; col += 8) {
                    int used_col = hflip ? (sz - 1 - col) : col;
                    int used_tile = ((((oam1 & 0xFF) >> 4) + (used_row >> 3)) << 4) |
                                    (((oam1 & 0x0F) + (used_col >> 3)) & 0x0F);
                    const uint16_t *addr = &ppu->vram[(obj_adr + used_tile * 16 + (used_row & 7)) & 0x7FFF];
                    uint32_t plane = (uint32_t)addr[0] | ((uint32_t)addr[8] << 16);
                    for (int px = 0; px < 8; px++) {
                        int bit = hflip ? px : (7 - px);
                        int c = (int)(((plane >> bit) & 1u) |
                                      (((plane >> (8 + bit)) & 1u) << 1) |
                                      (((plane >> (16 + bit)) & 1u) << 2) |
                                      (((plane >> (24 + bit)) & 1u) << 3));
                        tex[row * sz + col + px] = (c == 0) ? 0u : obj_pal[pal_off + c];
                    }
                }
            }
        }

        int ix = (int)floor(draw_x);
        int iy = (int)floor(draw_y);
        double fx = draw_x - (double)ix;
        double fy = draw_y - (double)iy;
        uint8_t spr_prio_hi = (uint8_t)SPRITE_PRIO_TO_PRIO_HI((oam1 >> 12) & 3u);
        int z_w = (frame_w <= kPpuMaxFrameWidth) ? frame_w : kPpuMaxFrameWidth;
        int z_bias = 0;

        if (fabs(fx) < 0.001 && fabs(fy) < 0.001) {
            /* Exact integer alignment: crisp 1:1 copy */
            for (int row = 0; row < sz; row++) {
                int dy = iy + row;
                if (dy < 0 || dy >= world_h) continue;
                uint32_t *dst_row = pixels + dy * stride;
                const uint8_t *zrow = (dy < 240) ? ppu->wsMainPrioHi[dy] : NULL;
                for (int col = 0; col < sz; col++) {
                    int dx = ix + col;
                    if (dx < 0 || dx >= frame_w) continue;
                    int zx = dx + z_bias;
                    if (zrow && zx >= 0 && zx < z_w && zrow[zx] > spr_prio_hi) continue;
                    uint32_t p = tex[row * sz + col];
                    if (p != 0u) dst_row[dx] = p;
                }
            }
        } else {
            /* Subpixel bilinear coverage compositing */
            int frac_x = (int)(fx * 256.0);
            int frac_y = (int)(fy * 256.0);
            if (frac_x < 0) frac_x = 0; else if (frac_x > 256) frac_x = 256;
            if (frac_y < 0) frac_y = 0; else if (frac_y > 256) frac_y = 256;
            /* Sample at (ox - draw_x) = (ox - ix) - fx, so weights are (1-fx, fx) for (tx-1, tx) */
            int w00 = frac_x * frac_y;
            int w10 = (256 - frac_x) * frac_y;
            int w01 = frac_x * (256 - frac_y);
            int w11 = (256 - frac_x) * (256 - frac_y);

            for (int oy = 0; oy <= sz; oy++) {
                int dy = iy + oy;
                if (dy < 0 || dy >= world_h) continue;
                uint32_t *dst_row = pixels + dy * stride;
                const uint8_t *zrow = (dy < 240) ? ppu->wsMainPrioHi[dy] : NULL;
                int sy0 = oy - 1;
                int sy1 = oy;
                for (int ox = 0; ox <= sz; ox++) {
                    int dx = ix + ox;
                    if (dx < 0 || dx >= frame_w) continue;
                    int zx = dx + z_bias;
                    if (zrow && zx >= 0 && zx < z_w && zrow[zx] > spr_prio_hi) continue;
                    int sx0 = ox - 1;
                    int sx1 = ox;

                    uint32_t p00 = (sx0 >= 0 && sx0 < sz && sy0 >= 0 && sy0 < sz) ? tex[sy0 * sz + sx0] : 0u;
                    uint32_t p10 = (sx1 >= 0 && sx1 < sz && sy0 >= 0 && sy0 < sz) ? tex[sy0 * sz + sx1] : 0u;
                    uint32_t p01 = (sx0 >= 0 && sx0 < sz && sy1 >= 0 && sy1 < sz) ? tex[sy1 * sz + sx0] : 0u;
                    uint32_t p11 = (sx1 >= 0 && sx1 < sz && sy1 >= 0 && sy1 < sz) ? tex[sy1 * sz + sx1] : 0u;

                    int cov = ((p00 ? w00 : 0) + (p10 ? w10 : 0) + (p01 ? w01 : 0) + (p11 ? w11 : 0));
                    if (cov <= 0) continue;

                    if (cov >= 65536) {
                        int r = (((p00 >> 16) & 0xFF) * w00 + ((p10 >> 16) & 0xFF) * w10 +
                                 ((p01 >> 16) & 0xFF) * w01 + ((p11 >> 16) & 0xFF) * w11) >> 16;
                        int g = (((p00 >> 8) & 0xFF) * w00 + ((p10 >> 8) & 0xFF) * w10 +
                                 ((p01 >> 8) & 0xFF) * w01 + ((p11 >> 8) & 0xFF) * w11) >> 16;
                        int b = ((p00 & 0xFF) * w00 + (p10 & 0xFF) * w10 +
                                 (p01 & 0xFF) * w01 + (p11 & 0xFF) * w11) >> 16;
                        dst_row[dx] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                    } else {
                        uint32_t bg = dst_row[dx];
                        int inv = 65536 - cov;
                        int fr = ((p00 ? ((p00 >> 16) & 0xFF) * w00 : 0) +
                                  (p10 ? ((p10 >> 16) & 0xFF) * w10 : 0) +
                                  (p01 ? ((p01 >> 16) & 0xFF) * w01 : 0) +
                                  (p11 ? ((p11 >> 16) & 0xFF) * w11 : 0)) / cov;
                        int fg = ((p00 ? ((p00 >> 8) & 0xFF) * w00 : 0) +
                                  (p10 ? ((p10 >> 8) & 0xFF) * w10 : 0) +
                                  (p01 ? ((p01 >> 8) & 0xFF) * w01 : 0) +
                                  (p11 ? ((p11 >> 8) & 0xFF) * w11 : 0)) / cov;
                        int fb = ((p00 ? (p00 & 0xFF) * w00 : 0) +
                                  (p10 ? (p10 & 0xFF) * w10 : 0) +
                                  (p01 ? (p01 & 0xFF) * w01 : 0) +
                                  (p11 ? (p11 & 0xFF) * w11 : 0)) / cov;
                        int r = (fr * cov + (int)((bg >> 16) & 0xFF) * inv) >> 16;
                        int g = (fg * cov + (int)((bg >> 8) & 0xFF) * inv) >> 16;
                        int b = (fb * cov + (int)(bg & 0xFF) * inv) >> 16;
                        dst_row[dx] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                    }
                }
            }
        }
    }
}

/*
 * Present and compose the final output frame:
 * 1. Present PPU raster with optional subpixel camera interpolation.
 * 2. Apply CRT filter if enabled.
 * 3. Render the 10-second fade-away "Menu [F1/Pad]" button.
 * 4. Render the slide-out options menu using the game's bitmap font.
 * 5. Render the upper-right corner Debug Window if enabled.
 */
int SoeDrawFrame(uint8_t *dst, size_t pitch, const uint8_t *field,
                 int frame_w, int frame_h, double alpha)
{
    s_warp_native_ov_active = false;

    /* 1. Present the PPU field */
    RtlWidescreenPresent(dst, pitch, field, frame_w, frame_h);

    uint16_t current_map = GetLoadedMapId();
    bool map_pillarboxed = (s_map_pillarbox[current_map & 0xFF] == 0);
    int extra = SoeGetWidescreenExtra();
    bool in_gameplay = ((g_ram[0x0B29] & 0x03) == 0x03);

    /* Apply subpixel camera scroll interpolation across display presentation refreshes */
    if (in_gameplay && !s_lock_60fps && alpha >= 0.0 && alpha < 1.0 && s_cam_initialized) {
        int dx = (int)s_cam_x_curr - (int)s_cam_x_prev;
        int dy = (int)s_cam_y_curr - (int)s_cam_y_prev;
        if (abs(dx) <= 16 && abs(dy) <= 16) {
            double px_prev = (double)s_cam_x_prev + s_cam_res_x_prev;
            double px_curr = (double)s_cam_x_curr + s_cam_res_x_curr;
            double py_prev = (double)s_cam_y_prev + s_cam_res_y_prev;
            double py_curr = (double)s_cam_y_curr + s_cam_res_y_curr;
            double shift_x = (px_prev + (px_curr - px_prev) * alpha) - (double)s_cam_x_curr;
            double shift_y = (py_prev + (py_curr - py_prev) * alpha) - (double)s_cam_y_curr;
            if (fabs(shift_x) >= 0.001 || fabs(shift_y) >= 0.001) {
                ApplySubpixelCameraScroll((uint32_t *)dst, (int)(pitch / 4), frame_w, frame_h,
                                          shift_x, shift_y);
            }
        }
    }
    RenderInterpolatedSprites((uint32_t *)dst, (int)(pitch / 4), frame_w, frame_h, extra, alpha);
    bool menu_mosaic = (extra > 0 && g_snes && g_snes->ppu &&
                        IsMode4MenuMosaicActive(g_snes->ppu));

    /* Tile Mode 4 menu screens (Save Select + Level/Stat/Act/Edit/Alchemy) across margins */
    if (menu_mosaic) {
        DrawMode4MenuSideMosaic(g_snes->ppu, (uint32_t *)dst, (int)(pitch / 4),
                                frame_w, frame_h, extra);
    }

    /* 1.5. Pure-C Programmatic Dialog Box & Window Prefs Live Preview (before CRT & Bloom filters) */
    if (s_dialog_theme != DIALOG_THEME_ORIGINAL && s_dialog_capture_active) {
        SoeRenderProgrammaticDialogs((uint32_t *)dst, (int)(pitch / 4), frame_w, frame_h);
    }
    SoeRenderWindowPrefsOverlay((uint32_t *)dst, (int)(pitch / 4), frame_w, frame_h);

    /* 2. CRT filter effect */
    if (s_crt_preset != CRT_PRESET_OFF) {
        ApplyCrtFilter(dst, pitch, frame_w, frame_h,
                       s_crt_preset,
                       s_crt_density,
                       s_crt_opacity,
                       s_crt_thickness,
                       s_crt_mask,
                       s_crt_bloom,
                       s_crt_vignette);
    }

    /* 3. General Bloom filter effect */
    if (s_bloom_preset != BLOOM_PRESET_OFF && s_bloom_intensity != BLOOM_INTENSITY_OFF) {
        ApplyBloomFilter(dst, pitch, frame_w, frame_h,
                         s_bloom_preset,
                         s_bloom_intensity,
                         s_bloom_threshold,
                         s_bloom_radius,
                         s_bloom_blend,
                         s_bloom_tint);
    }

    /* Auto-hide mouse cursor after 3 seconds of inactivity */
    if (s_cursor_visible && s_last_mouse_move_ticks != 0 &&
        (SDL_GetTicks() - s_last_mouse_move_ticks) >= 3000u) {
        snesrecomp_sdl_show_cursor(false);
        s_cursor_visible = false;
    }

    /* Apply algorithmic pillarboxes or map boundary clamping over extra widescreen tiles */
    if (extra > 0) {
        int clamp_left = 0;
        int clamp_right = frame_w;

        if (!menu_mosaic) {
            if (s_pillarbox_enabled && map_pillarboxed) {
                clamp_left = extra + 1;
                clamp_right = extra + 256 - 1;
            } else if (g_snes && g_snes->ppu) {
                Ppu *ppu = g_snes->ppu;
                int frame_left = (int)ppu->extraFrameLeft;
                clamp_left = frame_left - (int)ppu->extraLeftCur;
                clamp_right = frame_left + 256 + (int)ppu->extraRightCur;
            } else {
                clamp_left = extra - s_cur_left_space;
                clamp_right = extra + 256 + s_cur_right_space;
            }
        }

        if (clamp_left > 0 || clamp_right < frame_w) {
            /* Embedded black bar graphic (128px wide, compiled into binary) used for both sides */
            enum { SOE_PILLARBOX_BAR_W = 128 };
            static uint32_t s_pillarbox_bar_gfx[SOE_PILLARBOX_BAR_W];
            static bool s_pillarbox_bar_init = false;
            if (!s_pillarbox_bar_init) {
                for (int i = 0; i < SOE_PILLARBOX_BAR_W; i++) {
                    s_pillarbox_bar_gfx[i] = 0xFF000000u;
                }
                s_pillarbox_bar_init = true;
            }

            int left_w = clamp_left;
            if (left_w > frame_w) left_w = frame_w;
            if (left_w > SOE_PILLARBOX_BAR_W) left_w = SOE_PILLARBOX_BAR_W;

            int right_x = (clamp_right > 0 ? clamp_right : 0);
            int right_w = frame_w - right_x;
            if (right_w > SOE_PILLARBOX_BAR_W) right_w = SOE_PILLARBOX_BAR_W;

            bool hud_vis = ((g_ram[0x0B29] & 0x03) != 0);
            bool is_mode1_16x16 = (g_snes && g_snes->ppu) &&
                                  ((g_snes->ppu->bgmode & 0x07) == 1) &&
                                  ((g_snes->ppu->bgmode & 0x30) == 0x30);
            bool has_win = (g_snes && g_snes->ppu) &&
                           ((g_snes->ppu->screenWindowed[0] != 0) || (g_snes->ppu->screenWindowed[1] != 0));
            int clamp_h = (!menu_mosaic && hud_vis && is_mode1_16x16 && !has_win && s_frame_counter >= 60)
                              ? 192
                              : frame_h;
            if (clamp_h > frame_h) clamp_h = frame_h;

            for (int y = 0; y < clamp_h; y++) {
                uint32_t *row = (uint32_t *)dst + y * (int)(pitch / 4);
                if (left_w > 0) {
                    memcpy(row, s_pillarbox_bar_gfx, (size_t)left_w * sizeof(uint32_t));
                }
                if (right_w > 0) {
                    memcpy(row + right_x, s_pillarbox_bar_gfx, (size_t)right_w * sizeof(uint32_t));
                }
            }
        }
    }

    /* 3. Drawer slide animation: 20 px per frame transition (25% faster) */
    float menu_w = GetMenuWidth();
    float target_x = s_menu_open ? 0.0f : -menu_w;
    if (s_anim_x < target_x) {
        s_anim_x += 20.0f;
        if (s_anim_x > target_x) s_anim_x = target_x;
    } else if (s_anim_x > target_x) {
        s_anim_x -= 20.0f;
        if (s_anim_x < target_x) s_anim_x = target_x;
    }

    uint32_t *pixels = (uint32_t *)dst;
    int stride = (int)(pitch / 4);
    SoeUiThemeColors tc;
    GetUiThemeColors(&tc);

    /* 4. Render Slide-Out Menu Drawer (Dedicated Smooth High-Def Frosted Glass) */
    if (s_anim_x > -menu_w) {
        int drawer_x = (int)s_anim_x;
        int drawer_w = (int)menu_w;
        int rx0 = (drawer_x < 0) ? 0 : drawer_x;
        int rx1 = drawer_x + drawer_w;
        if (rx1 > frame_w) rx1 = frame_w;

        if (rx1 > rx0) {
            /* 3-pass cascaded Gaussian blur for a smooth, deep frosted glass backdrop */
            BlurRegionGaussian(pixels, stride, frame_w, frame_h, rx0, 0, rx1, frame_h);
            BlurRegionGaussian(pixels, stride, frame_w, frame_h, rx0, 0, rx1, frame_h);
            BlurRegionGaussian(pixels, stride, frame_w, frame_h, rx0, 0, rx1, frame_h);

            int p_idx = (int)s_dialog_theme;
            if (p_idx < 0 || p_idx >= DIALOG_GLASS_COUNT) p_idx = 0;
            const SoeGlassPalette *pal = &kGlassPalettes[p_idx];

            float top_r = (float)pal->top_r;
            float top_g = (float)pal->top_g;
            float top_b = (float)pal->top_b;
            float bot_r = (float)pal->bot_r;
            float bot_g = (float)pal->bot_g;
            float bot_b = (float)pal->bot_b;
            float tint_alpha = 0.80f;

            for (int y = 0; y < frame_h; y++) {
                float t = (frame_h > 1) ? ((float)y / (float)(frame_h - 1)) : 0.0f;
                /* Smooth ease curve + subtle top specular highlight, zero noise/dithering */
                float sheen = (y < 32) ? ((1.0f - (float)y / 32.0f) * 14.0f) : 0.0f;
                float tr = top_r + (bot_r - top_r) * t + sheen;
                float tg = top_g + (bot_g - top_g) * t + sheen;
                float tb = top_b + (bot_b - top_b) * t + sheen;
                uint32_t *row = pixels + y * stride;
                for (int x = rx0; x < rx1; x++) {
                    uint32_t bg = row[x];
                    float br = (float)((bg >> 16) & 0xFF);
                    float bgc = (float)((bg >> 8) & 0xFF);
                    float bb = (float)(bg & 0xFF);
                    int r = (int)(br * (1.0f - tint_alpha) + tr * tint_alpha);
                    int g = (int)(bgc * (1.0f - tint_alpha) + tg * tint_alpha);
                    int b = (int)(bb * (1.0f - tint_alpha) + tb * tint_alpha);
                    if (r > 255) r = 255; else if (r < 0) r = 0;
                    if (g > 255) g = 255; else if (g < 0) g = 0;
                    if (b > 255) b = 255; else if (b < 0) b = 0;
                    row[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                }
            }
        }

        /* Vertical accent right edge line */
        DrawSemiTransparentRect(pixels, stride, frame_h,
                                drawer_x + drawer_w - 2, 0, 2, frame_h,
                                tc.accent_r, tc.accent_g, tc.accent_b, 240);

        /* Title Banner */
        DrawSmallText(pixels, stride, frame_h, drawer_x + 10, 5, "SETTINGS", 0xFFFFFFFF);
        DrawSmallText(pixels, stride, frame_h, drawer_x + drawer_w - 85, 5, "LB/RB: Tab", tc.subtext_col);

        /* Top divider */
        DrawSemiTransparentRect(pixels, stride, frame_h,
                                drawer_x + 6, 15, drawer_w - 12, 1,
                                tc.div_r, tc.div_g, tc.div_b, 190);

        /* ── Tab Bar (2 rows of 4 tabs: y: 17..28, 30..41) ──────────────── */
        static const char *const kTabNames[TAB_COUNT] = {
            "Game", "Video", "Audio", "Fixes", "CRT", "Bloom", "Input", "Help"
        };
        int tab_bar_x = drawer_x + 6;
        int tab_bar_w = drawer_w - 12;
        int tab_w = tab_bar_w / 4;

        for (int t = 0; t < TAB_COUNT; t++) {
            int row_idx = t / 4;
            int col_idx = t % 4;
            int tx = tab_bar_x + col_idx * tab_w;
            int tw = (col_idx == 3) ? (tab_bar_x + tab_bar_w - tx) : (tab_w - 2);
            int ty = 17 + row_idx * 13;
            bool is_active = (t == (int)s_active_tab);

            DrawThemedHighlightRect(pixels, stride, frame_h, tx, ty, tw, 11, &tc, is_active);

            int name_w = MeasureSmallText(kTabNames[t]);
            int text_x = tx + (tw - name_w) / 2;
            DrawSmallText(pixels, stride, frame_h, text_x, ty + 2, kTabNames[t],
                          is_active ? 0xFFFFFFFF : tc.subtext_col);
        }

        /* Divider below Tab Bar */
        DrawSemiTransparentRect(pixels, stride, frame_h,
                                drawer_x + 6, 43, drawer_w - 12, 1,
                                tc.div_r, tc.div_g, tc.div_b, 190);

        /* ── Tab Page Content Area (y: 35..176) ─────────────────────────── */
        int start_y = 36;
        int row_h = 16;
        GetTabRowLayout(s_active_tab, &start_y, &row_h);
        int content_x = drawer_x + 10;
        int content_w = drawer_w - 20;
        int sel_row = s_tab_selected_row[s_active_tab];

        if (s_active_tab == TAB_GAME_OPTIONS) {
            /* Tab 0: Game Options with section headers & dividers */
            static const struct {
                int y;
                const char *title;
            } kGameSections[3] = {
                { 46, "SAVE STATE" },
                { 83, "LOAD STATE" },
                { 120, "GAME SETTINGS" },
            };
            for (int s = 0; s < 3; s++) {
                int hy = kGameSections[s].y;
                const char *ht = kGameSections[s].title;
                int htw = MeasureSmallText(ht);
                DrawSmallText(pixels, stride, frame_h, content_x + 2, hy, ht, tc.subtext_col);
                int line_x = content_x + 2 + htw + 6;
                int line_w = (content_x + content_w - 4) - line_x;
                if (line_w > 0) {
                    DrawSemiTransparentRect(pixels, stride, frame_h,
                                            line_x, hy + 3, line_w, 1,
                                            tc.div_r, tc.div_g, tc.div_b, 185);
                }
            }
#ifdef __ANDROID__
            for (int i = 0; i < GAME_OPT_EXIT_TO_DESKTOP; i++) {
#else
            for (int i = 0; i < GAME_OPT_EXIT_TO_LAUNCHER; i++) {
#endif
                int row_y = GetGameTabRowY(i);
                bool is_sel = (i == sel_row);
                int box_y = row_y - 1;
                int box_h = 10;
                int text_y = row_y;
                if (is_sel) {
                    DrawThemedHighlightRect(pixels, stride, frame_h,
                                            content_x, box_y, content_w, box_h, &tc, true);
                    DrawSmallText(pixels, stride, frame_h, content_x + 3, text_y, ">", 0xFFFFFFFF);
                }

                char label_buf[64];
                char val_buf[64];
                label_buf[0] = '\0';
                val_buf[0] = '\0';

                switch (i) {
                case GAME_OPT_SAVE_STATE_1:
                case GAME_OPT_SAVE_STATE_2:
                case GAME_OPT_SAVE_STATE_3: {
                    int slot = i - GAME_OPT_SAVE_STATE_1;
                    snprintf(label_buf, sizeof(label_buf), "Save State Slot %d", slot + 1);
                    snprintf(val_buf, sizeof(val_buf), "%s", SoeStateSlotExists(slot) ? "[Saved]" : "[Empty]");
                    break;
                }
                case GAME_OPT_LOAD_STATE_1:
                case GAME_OPT_LOAD_STATE_2:
                case GAME_OPT_LOAD_STATE_3: {
                    int slot = i - GAME_OPT_LOAD_STATE_1;
                    snprintf(label_buf, sizeof(label_buf), "Load State Slot %d", slot + 1);
                    snprintf(val_buf, sizeof(val_buf), "%s", SoeStateSlotExists(slot) ? "[Ready]" : "[Empty]");
                    break;
                }
                case GAME_OPT_DIALOG_THEME:
                    snprintf(label_buf, sizeof(label_buf), "Dialog Box Theme");
                    snprintf(val_buf, sizeof(val_buf), "%s", kDialogThemeNames[s_dialog_theme]);
                    break;
                case GAME_OPT_RING_MENU:
                    snprintf(label_buf, sizeof(label_buf), "Ring Menu");
                    snprintf(val_buf, sizeof(val_buf), "%s", s_enhanced_ring_menu ? "Enhanced" : "Original");
                    break;
#ifdef __ANDROID__
                case GAME_OPT_TOUCH_OVERLAY:
                    snprintf(label_buf, sizeof(label_buf), "Touch Overlay");
                    snprintf(val_buf, sizeof(val_buf), "%s", s_android_touch_overlay ? "On" : "Off");
                    break;
#endif
                case GAME_OPT_RESET_ALL:
                    snprintf(label_buf, sizeof(label_buf), "[ Reset All Settings to Default ]");
                    break;
                }

                if (i == GAME_OPT_RESET_ALL) {
                    int tw = MeasureSmallText(label_buf);
                    int cx = content_x + (content_w - tw) / 2;
                    DrawSmallText(pixels, stride, frame_h, cx, text_y, label_buf,
                                  is_sel ? 0xFFFFB454u : 0xFFE89838u);
                } else {
                    DrawSmallText(pixels, stride, frame_h, content_x + 10, text_y, label_buf, is_sel ? tc.sel_text_col : 0xFFFFFFFF);
                    if (val_buf[0]) {
                        int vw = MeasureSmallText(val_buf);
                        DrawSmallText(pixels, stride, frame_h, content_x + content_w - vw - 8, text_y, val_buf,
                                      is_sel ? 0xFFFFFFFF : tc.subtext_col);
                    }
                }
            }

            /* Centered single-line Exit button(s) */
            {
#ifdef __ANDROID__
                int exit_y = GetGameTabRowY(GAME_OPT_EXIT_TO_DESKTOP);
                int lx = 0, lw = 0, dx = 0, dw = 0;
                GetGameExitButtonBounds(drawer_x, drawer_w, &lx, &lw, &dx, &dw);
                bool sel_d = (sel_row == GAME_OPT_EXIT_TO_DESKTOP);
                if (sel_d) {
                    DrawThemedHighlightRect(pixels, stride, frame_h, dx - 3, exit_y, dw + 6, 9, &tc, true);
                }
                DrawSmallText(pixels, stride, frame_h, dx, exit_y, "[ Exit to Android Launcher ]",
                              sel_d ? 0xFFFF7878u : 0xFFE85454u);
#else
                int exit_y = GetGameTabRowY(GAME_OPT_EXIT_TO_LAUNCHER);
                int lx = 0, lw = 0, dx = 0, dw = 0;
                GetGameExitButtonBounds(drawer_x, drawer_w, &lx, &lw, &dx, &dw);
                bool sel_l = (sel_row == GAME_OPT_EXIT_TO_LAUNCHER);
                bool sel_d = (sel_row == GAME_OPT_EXIT_TO_DESKTOP);
                if (sel_l) {
                    DrawThemedHighlightRect(pixels, stride, frame_h, lx - 3, exit_y, lw + 6, 9, &tc, true);
                }
                if (sel_d) {
                    DrawThemedHighlightRect(pixels, stride, frame_h, dx - 3, exit_y, dw + 6, 9, &tc, true);
                }
                DrawSmallText(pixels, stride, frame_h, lx, exit_y, "[ Exit to Launcher ]",
                              sel_l ? 0xFF70B8FFu : 0xFF4C94E8u);
                DrawSmallText(pixels, stride, frame_h, dx, exit_y, "[ Exit to Desktop ]",
                              sel_d ? 0xFFFF7878u : 0xFFE85454u);
#endif
            }

            /* Live dynamic description of highlighted option */
            static const char *const kGameDescs[GAME_OPT_COUNT] = {
                "* Save full machine snapshot to Slot 1 (evermore.state1).",
                "* Save full machine snapshot to Slot 2 (evermore.state2).",
                "* Save full machine snapshot to Slot 3 (evermore.state3).",
                "* Restore full machine snapshot from Slot 1.",
                "* Restore full machine snapshot from Slot 2.",
                "* Restore full machine snapshot from Slot 3.",
                "* Glass theme color (Omnitopia, Volcano, etc.) or Original.",
                "* Ring Menu: Enhanced (smooth rotation & sprite cache) or Original.",
                "* Restores all Game, Video, CRT & Bloom settings.",
#ifdef __ANDROID__
                "* Save all settings and SRAM, then exit to Android launcher."
#else
                "* Save all settings and SRAM, then return to launcher.",
                "* Save all settings and SRAM, then exit to desktop."
#endif
            };
            const char *desc = (sel_row >= 0 && sel_row < GAME_OPT_COUNT) ? kGameDescs[sel_row] : "";
            if (s_menu_toast_timer > 0) {
                s_menu_toast_timer--;
                int tw = MeasureSmallText(s_menu_toast);
                DrawSmallText(pixels, stride, frame_h, content_x + (content_w - tw) / 2, 189,
                              s_menu_toast, 0xFF90FFB8);
            } else {
                DrawSmallText(pixels, stride, frame_h, content_x + 10, 189, desc, tc.subtext_col);
            }
        }
        else if (s_active_tab == TAB_FIXES) {
            /* Tab 3: Gameplay Bugfixes */
            int htw = MeasureSmallText("GAMEPLAY BUGFIXES");
            DrawSmallText(pixels, stride, frame_h, content_x + 2, 46, "GAMEPLAY BUGFIXES", tc.subtext_col);
            int line_x = content_x + 2 + htw + 6;
            int line_w = (content_x + content_w - 4) - line_x;
            if (line_w > 0) {
                DrawSemiTransparentRect(pixels, stride, frame_h,
                                        line_x, 49, line_w, 1,
                                        tc.div_r, tc.div_g, tc.div_b, 185);
            }
            int fix_start_y = start_y;
            int fix_row_h = row_h;
            for (int i = 0; i < FIX_OPT_COUNT; i++) {
                int row_y = fix_start_y + i * fix_row_h;
                bool is_sel = (i == sel_row);
                if (is_sel) {
                    DrawThemedHighlightRect(pixels, stride, frame_h,
                                            content_x, row_y - 1, content_w, 11, &tc, true);
                    DrawSmallText(pixels, stride, frame_h, content_x + 2, row_y + 1, ">", 0xFFFFFFFF);
                }

                const char *label = "";
                bool enabled = false;
                switch (i) {
                case FIX_OPT_SHEATH:
                    label = "Silver Sheath Attack Boost Fix";
                    enabled = s_fix_silver_sheath;
                    break;
                case FIX_OPT_ATLAS:
                    label = "Atlas / Defend Stat Underflow Fix";
                    enabled = s_fix_atlas_underflow;
                    break;
                case FIX_OPT_DOG_LVL:
                    label = "Dog Level 39+ Progression Fix";
                    enabled = s_fix_dog_leveling;
                    break;
                case FIX_OPT_SOFTLOCK:
                    label = "Boundary Softlock Prevention";
                    enabled = s_fix_softlocks;
                    break;
                }

                DrawSmallText(pixels, stride, frame_h, content_x + 10, row_y + 1, label,
                              is_sel ? tc.sel_text_col : 0xFFFFFFFF);
                const char *val_str = enabled ? "Enabled" : "Disabled";
                int vw = MeasureSmallText(val_str);
                DrawSmallText(pixels, stride, frame_h, content_x + content_w - vw - 8, row_y + 1, val_str,
                              is_sel ? 0xFFFFFFFF : tc.subtext_col);
            }

            static const char *const kFixDescs[FIX_OPT_COUNT] = {
                "* Fix: Silver Sheath correctly boosts sword attack.",
                "* Fix: Prevents Defend/Atlas buffs wrapping stats.",
                "* Fix: Fixes dog EXP formula past Lv 39 up to 99.",
                "* Fix: Prevents player clipping walls on map load."
            };
            const char *desc = (sel_row >= 0 && sel_row < FIX_OPT_COUNT) ? kFixDescs[sel_row] : "";
            DrawSmallText(pixels, stride, frame_h, content_x + 10, 189, desc, tc.subtext_col);
        }
        else if (s_active_tab == TAB_VIDEO_OPTIONS) {
            /* Tab 1: A/V Options (scrollable 12-row window) */
            if (sel_row < s_av_scroll) s_av_scroll = sel_row;
            if (sel_row >= s_av_scroll + kAvVisibleRows) s_av_scroll = sel_row - kAvVisibleRows + 1;
            int max_av_scroll = VIDEO_OPT_COUNT - kAvVisibleRows;
            if (max_av_scroll < 0) max_av_scroll = 0;
            if (s_av_scroll < 0) s_av_scroll = 0;
            if (s_av_scroll > max_av_scroll) s_av_scroll = max_av_scroll;

            for (int r = 0; r < kAvVisibleRows; r++) {
                int i = s_av_scroll + r;
                if (i >= VIDEO_OPT_COUNT) break;
                int row_y = start_y + r * row_h;
                bool is_sel = (i == sel_row);
                int box_y = (row_h <= 11) ? row_y : (row_y - 1);
                int box_h = (row_h <= 11) ? 11 : (row_h - 1);
                int text_y = (row_h <= 11) ? (row_y + 1) : (row_y + 2);

                if (is_sel) {
                    DrawThemedHighlightRect(pixels, stride, frame_h,
                                            content_x, box_y, content_w, box_h, &tc, true);
                    DrawSmallText(pixels, stride, frame_h, content_x + 2, text_y, ">", 0xFFFFFFFF);
                }

                char label_buf[64];
                char val_buf[64];
                label_buf[0] = '\0';
                val_buf[0] = '\0';

                switch (i) {
                case VIDEO_OPT_WIDESCREEN:
                    snprintf(label_buf, sizeof(label_buf), "Widescreen Aspect Ratio");
                    if (s_ws_mode == SOE_WS_16_9) snprintf(val_buf, sizeof(val_buf), "16:9 Widescreen");
                    else if (s_ws_mode == SOE_WS_21_9) snprintf(val_buf, sizeof(val_buf), "21:9 Ultrawide");
                    else snprintf(val_buf, sizeof(val_buf), "Off (4:3 Standard)");
                    break;
                case VIDEO_OPT_HUD_POSITION:
                    snprintf(label_buf, sizeof(label_buf), "Status Bar HUD Position");
                    snprintf(val_buf, sizeof(val_buf), "%s",
                             s_hud_mode == SOE_HUD_MOVE_LEFT ? "Move Left Margin" : "Original 4:3 Center");
                    break;
                case VIDEO_OPT_PILLARBOX:
                    snprintf(label_buf, sizeof(label_buf), "Per-Map Pillarbox Clamping");
                    snprintf(val_buf, sizeof(val_buf), "%s", s_pillarbox_enabled ? "Enabled" : "Disabled");
                    break;
                case VIDEO_OPT_SOLID_DIALOG:
                    snprintf(label_buf, sizeof(label_buf), "Dialog Box Theme");
                    snprintf(val_buf, sizeof(val_buf), "%s", kDialogThemeNames[s_dialog_theme]);
                    break;
                case VIDEO_OPT_ASPECT_RATIO:
                    snprintf(label_buf, sizeof(label_buf), "Display Aspect Mode");
                    if (g_config.display_aspect == 1) snprintf(val_buf, sizeof(val_buf), "8:7 Square");
                    else if (g_config.display_aspect == 2) snprintf(val_buf, sizeof(val_buf), "1:1 Pixel");
                    else snprintf(val_buf, sizeof(val_buf), "4:3 CRT TV");
                    break;
                case VIDEO_OPT_WINDOW_SCALE: {
                    snprintf(label_buf, sizeof(label_buf), "Window Scaling");
                    int cur_scale = snesrecomp_get_window_scale();
                    if (g_config.fullscreen) snprintf(val_buf, sizeof(val_buf), "Fullscreen");
                    else snprintf(val_buf, sizeof(val_buf), "%dx Native", cur_scale > 0 ? cur_scale : 4);
                    break;
                }
                case VIDEO_OPT_VSYNC: {
                    snprintf(label_buf, sizeof(label_buf), "VSync");
                    int vs = snesrecomp_get_vsync();
                    snprintf(val_buf, sizeof(val_buf), "%s",
                             vs == 2 ? "Adaptive" : (vs == 0 ? "Off" : "On"));
                    break;
                }
                case VIDEO_OPT_TRIPLE_BUFFER:
                    snprintf(label_buf, sizeof(label_buf), "Triple Buffering");
                    snprintf(val_buf, sizeof(val_buf), "%s",
                             snesrecomp_get_triple_buffering() ? "ON" : "OFF");
                    break;
                case VIDEO_OPT_CRT_SCANLINES:
                    snprintf(label_buf, sizeof(label_buf), "CRT Emulation Preset");
                    snprintf(val_buf, sizeof(val_buf), "%s", kCrtPresetNames[s_crt_preset]);
                    break;
                case VIDEO_OPT_SPRITE_LIMITS:
                    snprintf(label_buf, sizeof(label_buf), "Remove Sprite Flicker Limits");
                    snprintf(val_buf, sizeof(val_buf), "%s",
                             g_config.no_sprite_limits ? "ON (No Flicker)" : "OFF (Standard 32)");
                    break;
                case VIDEO_OPT_LOCK_60FPS:
                    snprintf(label_buf, sizeof(label_buf), "Framerate & Pacing");
                    snprintf(val_buf, sizeof(val_buf), "%s",
                             s_lock_60fps ? "Locked (60 FPS)" : "High-Hz Subpixel Scroll");
                    break;
                case VIDEO_OPT_INTERPOLATE_SPRITES:
                    snprintf(label_buf, sizeof(label_buf), "Subpixel Sprite Interpolation");
                    snprintf(val_buf, sizeof(val_buf), "%s",
                             s_interpolate_sprites ? "ON (Subpixel)" : "OFF (60 Hz Snap)");
                    break;
                case VIDEO_OPT_ELIMINATE_LAG:
                    snprintf(label_buf, sizeof(label_buf), "Eliminate Lag Frames (Fast CPU/DMA)");
                    snprintf(val_buf, sizeof(val_buf), "%s",
                             s_eliminate_lag_frames ? "ON (No Slowdown)" : "OFF (Original SNES)");
                    break;
                case VIDEO_OPT_RESET:
                    snprintf(label_buf, sizeof(label_buf), "[ Reset Video Settings to Default ]");
                    break;
                }

                if (i == VIDEO_OPT_RESET) {
                    int tw = MeasureSmallText(label_buf);
                    int cx = content_x + (content_w - tw) / 2;
                    DrawSmallText(pixels, stride, frame_h, cx, text_y, label_buf,
                                  is_sel ? 0xFFFFB454u : 0xFFE89838u);
                } else {
                    DrawSmallText(pixels, stride, frame_h, content_x + 10, text_y, label_buf, is_sel ? tc.sel_text_col : 0xFFFFFFFF);
                    if (val_buf[0]) {
                        int vw = MeasureSmallText(val_buf);
                        DrawSmallText(pixels, stride, frame_h, content_x + content_w - vw - 8, text_y, val_buf,
                                      is_sel ? 0xFFFFFFFF : tc.subtext_col);
                    }
                }
            }

            if (s_av_scroll > 0) {
                int ax = content_x + content_w - 6;
                int ay = start_y + 2;
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 2, ay,     1, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 1, ay + 1, 3, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax,     ay + 2, 5, 1, 255, 255, 144, 240);
            }
            if (s_av_scroll + kAvVisibleRows < VIDEO_OPT_COUNT) {
                int ax = content_x + content_w - 6;
                int ay = start_y + (kAvVisibleRows - 1) * row_h + 2;
                DrawSemiTransparentRect(pixels, stride, frame_h, ax,     ay,     5, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 1, ay + 1, 3, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 2, ay + 2, 1, 1, 255, 255, 144, 240);
            }

            /* Live dynamic description of highlighted option */
            static const char *const kVideoDescs[VIDEO_OPT_COUNT] = {
                "* Expands field of view to 16:9 or 21:9 ultrawide.",
                "* Aligns HUD meters to left margin or 4:3 center.",
                "* Clamps pillarbox on maps with edge distortion.",
                "* Renders dialogue boxes as solid glass blue or dither.",
                "* Switches between 4:3 CRT TV, 8:7 square & 1:1.",
                "* Adjusts native window scale multiplier or fullscreen.",
                "* Syncs presentation to display refresh (Off/On/Adaptive).",
                "* 3-buffer presentation ring across all render modes.",
                "* Quick CRT preset. Fine-tune details in the CRT tab.",
                "* Removes SNES 32 sprites-per-scanline flicker limit.",
                "* Unlocks high-refresh (100 Hz+) smooth subpixel camera scrolling.",
                "* Subpixel sprite interpolation for smooth high-Hz entity motion.",
                "* Runs 65816 CPU & DMA 4x faster between VBlanks to eliminate lag.",
                "* Restores Video, Aspect, Scale & CRT to defaults."
            };
            const char *desc = (sel_row >= 0 && sel_row < VIDEO_OPT_COUNT) ? kVideoDescs[sel_row] : "";
            if (s_menu_toast_timer > 0) {
                s_menu_toast_timer--;
                int tw = MeasureSmallText(s_menu_toast);
                DrawSmallText(pixels, stride, frame_h, content_x + (content_w - tw) / 2, 189,
                              s_menu_toast, 0xFF90FFB8);
            } else {
                DrawSmallText(pixels, stride, frame_h, content_x + 10, 189, desc, tc.subtext_col);
            }
        }
        else if (s_active_tab == TAB_AUDIO_OPTIONS) {
            /* Tab 2: Audio Settings */
            for (int i = 0; i < AUDIO_OPT_COUNT; i++) {
                int row_y = start_y + i * row_h;
                bool is_sel = (i == sel_row);

                if (is_sel) {
                    DrawThemedHighlightRect(pixels, stride, frame_h,
                                            content_x + 4, row_y, content_w - 8, row_h - 1, &tc, true);
                }

                int text_y = row_y + 2;
                char label_buf[64] = "";
                char val_buf[48] = "";

                switch (i) {
                case AUDIO_OPT_ENABLE:
                    snprintf(label_buf, sizeof(label_buf), "Audio Output");
                    snprintf(val_buf, sizeof(val_buf), "%s",
                             g_config.enable_audio ? "ON" : "OFF");
                    break;
                case AUDIO_OPT_VOLUME:
                    snprintf(label_buf, sizeof(label_buf), "Master Volume");
                    snprintf(val_buf, sizeof(val_buf), "%d%%", g_config.volume);
                    break;
                case AUDIO_OPT_FREQ:
                    snprintf(label_buf, sizeof(label_buf), "Audio Sample Rate");
                    snprintf(val_buf, sizeof(val_buf), "%u Hz", (unsigned)g_config.audio_freq);
                    break;
                case AUDIO_OPT_SAMPLES:
                    snprintf(label_buf, sizeof(label_buf), "Audio Buffer Size");
                    snprintf(val_buf, sizeof(val_buf), "%u samples", (unsigned)g_config.audio_samples);
                    break;
                case AUDIO_OPT_RESET:
                    snprintf(label_buf, sizeof(label_buf), "[ Reset Audio Settings to Default ]");
                    break;
                }

                if (i == AUDIO_OPT_RESET) {
                    int tw = MeasureSmallText(label_buf);
                    int cx = content_x + (content_w - tw) / 2;
                    DrawSmallText(pixels, stride, frame_h, cx, text_y, label_buf,
                                  is_sel ? 0xFFFFB454u : 0xFFE89838u);
                } else {
                    DrawSmallText(pixels, stride, frame_h, content_x + 10, text_y, label_buf,
                                  is_sel ? tc.sel_text_col : 0xFFFFFFFF);
                    if (val_buf[0]) {
                        int vw = MeasureSmallText(val_buf);
                        DrawSmallText(pixels, stride, frame_h, content_x + content_w - vw - 8, text_y, val_buf,
                                      is_sel ? 0xFFFFFFFF : tc.subtext_col);
                    }
                }
            }

            static const char *const kAudioDescs[AUDIO_OPT_COUNT] = {
                "* Enables or mutes host audio output.",
                "* Adjusts master mixer volume (0% to 100%).",
                "* Sets audio output sample rate (22050-48000 Hz).",
                "* Sets audio buffer size (256-2048 samples).",
                "* Restores Audio settings to defaults."
            };
            const char *desc = (sel_row >= 0 && sel_row < AUDIO_OPT_COUNT) ? kAudioDescs[sel_row] : "";
            if (s_menu_toast_timer > 0) {
                s_menu_toast_timer--;
                int tw = MeasureSmallText(s_menu_toast);
                DrawSmallText(pixels, stride, frame_h, content_x + (content_w - tw) / 2, 189,
                              s_menu_toast, 0xFF90FFB8);
            } else {
                DrawSmallText(pixels, stride, frame_h, content_x + 10, 189, desc, tc.subtext_col);
            }
        }
        else if (s_active_tab == TAB_CRT_OPTIONS) {
            /* Tab 2: CRT Filter Options */
            for (int i = 0; i < CRT_OPT_COUNT; i++) {
                int row_y = start_y + i * row_h;
                bool is_sel = (i == sel_row);

                if (is_sel) {
                    DrawThemedHighlightRect(pixels, stride, frame_h,
                                            content_x, row_y - 1, content_w, 14, &tc, true);
                    DrawSmallText(pixels, stride, frame_h, content_x + 2, row_y + 2, ">", 0xFFFFFFFF);
                }

                char label_buf[64];
                char val_buf[64];
                label_buf[0] = '\0';
                val_buf[0] = '\0';

                switch (i) {
                case CRT_OPT_PRESET:
                    snprintf(label_buf, sizeof(label_buf), "CRT Emulation Preset");
                    snprintf(val_buf, sizeof(val_buf), "%s", kCrtPresetNames[s_crt_preset]);
                    break;
                case CRT_OPT_JUST_SCANLINES:
                    snprintf(label_buf, sizeof(label_buf), "[ Just Scanlines (Disable Masks) ]");
                    break;
                case CRT_OPT_DENSITY:
                    snprintf(label_buf, sizeof(label_buf), "Scanline Spacing / Density");
                    snprintf(val_buf, sizeof(val_buf), "%s", kCrtDensityNames[s_crt_density]);
                    break;
                case CRT_OPT_OPACITY:
                    snprintf(label_buf, sizeof(label_buf), "Scanline Opacity / Strength");
                    snprintf(val_buf, sizeof(val_buf), "%s", kCrtOpacityNames[s_crt_opacity]);
                    break;
                case CRT_OPT_THICKNESS:
                    snprintf(label_buf, sizeof(label_buf), "Scanline Beam Thickness");
                    snprintf(val_buf, sizeof(val_buf), "%s", kCrtThicknessNames[s_crt_thickness]);
                    break;
                case CRT_OPT_MASK:
                    snprintf(label_buf, sizeof(label_buf), "Phosphor Shadow Mask");
                    snprintf(val_buf, sizeof(val_buf), "%s", kCrtMaskNames[s_crt_mask]);
                    break;
                case CRT_OPT_BLOOM:
                    snprintf(label_buf, sizeof(label_buf), "Luminance / Brightness Boost");
                    snprintf(val_buf, sizeof(val_buf), "%s", kCrtBloomNames[s_crt_bloom]);
                    break;
                case CRT_OPT_VIGNETTE:
                    snprintf(label_buf, sizeof(label_buf), "Curved Glass Tube Vignette");
                    snprintf(val_buf, sizeof(val_buf), "%s", kCrtVignetteNames[s_crt_vignette]);
                    break;
                case CRT_OPT_RESET:
                    snprintf(label_buf, sizeof(label_buf), "[ Reset CRT Settings to Default ]");
                    break;
                }

                if (i == CRT_OPT_JUST_SCANLINES || i == CRT_OPT_RESET) {
                    int tw = MeasureSmallText(label_buf);
                    int cx = content_x + (content_w - tw) / 2;
                    uint32_t col = (i == CRT_OPT_RESET)
                                       ? (is_sel ? 0xFFFFB454u : 0xFFE89838u)
                                       : (is_sel ? 0xFFFFFFFFu : tc.subtext_col);
                    DrawSmallText(pixels, stride, frame_h, cx, row_y + 2, label_buf, col);
                } else {
                    DrawSmallText(pixels, stride, frame_h, content_x + 10, row_y + 2, label_buf, 0xFFFFFFFF);
                    if (val_buf[0]) {
                        int vw = MeasureSmallText(val_buf);
                        DrawSmallText(pixels, stride, frame_h, content_x + content_w - vw - 8, row_y + 2, val_buf,
                                      is_sel ? 0xFFFFFFFF : tc.subtext_col);
                    }
                }
            }

            /* Live dynamic description of highlighted option */
            static const char *const kCrtDescs[CRT_OPT_COUNT] = {
                "* Selects 1-click CRT or Just Scanlines profiles.",
                "* Disables masks for pure horizontal scanlines only.",
                "* Scanline density: Fine 1px (close) up to 4px wide.",
                "* Darkness of scanlines from 15% subtle to 100% black.",
                "* Electron beam profile: Thin single or thick double.",
                "* Adds vertical/grid mask (Off = pure scanlines).",
                "* Restores punch and phosphor glow dimmed by scanlines.",
                "* Simulates curved CRT cathode ray tube edge falloff.",
                "* Restores CRT to Fine 1px 15% (Pure Horizontal)."
            };
            const char *desc = (sel_row >= 0 && sel_row < CRT_OPT_COUNT) ? kCrtDescs[sel_row] : "";
            if (s_menu_toast_timer > 0) {
                s_menu_toast_timer--;
                int tw = MeasureSmallText(s_menu_toast);
                DrawSmallText(pixels, stride, frame_h, content_x + (content_w - tw) / 2, 189,
                              s_menu_toast, 0xFF90FFB8);
            } else {
                DrawSmallText(pixels, stride, frame_h, content_x + 10, 189, desc, tc.subtext_col);
            }
        }
        else if (s_active_tab == TAB_BLOOM_OPTIONS) {
            /* Tab 3: Bloom Filter Options */
            for (int i = 0; i < BLOOM_OPT_COUNT; i++) {
                int row_y = start_y + i * row_h;
                bool is_sel = (i == sel_row);

                if (is_sel) {
                    DrawThemedHighlightRect(pixels, stride, frame_h,
                                            content_x, row_y - 1, content_w, 14, &tc, true);
                    DrawSmallText(pixels, stride, frame_h, content_x + 2, row_y + 2, ">", 0xFFFFFFFF);
                }

                char label_buf[64];
                char val_buf[64];
                label_buf[0] = '\0';
                val_buf[0] = '\0';

                switch (i) {
                case BLOOM_OPT_PRESET:
                    snprintf(label_buf, sizeof(label_buf), "Bloom Effect Preset");
                    snprintf(val_buf, sizeof(val_buf), "%s", kBloomPresetNames[s_bloom_preset]);
                    break;
                case BLOOM_OPT_INTENSITY:
                    snprintf(label_buf, sizeof(label_buf), "Bloom Glow Intensity");
                    snprintf(val_buf, sizeof(val_buf), "%s", kBloomIntensityNames[s_bloom_intensity]);
                    break;
                case BLOOM_OPT_THRESHOLD:
                    snprintf(label_buf, sizeof(label_buf), "Luminance Glow Cutoff");
                    snprintf(val_buf, sizeof(val_buf), "%s", kBloomThreshNames[s_bloom_threshold]);
                    break;
                case BLOOM_OPT_RADIUS:
                    snprintf(label_buf, sizeof(label_buf), "Bloom Blur Spread Radius");
                    snprintf(val_buf, sizeof(val_buf), "%s", kBloomRadiusNames[s_bloom_radius]);
                    break;
                case BLOOM_OPT_BLEND:
                    snprintf(label_buf, sizeof(label_buf), "Glow Blending Mode");
                    snprintf(val_buf, sizeof(val_buf), "%s", kBloomBlendNames[s_bloom_blend]);
                    break;
                case BLOOM_OPT_TINT:
                    snprintf(label_buf, sizeof(label_buf), "Atmosphere Color Tint");
                    snprintf(val_buf, sizeof(val_buf), "%s", kBloomTintNames[s_bloom_tint]);
                    break;
                case BLOOM_OPT_RESET:
                    snprintf(label_buf, sizeof(label_buf), "[ Reset Bloom Settings to Default ]");
                    break;
                }

                if (i == BLOOM_OPT_RESET) {
                    int tw = MeasureSmallText(label_buf);
                    int cx = content_x + (content_w - tw) / 2;
                    DrawSmallText(pixels, stride, frame_h, cx, row_y + 2, label_buf,
                                  is_sel ? 0xFFFFB454u : 0xFFE89838u);
                } else {
                    DrawSmallText(pixels, stride, frame_h, content_x + 10, row_y + 2, label_buf, 0xFFFFFFFF);
                    if (val_buf[0]) {
                        int vw = MeasureSmallText(val_buf);
                        DrawSmallText(pixels, stride, frame_h, content_x + content_w - vw - 8, row_y + 2, val_buf,
                                      is_sel ? 0xFFFFFFFF : tc.subtext_col);
                    }
                }
            }

            /* Live dynamic description of highlighted option */
            static const char *const kBloomDescs[BLOOM_OPT_COUNT] = {
                "* Selects curated 1-click bloom and glow presets.",
                "* Brightness and strength of the glowing bloom aura.",
                "* Minimum luminance required for a pixel to emit glow.",
                "* Pixel blur radius: controls how far light bleeds out.",
                "* Blending algorithm: Screen glow vs Additive neon.",
                "* Color atmosphere tint applied to the emitted glow.",
                "* Restores Bloom to default (50%, 80, 4px, Additive)."
            };
            const char *desc = (sel_row >= 0 && sel_row < BLOOM_OPT_COUNT) ? kBloomDescs[sel_row] : "";
            if (s_menu_toast_timer > 0) {
                s_menu_toast_timer--;
                int tw = MeasureSmallText(s_menu_toast);
                DrawSmallText(pixels, stride, frame_h, content_x + (content_w - tw) / 2, 185,
                              s_menu_toast, 0xFF90FFB8);
            } else {
                DrawSmallText(pixels, stride, frame_h, content_x + 10, start_y + BLOOM_OPT_COUNT * row_h + 10,
                              desc, tc.subtext_col);
            }
        }
        else if (s_active_tab == TAB_INPUT_OPTIONS) {
            /* Tab 2: Input Options */
            for (int i = 0; i < INPUT_OPT_COUNT; i++) {
                int row_y = start_y + i * row_h;
                bool is_sel = (i == sel_row);

                if (is_sel) {
                    DrawThemedHighlightRect(pixels, stride, frame_h,
                                            content_x, row_y - 1, content_w, 14, &tc, true);
                    DrawSmallText(pixels, stride, frame_h, content_x + 2, row_y + 2, ">", 0xFFFFFFFF);
                }

                char label_buf[64];
                char val_buf[64];
                label_buf[0] = '\0';
                val_buf[0] = '\0';

                switch (i) {
                case INPUT_OPT_VOLUME:
                    snprintf(label_buf, sizeof(label_buf), "Master Audio Volume");
                    if (g_config.volume == 0) snprintf(val_buf, sizeof(val_buf), "Muted (0%%)");
                    else snprintf(val_buf, sizeof(val_buf), "%d%%", (int)g_config.volume);
                    break;
                case INPUT_OPT_CONTROLLER:
                    snprintf(label_buf, sizeof(label_buf), "Primary Gamepad Slot");
                    snprintf(val_buf, sizeof(val_buf), "Player 1 (Auto-detected)");
                    break;
                case INPUT_OPT_KEYBINDS:
                    snprintf(label_buf, sizeof(label_buf), "Custom Keybinds File");
                    snprintf(val_buf, sizeof(val_buf), "keybinds.ini (Loaded)");
                    break;
                case INPUT_OPT_HOTKEY_MENU:
                    snprintf(label_buf, sizeof(label_buf), "Slide Menu Toggle Hotkey");
                    snprintf(val_buf, sizeof(val_buf), "LT / Esc / F1");
                    break;
                case INPUT_OPT_HOTKEY_DEBUG:
                    snprintf(label_buf, sizeof(label_buf), "Debug Overlay Hotkey");
                    snprintf(val_buf, sizeof(val_buf), "RT / F2");
                    break;
                }

                DrawSmallText(pixels, stride, frame_h, content_x + 10, row_y + 2, label_buf, 0xFFFFFFFF);
                if (val_buf[0]) {
                    int vw = MeasureSmallText(val_buf);
                    DrawSmallText(pixels, stride, frame_h, content_x + content_w - vw - 8, row_y + 2, val_buf,
                                  is_sel ? 0xFFFFFFFF : tc.subtext_col);
                }
            }

            /* Live dynamic description of highlighted option */
            static const char *const kInputDescs[INPUT_OPT_COUNT] = {
                "* Master audio output volume level (0% to 100%).",
                "* Gamepad controller assigned to Player 1 input.",
                "* Custom keybinds configuration file (keybinds.ini).",
                "* Hotkey used to toggle this settings menu drawer.",
                "* Hotkey used to toggle the floating debug tools overlay."
            };
            const char *desc = (sel_row >= 0 && sel_row < INPUT_OPT_COUNT) ? kInputDescs[sel_row] : "";
            DrawSmallText(pixels, stride, frame_h, content_x + 10, start_y + INPUT_OPT_COUNT * row_h + 8,
                          desc, tc.subtext_col);

            /* Keybinds summary box */
            int box_y = start_y + INPUT_OPT_COUNT * row_h + 23;
            DrawThemedHighlightRect(pixels, stride, frame_h, content_x, box_y, content_w, 42, &tc, false);
            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 6, "Keys : Z:Run/Attack  X:Action/A  S:Menu/X  A:Select/Y", 0xFFFFFFFF);
            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 18, "Pad  : A:Talk/Action  B:Run/Attack  Y:Ring  X:Wind", tc.subtext_col);
            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 30, "Trigs: LT: Settings Menu    RT: Debug Tools Overlay", tc.subtext_col);
        }
        else if (s_active_tab == TAB_HELP_README) {
            /* Tab 5: Help / ReadMe */
            int box_y = start_y + 1;
            int box_h = 152;
            DrawThemedHighlightRect(pixels, stride, frame_h, content_x, box_y, content_w, box_h, &tc, false);

            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 4,  "Secret of Evermore PC Native Recompilation (Evertopia)", 0xFFFFFFFF);
            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 14, "----------------------------------------", tc.muted_col);
            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 24, "Navigation: D-Pad / Arrows; A toggles; B / Esc closes", tc.subtext_col);
            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 35, "Switch Tab: LB / RB (or [ / ]), or click header", tc.subtext_col);
            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 46, "Filters   : CRT scanlines, bloom glow & Glass UI", tc.subtext_col);
            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 57, "Debug Menu: RT / F2 / ` opens Warps, Forms, Stats", tc.subtext_col);
            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 68, "Host Keys : Alt+Enter Fullscreen | Tab Turbo", tc.subtext_col);
            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 79, "States    : F11 Save Browser | F12 Rewind", tc.subtext_col);
            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 90, "Launcher  : --launcher or SkipLauncher=0 opens UI", tc.subtext_col);
            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 101,"Save Files: config.ini, keybinds.ini, *_config.json", tc.subtext_col);
            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 113,"License   : PolyForm Noncommercial 1.0.0 / MIT", tc.muted_col);
            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 125,"Thanks to Square USA, snesrecomp, snesrev & SDL", tc.muted_col);
            DrawSmallText(pixels, stride, frame_h, content_x + 6, box_y + 137,"Evertopia : Named by EddieEddie90 - Thank you!", tc.muted_col);
        }

        /* Bottom separator */
        if (s_reset_confirm != RESET_CONFIRM_NONE) {
            int dlg_w = 188;
            int dlg_h = 54;
            int dlg_x = content_x + (content_w - dlg_w) / 2;
            int dlg_y = start_y + 42;
            DrawThemedWindowPanel(pixels, stride, frame_w, frame_h, dlg_x, dlg_y, dlg_w, dlg_h, 0, 2, true, false);
            DrawSmallText(pixels, stride, frame_h, dlg_x + 56, dlg_y + 8, "ARE YOU SURE?", 0xFFFFB040u);
            const char *msg = "Reset settings to default?";
            if (s_reset_confirm == RESET_CONFIRM_ALL) msg = "Reset ALL settings to default?";
            else if (s_reset_confirm == RESET_CONFIRM_VIDEO) msg = "Reset Video settings to default?";
            else if (s_reset_confirm == RESET_CONFIRM_CRT) msg = "Reset CRT settings to default?";
            else if (s_reset_confirm == RESET_CONFIRM_BLOOM) msg = "Reset Bloom settings to default?";
            else if (s_reset_confirm == RESET_CONFIRM_EXIT_LAUNCHER) msg = "Exit to Launcher?";
#ifdef __ANDROID__
            else if (s_reset_confirm == RESET_CONFIRM_EXIT_DESKTOP) msg = "Exit to Android Launcher?";
#else
            else if (s_reset_confirm == RESET_CONFIRM_EXIT_DESKTOP) msg = "Exit to Desktop?";
#endif
            DrawSmallText(pixels, stride, frame_h, dlg_x + 12, dlg_y + 20, msg, 0xFFFFFFFFu);

            int btn_w = 54;
            int btn_h = 14;
            int yes_x = dlg_x + 30;
            int no_x = dlg_x + dlg_w - 30 - btn_w;
            int btn_y = dlg_y + 33;
            DrawThemedHighlightRect(pixels, stride, frame_h, yes_x, btn_y, btn_w, btn_h, &tc, s_reset_confirm_choice == 0);
            DrawSmallText(pixels, stride, frame_h, yes_x + 18, btn_y + 4, "YES",
                          s_reset_confirm_choice == 0 ? 0xFFFFFFFFu : tc.muted_col);
            DrawThemedHighlightRect(pixels, stride, frame_h, no_x, btn_y, btn_w, btn_h, &tc, s_reset_confirm_choice == 1);
            DrawSmallText(pixels, stride, frame_h, no_x + 21, btn_y + 4, "NO",
                          s_reset_confirm_choice == 1 ? 0xFFFFFFFFu : tc.muted_col);
        }
        DrawSemiTransparentRect(pixels, stride, frame_h,
                                drawer_x + 6, frame_h - 26, drawer_w - 12, 1,
                                tc.div_r, tc.div_g, tc.div_b, 190);

        /* Bottom Navigation Hints Footer (centered) */
        const char *footer_text = "LB/RB: Tab | D-Pad: Move | A: Toggle | B/LT/Esc: Close";
        int foot_w = MeasureSmallText(footer_text);
        int foot_x = drawer_x + (drawer_w - foot_w) / 2;
        if (foot_x < drawer_x + 6) foot_x = drawer_x + 6;
        DrawSmallText(pixels, stride, frame_h, foot_x, frame_h - 20,
                      footer_text, tc.subtext_col);
    } else {
        /*
         * 10-Second "Menu [LT/F1]" (left) & "Debug [Hold RT/F2]" (right) Tabs:
         * Stay on screen for 10 seconds (600 frames @ 60 FPS), then scroll
         * offscreen to the left / right over 60 frames with identical timing.
         */
        if (s_frame_counter < 660) {
            const uint8_t btn_alpha = 210;
            const int l_w = 86;
            const int r_w = 104;
            int l_off = 0;
            int r_off = 0;
            if (s_frame_counter >= 600) {
                int t = (int)(s_frame_counter - 600);
                l_off = -(t * l_w) / 60;
                r_off = (t * r_w) / 60;
            }
            /* Left tab: Menu [LT/F1] */
            int lx = l_off;
            DrawSemiTransparentRect(pixels, stride, frame_h, lx, 7, l_w, 13,
                                    tc.inact_r, tc.inact_g, tc.inact_b, btn_alpha);
            DrawSemiTransparentRect(pixels, stride, frame_h, lx + l_w - 2, 7, 2, 13,
                                    tc.accent_r, tc.accent_g, tc.accent_b, btn_alpha);
            DrawSmallText(pixels, stride, frame_h, lx + 4, 10, "Menu [LT/F1]", 0xFFFFFFFF);

            /* Right tab: Debug [Hold RT/F2] */
            int rx = frame_w - r_w + r_off;
            DrawSemiTransparentRect(pixels, stride, frame_h, rx, 7, r_w, 13,
                                    tc.inact_r, tc.inact_g, tc.inact_b, btn_alpha);
            DrawSemiTransparentRect(pixels, stride, frame_h, rx, 7, 2, 13,
                                    tc.accent_r, tc.accent_g, tc.accent_b, btn_alpha);
            DrawSmallText(pixels, stride, frame_h, rx + 5, 10, "Debug [Hold RT/F2]", 0xFFFFFFFF);
        }
    }

    /* 5. Render Upper-Right Map Pillarbox Badge (hidden at launch; unhidden via Debug -> Pillarbox Edit Mode) */
    uint16_t loaded_map = GetLoadedMapId();
    uint8_t pb_val = s_map_pillarbox[loaded_map & 0xFF];

    {
        float dtgt = s_debug_overlay ? 1.0f : 0.0f;
        if (s_dbg_slide < dtgt) { s_dbg_slide += 0.125f; if (s_dbg_slide > dtgt) s_dbg_slide = dtgt; }
        else if (s_dbg_slide > dtgt) { s_dbg_slide -= 0.125f; if (s_dbg_slide < dtgt) s_dbg_slide = dtgt; }

        float wtgt = (s_debug_overlay && s_warp_win_open) ? 1.0f : 0.0f;
        if (s_warp_slide < wtgt) { s_warp_slide += 0.125f; if (s_warp_slide > wtgt) s_warp_slide = wtgt; }
        else if (s_warp_slide > wtgt) { s_warp_slide -= 0.125f; if (s_warp_slide < wtgt) s_warp_slide = wtgt; }
    }
    bool dbg_stack = s_debug_overlay || s_dbg_slide > 0.0f;
    if (dbg_stack || (PB_EDIT_ACTIVE && s_anim_x <= -menu_w + 1.0f)) {
        int badge_x, badge_y, badge_w, badge_h, text_x, text_y;
        char badge_buf[24];
        GetMapBadgeLayout(frame_w, loaded_map, pb_val,
                          &badge_x, &badge_y, &badge_w, &badge_h,
                          &text_x, &text_y, badge_buf, sizeof(badge_buf));

        /* Themed badge background */
        DrawThemedHighlightRect(pixels, stride, frame_h,
                                badge_x, badge_y, badge_w, badge_h,
                                &tc, s_map_picker_open || s_map_badge_focused);

        uint32_t text_col = (pb_val == 0) ? 0xFFFFB050 : ((pb_val == 1) ? 0xFF60FF60 : 0xFFFF60FF);
        DrawSmallText(pixels, stride, frame_h, text_x, text_y, badge_buf, text_col);

        /* Coords display container above dropdown box, matching dropdown style and width */
        {
            int coord_w = badge_w;
            int coord_h = dbg_stack ? 15 : 17;
            int coord_x = badge_x;
            int coord_y = dbg_stack ? badge_y - 17 : 6;
            int text_y_off = dbg_stack ? 4 : 5;

            if (dbg_stack) {
                int music_y = coord_y - 17;
                uint16_t music_id = ReadLe16(&g_ram[0x0E4D]);
                char mus_str[28];
                snprintf(mus_str, sizeof(mus_str), "Music ID: %u ($%02X)",
                         (unsigned)music_id, (unsigned)(music_id & 0xFFu));
                int mus_tw = MeasureSmallText(mus_str);
                DrawThemedHighlightRect(pixels, stride, frame_h,
                                        coord_x, music_y, coord_w, coord_h,
                                        &tc, false);
                DrawSmallText(pixels, stride, frame_h,
                              coord_x + (coord_w - mus_tw) / 2, music_y + text_y_off,
                              mus_str, 0xFFFFFFFF);
            }

            /* Two sub-containers for X and Y matching the dropdown styling */
            int gap = 4;
            int half_w = (coord_w - gap) / 2;
            int x_box_x = coord_x;
            int y_box_x = coord_x + coord_w - half_w;

            /* Live player/entity map coordinates in Secret of Evermore:
             * $7E:0F42 is the controlled entity pointer (or $7E:0F3E for Boy).
             * Entity structure holds X pixel position at offset +0x1A and Y at +0x1C.
             * Falls back to camera coordinates ($7E:010E, $7E:0110) or $7E:0A06/$7E:0A08 if unspawned.
             */
            uint16_t ent_ptr = (uint16_t)(g_ram[0x0F42] | (g_ram[0x0F43] << 8));
            if (ent_ptr == 0 || ent_ptr >= 0x1FFF0) {
                ent_ptr = (uint16_t)(g_ram[0x0F3E] | (g_ram[0x0F3F] << 8));
            }

            uint16_t map_x = 0;
            uint16_t map_y = 0;
            if (ent_ptr > 0 && ent_ptr < 0x1FF00) {
                map_x = (uint16_t)(g_ram[ent_ptr + 0x1A] | (g_ram[ent_ptr + 0x1B] << 8));
                map_y = (uint16_t)(g_ram[ent_ptr + 0x1C] | (g_ram[ent_ptr + 0x1D] << 8));
            } else {
                map_x = (uint16_t)(g_ram[0x010E] | (g_ram[0x010F] << 8));
                map_y = (uint16_t)(g_ram[0x0110] | (g_ram[0x0111] << 8));
            }

            char x_str[16];
            char y_str[16];
            snprintf(x_str, sizeof(x_str), "X: %u", (unsigned)map_x);
            snprintf(y_str, sizeof(y_str), "Y: %u", (unsigned)map_y);

            int x_tw = MeasureSmallText(x_str);
            int y_tw = MeasureSmallText(y_str);

            /* Render X box */
            DrawThemedHighlightRect(pixels, stride, frame_h,
                                    x_box_x, coord_y, half_w, coord_h,
                                    &tc, false);
            DrawSmallText(pixels, stride, frame_h,
                          x_box_x + (half_w - x_tw) / 2, coord_y + text_y_off,
                          x_str, 0xFFFFFFFF);

            /* Render Y box */
            DrawThemedHighlightRect(pixels, stride, frame_h,
                                    y_box_x, coord_y, half_w, coord_h,
                                    &tc, false);
            DrawSmallText(pixels, stride, frame_h,
                          y_box_x + (half_w - y_tw) / 2, coord_y + text_y_off,
                          y_str, 0xFFFFFFFF);
        }

        /* Render Number Picker Popup Dropdown (locked between 0 and 1) */
        if (s_map_picker_open && !dbg_stack) {
            int picker_w = badge_w;
            int picker_h = 42;
            int picker_x = badge_x;
            int picker_y = badge_y + badge_h + 2;

            DrawThemedWindowPanel(pixels, stride, frame_w, frame_h,
                                  picker_x, picker_y, picker_w, picker_h,
                                  42, 0, true, false);
            DrawRectOutline(pixels, stride, frame_h,
                            picker_x, picker_y, picker_w, picker_h,
                            tc.sel_brd_r, tc.sel_brd_g, tc.sel_brd_b, 235);

            char hdr_buf[32];
            snprintf(hdr_buf, sizeof(hdr_buf), "Set Map %u Mode:", (unsigned)loaded_map);
            DrawSmallText(pixels, stride, frame_h, picker_x + 6, picker_y + 3, hdr_buf, 0xFFFFFFFF);

            /* Divider line */
            DrawSemiTransparentRect(pixels, stride, frame_h,
                                    picker_x + 4, picker_y + 13, picker_w - 8, 1,
                                    tc.div_r, tc.div_g, tc.div_b, 180);

            /* Row 0: [0] 4:3 Pillarbox */
            int r0_y = picker_y + 16;
            if (pb_val == 0) {
                DrawSemiTransparentRect(pixels, stride, frame_h,
                                        picker_x + 3, r0_y - 1, picker_w - 6, 10,
                                        tc.sel_r, tc.sel_g, tc.sel_b, 215);
                DrawSmallText(pixels, stride, frame_h, picker_x + 5, r0_y + 1, "> [0] 4:3 Pillar", 0xFFFFB050);
            } else {
                DrawSmallText(pixels, stride, frame_h, picker_x + 13, r0_y + 1, "[0] 4:3 Pillar", tc.subtext_col);
            }

            /* Row 1: [1] Widescreen */
            int r1_y = picker_y + 28;
            if (pb_val == 1) {
                DrawSemiTransparentRect(pixels, stride, frame_h,
                                        picker_x + 3, r1_y - 1, picker_w - 6, 10,
                                        tc.sel_r, tc.sel_g, tc.sel_b, 215);
                DrawSmallText(pixels, stride, frame_h, picker_x + 5, r1_y + 1, "> [1] Widescreen", 0xFF60FF60);
            } else {
                DrawSmallText(pixels, stride, frame_h, picker_x + 13, r1_y + 1, "[1] Widescreen", tc.subtext_col);
            }
        }
    }

    /* 6. Render Floating Upper-Right Corner Debug Window (Active Theme + Ornate In-Game Frame) */
    if (dbg_stack) {
        if (GetDebugToolsSlideFactor() > 0.001f) {
        int dbg_x, dbg_y, dbg_w, dbg_h;
        GetDebugWindowRect(frame_w, &dbg_x, &dbg_y, &dbg_w, &dbg_h);

        /* Active window backdrop (Glass theme or Classic $7E:0B15 pattern) tucked 3px inside the ornate border */
        DrawThemedWindowPanel(pixels, stride, frame_w, frame_h,
                              dbg_x, dbg_y, dbg_w, dbg_h,
                              38, 3, true, true);
        /* Active in-game ornate dialog border frame ($7E:0B17) */
        DrawRomDialogFrame(pixels, stride, frame_w, frame_h,
                           dbg_x, dbg_y, dbg_w / 8, dbg_h / 8);

        /* Debug Header */
        int hdr_w = MeasureSmallText("== Debug Tools ==");
        DrawSmallText(pixels, stride, frame_h, dbg_x + (dbg_w - hdr_w) / 2, dbg_y + 9, "== Debug Tools ==", 0xFFFFFFFF);

        /* Live Loaded Map ID in Hex & Decimal with Pillarbox Value */
        char map_info_buf[32];
        snprintf(map_info_buf, sizeof(map_info_buf), "Map %u ($%04X) [%d]",
                 (unsigned)loaded_map, (unsigned)loaded_map, (int)pb_val);
        uint32_t map_col = (pb_val == 0) ? 0xFFFFB050 : ((pb_val == 1) ? 0xFF90F0FF : 0xFFFF90FF);
        DrawSmallText(pixels, stride, frame_h, dbg_x + 11, dbg_y + 18, map_info_buf, map_col);

        DrawSemiTransparentRect(pixels, stride, frame_h,
                                dbg_x + 9, dbg_y + 27, dbg_w - 18, 1,
                                tc.div_r, tc.div_g, tc.div_b, 190);

        int dbg_start_y = dbg_y + 30;
        int dbg_row_h = 11;
        if (s_dbg_selected_row < 0 || s_dbg_selected_row >= DBG_TOTAL_COUNT) {
            s_dbg_selected_row = 0;
        }

        for (int i = 0; i < DBG_TOTAL_COUNT; i++) {
            int row_y = dbg_start_y + i * dbg_row_h;
            bool is_sel = (i == s_dbg_selected_row);

            if (is_sel) {
                DrawThemedHighlightRect(pixels, stride, frame_h,
                                        dbg_x + 9, row_y - 1, dbg_w - 18, 10, &tc, true);
                DrawSmallText(pixels, stride, frame_h, dbg_x + 12, row_y, ">", 0xFFFFFFFF);
            }

            char text_buf[32];
            switch (i) {
            case DBG_GIVE_EVERYTHING:
                snprintf(text_buf, sizeof(text_buf), "[+] Give Everything");
                break;
            case DBG_INF_HP:
                snprintf(text_buf, sizeof(text_buf), "[%c] Infinite HP",
                         s_inf_hp_active ? 'X' : ' ');
                break;
            case DBG_INF_STAMINA:
                snprintf(text_buf, sizeof(text_buf), "[%c] Infinite Stamina",
                         s_inf_stamina_active ? 'X' : ' ');
                break;
            case DBG_ONE_HIT_KO:
                snprintf(text_buf, sizeof(text_buf), "[%c] One-Hit KO (999)",
                         s_one_hit_ko_active ? 'X' : ' ');
                break;
            case DBG_WALK_WALLS:
                snprintf(text_buf, sizeof(text_buf), "[%c] Walk Thru Walls",
                         (s_walk_walls_active || s_post_warp_walk_timer > 0) ? 'X' : ' ');
                break;
            case DBG_DOG_FORM:
                /* Right arrow beside option that cycles through dog forms */
                snprintf(text_buf, sizeof(text_buf), "Dog: %s >", kDogFormNames[s_dog_form]);
                break;
            case DBG_WARP:
                snprintf(text_buf, sizeof(text_buf), "Warp [A] >");
                break;
            case DBG_JUKEBOX:
                snprintf(text_buf, sizeof(text_buf), "Jukebox [A] >");
                break;
            case DBG_KEY_TRADE_ITEMS:
                snprintf(text_buf, sizeof(text_buf), "Key/Trade Items [A] >");
                break;
            default:
                text_buf[0] = '\0';
                break;
            }

            DrawSmallText(pixels, stride, frame_h, dbg_x + 20, row_y, text_buf,
                          is_sel ? 0xFFFFFFFF : tc.subtext_col);
        }

        /* Bottom separator matching Settings drawer */
        DrawSemiTransparentRect(pixels, stride, frame_h,
                                dbg_x + 9, dbg_y + 134, dbg_w - 18, 1,
                                tc.div_r, tc.div_g, tc.div_b, 190);

        /* Toast feedback or Settings-style footer hints */
        if (s_debug_toast_timer > 0) {
            s_debug_toast_timer--;
            DrawSemiTransparentRect(pixels, stride, frame_h,
                                    dbg_x + 9, dbg_y + 139, dbg_w - 18, 11,
                                    tc.sel_r, tc.sel_g, tc.sel_b, 245);
            DrawSmallText(pixels, stride, frame_h, dbg_x + 12, dbg_y + 141, s_debug_toast, 0xFFFFFFFF);
        } else {
            const char *dbg_foot = "A: Activate  |  B: Close";
            int dfw = MeasureSmallText(dbg_foot);
            DrawSmallText(pixels, stride, frame_h, dbg_x + (dbg_w - dfw) / 2, dbg_y + 141,
                          dbg_foot, tc.subtext_col);
        }
        }

        /* 6b. Render Jukebox Sub-Window to the left of Debug Tools (fixed steel border + Omnitopia Blue Glass, independent of in-game theme) */
        if (s_jukebox_open) {
            BuildJukeboxSongList();
            ClampJukeboxScroll();

            int jb_x, jb_y, jb_w, jb_h;
            GetJukeboxWindowRect(frame_w, &jb_x, &jb_y, &jb_w, &jb_h);

            SoeUiThemeColors jb_tc;
            GetUiThemeColorsFor(DIALOG_THEME_BLUE_GLASS, &jb_tc);

            DrawThemedWindowPanelFor(DIALOG_THEME_BLUE_GLASS, pixels, stride, frame_w, frame_h,
                                     jb_x, jb_y, jb_w, jb_h,
                                     38, 3, true, true);
            DrawRomDialogFrameForIndex(pixels, stride, frame_w, frame_h,
                                       jb_x, jb_y, jb_w / 8, jb_h / 8, 0x0Au);

            int jb_hdr_w = MeasureSmallText("== Jukebox ==");
            DrawSmallText(pixels, stride, frame_h, jb_x + (jb_w - jb_hdr_w) / 2, jb_y + 9,
                          "== Jukebox ==", 0xFFFFFFFF);

            uint16_t cur_song = ReadLe16(&g_ram[0x0E4D]);
            bool has_song_playing = (g_ram[0x0E4B] != 0);
            char jb_sub[40];
            if (has_song_playing) {
                snprintf(jb_sub, sizeof(jb_sub), "Now: Song $%02X  [%d/%d]",
                         (unsigned)cur_song,
                         (s_jukebox_song_count > 0) ? (s_jukebox_cursor + 1) : 0,
                         s_jukebox_song_count);
            } else {
                snprintf(jb_sub, sizeof(jb_sub), "Now: --  [%d/%d]",
                         (s_jukebox_song_count > 0) ? (s_jukebox_cursor + 1) : 0,
                         s_jukebox_song_count);
            }
            DrawSmallText(pixels, stride, frame_h, jb_x + 11, jb_y + 18, jb_sub, 0xFF90F0FF);

            DrawSemiTransparentRect(pixels, stride, frame_h,
                                    jb_x + 9, jb_y + 27, jb_w - 18, 1,
                                    jb_tc.div_r, jb_tc.div_g, jb_tc.div_b, 190);

            /* Top Checkbox Row: [X] Keep Song Playing */
            int keep_y = jb_y + 30;
            if (s_jukebox_keep_focused) {
                DrawThemedHighlightRect(pixels, stride, frame_h,
                                        jb_x + 9, keep_y - 1, jb_w - 18, 10, &jb_tc, true);
                DrawSmallText(pixels, stride, frame_h, jb_x + 12, keep_y, ">", 0xFFFFFFFF);
            }
            char keep_buf[36];
            snprintf(keep_buf, sizeof(keep_buf), "[%c] Keep Song Playing",
                     s_jukebox_lock_song ? 'X' : ' ');
            uint32_t keep_col = s_jukebox_lock_song ? 0xFF60FF60 :
                                (s_jukebox_keep_focused ? 0xFFFFFFFF : 0xFFFFE080);
            DrawSmallText(pixels, stride, frame_h, jb_x + 20, keep_y, keep_buf, keep_col);

            DrawSemiTransparentRect(pixels, stride, frame_h,
                                    jb_x + 9, jb_y + 41, jb_w - 18, 1,
                                    jb_tc.div_r, jb_tc.div_g, jb_tc.div_b, 140);

            int jb_start_y = jb_y + 44;
            int jb_row_h = 11;
            for (int r = 0; r < kJukeboxVisibleRows; r++) {
                int idx = s_jukebox_scroll + r;
                if (idx >= s_jukebox_song_count) break;
                int row_y = jb_start_y + r * jb_row_h;
                bool is_sel = (!s_jukebox_keep_focused && idx == s_jukebox_cursor);
                uint8_t sid = s_jukebox_songs[idx];
                bool is_playing = (has_song_playing && (uint16_t)sid == cur_song);

                if (is_sel) {
                    DrawThemedHighlightRect(pixels, stride, frame_h,
                                            jb_x + 9, row_y - 1, jb_w - 27, 10, &jb_tc, true);
                    DrawSmallText(pixels, stride, frame_h, jb_x + 12, row_y, ">", 0xFFFFFFFF);
                }

                char song_buf[36];
                snprintf(song_buf, sizeof(song_buf), "Track %02d (ID $%02X)%s",
                         idx + 1, (unsigned)sid, is_playing ? " *" : "");
                uint32_t col = is_playing ? 0xFF60FF60 : (is_sel ? 0xFFFFFFFF : jb_tc.subtext_col);
                DrawSmallText(pixels, stride, frame_h, jb_x + 20, row_y, song_buf, col);
            }

            if (s_jukebox_scroll > 0) {
                int ax = jb_x + jb_w - 14;
                int ay = jb_start_y + 2;
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 2, ay,     1, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 1, ay + 1, 3, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax,     ay + 2, 5, 1, 255, 255, 144, 240);
            }
            if (s_jukebox_scroll + kJukeboxVisibleRows < s_jukebox_song_count) {
                int ax = jb_x + jb_w - 14;
                int ay = jb_start_y + (kJukeboxVisibleRows - 1) * jb_row_h + 2;
                DrawSemiTransparentRect(pixels, stride, frame_h, ax,     ay,     5, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 1, ay + 1, 3, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 2, ay + 2, 1, 1, 255, 255, 144, 240);
            }

            DrawSemiTransparentRect(pixels, stride, frame_h,
                                    jb_x + 9, jb_y + 134, jb_w - 18, 1,
                                    jb_tc.div_r, jb_tc.div_g, jb_tc.div_b, 190);

            const char *jb_f1 = "[A] Play   [B] Close";
            const char *jb_f2 = s_jukebox_lock_song ? "[Y] Keep Song: ON" : "[Y] Keep Song: OFF";
            int fw1 = MeasureSmallText(jb_f1);
            int fw2 = MeasureSmallText(jb_f2);
            DrawSmallText(pixels, stride, frame_h, jb_x + (jb_w - fw1) / 2, jb_y + 138, jb_f1, 0xFFFFFFFF);
            DrawSmallText(pixels, stride, frame_h, jb_x + (jb_w - fw2) / 2, jb_y + 148, jb_f2,
                          s_jukebox_lock_song ? 0xFF60FF60 : jb_tc.subtext_col);
        }

        /* 6b-2. Render Key & Trade Items Sub-Window to the left of Debug Tools */
        if (s_key_trade_open) {
            ClampKeyTradeScroll();

            int kt_x, kt_y, kt_w, kt_h;
            GetJukeboxWindowRect(frame_w, &kt_x, &kt_y, &kt_w, &kt_h);

            SoeUiThemeColors kt_tc;
            GetUiThemeColorsFor(DIALOG_THEME_BLUE_GLASS, &kt_tc);

            DrawThemedWindowPanelFor(DIALOG_THEME_BLUE_GLASS, pixels, stride, frame_w, frame_h,
                                     kt_x, kt_y, kt_w, kt_h,
                                     38, 3, true, true);
            DrawRomDialogFrameForIndex(pixels, stride, frame_w, frame_h,
                                       kt_x, kt_y, kt_w / 8, kt_h / 8, 0x0Au);

            int kt_hdr_w = MeasureSmallText("== Key & Trade ==");
            DrawSmallText(pixels, stride, frame_h, kt_x + (kt_w - kt_hdr_w) / 2, kt_y + 9,
                          "== Key & Trade ==", 0xFFFFFFFF);

            char kt_sub[40];
            snprintf(kt_sub, sizeof(kt_sub), "Click/A: Add Item  [%d/%d]",
                     s_key_trade_cursor + 1, kKeyTradeItemCount);
            DrawSmallText(pixels, stride, frame_h, kt_x + 11, kt_y + 18, kt_sub, 0xFF90F0FF);

            DrawSemiTransparentRect(pixels, stride, frame_h,
                                    kt_x + 9, kt_y + 27, kt_w - 18, 1,
                                    kt_tc.div_r, kt_tc.div_g, kt_tc.div_b, 190);

            int kt_start_y = kt_y + 31;
            int kt_row_h = 11;
            for (int r = 0; r < kKeyTradeVisibleRows; r++) {
                int idx = s_key_trade_scroll + r;
                if (idx >= kKeyTradeItemCount) break;
                int row_y = kt_start_y + r * kt_row_h;
                bool is_sel = (idx == s_key_trade_cursor);
                const SoeKeyTradeItemDef *it = &kKeyTradeItems[idx];

                if (is_sel) {
                    DrawThemedHighlightRect(pixels, stride, frame_h,
                                            kt_x + 9, row_y - 1, kt_w - 27, 10, &kt_tc, true);
                    DrawSmallText(pixels, stride, frame_h, kt_x + 12, row_y, ">", 0xFFFFFFFF);
                }

                char row_buf[40];
                bool owned = false;
                if (it->mask != 0) {
                    owned = (g_ram[it->addr] & it->mask) != 0;
                    snprintf(row_buf, sizeof(row_buf), "[%c] %s", owned ? 'X' : ' ', it->name);
                } else {
                    uint16_t cnt = ReadLe16(&g_ram[it->addr]);
                    owned = (cnt > 0);
                    snprintf(row_buf, sizeof(row_buf), "[%2u] %s", (unsigned)cnt, it->name);
                }
                /* Truncate long names cleanly if needed to fit inside kt_w - 28 */
                while (row_buf[0] != '\0' && MeasureSmallText(row_buf) > kt_w - 30) {
                    size_t len = strlen(row_buf);
                    if (len <= 1) break;
                    row_buf[len - 1] = '\0';
                }
                uint32_t col = owned ? 0xFF60FF60 : (is_sel ? 0xFFFFFFFF : kt_tc.subtext_col);
                DrawSmallText(pixels, stride, frame_h, kt_x + 20, row_y, row_buf, col);
            }

            if (s_key_trade_scroll > 0) {
                int ax = kt_x + kt_w - 14;
                int ay = kt_start_y + 2;
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 2, ay,     1, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 1, ay + 1, 3, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax,     ay + 2, 5, 1, 255, 255, 144, 240);
            }
            if (s_key_trade_scroll + kKeyTradeVisibleRows < kKeyTradeItemCount) {
                int ax = kt_x + kt_w - 14;
                int ay = kt_start_y + (kKeyTradeVisibleRows - 1) * kt_row_h + 2;
                DrawSemiTransparentRect(pixels, stride, frame_h, ax,     ay,     5, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 1, ay + 1, 3, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 2, ay + 2, 1, 1, 255, 255, 144, 240);
            }

            DrawSemiTransparentRect(pixels, stride, frame_h,
                                    kt_x + 9, kt_y + 134, kt_w - 18, 1,
                                    kt_tc.div_r, kt_tc.div_g, kt_tc.div_b, 190);

            const char *kt_f1 = "[A] Add/Toggle  [B] Close";
            const char *kt_f2 = "Trade: Left -1 / Right +1";
            int fw1 = MeasureSmallText(kt_f1);
            int fw2 = MeasureSmallText(kt_f2);
            DrawSmallText(pixels, stride, frame_h, kt_x + (kt_w - fw1) / 2, kt_y + 138, kt_f1, 0xFFFFFFFF);
            DrawSmallText(pixels, stride, frame_h, kt_x + (kt_w - fw2) / 2, kt_y + 148, kt_f2, kt_tc.subtext_col);
        }

        /* 6b-3. Render Warp Locations Sub-Window in Debug Tools' slot while Debug Tools slides away */
        if ((s_warp_win_open || s_warp_slide > 0.0f) && GetWarpToolsSlideFactor() > 0.001f) {
            BuildWarpLocationList();
            ClampWarpScroll();

            int wp_x, wp_y, wp_w, wp_h;
            GetWarpWindowRect(frame_w, &wp_x, &wp_y, &wp_w, &wp_h);

            SoeUiThemeColors wp_tc;
            GetUiThemeColorsFor(DIALOG_THEME_BLUE_GLASS, &wp_tc);

            DrawThemedWindowPanelFor(DIALOG_THEME_BLUE_GLASS, pixels, stride, frame_w, frame_h,
                                     wp_x, wp_y, wp_w, wp_h,
                                     38, 3, true, true);
            DrawRomDialogFrameForIndex(pixels, stride, frame_w, frame_h,
                                       wp_x, wp_y, wp_w / 8, wp_h / 8, 9u);

            int wp_hdr_w = MeasureSmallText("== Warp To ==");
            DrawSmallText(pixels, stride, frame_h, wp_x + (wp_w - wp_hdr_w) / 2, wp_y + 9,
                          "== Warp To ==", 0xFFFFFFFF);

            char wp_sub[40];
            if (s_warp_location_count > 0 && s_warp_cursor >= 0 && s_warp_cursor < s_warp_location_count) {
                uint16_t sel_id = s_warp_locations[s_warp_cursor].map_id;
                snprintf(wp_sub, sizeof(wp_sub), "Map %u ($%02X)  [%d/%d]",
                         (unsigned)sel_id, (unsigned)sel_id,
                         s_warp_cursor + 1, s_warp_location_count);
            } else {
                snprintf(wp_sub, sizeof(wp_sub), "No Maps  [0/0]");
            }
            DrawSmallText(pixels, stride, frame_h, wp_x + 11, wp_y + 18, wp_sub, 0xFF90F0FF);

            DrawSemiTransparentRect(pixels, stride, frame_h,
                                    wp_x + 9, wp_y + 27, wp_w - 18, 1,
                                    wp_tc.div_r, wp_tc.div_g, wp_tc.div_b, 190);

            uint16_t cur_loaded_map = GetLoadedMapId();
            int wp_start_y = wp_y + 31;
            int wp_row_h = 11;
            for (int r = 0; r < kWarpVisibleRows; r++) {
                int idx = s_warp_scroll + r;
                if (idx >= s_warp_location_count) break;
                int row_y = wp_start_y + r * wp_row_h;
                bool is_sel = (idx == s_warp_cursor);
                uint16_t mid = s_warp_locations[idx].map_id;
                bool is_current = (mid == cur_loaded_map);

                if (is_sel) {
                    DrawThemedHighlightRect(pixels, stride, frame_h,
                                            wp_x + 9, row_y - 1, wp_w - 27, 10, &wp_tc, true);
                    DrawSmallText(pixels, stride, frame_h, wp_x + 12, row_y, ">", 0xFFFFFFFF);
                }

                char row_buf[80];
                snprintf(row_buf, sizeof(row_buf), "%s ($%02X)", GetMapName(mid), (unsigned)mid);
                int max_row_w = wp_w - 30;
                while (MeasureSmallText(row_buf) > max_row_w && strlen(row_buf) > 10) {
                    size_t rlen = strlen(row_buf);
                    if (rlen > 9) {
                        char id_tag[8];
                        snprintf(id_tag, sizeof(id_tag), " ($%02X)", (unsigned)mid);
                        size_t prefix_len = rlen - strlen(id_tag);
                        if (prefix_len > 4) {
                            row_buf[prefix_len - 4] = '.';
                            row_buf[prefix_len - 3] = '.';
                            row_buf[prefix_len - 2] = '.';
                            snprintf(row_buf + prefix_len - 1, sizeof(row_buf) - (prefix_len - 1), "%s", id_tag);
                        } else {
                            break;
                        }
                    } else {
                        break;
                    }
                }
                while (row_buf[0] != '\0' && MeasureSmallText(row_buf) > wp_w - 30) {
                    size_t len = strlen(row_buf);
                    if (len <= 1) break;
                    row_buf[len - 1] = '\0';
                }
                uint32_t col = is_current ? 0xFF60FF60 : (is_sel ? 0xFFFFFFFF : wp_tc.subtext_col);
                DrawSmallText(pixels, stride, frame_h, wp_x + 20, row_y, row_buf, col);
            }

            if (s_warp_scroll > 0) {
                int ax = wp_x + wp_w - 14;
                int ay = wp_start_y + 2;
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 2, ay,     1, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 1, ay + 1, 3, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax,     ay + 2, 5, 1, 255, 255, 144, 240);
            }
            if (s_warp_scroll + kWarpVisibleRows < s_warp_location_count) {
                int ax = wp_x + wp_w - 14;
                int ay = wp_start_y + (kWarpVisibleRows - 1) * wp_row_h + 2;
                DrawSemiTransparentRect(pixels, stride, frame_h, ax,     ay,     5, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 1, ay + 1, 3, 1, 255, 255, 144, 240);
                DrawSemiTransparentRect(pixels, stride, frame_h, ax + 2, ay + 2, 1, 1, 255, 255, 144, 240);
            }

            DrawSemiTransparentRect(pixels, stride, frame_h,
                                    wp_x + 9, wp_y + 134, wp_w - 18, 1,
                                    wp_tc.div_r, wp_tc.div_g, wp_tc.div_b, 190);

            const char *wp_f1 = "[A] Warp     [B] Close";
            const char *wp_f2 = "Up/Dn: Scroll   LB/RB: +/-10";
            int fw1 = MeasureSmallText(wp_f1);
            int fw2 = MeasureSmallText(wp_f2);
            DrawSmallText(pixels, stride, frame_h, wp_x + (wp_w - fw1) / 2, wp_y + 138, wp_f1, 0xFFFFFFFF);
            DrawSmallText(pixels, stride, frame_h, wp_x + (wp_w - fw2) / 2, wp_y + 148, wp_f2, wp_tc.subtext_col);

            /* 6b-4. Render scaled PNG map preview in a separate window to the left of Warp To */
            int pv_x, pv_y, pv_w, pv_h;
            if (GetWarpPreviewWindowRect(frame_w, wp_x, wp_y, wp_w, wp_h,
                                         &pv_x, &pv_y, &pv_w, &pv_h)) {
                DrawThemedWindowPanelFor(DIALOG_THEME_BLUE_GLASS, pixels, stride, frame_w, frame_h,
                                         pv_x, pv_y, pv_w, pv_h,
                                         38, 3, true, true);
                DrawRomDialogFrameForIndex(pixels, stride, frame_w, frame_h,
                                           pv_x, pv_y, pv_w / 8, pv_h / 8, 9u);

                int pv_hdr_w = MeasureSmallText("Map View");
                DrawSmallText(pixels, stride, frame_h, pv_x + (pv_w - pv_hdr_w) / 2, pv_y + 9,
                              "Map View", 0xFFFFFFFF);

                int box_x = pv_x + 9;
                int box_y = pv_y + 30;
                int box_w = pv_w - 18;
                int box_h = pv_h - 51;
                DrawSemiTransparentRect(pixels, stride, frame_h,
                                        pv_x + 9, pv_y + 27, pv_w - 18, 1,
                                        wp_tc.div_r, wp_tc.div_g, wp_tc.div_b, 190);
                DrawSemiTransparentRect(pixels, stride, frame_h,
                                        box_x, box_y, box_w, box_h,
                                        6, 12, 26, 125);

                if (s_warp_location_count > 0 && s_warp_cursor >= 0 && s_warp_cursor < s_warp_location_count) {
                    uint16_t sel_id = s_warp_locations[s_warp_cursor].map_id;
                    UpdateWarpMapPreview(sel_id);

                    char pv_dim[32];
                    uint32_t dim_col = 0xFF90F0FF;
                    if (s_warp_preview_valid) {
                        snprintf(pv_dim, sizeof(pv_dim), "%dx%d", s_warp_preview_src_w, s_warp_preview_src_h);
                    } else {
                        snprintf(pv_dim, sizeof(pv_dim), "No PNG");
                        dim_col = 0xFFFF9090;
                    }
                    int dim_w = MeasureSmallText(pv_dim);
                    DrawSmallText(pixels, stride, frame_h, pv_x + (pv_w - dim_w) / 2, pv_y + 18, pv_dim, dim_col);

                    if (s_warp_preview_valid && box_w > 2 && box_h > 2) {
                        s_warp_native_ov_map_id = sel_id;
                        s_warp_native_ov_x = (float)(box_x + 1);
                        s_warp_native_ov_y = (float)(box_y + 1);
                        s_warp_native_ov_w = (float)(box_w - 2);
                        s_warp_native_ov_h = (float)(box_h - 2);
                        s_warp_native_ov_fw = frame_w;
                        s_warp_native_ov_fh = frame_h;
                        s_warp_native_ov_active = !s_give_all_confirm_open;
                    } else if (!s_warp_preview_valid) {
                        DrawSmallTextWrappedCentered(pixels, stride, frame_h,
                                                     box_x, box_y, box_w, box_h,
                                                     "Dump maps to PNGs on the launcher for Warp Previews",
                                                     wp_tc.subtext_col);
                    }

                    DrawSemiTransparentRect(pixels, stride, frame_h,
                                            pv_x + 9, pv_y + pv_h - 19, pv_w - 18, 1,
                                            wp_tc.div_r, wp_tc.div_g, wp_tc.div_b, 190);
                    char pv_ft[24];
                    snprintf(pv_ft, sizeof(pv_ft), "Map %03u.png", (unsigned)sel_id);
                    int pv_ft_w = MeasureSmallText(pv_ft);
                    DrawSmallText(pixels, stride, frame_h, pv_x + (pv_w - pv_ft_w) / 2, pv_y + pv_h - 15,
                                  pv_ft, wp_tc.subtext_col);
                }
            }
        } else if (!s_warp_win_open && s_warp_slide <= 0.0f) {
            s_warp_preview_cached_id = -1;
            s_warp_preview_valid = false;
            s_warp_native_ov_active = false;
        }

        /* 6c. Render warning confirmation modal above Debug Tools & Jukebox for "Give Everything" */
        if (s_give_all_confirm_open) {
            int dlg_x, dlg_y, dlg_w, dlg_h;
            GetGiveAllConfirmRect(frame_w, frame_h, &dlg_x, &dlg_y, &dlg_w, &dlg_h);

            DrawSemiTransparentRect(pixels, stride, frame_h, 0, 0, frame_w, frame_h, 0, 0, 0, 125);
            DrawThemedWindowPanel(pixels, stride, frame_w, frame_h,
                                  dlg_x, dlg_y, dlg_w, dlg_h,
                                  38, 3, true, true);
            DrawRomDialogFrame(pixels, stride, frame_w, frame_h, dlg_x, dlg_y, dlg_w / 8, dlg_h / 8);

            const char *warn_title = "!! WARNING !!";
            int wtw = MeasureSmallText(warn_title);
            DrawSmallText(pixels, stride, frame_h, dlg_x + (dlg_w - wtw) / 2, dlg_y + 10,
                          warn_title, 0xFFFFB050);
            DrawSemiTransparentRect(pixels, stride, frame_h,
                                    dlg_x + 10, dlg_y + 21, dlg_w - 20, 1,
                                    tc.div_r, tc.div_g, tc.div_b, 190);

            const char *l1 = "Give Everything maxes Lv99, stats,";
            const char *l2 = "all money, gear, spells & items.";
            const char *l3 = "This action cannot be undone!";
            DrawSmallText(pixels, stride, frame_h, dlg_x + (dlg_w - MeasureSmallText(l1)) / 2, dlg_y + 26, l1, 0xFFFFFFFF);
            DrawSmallText(pixels, stride, frame_h, dlg_x + (dlg_w - MeasureSmallText(l2)) / 2, dlg_y + 37, l2, 0xFFFFFFFF);
            DrawSmallText(pixels, stride, frame_h, dlg_x + (dlg_w - MeasureSmallText(l3)) / 2, dlg_y + 49, l3, 0xFFFF7070);

            int btn_y = dlg_y + 64;
            int btn_h = 13;
            int cancel_x = dlg_x + 16, cancel_w = 88;
            int yes_x = dlg_x + dlg_w - 16 - 96, yes_w = 96;
            const char *c_lbl = "[ Cancel ]";
            const char *y_lbl = "[ Yes, Give All ]";

            if (s_give_all_confirm_choice == 0) {
                DrawThemedHighlightRect(pixels, stride, frame_h, cancel_x, btn_y, cancel_w, btn_h, &tc, true);
            } else {
                DrawSemiTransparentRect(pixels, stride, frame_h, cancel_x, btn_y, cancel_w, btn_h, 0, 0, 0, 95);
                DrawRectOutline(pixels, stride, frame_h, cancel_x, btn_y, cancel_w, btn_h, tc.div_r, tc.div_g, tc.div_b, 180);
            }
            DrawSmallText(pixels, stride, frame_h,
                          cancel_x + (cancel_w - MeasureSmallText(c_lbl)) / 2, btn_y + 2,
                          c_lbl, (s_give_all_confirm_choice == 0) ? 0xFFFFFFFF : tc.subtext_col);

            if (s_give_all_confirm_choice == 1) {
                DrawThemedHighlightRect(pixels, stride, frame_h, yes_x, btn_y, yes_w, btn_h, &tc, true);
            } else {
                DrawSemiTransparentRect(pixels, stride, frame_h, yes_x, btn_y, yes_w, btn_h, 0, 0, 0, 95);
                DrawRectOutline(pixels, stride, frame_h, yes_x, btn_y, yes_w, btn_h, tc.div_r, tc.div_g, tc.div_b, 180);
            }
            DrawSmallText(pixels, stride, frame_h,
                          yes_x + (yes_w - MeasureSmallText(y_lbl)) / 2, btn_y + 2,
                          y_lbl, (s_give_all_confirm_choice == 1) ? 0xFFFFFFFF : tc.subtext_col);

            DrawSemiTransparentRect(pixels, stride, frame_h,
                                    dlg_x + 10, dlg_y + 81, dlg_w - 20, 1,
                                    tc.div_r, tc.div_g, tc.div_b, 190);
            const char *c_foot = "A: Confirm   |   B: Cancel";
            DrawSmallText(pixels, stride, frame_h,
                          dlg_x + (dlg_w - MeasureSmallText(c_foot)) / 2, dlg_y + 85,
                          c_foot, tc.subtext_col);
        }
    }

    if (s_state_toast[0] && SDL_GetTicks64() < s_state_toast_until_ms) {
        SoeUiThemeColors toast_tc;
        GetUiThemeColors(&toast_tc);
        int tw = MeasureSmallText(s_state_toast);
        int box_w = tw + 16;
        int box_h = 15;
        int box_x = frame_w - box_w - 8;
        int box_y = 8;
        DrawSemiTransparentRect(pixels, stride, frame_h, box_x, box_y, box_w, box_h,
                                toast_tc.sel_r, toast_tc.sel_g, toast_tc.sel_b, 215);
        DrawRectOutline(pixels, stride, frame_h, box_x, box_y, box_w, box_h,
                        toast_tc.sel_brd_r, toast_tc.sel_brd_g, toast_tc.sel_brd_b, 235);
        DrawSmallText(pixels, stride, frame_h, box_x + 8, box_y + 4, s_state_toast, 0xFFFFFFFF);
    }

    if (s_pending_screenshot) {
        s_pending_screenshot = false;
        CaptureFullScreenshot(pixels, stride, frame_w, frame_h);
    }

    const char *dump_path = getenv("SOE_DUMP_FRAME");
    if (dump_path) {
        unsigned target_frame = (unsigned)atoi(getenv("SOE_DUMP_FRAME_NUM") ? getenv("SOE_DUMP_FRAME_NUM") : "30");
        if (s_frame_counter == target_frame) {
            SaveFramebufferPng(dump_path, pixels, stride, frame_w, frame_h);
        }
    }

    return 1;
}

int SoeWindowBaseWidth(int frame_w)
{
    return (frame_w * 5 + 2) / 4;
}

int SoeWindowBaseHeight(void)
{
    return 240;
}
