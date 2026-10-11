# gm14-x86_64

**`gm14-x86_64` is the dedicated PC (Windows & Linux x86_64) runner and test suite for GameMaker: Studio 1.4 games (`data.win`).**

It provides 1:1 GameMaker: Studio 1.4 runtime parity on desktop platforms, serving as both a standalone desktop runner and the gold-standard test harness for verifying game logic, asset parsing, and bytecode execution before deploying to console targets like [`gm14-3ds`](https://github.com/Dxrmy/gm14-3ds).

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
  - Real-time keyboard mapping (Arrows, Z/Enter, X/Shift, C/Ctrl, Esc).
  - Dynamic fullscreen toggling via `F4` or `Alt+Enter`.
  - Headless execution and PNG frame capture for continuous integration and automated testing.
- **1:1 Bytecode VM & Engine Systems**:
  - **Bytecode v15, v16 & v17 Support**: Full stack machine supporting modern GameMaker bytecode versions and chunk formats.
  - **Path Traversal Engine**: Complete GameMaker path system (`path_start`, `path_end`, `path_position`, `path_speed`, etc.) with `PATH` chunk parsing and two-pass entrance marker placement.
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

## Configuration (`gm14.ini`)

Copy `gm14.ini.example` to `gm14.ini` to customize runtime settings:

```ini
[engine]
; Maximum texture sheets kept in RAM (2 on 3DS, 6 on low-spec/PC/Pi, 0 = unlimited)
max_texture_pages = 6
; Audio PCM cache budget in MB (8 MB on 3DS, 16 MB on low-spec/PC)
pcm_cache_budget_mb = 16
; Bytecode loop execution watchdog budget
instruction_budget = 2000000
; Safety margin in pixels for tile frustum culling
culling_margin = 32
; Enable additive blend modes (bm_add)
enable_blend_modes = 1
; Target FPS (0 = default room_speed)
fps = 30
; Save directory
save_directory = ./saves
; Diagnostic sink logging
verbose_logging = 0
```

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

Output:
```
[1/10] Opening data.win...
...
Game data loaded successfully!
  [info] embedded OGG sounds decoded: 5

181 passed, 0 failed
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

- **[Dxrmy/gm14](https://github.com/Dxrmy/gm14)** — Core portable runtime.
- **[Dxrmy/gm14-3ds](https://github.com/Dxrmy/gm14-3ds)** — Nintendo 3DS hardware port.

---

## License

MIT License — see [`LICENSE`](LICENSE).
