/*
 * Secret of Evermore — desktop host shim.
 *
 * Identity and hooks, nothing else. The host itself is the framework's
 * (snesrecomp/runner/src/desktop/host_main.h): the pre-boot launcher, ROM
 * resolution and digest checks, config.ini and keybinds.ini, the window and
 * the SDL / OpenGL presenters, audio, gamepads, the in-game save-state
 * browser and rewind filmstrip, the OSD, the pacing clock, crash handlers and
 * the post-mortem report, mod packages, Generate & rebuild, and the netplay
 * barrier when the project is built with it. A fix there reaches this
 * project on a submodule pull; nothing in this file needs to change for it.
 *
 * This file used to BE the host -- a ~700-line copy made at scaffold time --
 * so every project scaffolded before a fix kept the bug. Two ports of the
 * same game, a week apart, had different hosts.
 *
 * Nothing here ever ships a ROM. The player supplies one; the host makes
 * that easy (launcher, then positional argument, then a copy beside the
 * executable, then the rom.cfg cache, then a file picker) and checks what it
 * is handed against the digests in rom_identity.txt.
 *
 * Grow this file only with what is specific to THIS title: a custom
 * presenter (prepare_frame / draw_frame), an SPC player, a pacing rule
 * (keep_pacing_debt), a Mods provider. See SnesDesktopHostGame for the
 * complete list of hooks and what each is for.
 */

#include "host_main.h"
#include "game_rtl.h"
#include "slide_menu.h"
#include "spc_jukebox.h"
#include "asset_cache.h"
#include "coop_2p.h"
#include "snesrecomp_rom_identity.h"  /* generated from rom_identity.txt */
#include "snes/snes.h"
#include "snes/ppu.h"
#include <stdio.h>
#include <string.h>

#ifdef RECOMP_LAUNCHER
#include "recomp_launcher.h"
#endif

#ifndef __ANDROID__
#define SDL_MAIN_HANDLED 1
#endif
#include "desktop/sdl_compat.h"

#ifndef SNES_GAME_VERSION
#define SNES_GAME_VERSION "dev"
#endif

/*
 * Default config.ini written when none exists beside the executable:
 * 4x scale, Fullscreen = 1 (native desktop), 8:7 square pixel aspect, OpenGL (Linux) / SDL + Vulkan,
 * widescreen enabled, triple buffering on, no sprite limit, 48000Hz audio at 20% volume,
 * Player 1 on Gamepad with 10% deadzone.
 */
static const char kSoeDefaultConfigIni[] =
    "[General]\n"
    "# Automatically save state on quit and reload on start\n"
    "Autosave = 0\n"
    "\n"
    "# Disable the SDL_Delay that happens each frame (slightly better\n"
    "# perf if your display is set to exactly 60hz)\n"
    "DisableFrameDelay = 0\n"
    "\n"
    "SkipLauncher = 0\n"
    "RunAhead = 0\n"
    "[Graphics]\n"
    "# Window size (Auto or WidthxHeight)\n"
    "WindowSize = Auto\n"
    "\n"
    "# Fullscreen mode (0=windowed, 1=desktop fullscreen, 2=fullscreen)\n"
#ifdef __ANDROID__
    "Fullscreen = 1\n"
#else
    "Fullscreen = 0\n"
#endif
    "\n"
    "# Window scale (1=100%, 2=200%, 3=300%, etc.)\n"
    "WindowScale = 4\n"
    "\n"
    "# Use the optimized SNES PPU implementation\n"
    "NewRenderer = 1\n"
    "\n"
    "# Don't keep the aspect ratio\n"
    "IgnoreAspectRatio = 0\n"
    "\n"
    "# Remove the sprite limits per scan line\n"
    "NoSpriteLimits = 1\n"
    "\n"
    "Mode7Enabled = 1\n"
    "Mode7Scale = 8\n"
    "Mode7Supersample = 1\n"
    "DisplayAspect = 8:7\n"
#if defined(__linux__) && !defined(__ANDROID__)
    "OutputMethod = OpenGL\n"
    "LinearFiltering = 0\n"
    "Shader = \n"
    "Widescreen = 1\n"
    "FrameBlend = 0\n"
    "VSync = 1\n"
    "Renderer = opengl\n"
    "TripleBuffering = 1\n"
#elif defined(__ANDROID__)
    "OutputMethod = SDL\n"
    "LinearFiltering = 0\n"
    "Shader = \n"
    "Widescreen = 1\n"
    "FrameBlend = 0\n"
    "VSync = 1\n"
    "Renderer = auto\n"
    "TripleBuffering = 1\n"
#else
    "OutputMethod = SDL\n"
    "LinearFiltering = 0\n"
    "Shader = \n"
    "Widescreen = 1\n"
    "FrameBlend = 0\n"
    "VSync = 1\n"
    "Renderer = auto\n"
    "TripleBuffering = 1\n"
#endif
    "[Sound]\n"
    "EnableAudio = 1\n"
    "# 0..100; VolumeUp / VolumeDown move it in 5% steps and show a bar.\n"
    "Volume = 20\n"
    "AudioFreq = 48000\n"
    "AudioChannels = 2\n"
    "AudioSamples = 512\n"
    "\n"
    "[KeyMap]\n"
    "# This section is for system-level shortcuts (save/load state,\n"
    "# fullscreen, pause, etc.). The 12 SNES controller buttons live\n"
    "# in keybinds.ini next to the executable.\n"
    "Fullscreen = Alt+Return\n"
    "Reset = Ctrl+r\n"
    "Pause = Shift+p\n"
    "PauseDimmed = p\n"
    "Turbo = Tab\n"
    "WindowBigger = Ctrl+Up\n"
    "WindowSmaller = Ctrl+Down\n"
    "VolumeUp = Keypad +\n"
    "VolumeDown = Keypad -\n"
    "DisplayPerf = f\n"
    "ToggleRenderer = r\n"
    "SaveStateMenu = None\n"
    "Rewind = F12\n"
    "SaveState1 = F5\n"
    "SaveState2 = F6\n"
    "SaveState3 = F7\n"
    "LoadState1 = F9\n"
    "LoadState2 = F10\n"
    "LoadState3 = F11\n"
    "Load =     None,     F2,     F3,     F4,   None,   None,   None,     F8,   None,   None\n"
    "Save = Shift+F1,Shift+F2,Shift+F3,Shift+F4,Shift+F5,Shift+F6,Shift+F7,Shift+F8,Shift+F9,Shift+F10\n"
    "\n"
    "[GamepadMap]\n"
    "# Enable each player's gamepad slot. SDL_GameController-compatible\n"
    "# controllers (Xbox, PlayStation, Switch Pro, etc.) auto-detect\n"
    "# when plugged in. Set to false to force keyboard-only.\n"
    "EnableGamepad1 = true\n"
    "EnableGamepad2 = false\n"
    "\n"
    "# Default Xbox-layout mapping. Order matches kKeys_Controls:\n"
    "#   Up, Down, Left, Right, Select, Start, A, B, X, Y, L, R\n"
    "# Edit + restart to rebind. Shoulder = L1/Lb (top), trigger = L2.\n"
    "Controls =   DpadUp, DpadDown, DpadLeft, DpadRight, Back, Start, B, A, Y, X, Lb, Rb\n"
    "ControlsP2 = DpadUp, DpadDown, DpadLeft, DpadRight, Back, Start, B, A, Y, X, Lb, Rb\n"
    "GamepadDeadzone = 3277\n"
    "\n"
    "[Netplay]\n"
    "PlayerName = \n"
    "\n"
    "[Controller]\n"
    "SourceP1 = 2\n"
    "SourceP2 = 0\n";

#ifdef RECOMP_LAUNCHER
static void SoeConfigureLauncher(struct RecompLauncherCGameInfo *gi)
{
    if (!gi) return;

    /* Enables "Restore Defaults" in the launcher Settings footer */
    static RecompLauncherCSettings s_default_settings;
    memset(&s_default_settings, 0, sizeof(s_default_settings));
    s_default_settings.output_method = 0;    /* SDL */
    s_default_settings.window_scale  = 4;    /* 4x Native */
#ifdef __ANDROID__
    s_default_settings.fullscreen    = 1;    /* Native Fullscreen */
#else
    s_default_settings.fullscreen    = 0;    /* Windowed */
#endif
    s_default_settings.ignore_aspect = 0;
    s_default_settings.linear_filter = 0;    /* Sharp nearest pixels */
    s_default_settings.widescreen    = 1;
    s_default_settings.enable_audio  = 1;
    s_default_settings.audio_freq    = 48000;
    s_default_settings.volume        = 20;
    s_default_settings.player_src[0] = 2;    /* Player 1: Gamepad */
    s_default_settings.player_src[1] = 0;    /* Player 2: Disabled */
    for (int i = 0; i < RECOMP_LAUNCHER_MAX_PLAYERS; i++) {
        s_default_settings.deadzone[i] = 10; /* 10% (3277 / 32767) */
    }
    s_default_settings.skip_launcher = 0;
    s_default_settings.aspect_index  = kSnesDisplayAspect_SquarePixels8x7; /* 8:7 */
    s_default_settings.vsync         = RECOMP_LAUNCHER_VSYNC_ON;
    s_default_settings.renderer      = 0;
#if defined(__linux__) && !defined(__ANDROID__)
    s_default_settings.output_method = 2;    /* OpenGL */
    if (gi->renderer_labels && gi->num_renderers > 0) {
        for (int i = 0; i < gi->num_renderers; i++) {
            if (gi->renderer_labels[i] && strcmp(gi->renderer_labels[i], "OpenGL") == 0) {
                s_default_settings.renderer = i;
                break;
            }
        }
    }
    snprintf(s_default_settings.renderer_id, sizeof(s_default_settings.renderer_id), "opengl");
#elif defined(__ANDROID__)
    s_default_settings.output_method = 0;    /* SDL (GLES2/Vulkan) */
    s_default_settings.renderer      = 0;    /* Auto */
    snprintf(s_default_settings.renderer_id, sizeof(s_default_settings.renderer_id), "auto");
#else
    s_default_settings.output_method = 0;    /* SDL (Auto: Vulkan/Direct3D/OpenGL) */
    s_default_settings.renderer      = 0;    /* Auto */
    snprintf(s_default_settings.renderer_id, sizeof(s_default_settings.renderer_id), "auto");
#endif
    s_default_settings.frame_blend   = 0;
    s_default_settings.run_ahead     = 0;
    gi->default_settings = &s_default_settings;
    gi->credits_text =
        "Secret of Evermore: Evertopia\n\n"
        "Special Thanks:\n"
        "- EddieEddie90 — For coming up with the name \"Evertopia\"!\n"
        "- Square Soft / Square USA (Redmond, WA) & Nintendo — Creators of Secret of Evermore (1995) and Jeremy Soule for its unforgettable soundtrack.\n"
        "- Matthew Stanley & Contributors — snesrecomp and recomp-ui.\n"
        "- snesrev, LakeSnes (elzo_d), xander-haj/Z3R, SDL3, Dear ImGui, and the SNES preservation community.";
}
#endif

static void SoeAfterSnesInit(void)
{
    SoeInitUiSfx();
#ifdef __ANDROID__
    SoeAndroidFirstBootExtract(SoeAssetCacheInit);
#else
    SoeAssetCacheInit();
#endif
    SoeCoop2pInit();
}

/*
 * Secret of Evermore Desktop Host Configuration
 *
 * Configures identity, native widescreen PPU support, aspect ratio controls,
 * and presentation hooks connecting to the in-game slide-out menu (slide_menu.c).
 */
static const SnesDesktopHostGame kGameHost = {
    /* ── Identity ─────────────────────────────────────────────────────────── */
    .display_name             = "Secret of Evermore",
    .window_title             = "Secret of Evermore (Evertopia)",
    .launcher_title           = "Secret of Evermore - Launcher",
    .region                   = SNESRECOMP_ROM_REGION,
    .rom_file                 = SNESRECOMP_ROM_FILE,
    .expected_sha256_hex      = SNESRECOMP_ROM_EXPECTED_SHA256,
    .expected_crc32_hex       = SNESRECOMP_ROM_EXPECTED_CRC32,
    .game_id                  = SNESRECOMP_ROM_GAME_ID,
    .build_version            = SNES_GAME_VERSION,
    .default_config_ini       = kSoeDefaultConfigIni,
    .game_info                = &kGameInfo,
    .num_players              = 2,
    .sram_path                = "saves/save.srm",

    /* ── Widescreen & Presentation Features ───────────────────────────────── */
    .widescreen_supported     = 1, /* Expose widescreen controls in launcher */
    .native_widescreen        = 1, /* Enable runner's widened PPU rasterizer */
    .display_aspect_supported = 1, /* Expose 4:3, 8:7, and 1:1 pixel aspect choices */
    .state_menu_hotkeys       = 1, /* F11/F12 state menu hotkeys */

    /* ── Presentation & Simulation Hooks ──────────────────────────────────── */
    .after_config             = SoeSlideMenuInit,    /* Hook input event filter */
    .prepare_frame            = SoePrepareFrame,     /* Frame dimensions (342, 426, or 256) */
    .begin_sim_frame          = SoeBeginSimFrame,    /* Widescreen margins, 4:3 start menu, HUD anchor */
    .menu_paused              = SoeMenuPaused,       /* Freeze 65816 while slide-out menu is open/animating */
    .menu_paused_tick         = SoeMenuPausedTick,   /* Poll menu input & Jukebox SPC700 commands while paused */
    .on_save_state            = SoeSaveStateSlot,    /* 3-slot atomic save state */
    .on_load_state            = SoeLoadStateSlot,    /* 3-slot atomic load state */
    .slot_state_hotkeys       = 1,                   /* F5..F7 Save 1..3, F9..F11 Load 1..3 */
    .after_snes_init          = SoeAfterSnesInit,    /* First-boot UI SFX + Asset decompression cache */
    .mix_ui_audio             = SoeMixUiAudio,       /* Host-side UI SFX mixer */
    .get_native_overlay       = SoeGetNativeOverlay, /* High-res PC-resolution Warp PNG preview */
    .presentation_hz          = SoePresentationHz,   /* Decoupled high-refresh (100 Hz+) presentation */
    .draw_frame               = SoeDrawFrame,        /* Render PPU frame & slide-out options menu */
    .window_base_width        = SoeWindowBaseWidth,  /* Window scaling width calculations */
    .window_base_height       = SoeWindowBaseHeight, /* Window scaling height */
#ifdef RECOMP_LAUNCHER
    .configure_launcher       = SoeConfigureLauncher,
#endif
};

#ifndef __ANDROID__
#undef main   /* desktop: keep plain main() even if SDL_main.h remapped it */
int main(int argc, char **argv)
#else
__attribute__((visibility("default"))) int SDL_main(int argc, char **argv)
#endif
{
#ifdef __ANDROID__
    /* Prevent Android Back button / controller B-as-Back from killing the app. */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    /* Allow controllers to work cleanly even when USB/Bluetooth devices attach. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#endif
    /* Check for --export-maps argument */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--export-maps") == 0) {
#ifdef _WIN32
            _putenv("SOE_EXPORT_ALL_MAPS=1");
#else
            setenv("SOE_EXPORT_ALL_MAPS", "1", 1);
#endif
            argv[i] = (char *)"--no-launcher";
            break;
        }
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--export-music") == 0) {
#ifdef _WIN32
            _putenv("SOE_EXPORT_ALL_MUSIC=1");
#else
            setenv("SOE_EXPORT_ALL_MUSIC", "1", 1);
#endif
            argv[i] = (char *)"--no-launcher";
            break;
        }
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--export-sfx") == 0) {
#ifdef _WIN32
            _putenv("SOE_EXPORT_ALL_SFX=1");
#else
            setenv("SOE_EXPORT_ALL_SFX", "1", 1);
#endif
            argv[i] = (char *)"--no-launcher";
            break;
        }
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--export-spc") == 0) {
#ifdef _WIN32
            _putenv("SOE_EXPORT_ALL_SPC=1");
#else
            setenv("SOE_EXPORT_ALL_SPC", "1", 1);
#endif
            argv[i] = (char *)"--no-launcher";
            break;
        }
    }
#ifdef RECOMP_LAUNCHER
    SoeRegisterLauncherSpcJukebox();
#endif
    return snesrecomp_desktop_main(&kGameHost, argc, argv);
}
