# gm14-x86_64

**`gm14-x86_64` is the dedicated PC (Windows & Linux x86_64) runner and verification test suite for GameMaker: Studio 1.4 games (`data.win`).**

It provides 1:1 GameMaker: Studio 1.4 runtime parity on desktop platforms, serving as both a standalone desktop runner and the gold-standard test harness for verifying game logic, asset parsing, and bytecode execution before deploying to console targets like [`gm14-3ds`](https://github.com/Dxrmy/gm14-3ds). Platform-agnostic core logic is maintained in [`gm14`](https://github.com/Dxrmy/gm14).

---

## Overall Implementation Progress

| Subsystem | Progress Bar | Status | Coverage / Details |
|---|---|---|---|
| **`data.win` Chunk & Asset Parsing** | `[██████████]` 100% | **Complete** | All 24 chunks parsed (FORM, GEN8, OPTN, SOND, SPRT, BGND, SCPT, FONT, OBJT, ROOM, DAFL, TPAG, CODE, VARI, FUNC, STRG, TXTR, AUDO, PATH, ...) |
| **Bytecode Virtual Machine** | `[█████████░]` 96% | **In Progress** | 891k ops across 6,272 code entries, stack machine, scopes, user script shadowing, compound array operations |
| **Automated Unit Test Suite** | `[██████████]` 100% | **Complete** | **181 / 181 passing unit tests** across all core runtime subsystems |
| **Graphics & Compositor** | `[█████████░]` 92% | **In Progress** | Origin-pivoted sprite rotations, CCW conventions, blend modes (`bm_add`), tile atlas UV mapping & depth layering |
| **Path Traversal & Kinematics** | `[█████████░]` 95% | **In Progress** | `PATH` interpolation, entrance marker placement, directional heading calculation, path action handling |
| **Audio & Music Mixer** | `[█████████░]` 95% | **In Progress** | Multi-voice mixer, native WaveOut/ALSA, `stb_vorbis` decoding, GM `audio_*` and Undertale `caster_*` engines |
| **Battle Engine & Menu Flow** | `[█████████░]` 90% | **In Progress** | Battle controller, FIGHT / ACT / ITEM / MERCY navigation, monster setup, damage computation, target cursors |
| **Overworld & Dialog Engine** | `[█████████░]` 92% | **In Progress** | C-menu item/stat screens, multi-page typewriter text rendering (`OBJ_WRITER`), camera tracking, room transitions |
| **Memory Budgets & Config (`gm14.ini`)** | `[██████████]` 100% | **Complete** | Explicit INI config with bounded LRU texture cache, bounded PCM audio budget, watchdog, and culling margins |
| **Target & Emulator Parity** | `[█████████░]` 95% | **In Progress** | Win32 desktop runner (`gm14_runner.exe`), Steam deployment, Azahar / Citra 3DS emulation parity |

---

## Hardware & Target Profiles

The engine architecture provides platform-tailored memory and performance profiles configurable via `gm14.ini`:

| Feature / Setting | Nintendo 3DS (Old) | Nintendo 3DS (New) | Low-End PC / Raspberry Pi | Desktop PC (x86_64) |
|---|---|---|---|---|
| **Max Texture Pages** | `2` (~8 MB VRAM) | `4` (~16 MB VRAM) | `6` (~24 MB RAM) | `16` or `0` (unlimited) |
| **PCM Audio Budget** | `8 MB` | `16 MB` | `16 MB` | `64 MB` |
| **Instruction Watchdog** | `250,000` steps | `500,000` steps | `1,000,000` steps | `2,000,000` steps |
| **Frame Pacing** | 2-VBlank sync (30 FPS) | 2-VBlank sync (30 FPS) | Sleep / Timer (30/60 FPS) | QPC Timestep (30/60 FPS) |
| **Audio Backend** | `ndsp` / CSND | `ndsp` / CSND | ALSA | Win32 WaveOut / ALSA |
| **Tile Culling Margin** | `16 px` | `24 px` | `32 px` | `48 px` |

---

## Configuration Guide (`gm14.ini`)

The runtime does not rely on hidden hardcoded heuristics; all memory limits, safety margins, and performance throttles are configured through `gm14.ini`.

### Where to Place `gm14.ini`

- **Windows / Linux PC (`gm14-x86_64` & `gm14`):** Place `gm14.ini` in the same directory as the executable (`gm14_runner.exe`, `vm_tests.exe`, or `gm14_host`).
- **Nintendo 3DS (`gm14-3ds` on hardware):** Place `gm14.ini` on the SD card at `sdmc:/3ds/gm14/gm14.ini` alongside `gm14.3dsx` and `data.win`.
- **Azahar / Citra 3DS Emulator:** Place `gm14.ini` in Azahar's virtual SDMC directory (`%APPDATA%\Azahar\sdmc\3ds\gm14\gm14.ini`) or next to the loaded `.elf` / `.3dsx`.

### How to Create or Edit `gm14.ini`

Copy the included template file to create your active configuration:

```bash
# On Windows (PowerShell or CMD)
copy gm14.ini.example gm14.ini

# On Linux / macOS
cp gm14.ini.example gm14.ini
```

You can open and edit `gm14.ini` in any text editor (Notepad, VS Code, nano, etc.).

### Detailed Setting Reference

```ini
[engine]
; --------------------------------------------------------------------------
; 1. max_texture_pages
; --------------------------------------------------------------------------
; Maximum number of 2048x2048 / 1024x1024 32-bit RGBA texture sheets cached
; in RAM simultaneously using an LRU (Least-Recently-Used) eviction policy.
; - 2  : ~8 MB VRAM budget (essential for Old Nintendo 3DS)
; - 4  : ~16 MB VRAM budget (ideal for New Nintendo 3DS)
; - 6  : ~24 MB RAM budget (ideal for Low-End Laptops / Raspberry Pi)
; - 16 : ~64 MB RAM budget (recommended for modern PC systems)
; - 0  : Unlimited (loads all sheets into RAM; only for machines with plenty of RAM)
; Tuning: Lower this value if the game runs out of memory or crashes during room
; transitions. Raise this value to prevent texture reload pauses.
max_texture_pages = 6

; --------------------------------------------------------------------------
; 2. pcm_cache_budget_mb
; --------------------------------------------------------------------------
; Total memory cap in Megabytes for pre-decoded uncompressed PCM sound samples.
; - 8  : Old 3DS (conserves heap space for game logic)
; - 16 : Low-End Laptops / Raspberry Pi / New 3DS
; - 64 : Desktop PC (keeps all active sound effects instantaneously ready in RAM)
; Tuning: Lower on low-RAM devices; raise if you want zero disk latency when
; triggering repetitive sound effects.
pcm_cache_budget_mb = 16

; --------------------------------------------------------------------------
; 3. instruction_budget
; --------------------------------------------------------------------------
; Watchdog instruction step ceiling per step/event. If a GML script or loop
; executes more bytecode instructions than this threshold in a single frame,
; the engine aborts the loop to prevent freezing the system.
; - 250000  : Old 3DS (safely breaks runaway loops on slower CPUs)
; - 1000000 : Low-End PC / Raspberry Pi
; - 2000000 : Modern PC (handles heavy room initialization scripts smoothly)
; Tuning: Raise if legitimate procedural loops or room setups trip the watchdog.
instruction_budget = 2000000

; --------------------------------------------------------------------------
; 4. culling_margin
; --------------------------------------------------------------------------
; Frustum safety margin in pixels added around the camera viewport.
; Tiles and instances outside this bounding box are culled from rendering.
; - 16 : Tight culling for Old 3DS (saves PICA200 draw calls)
; - 24 : New 3DS
; - 32 : Low-End PC / Raspberry Pi (balances draw calls and smooth scrolling)
; - 48 : Modern PC (eliminates any edge popping on high-res displays)
culling_margin = 32

; --------------------------------------------------------------------------
; 5. enable_blend_modes
; --------------------------------------------------------------------------
; Controls hardware/software additive blend mode support (`bm_add`).
; - 1 : Enabled (full visual accuracy for magic projectiles, glow, dust)
; - 0 : Disabled (falls back to standard alpha blending; for ultra-low CPUs)
enable_blend_modes = 1

; --------------------------------------------------------------------------
; 6. fps
; --------------------------------------------------------------------------
; Frame rate pacing override.
; - 0  : Use native GameMaker room_speed (e.g. 30 FPS for Undertale)
; - 30 : Lock strictly to 30 FPS
; - 60 : Lock strictly to 60 FPS (for 60 FPS GM titles)
fps = 30

; --------------------------------------------------------------------------
; 7. save_directory
; --------------------------------------------------------------------------
; Directory path where INI files (`undertale.ini`) and text saves (`file0`,
; `file9`) are written.
; - ./saves                 : Standard relative path on PC
; - sdmc:/3ds/gm14/saves/   : Dedicated SD card path on Nintendo 3DS
save_directory = ./saves

; --------------------------------------------------------------------------
; 8. verbose_logging
; --------------------------------------------------------------------------
; Diagnostic console logging.
; - 0 : Disabled (maximum performance)
; - 1 : Enabled (logs opcode execution, room transitions, and instance events)
verbose_logging = 0
```

### Ready-to-Use Configuration Presets

#### Preset: Low-End Laptop / Raspberry Pi (Low Memory)
```ini
[engine]
max_texture_pages = 6
pcm_cache_budget_mb = 16
instruction_budget = 1000000
culling_margin = 32
enable_blend_modes = 1
fps = 30
save_directory = ./saves
verbose_logging = 0
```

#### Preset: Modern Desktop PC (High Performance)
```ini
[engine]
max_texture_pages = 16
pcm_cache_budget_mb = 64
instruction_budget = 2000000
culling_margin = 48
enable_blend_modes = 1
fps = 30
save_directory = ./saves
verbose_logging = 0
```

#### Preset: Nintendo 3DS (Hardware / Azahar Emulator)
```ini
[engine]
max_texture_pages = 2
pcm_cache_budget_mb = 8
instruction_budget = 250000
culling_margin = 16
enable_blend_modes = 1
fps = 30
save_directory = sdmc:/3ds/gm14/saves/
verbose_logging = 0
```

---

## Features

- **100% Passing Test Suite**: **181 unit tests** validating opcode execution, variable chains, array references, type conversions, math, string manipulation, INI persistence, data structures (`ds_grid`, `ds_list`, `ds_map`), surfaces, path traversal, marker entrance placement, and audio playback.
- **Configurable Engine Architecture (`gm14.ini`)**:
  - Fully configurable via explicit `gm14.ini` (no hidden platform auto-detection heuristics).
  - **Bounded LRU Texture Cache**: Configurable limit of cached texture sheets (default 6 pages / ~24 MB on low-spec PC/Pi, 2 pages / ~8 MB on 3DS). Eliminates thrashing and out-of-memory errors.
  - **Bounded PCM Audio Budget**: Configurable sound buffer budget (default 16 MB on PC/Pi, 8 MB on 3DS).
  - **Watchdog Instruction Budget**: Configurable execution step watchdog (default 2,000,000 steps).
  - **Safety Margins**: Configurable culling margin (default 32 px) and additive blend modes (`bm_add`).
- **Interactive Native PC Runner (`gm14_runner.exe`)**:
  - Native Win32 window loop with fixed-timestep game pacing (30 FPS default for Undertale).
  - Double-buffered sharp nearest-neighbor GDI blitting (`StretchDIBits`) with letterbox/pillarbox aspect-ratio preservation.
  - Real-time keyboard mapping (Arrows, WASD, Z/Enter, X/Shift, C/Ctrl, Esc).
  - Dynamic fullscreen toggling via `F4` or `Alt+Enter`.
  - Headless execution and PNG frame capture for continuous integration and automated testing.
- **1:1 Bytecode VM & Engine Systems**:
  - **Bytecode v15, v16 & v17 Support**: Full stack machine supporting modern GameMaker bytecode versions and chunk formats.
  - **Path Traversal Engine**: Complete GameMaker path system (`path_start`, `path_end`, `path_position`, `path_speed`, etc.) with `PATH` chunk parsing, directional heading calculation, and two-pass entrance marker placement.
  - **Accurate Sprite Transformations**: Origin-pivoted rotation for `draw_sprite_ext`, `draw_self`, and default instance drawing, matching GameMaker's counter-clockwise angle convention (fixing spinning entities such as `obj_winkstar`).
  - **Blend Mode Pipeline**: Support for `draw_set_blend_mode`, `draw_set_blend_mode_ext`, and `draw_get_blend_mode` with software additive blending (`bm_add`).
  - **Compressed Texture Decoding**: Native support for QOI (`qoi.hpp`) and BZip2 (`bunzip.hpp`) compressed texture sheet formats.
  - **Script Shadowing**: User scripts shadow builtins (`control_*`, `draw_*`), preserving custom engine logic.
  - **Dynamic StackTop Resolution**: Full support for `inst_type == -9` / `ref_type == 0x80` stack-popped instance targets in variable reads/writes.
  - **Object 0 De-aliasing**: Proper separation of instance `self` (`-1`) from object index 0 (`dw.objects[0]`).
  - **Lifecycle & Events**: Complete support for Create (`0,0`), Destroy (`1,0`), Alarm (`2,i`), Step (`3,sub`: Begin/Step/End), Draw (`8,sub`: Pre-Draw, Draw Begin, Draw, Draw End, Post-Draw, GUI), and Collision (`4,target`) events.
  - **Motion & Animation**: Automated `xprevious`/`yprevious` tracking, `hspeed`/`vspeed` motion integration, friction, and `image_index += image_speed` animation stepping.
  - **Bounding Box & Scaling**: Accurate `bbox_of` collision calculation respecting sprite margins (`ml`, `mr`, `mt`, `mb`), origin offsets, and instance scale (`image_xscale`, `image_yscale`).
- **Complete Audio & Music Engine**:
  - Native Windows **WaveOut** output backend (`-lwinmm`) and Linux **ALSA** backend.
  - Full support for GameMaker audio (`audio_play_sound`, `audio_sound_gain`, `audio_sound_pitch`, `audio_stop_sound`, `audio_is_playing`, etc.).
  - Undertale **`caster_*`** audio engine (`caster_load`, `caster_play`, `caster_loop`, `caster_stop`, `caster_pause`, `caster_set_volume`, `caster_set_pitch`, `caster_is_playing`, etc.) with filesystem lookup for external music files (`mus_*.ogg`).
  - Embedded Ogg Vorbis and WAV decoder via `stb_vorbis`.
  - Multi-voice software mixer with volume gain, pitch shift, looping, and pause/resume.
- **Save System & Sandbox**:
  - Full INI file persistence (`ini_open`, `ini_read_real`, `ini_read_string`, `ini_write_real`, `ini_write_string`, `ini_key_exists`, `ini_section_exists`, `ini_close`).
  - Sequential text file I/O (`file_text_open_*`, `file_text_read_*`, `file_text_write_*`, `file_text_close`).
- **Surfaces & 2D Drawing Pipeline**:
  - Full RGBA surface creation, destruction, stacking, and target redirection (`surface_create`, `surface_set_target`, `surface_reset_target`, `surface_free`, `draw_surface`, `draw_surface_ext`, `draw_clear_alpha`).
  - 2D primitives (`draw_rectangle`, `draw_rectangle_colour`, `draw_line`, `draw_line_width`, `draw_set_color`, `draw_set_alpha`).
- **High-Fidelity Room Renderer**:
  - Corrected tile texture atlas UV mapping (`tp->sx + tl->srcx`, `tp->sy + tl->srcy`).
  - Independent horizontal and vertical background tiling (`tile_x` vs `tile_y`).
  - Instance scale transformations, flipping, depth-sorted rendering, and color modulation.

---

## Undertale 1.0.0.1539 Compatibility

Verified directly against full Undertale `data.win` (bytecode v16):

```
Chunks parsed        : 24 (FORM, GEN8, OPTN, SOND, SPRT, BGND, SCPT, FONT, OBJT, ROOM, DAFL, TPAG, CODE, VARI, FUNC, STRG, TXTR, AUDO, PATH, ...)
Strings              : 63,977
Bytecode ops         : 891,526 instructions across 6,272 code entries
Variables / Functions: 10,899 / 427 (206,598 var refs, 78,929 func refs)
Sprites / Objects    : 2,583 / 1,709
Rooms                : 336
Unit tests           : 181 passed, 0 failed
```

---

## Contents

| File | Purpose |
|---|---|
| `main_runner.cpp` | Interactive playable Win32 desktop runner (`gm14_runner.exe`) |
| `vm.cpp` / `vm.hpp` | Virtual machine, opcode dispatcher, instance pool, event loop, builtins |
| `dw.cpp` / `dw.hpp` | `data.win` IFF parser, chunk unpacker, room compositor |
| `audio.cpp` / `audio.hpp` | Audio engine, Windows WaveOut / ALSA backends, mixer, LRU cache |
| `bunzip.cpp` / `bunzip.hpp` | Lightweight BZip2 decompressor for compressed assets |
| `qoi.hpp` | QOI texture decoding header |
| `gm14.ini.example` | Template engine configuration for memory and performance tuning |
| `vm_tests.cpp` | Comprehensive 181-test test suite |
| `vm_host.cpp` | Headless VM host runner (boots game, steps rooms, outputs state) |
| `main_host.cpp` | DataWin inspector CLI (dumps stats and renders rooms to PNG) |
| `stb_image.h` / `stb_image_write.h` | Image decoding and PNG writing |
| `stb_vorbis.c` / `stb_vorbis_impl.c` | Ogg Vorbis audio decoding |
| `Makefile` | MinGW-w64 (Windows) and GCC/Clang (Linux) build file |
| `CMakeLists.txt` | Cross-platform CMake configuration |

---

## Controls & Hotkeys

| Key | Function | GameMaker Equivalent |
|---|---|---|
| **Arrow Keys / WASD** | Move character / Navigate menus | `vk_left`, `vk_right`, `vk_up`, `vk_down` |
| **Z / Enter** | Confirm / Interact | `ord("Z")`, `vk_enter`, `gp_face1` |
| **X / Shift** | Cancel / Run / Skip | `ord("X")`, `vk_shift`, `gp_face2` |
| **C / Ctrl** | Menu / Item / Cell screen | `ord("C")`, `vk_control`, `gp_face3` |
| **F4 / Alt+Enter** | Toggle Fullscreen | Hardware display mode toggle |
| **[ / ]** | Slow down / Speed up (±0.5x) | Engine speed modifier |
| **\\** | Reset speed to 1.0x | Engine speed modifier |
| **F1** | Unfreeze player (`global.interact = 0`) | Debug shortcut |
| **F2** | Dump room & player coordinates | Debug diagnostics |
| **PageUp / PageDown** | Skip rooms backward / forward | Debug room navigation |
| **Home** | Reload current room | Debug reset |

---

## Building

### Windows (MinGW-w64)

```bash
mingw32-make -j24
```

### Linux (GCC / Clang)

```bash
make -j24
```

### CMake (Windows / Linux / macOS)

```bash
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

---

## Running

### Run the Test Suite

```bash
./vm_tests path/to/data.win
```

### Run the Interactive PC Runner

```bash
./gm14_runner.exe --data "path/to/data.win"
```

### Run Headless Verification

Step Undertale headless for 60 frames and save output:

```bash
./gm14_runner.exe --data "path/to/data.win" --headless 60 --dump-frame out.png
```

---

## Related Projects

- **[Dxrmy/gm14](https://github.com/Dxrmy/gm14)** — Platform-agnostic core runtime and VM reference.
- **[Dxrmy/gm14-3ds](https://github.com/Dxrmy/gm14-3ds)** — Nintendo 3DS hardware and Azahar emulator port.

---

## License

MIT License — see [`LICENSE`](LICENSE).
