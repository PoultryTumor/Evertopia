# Secret of Evermore PC — Native Recompilation (`Evertopia`)

A native PC static recompilation port of **Secret of Evermore** (SNES, USA), built on [snesrecomp](https://github.com/RetroPortingToolKit/snesrecomp) and [recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui).

> **Legal Notice:** You must legally own a copy of *Secret of Evermore* (USA). **No ROM data, copyrighted game code, or assets are distributed** in this repository or in any release package. Recompiled game C sources are generated locally on your machine from your own verified ROM dump.

---

## What's New

### v0.9.10
- **Swamp / Dark-Surface Widescreen Snap Fix (Map 103 & Ultrawide):** Fixed horizontal screen snapping when standing still or walking near dark playable surfaces such as the Prehistoria Swamp tar pits (Map 103, `$0067`) in 21:9 ultrawide.
  - Tightened the uniform-void RGB555 color threshold in `SoeIsTileUniformVoid()` and `SoeRoomColHasContent()` from `<= 4` to `<= 2`, so true near-black multi-chamber void walls still clamp cleanly while dark surfaces like Swamp tar (`R=3, G=3, B=4`) are never mistaken for void or toggled back and forth by animated tar bubbles.
- **All Platforms Updated (`v0.9.10`):** Rebuilt and updated Linux x64, Android arm64 APK (`versionCode 910`), and Windows x64 Setup Pack releases.

### v0.9.9
- **Parallax Map Edge Widescreen Clamping Fix (Map 56 & All Parallax Maps):** Fixed a bug where walking all the way to the left edge (`X = 0`) or top edge (`Y = 0`) of parallax-scrolling maps (such as Map 56 — Prehistoria Jungle) caused the screen to suddenly snap into a 4:3 pillarbox until walking back away from the edge.
  - When BG1 and BG2 share the same scroll (`0`) on one axis at a map boundary while diverging via parallax on the perpendicular axis, `SoeGetWorldCamScrollLayer()` now falls back to checking the perpendicular axis instead of defaulting to BG1 (the sparse canopy layer), keeping BG2 (the playable ground) selected so the viewport anchors seamlessly in full widescreen.
- **Game Tab Description Cleanup:** Fixed a misaligned footer description array (`kGameDescs`) on the **Game** tab after moving **Ring Menu** to the **Fixes** tab.
- **All Platforms Updated (`v0.9.9`):** Rebuilt and updated Linux x64, Android arm64 APK (`versionCode 909`), and Windows x64 Setup Pack releases.

### v0.9.8
- **Brighter Selected Menu Text Highlight:** Brightened the theme-tinted highlight text color (`sel_text_col`) on the currently selected menu item across all glass themes and the classic blue theme so active selections stand out more clearly and are easier to read.
- **Consistent `On` / `Off` Setting Labels:** Standardized all settings and toggles across the menus from `"ON"` / `"OFF"` to `"On"` / `"Off"`.
- **Moved Ring Menu Setting to Fixes Tab:** Moved the **Ring Menu** (`Enhanced` / `Original`) toggle from the **Game** tab to the **Fixes** tab alongside the other gameplay fixes.
- **Android Touch Overlay Default Opacity (`20%`):** Confirmed `20%` as the default and first-boot opacity for the Android touch controller overlay.
- **All Platforms Updated (`v0.9.8`):** Rebuilt and updated Linux x64, Android arm64 APK (`versionCode 908`), and Windows x64 Setup Pack releases.

### v0.9.7
- **Android Joypad-Only Menu Navigation:** Menus, tabs, Debug Tools, and `"ARE YOU SURE?"` confirmation dialogs on Android are now controlled strictly via joypad input — either a plugged-in/Bluetooth physical controller (matching the Linux/desktop controls) or the on-screen touch controller overlay (`Thumbstick` to move, **`A`** to activate/confirm, **`B`** to close/cancel, **`L`/`R`** to switch tabs, **`LT`** for Slide-Out Menu, **`RT`** for Debug Tools). Direct screen-tap clicking on menu items is disabled on Android to prevent accidental taps.
- **Swipe-to-Restore Overlay Failsafe:** When the touch overlay is turned **Off** and no physical controller is connected, sliding a finger across the screen re-enables the touch overlay and opens the Settings menu so you immediately have joypad navigation back.

### v0.9.6
- **Android Touch Overlay Refinements (`LT`/`RT` Triggers, Opacity Setting & Swipe Failsafe):**
  - Added onscreen **`LT`** (Slide-Out Settings Menu) and **`RT`** (Debug Overlay) back triggers beside `L` and `R` using **Kenney's Input Prompts** (CC0 1.0 Public Domain, [kenney.nl](https://kenney.nl)).
  - Lowered default touch overlay opacity by 50% (to **`20%`** idle opacity) for an even cleaner ghostly float, and added an **`Overlay Opacity`** (`10%`–`100%`) setting in the Android **Game** menu tab (saved to `video_config.json`).
  - Fixed **"ARE YOU SURE?"** `YES` / `NO` confirmation dialogs so they respond directly to touchscreen taps (and mouse clicks), and ensured touches on the slide-out menu drawer pass cleanly through the overlay.
  - Added a **swipe-to-open-menu failsafe**: if the touch overlay is turned **Off** and no physical controller is connected, sliding a finger across the screen opens the Settings menu so you can easily re-enable the overlay.

### v0.9.5
- **Android Translucent Touch Controller Overlay (Android-Only):** Added a built-in SDL3 on-screen touch controller for Android using crisp transparent icons from **Kenney's Input Prompts** (CC0 1.0 Public Domain, [kenney.nl](https://kenney.nl)): 8-way virtual thumbstick (bottom-left), SNES `X / Y / A / B` diamond (bottom-right), `L` / `R` shoulders (upper corners), `Select` / `Start` (bottom-center), and a subtle `Home` menu toggle (top-center) with ghostly float opacity and pressed highlight feedback.
  - Enabled (**On**) by default on first boot, and toggleable anytime in the Android **Game** menu tab (**Touch Overlay: On / Off**, saved to `video_config.json`).
- **Default Glass Theme — Omnitopia Blue:** Updated the default / first-boot dialog box & UI glass theme across all platforms (Linux, Windows, Android) and *Reset All Settings to Default* to **Omnitopia Blue** (the first glass theme).

### v0.9.4
- **Infinite Stamina Fix (Clean Bar & No Sword-Slash Lockout):** Fixed the Infinite Stamina debug toggle so the stamina bar stays completely blank with zero flickering/glitching while running or attacking, and fixed the bug where slashing a sword caused nonstop slashing and locked out player control.
  - Clamped Boy (`$4EB7`) and Dog (`$4F65`) stamina to exact `0x0400` (100% neutral/ready) instead of `0x0404` (which crossed the `>= 0x0401` weapon-charge threshold at `$8F:CE66` and triggered infinite attacks), while still preserving values `> 0x0400` so holding Attack for Level 1–3 weapon charges continues to work.
  - Cleared the `"100%"` HUD linger timers (`$0B4D` / `$0B4F`) at `0x0400` and relocated the runtime running-stamina ROM patch from the mistaken `$8F:BA71` address to the real running-drain routine at `$90:82B2` (with `$90:82A8` forced to LLE in `symbols.toml`).

### v0.9.3
- **Windows x64 Setup Pack Fix (Still Experimental):** Fixed the `ninja: error: unknown target 'Evertopia'` build failure during first-run setup by aligning the desktop CMake target name with the `Evertopia` executable (also shortening Windows `.obj` paths by 17 characters to help avoid `PATH_MAX` warnings). *Tip: on Windows, extract the zip into a short folder path like `C:\Games\Evertopia` to stay well under Windows' 260-character path limit.*
- **Clean Windows Zip Packaging & Rebuild Script:** Removed the accidentally embedded `evertopia-0.1.0-linux-x64.zip` from the repository/Windows pack, fixed the Windows deferred rebuild script (`recomp_deferred_rebuild.cmd`) so `cmake --build` exit codes are no longer masked by PowerShell piping and paths use clean Windows backslashes, enabled `SLICE8` CRC32 by default for all player builds, and defaulted Windows SDL3 renderer selection to `Auto` (prefers Vulkan when available with clean Direct3D/OpenGL fallback).
- **Automated Windows Smoke Test:** GitHub Actions now unpacks the freshly built Windows Setup Pack into a path containing spaces/parentheses and verifies a full configure + `--target Evertopia` build using only the embedded toolchain before publishing. *(Windows remains marked **Experimental** until tested on a physical Windows PC.)*

### v0.9.2
- **Clean Debug Map Warp (Sprites, Palettes & Full BG Load):** Relocated the injected debug warp script out of Secret of Evermore's live `$80:9479..$80:94D8` sprite/OAM attribute table into unused `$81:98E8..$81:98EE` padding, eliminating permanent post-warp sprite/palette corruption across all maps.
- **Uninterrupted Post-Warp Tilemap Upload:** Fixed `[Warp Monitor]` prematurely clearing SNES Forced Blank (`$0106` bit 7) on the first frame after a map ID change; rooms now finish draining VRAM/CGRAM/tilemap/sprite DMA queues and fade in cleanly via `Opcode 0x26` + `SoeRoomTransitionDrainIfReady()` with no black initial viewport.

### v0.9.1
- **Parallax Camera & Foliage Scrolling Fix (Map 65 & All Parallax Maps):** Fixed widescreen margin jumps and overhead foliage judder on parallax-scrolling maps (e.g. Map 65 — Prehistoria Jungle) by locking world-camera layer detection directly to Secret of Evermore's true WRAM camera (`$7E:0059` / `$7E:005B`).
- **Android One-Time Asset Extraction & UI Polish:** On Android, first launch now automatically extracts sprites, tilesets, and all 127 maps with a live **"One-Time Asset Extraction"** progress bar and percentage screen before starting the game, and replaces desktop exit options with **Exit to Android Launcher**.

### v0.9.0
- **Android APK Release (`arm64-v8a`):** Added an Android `.apk` installer (`Evertopia-0.9.0-android-arm64.apk`) with full SDL3 built-in, Bluetooth, and USB game controller support (tested for handhelds like the Mangmi Air X and Android tablets with USB controllers), immersive landscape display, first-boot Android file picker for selecting your legally owned ROM, and the full Evertopia Launcher as the opening screen.
- **Updated Out-of-the-Box Defaults:** All cheats (*Infinite HP*, *Infinite Stamina*, *One-Hit KO*, *Walk Through Walls*) now default to **Off** out of the box (vanilla bug fixes remain **On**); default dialog theme updated to **Ivor Tower**, sprite interpolation defaults to **Off**, desktop defaults to windowed mode (native fullscreen on Android), and Player 1 Gamepad / Player 2 Disabled match standard single-player setup.
- **Map 49 (Podunk 1965 Intro) Parallax Scrolling Fix:** Disabled horizontal widescreen clamping on Map 49 (`$0031`) so both the Podunk street layer and the foreground foliage parallax layer scroll smoothly without stuttering.

### v0.8.0
- **Windows x64 Release (Experimental):** Added an automated Windows x64 Setup Pack (`evertopia-0.8.0-windows-x64.zip`) built natively via GitHub Actions with the portable toolchain embedded — unzip, run `Evertopia.exe`, and select your ROM to build and play locally (if Windows SmartScreen prompts on first run, click *More info → Run anyway*).
- **Map Exporter Fix (Large & Complex Maps):** Fixed garbled PNG exports on large/complex maps (including Great Pyramid Exterior, Gomi's Tower, Jungle South of Fire Eyes' Village, Oglin Caves, Swamp Maze, and Map 113) by waiting for `$7F` decompression, VRAM tileset streaming, CGRAM BG palettes, and screen unblanking to settle before capture, plus SNES-accurate 10-bit tile / 32K-word VRAM wrapping.
- **Multi-Chamber Widescreen Clamping:** Fixed widescreen and ultrawide room-edge clamping on multi-chamber maps that separate rooms with solid flat dark-void BG2 tiles (e.g. Map 60 — Volcano Two Pots Room), eliminating dark void strips past chamber walls.
- **Parallax Layer Widescreen Clamping:** Fixed widescreen and ultrawide boundary clamping on parallax-scrolling maps (e.g. Map 56 — Prehistoria Jungle South of Fire Eyes' Village) by probing the active world-camera layer so overhead BG1 foliage/vines no longer expose black void underneath.

### v0.6.1
- **Launcher SPC Ripper & Jukebox:** Added an SPC ripper and interactive Jukebox player directly in the Evertopia launcher alongside bug fixes.

### v0.6.0
- **Initial Linux Setup Pack Release:** Native `snesrecomp` PC port with true 16:9 / 21:9 widescreen, extended Mode 4 stone-mosaic menus, CRT/Bloom shaders, custom glass dialog themes, save-state & rewind browsers, and per-map pillarbox editor.

---

## What This Is

Instead of running inside a traditional emulator loop, **Evertopia** statically recompiles the USA SNES ROM into native C compiled directly against the `snesrecomp` beam-aligned PPU/APU runtime and SDL3/OpenGL presenter. This allows native enhancements like true 16:9 / 21:9 widescreen rendering, zero sprite flicker limits, custom glass dialog themes, CRT/Bloom post-processing, save-state/rewind browsers, and live debug utilities while preserving authentic SNES gameplay logic and SPC700 audio.

---

## Verified ROM Identity

Only the standard **USA** release of *Secret of Evermore* is accepted (verified automatically by CRC32 / SHA-256 at build time and in the launcher):

| Property | Value |
|---|---|
| File | `evermore.smc` |
| Region | USA (NTSC) |
| Mapping | HiROM (`3,145,728` bytes unheadered; 512-byte SMC header stripped automatically if present) |
| CRC32 | `a5c0045e` |
| SHA-256 | `17c864a76d498feb6479eee8e7d6807b951c66225033228622bb66754baab1db` |

---

## What's Added Over Vanilla SNES

### Widescreen & Visual Upgrades
- **True Widescreen (`16:9` & `21:9` Ultrawide, plus `4:3`):**
  - Streams BG1/BG2 room tilemaps directly from WRAM `$7F` for native `342×224` (`16:9`) and `426×224` (`21:9`) output.
  - Expands enemy/object culling and alchemy targeting bounds across the widened view.
  - Dynamically clamps room borders cleanly and wraps horizontal 48-tile intro/title backdrops (`Map 17` & `Map 97`) seamlessly without right-edge black bars.
- **Smart HUD Repositioning:**
  - Shifts the bottom Boy/Dog status bars and portraits cleanly into the widescreen margins (`Move Left Margin`) or keeps `Original 4:3 Center`.
  - Automatically detects single-character segments (e.g., Nobilia splits and solo areas) so lone HUD bars and portraits remain cleanly centered.
- **Extended Mode 4 Stone-Mosaic Menus:**
  - Widens **Save Select / Load / Name Entry** and all six Ring Menu submenus (**Level**, **Stat.**, **Act.**, **Edit.**, **Edit.**, **Alchemy**) with seamless stone tiling and a subtle black-to-transparent outer edge vignette, decoupled from `Map 97`'s Title Screen pillarbox setting.
- **Per-Map Pillarbox Editor (`Pillarbox Edit Mode`):**
  - Off by default at launch so gameplay stays clean. Enable **`Pillarbox Edit Mode`** in the **Debug Tools** overlay (`RT` / `F2` / `` ` ``) to unhide the upper-right **`Map X: [0/1]`** badge, dropdown picker (`[0] 4:3 Pillar` / `[1] Widescreen`), and `0`/`1` quick-toggle keys. Your toggle state saves to `video_config.json`, and per-map changes persist to `map_pillarbox.json`.
- **Per-Line Sprite Limits Removed:**
  - Removes the SNES 32-sprite-per-scanline flicker cap by default (toggleable back to `Standard SNES (32)`).
- **Lock Framerate to 60 FPS:**
  - Enabled by default under **Video** settings (locks real-time pacing to the SNES NTSC rate; toggleable to `Unlocked`).

### Programmatic Glass Themes & Post-Processing
- **Dialog & Menu Glass Themes (`dialog_config.json`):**
  - Choose between **8 translucent Glass themes** (*Omnitopia Blue*, *Volcano Orange*, *Swamp Green*, *Nobilia Gold*, *Ebon Keep Purple*, *Crustacea Teal*, *Colosseum Red*, *Ivor Tower Slate*) or **Original (SNES)**.
  - Includes configurable drop shadows (`Subtle`, `Medium`, `Off`). Both the slide-out **Settings** drawer and floating **Debug** window follow the active theme, and in-game **Window Prefs** previews themes live.
- **CRT Emulation (`crt_config.json`):**
  - Dedicated **CRT** tab with presets (*Off*, *Just Scanlines*, *Fine 1px*, *Deep*, *Subtle*, *Classic*, *Solid Retro*, *Trinitron*, *Custom*) plus individual controls for scanline density, opacity, beam thickness, phosphor masks, luminance boost, and vignette.
- **Bloom / Glow (`bloom_config.json`):**
  - Dedicated **Bloom** tab with curated presets (*Default*, *Subtle*, *Soft Focus*, *Warm Sun*, *Mystic Cyan*, *Neon*, *Intense*, *Off*) plus intensity, threshold, spread radius, blend mode (*Screen*, *Additive*, *Soft Light*), and color tint.

### Vanilla Bug Fixes & Quality of Life (Enabled by Default)
Toggleable individually in the **Game** tab:
- **Silver Sheath Attack Boost Fix**
- **Atlas / Defend Stat Underflow Fix**
- **Dog Level 39+ Progression Fix** (fixes Dog EXP curve past Lv 39 up to 99)
- **Boundary Softlock Prevention**
- **Toaster Dog Audio Loop Guard:** Prevents infinite/stuck `SFX $0048` sound loops when using the Toaster Dog form.
- **Exit to Desktop:** At the bottom of the **Game** tab (highlighted in red) to explicitly flush all config files and battery SRAM before exiting.

### Launcher & Tools
- **ROM Details Panel:** Displays ROM Title, Header status (`Headered 512B SMC` vs `Unheadered`), Country/Region, ROM Mapping (`HiROM`/`LoROM`, `FastROM`/`SlowROM`), ROM/SRAM size, and Revision/Checksum right below **Browse For ROM**.
- **Startup Checkboxes:** **Skip launcher on boot** and **Start Full Screen** (persists borderless fullscreen immediately to `config.ini`).
- **Save Manager:** Import or clear battery SRAM (`saves/save.srm`).
- **All-Map Exporter:** Export all 127 maps cleanly as PNGs into `./MAPS/` via **Export All Maps to MAPS/** in the launcher (or `--export-maps` on the CLI).
- **Floating Debug Overlay (`RT` / `F2` / `` ` ``):** **Give Everything** (with irreversible confirmation warning: maxes all 4 era currencies, Lv99/EXP/HP/combat stats, all 13 weapons + Lv3 skill + Bazooka ammo, all body armors/helmets/bracers/rings/collars, all 35 alchemy spells + 99 ingredients, consumables/Call Beads, all 14 charms, and all 13 trade goods), Infinite HP, Infinite Stamina, One-Hit KO (999), Walk Through Walls, Dog Form switcher (*Wolf*, *Greyhound*, *Poodle*, *Toaster*, *Spot*), Map Warp (`0..255`), **Pillarbox Edit Mode**, and **Jukebox** (sub-window to the left of Debug Tools to cycle and play all 69 valid soundtrack tracks with `A` / close with `B`).

---

## Controls, Hotkeys & Reopening the Launcher

### Default Gameplay Controls
Keyboard bindings can be edited in `keybinds.ini` or configured in the launcher.

| Action | Keyboard Default (`keybinds.ini`) | Gamepad |
|---|---|---|
| D-Pad / Move | `Arrow Keys` | D-Pad / Left Stick |
| Talk / Action (`A`) | `X` | `A` (South) |
| Run / Attack (`B`) | `Z` | `B` (East) |
| Menu / Swap Character (`X`) | `S` | `X` (North) |
| Ring Menu (`Y`) | `A` | `Y` (West) |
| `L` / `R` | `C` / `V` | `LB` / `RB` |
| `Start` / `Select` | `Enter` / `Right Shift` | `Start` / `Back` |

### Settings, Debug & Host Shortcuts
| Shortcut | Action |
|---|---|
| `F1` / `Esc` / `LT` (Pad) | Toggle **Settings Slide-Out Menu** (`LB`/`RB` or `[`/`]` switches tabs; `B` on pad or `Z`/`Backspace`/`Esc` closes; mouse supported) |
| `F2` / `` ` `` (Grave) / `RT` (Pad) | Toggle **Debug Tools Overlay** (`B` on pad or `F2`/`Esc` closes) |
| `Alt + Enter` | Toggle **Borderless Fullscreen** |
| `Tab` | Hold **Turbo** (Fast-Forward) |
| `F11` / `Select + R` (Pad) | Open **Save-State Browser** (`F2`–`F10` quick load, `Shift+F1`–`F10` quick save) |
| `F12` / `Select + R3` (Pad) | Open **Rewind Filmstrip** |
| `Ctrl + Up` / `Ctrl + Down` | Increase / decrease window scale |
| `Ctrl + R` | Reset game |
| `Ctrl + S` | Capture full-frame `.png` screenshot to `./Screenshots/` (3s cooldown) |
| `P` / `Shift + P` | Pause (dimmed) / Pause |
| `Keypad +` / `Keypad -` | Master volume up / down |
| `F` / `R` | Toggle FPS readout / toggle renderer |

### Reopening the Launcher If "Skip launcher on boot" Is Enabled
Once **Skip launcher on boot** is checked, future launches boot straight into the game. To open the launcher again:
1. Run from a terminal or shortcut with **`--launcher`**:
   ```sh
   ./Evertopia --launcher
   ```
   (or `./build/Evertopia --launcher`), then uncheck **Skip launcher on boot**; **OR**
2. Open `config.ini` next to the executable and set `SkipLauncher = 0` under `[General]`.

### Command-Line Flags & Saved Files
- **CLI Flags:** `--launcher`, `--no-launcher`, `--rom <path>`, `--config <path>`, `--export-maps`, `--paused`, `--script <path>`, `--framedump <dir>`, `--help`
- **Generated Config / Save Files:** `config.ini`, `keybinds.ini`, `rom.cfg`, `saves/save.srm`, `video_config.json`, `crt_config.json`, `bloom_config.json`, `dialog_config.json`, `map_pillarbox.json`

---

## Building from Source

Install OpenGL development headers if not already present (e.g. `sudo dnf install libglvnd-devel mesa-libGL-devel` on Fedora/Nobara, or `sudo apt install libgl-dev` on Debian/Ubuntu), then:

```sh
git submodule update --init --recursive
bash tools/regen.sh --rom /path/to/evermore.smc
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run `./build/Evertopia` (or copy it beside your `evermore.smc`).

---

## Special Thanks & Acknowledgements

- **EddieEddie90** — Special thanks for coming up with the name **Evertopia**!
- **Square Soft / Square USA (Redmond, WA) & Nintendo** — Special thanks to everyone involved in creating **Secret of Evermore** (1995): the designers, programmers, artists, writers, localization team, and **Jeremy Soule** for the unforgettable soundtrack.
- **Matthew Stanley & Contributors** — Creators of [snesrecomp](https://github.com/RetroPortingToolKit/snesrecomp) and [recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui), which power the static recompiler, beam-aligned PPU/runtime, and launcher.
- **Kenney** ([www.kenney.nl](https://kenney.nl)) — Creator of **Kenney's Input Prompts** (CC0 1.0 Public Domain), used for the crisp translucent on-screen controller buttons and thumbstick graphics in the Android touch overlay.
- **snesrev** (`zelda3` / `smw`), **LakeSnes** (`elzo_d` 65816 CPU core), **xander-haj/Z3R**, **SDL3**, **Dear ImGui**, and the SNES preservation and reverse-engineering community.

---

## License

- **Project Source (`SecretOfEvermoreSNESRecomp` & `snesrecomp`):** Licensed under the **PolyForm Noncommercial License 1.0.0** — see [`LICENSE`](LICENSE) and [`snesrecomp/PERMISSIONS.md`](snesrecomp/PERMISSIONS.md). *(Note: Because the underlying `snesrecomp` framework uses PolyForm Noncommercial 1.0.0 rather than GPL, this project keeps PolyForm Noncommercial 1.0.0 alongside its upstream MIT notices.)*
- **Third-Party MIT Components:** `recomp-ui` ([`recomp-ui/LICENSE`](recomp-ui/LICENSE)) and retained upstream `snesrev` / `LakeSnes` SNES hardware core files are licensed under the **MIT License** — see [`snesrecomp/THIRD_PARTY_ATTRIBUTION.md`](snesrecomp/THIRD_PARTY_ATTRIBUTION.md).
- **Game Data:** Not covered by any license above; **no ROM or copyrighted Square / Nintendo assets are included**.
