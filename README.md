# gm14-x86_64

**`gm14-x86_64` is the dedicated PC (Windows & Linux x86_64) runner and test suite for GameMaker: Studio 1.4 games (`data.win`).**

It provides 1:1 GameMaker: Studio 1.4 runtime parity on desktop platforms, serving as both a standalone desktop runner and the gold-standard test harness for verifying game logic, asset parsing, and bytecode execution before deploying to console targets like [`gm14-3ds`](https://github.com/Dxrmy/gm14-3ds).

---

## Features

- **100% Passing Test Suite**: 109 unit tests validating opcode execution, variable chains, array indices, type conversions, math, string, color, random, audio, and room lifecycle.
- **1:1 Bytecode VM**: Complete stack machine for GameMaker bytecode v15 and v16.
  - **Script Shadowing**: User scripts shadow builtins (`control_*`, `draw_*`), preserving custom engine logic.
  - **Dynamic StackTop Resolution**: Full support for `inst_type == -9` / `ref_type == 0x80` stack-popped instance targets in variable reads/writes.
  - **Object 0 De-aliasing**: Proper separation of instance `self` (`-1`) from object index 0 (`dw.objects[0]`).
  - **Lifecycle & Events**: Complete support for Create (`0,0`), Destroy (`1,0`), Alarm (`2,i`), Step (`3,sub`: Begin/Step/End), and Collision (`4,target`) events.
  - **Motion & Animation**: Automated `xprevious`/`yprevious` tracking, `hspeed`/`vspeed` motion integration, and `image_index += image_speed` animation stepping.
  - **Bounding Box & Scaling**: Accurate `bbox_of` collision calculation respecting sprite margins (`ml`, `mr`, `mt`, `mb`), origin offsets, and instance scale (`image_xscale`, `image_yscale`).
- **Complete Audio Engine**:
  - Native Windows **WaveOut** output backend (`-lwinmm`) and Linux **ALSA** backend.
  - Embedded Ogg Vorbis and WAV decoder via `stb_vorbis`.
  - Multi-voice software mixer supporting volume gain, pitch shift, looping, and pause/resume.
  - Bounded memory LRU audio cache to prevent heap exhaustion.
- **High-Fidelity Room Renderer**:
  - Corrected tile texture atlas UV mapping (`tp->sx + tl->srcx`, `tp->sy + tl->srcy`).
  - Independent horizontal and vertical background tiling (`tile_x` vs `tile_y`).
  - Instance scale transformations, flipping, and color modulation.
  - LRU texture page cache (`TexturePageCache`).

---

## Undertale 1.0.0.1539 Compatibility

Verified directly against full Undertale `data.win` (bytecode v16):

```
Chunks parsed        : 24 (FORM, GEN8, OPTN, SOND, SPRT, BGND, SCPT, FONT, OBJT, ROOM, DAFL, TPAG, CODE, VARI, FUNC, STRG, TXTR, AUDO, ...)
Strings              : 63,977
Bytecode ops         : 891,526 instructions across 6,272 code entries
Variables / Functions: 10,899 / 427 (206,598 var refs, 78,929 func refs)
Sprites / Objects    : 2,583 / 1,709
Rooms                : 336
Unit tests           : 109 passed, 0 failed
```

---

## Contents

| File | Purpose |
|---|---|
| `vm.cpp` / `vm.hpp` | Virtual machine, opcode dispatcher, instance pool, event loop |
| `dw.cpp` / `dw.hpp` | `data.win` IFF parser, chunk unpacker, room compositor |
| `audio.cpp` / `audio.hpp` | Audio engine, Windows WaveOut / ALSA backends, mixer, LRU cache |
| `vm_tests.cpp` | Comprehensive 109-test test suite |
| `vm_host.cpp` | Headless VM host runner (boots game, steps rooms, outputs state) |
| `main_host.cpp` | DataWin inspector CLI (dumps stats and renders rooms to PNG) |
| `stb_image.h` / `stb_image_write.h` | Image decoding and PNG writing |
| `stb_vorbis.c` / `stb_vorbis_impl.c` | Ogg Vorbis audio decoding |
| `Makefile` | MinGW-w64 (Windows) and GCC/Clang (Linux) build file |
| `CMakeLists.txt` | Cross-platform CMake configuration |

---

## Building

### Windows (MinGW-w64)

```bash
make
```

Or manually using `g++`:

```bash
# Compile object files
g++ -std=c++17 -O2 -c dw.cpp -o dw.o
g++ -std=c++17 -O2 -c audio.cpp -o audio.o
g++ -std=c++17 -O2 -c stb_vorbis_impl.c -o stb_vorbis_impl.o
g++ -std=c++17 -O2 -c vm.cpp -o vm.o

# Build executables
g++ -std=c++17 -O2 -o vm_tests.exe vm_tests.cpp vm.o dw.o audio.o stb_vorbis_impl.o -lwinmm -static
g++ -std=c++17 -O2 -o vm_host.exe vm_host.cpp vm.o dw.o audio.o stb_vorbis_impl.o -lwinmm -static
g++ -std=c++17 -O2 -o gm14_host.exe main_host.cpp dw.o -lwinmm -static
```

### Linux (GCC / Clang)

```bash
make
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

109 passed, 0 failed
```

### Run the Headless VM

Step Undertale from room 0 (`room_start`) through room 1 (`room_introstory`), stepping 150 frames through the typewriter text writer:

```bash
./vm_host path/to/data.win
```

### Inspect `data.win` and Render Rooms to PNG

```bash
# Print general statistics:
./gm14_host path/to/data.win

# Render specific room indices to PNG:
./gm14_host path/to/data.win 2 4 6 108
```

---

## Related Projects

- **[Dxrmy/gm14](https://github.com/Dxrmy/gm14)** — Core portable runtime.
- **[Dxrmy/gm14-3ds](https://github.com/Dxrmy/gm14-3ds)** — Nintendo 3DS hardware port.

---

## License

MIT License — see [`LICENSE`](LICENSE).
