# Secret of Evermore PC — Native Recompilation (`Evertopia`)

A native PC static recompilation port of **Secret of Evermore** (SNES, USA), built on [snesrecomp](https://github.com/RetroPortingToolKit/snesrecomp) and [recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui).

> **Legal Notice:** You must legally own a copy of *Secret of Evermore* (USA). **No ROM data, copyrighted game code, or assets are distributed** in this repository or in any release package. Recompiled game C sources are generated locally on your machine from your own verified ROM dump.

---

<img width="2560" height="1440" alt="Screenshot from 2026-11-18 00-39-09" src="https://github.com/user-attachments/assets/409ad5b3-8b0f-42a9-8ade-b74cc839ee71" /><img width="2560" height="1440" alt="Screenshot from 2026-11-18 00-42-56" src="https://github.com/user-attachments/assets/63b45fe4-e63e-4d1e-a9ad-8ec9fd34680f" />
<img width="2560" height="1440" alt="Screenshot from 2026-11-18 00-41-33" src="https://github.com/user-attachments/assets/0398ae72-850e-4d28-90aa-acc446172ffb" />
<img width="2560" height="1440" alt="Screenshot from 2026-11-18 00-32-38" src="https://github.com/user-attachments/assets/d02daaa0-336d-4145-836b-9aafc9b04a40" />
<img width="1280" height="720" alt="Screenshot from 2026-11-15 06-31-43" src="https://github.com/user-attachments/assets/1f580559-ed46-4238-a2bf-7c2bf86a77e4" />
<img width="2560" height="1440" alt="Screenshot from 2026-11-13 05-20-49" src="https://github.com/user-attachments/assets/78c5bb1b-94ca-40f7-b7ac-463211e8a3e9" />



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
- **snesrev** (`zelda3` / `smw`), **LakeSnes** (`elzo_d` 65816 CPU core), **xander-haj/Z3R**, **SDL3**, **Dear ImGui**, and the SNES preservation and reverse-engineering community.

---

## License

- **Project Source (`SecretOfEvermoreSNESRecomp` & `snesrecomp`):** Licensed under the **PolyForm Noncommercial License 1.0.0** — see [`LICENSE`](LICENSE) and [`snesrecomp/PERMISSIONS.md`](snesrecomp/PERMISSIONS.md). *(Note: Because the underlying `snesrecomp` framework uses PolyForm Noncommercial 1.0.0 rather than GPL, this project keeps PolyForm Noncommercial 1.0.0 alongside its upstream MIT notices.)*
- **Third-Party MIT Components:** `recomp-ui` ([`recomp-ui/LICENSE`](recomp-ui/LICENSE)) and retained upstream `snesrev` / `LakeSnes` SNES hardware core files are licensed under the **MIT License** — see [`snesrecomp/THIRD_PARTY_ATTRIBUTION.md`](snesrecomp/THIRD_PARTY_ATTRIBUTION.md).
- **Game Data:** Not covered by any license above; **no ROM or copyrighted Square / Nintendo assets are included**.
