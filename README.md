# VitaKotor2

**Star Wars: Knights of the Old Republic II: The Sith Lords on PlayStation Vita.**

This is an experimental native loader for the Android release of KOTOR II. It
loads the original ARM shared libraries from a user-supplied copy of the game,
maps their platform APIs to Vita implementations, renders through
[vitaGL](https://github.com/Rinnegatamante/vitaGL), and provides a custom FMOD
audio backend over `sceAudiodec` and `sceAudioOut`.

> You must own the Android version of KOTOR II. This repository contains only
> loader code and Vita presentation assets. It does not include the game APK,
> shared libraries, OBB archives, or other copyrighted game data.

## Current Status

**Playable work in progress (v0.3.0). Core gameplay and the major
platform-integration paths work on physical Vita. In the tested gameplay v0.3.0
averages about 25 FPS: about 19–20 FPS in the busiest scenes and 34–37 FPS in
light ones, up from about 13 FPS in v0.2.0. Performance tuning and broad
playthrough validation remain active.**

### Working and validated

- Android ARM libraries and OBB archives load correctly.
- Main menu, character creation, gameplay, combat, dialogue, inventory, and area
  transitions work.
- Vita buttons and analog sticks control menus and gameplay.
- Character and manual-save names use the Vita on-screen keyboard.
- Local manual, quick, and automatic saves can be created, deleted, and loaded.
- GUI layouts remain intact after saving, loading, and module transitions.
- Nested GUI viewports clip correctly, including the six character-class panels.
- Menu audio, voices, sound effects, positional audio, streamed area music, and
  long music tracks play through the Vita audio backend.
- Intro and in-game movies play through KOTOR II's embedded decoder with
  synchronized audio.
- 16-bit texture uploads keep the working set inside Vita CDRAM and avoid the
  long-session geometry corruption caused by texture thrashing.
- Large-allocation isolation prevents the fragmentation-driven area-transition
  failures seen in older builds.
- Packed VBO offsets preserve correct character skinning and geometry.
- LiveArea artwork is installed in Vita-compatible formats.
- Combat no longer freezes for 0.2–0.6 s when attacking (see Performance).

## Performance

Measured on a physical Vita at the CPU's 444 MHz. Frame times are per 10 s
window in the tested gameplay. Each change below was A/B-tested on hardware; the
[performance reference](docs/specs/PERFORMANCE.md) has the measurements.

| Release | Busiest tested scenes | Light scenes |
|---|---|---|
| v0.2.0 | about 13 FPS | — |
| v0.3.0 | about 19–20 FPS (49–53 ms) | 34–37 FPS (27–30 ms) |

What is enabled by default:

- **Since v0.1.0/v0.2.0:**
  - audio-lock contention removed;
  - 16-bit textures and the packed-VBO fix;
  - large-allocation isolation;
  - the vitaGL performance recipe (shader cache, sampler and texture
    speedhacks) with a uniform-overflow clamp;
  - redundant-bind filters, the adaptive-render-skip override, and the engine's
    visibility test.
- **New in v0.3.0:**
  - **GPU occlusion culling** of creatures, placeables and doors hidden behind
    walls (the object pass halved at the heaviest tested spot).
  - **vitaGL on its own CPU core:** the game thread records GL commands and a
    worker thread on core 2 replays them.
  - **GL state filter:** repeated state calls are dropped before Aspyr's
    translation layer (about 5 ms per frame).
  - **Archive read cache:** scripts are no longer re-read from the memory card
    on every run (mean frame 75 → 60 ms).
  - **Talk table in RAM:** this removed the combat stutter.
  - **Resource cache sized from real memory:** it was falling back to 16 MB and
    evicting constantly.
  - **AI caches:** walk-pass and line-of-sight results are reused (about
    1.6 ms per frame).
  - **Server AI budget of 3 ms per frame** instead of 10 ms (about 4–5 ms per
    frame).
  - **Audio update:** the FMOD channel scan, which runs about 50 times per
    frame, is skipped when no sound has ended (about 1 ms per frame).
  - **Mobile bloom skipped** (GPU 36 → 22 ms per frame, no visible difference
    noticed).
  - **No shadows** (about 4.4 ms and 80 draws per frame), through the game's
    own option: the loader sets `Shadows=0` and `Soft Shadows=0` in
    `swkotor2.ini` at every launch.
  - **No GL call timing in release builds.**
- **On main since v0.3.0 (not yet in a release):**
  - **Native DXT textures:** DXT1/3/5 textures go to the GPU compressed
    instead of being decoded to RGBA on the CPU (texture building in stalls
    8.9 → 3.3 s in the same scenes).
  - **Streamed music and voice:** no stream is decoded or read whole on the
    game thread any more. Music starts no longer freeze the game (stingers took
    up to 0.6 s). Streams the hardware decoder cannot take decode with minimp3
    on the audio thread.
  - **Faster first-time images:** mip chains of uncompressed images (menu art,
    minimap, portraits, loading screens) are made on the GPU instead of the
    CPU (in loading, 332 → 76 ms per image on average).
  - **No card access for missing loose files:** the engine looks for loose
    copies of nearly every resource first; misses in the read-only folders are
    now answered from memory.

### Remaining limitations

| Area | Current behavior |
|---|---|
| Startup | A cold launch can remain black for roughly two minutes while archives are scanned and shaders compile. Wait before assuming the process has hung. |
| Performance | Heavy scenes remain CPU-bound, mostly in the engine's per-object rendering work: about 19–20 FPS in the busiest tested scenes. A 10 m distance filter can reduce draws further but remains too experimental for releases. See the [performance reference](docs/specs/PERFORMANCE.md). |
| Visual changes | There are no dynamic shadows: the loader sets the game's own `Shadows=0` (and `Soft Shadows=0`) in `swkotor2.ini` at every launch. The in-game option still works until the next launch. The mobile bloom passes are skipped (no game option controls them). Rooms keep their baked lighting. |
| AI updates | The server AI gets 3 ms per frame instead of 10 ms, so each object is updated less often (creatures about every third frame in busy areas). Nothing was noticed in testing, but reactions in very crowded scenes may lag slightly. |
| Audio | Known issue: some sounds can cut in and out. Under investigation. |
| Latency | The GL worker lets the game run up to one frame ahead of the display. An object stepping out from cover can appear up to three frames late (occlusion culling). |
| Hitches | Occasional single frames of about 0.12–0.23 s remain when new content is first read from the memory card (about 10 MB/s): the minimap's first draw, dialog entries and their voices, creatures spawned by scripts. Loading screens still build some large images on the CPU. |
| Shader cache | Releases use the validated cache-enabled/no-`LOG_ERRORS` vitaGL recipe, which removes major first-use shader stalls and includes a uniform-overflow clamp. An older cache experiment faulted on a warm launch, so cache recovery remains documented and broader validation is ongoing. |
| Validation | The tested routes, saves, menus, audio, and movies work, but a complete start-to-finish playthrough has not yet been certified. |
| Text entry | Printable ASCII is supported. Emulator builds may provide a desktop fallback when the emulated common-dialog keyboard does not appear. |
| Touch | Front and rear touch are intentionally disabled; the port uses physical controls. |
| Platform features | No trophies, cloud saves, or online platform integration. |

## Requirements

### On the Vita

- A Vita or PS TV capable of running HENkaku/taiHEN.
- [`kubridge.skprx`](https://github.com/bythos14/kubridge/releases) installed and
  enabled as a taiHEN plugin.
- `libshacccg.suprx` in `ur0:data/` for runtime shader compilation.
- About **4 GB free on `ux0:`** for the tested game data. The exact requirement
  depends on the Android release and optional restored-content files.

### From Your Android Copy

Use the 32-bit ARM (`armeabi-v7a`) build of KOTOR II. The tested layout uses:

- `libkotor2.so`
- `libObbVfs.so`
- `libc++_shared.so`
- `libminiz.so`
- `libLzmaLib.so`
- `main.213.com.aspyr.swkotorii.obb`
- `patch.14.com.aspyr.swkotorii.obb`

Different Android versions may use different OBB version numbers. This loader's
paths are currently compiled for the names above.

## Installation

1. Install `KOTOR2.vpk` with VitaShell. The title ID is `KOTR00002`.
2. Create `ux0:data/kotor2/`.
3. Copy the five required ARM libraries into that directory.
4. Copy both OBB archives without renaming them from the tested names.
5. Copy the APK's `assets/` directory, preserving its contents and structure.
   The tested release includes `AVConfig.json`, `iosdialog.otf`, and
   `supplierconfig.json`.
6. Optionally copy restored-content data into `dlc/`, preserving its original
   directory structure.

The tested result is:

```text
ux0:data/kotor2/
|-- assets/                                  # required APK assets
|-- dlc/                                     # optional restored-content data
|-- libc++_shared.so
|-- libkotor2.so
|-- libLzmaLib.so
|-- libminiz.so
|-- libObbVfs.so
|-- main.213.com.aspyr.swkotorii.obb
`-- patch.14.com.aspyr.swkotorii.obb
```

The loader creates writable directories, `swkotor2.ini`, and `log.txt` on first
run. Game shaders are loaded through the game-data path and compiled at runtime.
The dialogue font is read from `assets/iosdialog.otf`.

### First Launch

The first launch currently spends roughly two minutes on a black screen while
the OBB data is scanned and shaders are compiled. A brief static game/credit
screen may appear before it returns to black. Menu music can begin before the
menu becomes visible.

There is no progress indicator yet. Keep the Vita awake and wait. If the screen
is still black after several minutes, exit and inspect `ux0:data/kotor2/log.txt`.

## Controls

- Left stick: movement
- Right stick: camera
- D-pad: menu navigation
- Cross: accept/action
- Circle: cancel
- Square and Triangle: mapped game actions
- L and R: shoulder actions
- Start: pause/menu

Both touch panels are intentionally disabled.

Character-name and manual-save-name fields open the Vita on-screen keyboard.
Accepted text is delivered directly to KOTOR II's existing character dispatcher;
printable ASCII is supported. Emulator builds may provide a desktop fallback if
the emulated common-dialog keyboard accepts initialization but does not appear.

## Troubleshooting

The primary diagnostic file is:

```text
ux0:data/kotor2/log.txt
```

Depending on the build profile, it includes startup, archive access, shader,
audio, benchmark, and CPU-fault information. Maximum-FPS builds intentionally
suppress routine lines while preserving failures and panic crash dumps.

| Symptom | Check |
|---|---|
| Immediate launch failure | Confirm `kubridge.skprx` is installed and enabled. |
| Permanent black screen before shaders | Confirm `ur0:data/libshacccg.suprx` exists. |
| Black screen on a cache-enabled warm launch | Remove `ux0:data/shader_cache/KOTR00002/` and relaunch so the cache is rebuilt. If it repeats, use a cache-disabled recovery build and report `log.txt`. |
| Missing game data | Confirm all five libraries, both exactly named OBB files, and the APK `assets/` directory exist under `ux0:data/kotor2/`. |
| Looks frozen | Check the end of `log.txt`. The crash handler parks the process after writing a `[CRASH]` block. |
| Rendering problems you suspect the GL worker of | Create `ux0:data/kotor2/glw_mode.txt` containing `0` and relaunch. vitaGL then runs on the game thread, as in v0.2.0. Delete the file to turn the worker back on. |
| Texture problems you suspect the native DXT path of | Create `ux0:data/kotor2/dxt_mode.txt` containing `0` and relaunch. DXT textures are then decoded on the CPU, as in v0.3.0. Delete the file to turn the native path back on. |
| Image problems you suspect the GPU mipmaps of | Create `ux0:data/kotor2/mipgen_mode.txt` containing `0` and relaunch. The engine then builds every mip chain on the CPU, as in v0.3.0. Delete the file to turn it back on. |

Releases omit vitaGL's animated splash but enable the custom GLSL disk cache as
part of the validated high-performance recipe. If a warm launch black-screens,
remove `ux0:data/shader_cache/KOTR00002/` and relaunch to rebuild it. See
[`docs/specs/PERFORMANCE.md`](docs/specs/PERFORMANCE.md) for the exact profile.

## Building From Source

### Prerequisites

Install a current [VitaSDK](https://vitasdk.org/) toolchain and its packaged
libraries. The build needs CMake, Make, Git, a C/C++ host toolchain, and the
VitaSDK packages used by `CMakeLists.txt`, including SDL2, vitaGL, vitaShaRK,
SceShaccCgExt, math-neon, FreeType, libpng, zlib, bzip2, pthreads, taiHEN, and
kubridge.

Set the standard SDK environment variables:

```sh
export VITASDK="$HOME/vitasdk"
export PATH="$VITASDK/bin:$PATH"
```

### Build the required vitaGL revision

KOTOR II requires the pinned vitaGL revision and the included packed-VBO offset
fix:

```sh
git clone https://github.com/Rinnegatamante/vitaGL.git
cd vitaGL
git checkout 38d2f9704b6b241965ed7086aeaacd69f044f2c5
git apply --unidiff-zero /path/to/VitaKotor2/patches/vitaGL-packed-vbo-offset.patch
make clean
make NO_SPLASHSCREEN=1 HAVE_SHADER_CACHE=1 \
  SAMPLERS_SPEEDHACK=1 TEXTURES_SPEEDHACK=1 -j"$(nproc)"
make install
```

Do not add `LOG_ERRORS` or `DRAW_SPEEDHACK` to the release performance recipe.
The loader's uniform-capacity clamp and the packed-VBO patch must remain in
place. See [`docs/specs/PERFORMANCE.md`](docs/specs/PERFORMANCE.md) for the
validated settings, recovery procedure, and experimental alternatives.

### Prepare private build inputs

The repository does not contain copyrighted game data. Create a sibling
`runtime/` directory from your own 32-bit ARM Android copy:

```text
KOTOR2/
|-- runtime/
|   `-- assets/       # APK assets, including shader and font resources
`-- VitaKotor2/       # this source tree
```

The CMake build bundles supported `.vert`, `.frag`, `.glsl`, `.txi`, `.otf`, and
`.ttf` resources found under `runtime/assets/`. If those resources are absent,
the resulting loader falls back to files installed on the Vita where possible.
The Android libraries and OBB archives are runtime inputs and are not linked
into the VPK.

### Configure and build

From the repository root:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake"
KOTOR2_ALLOW_UNSAFE_SHADER_CACHE=1 cmake --build build -j"$(nproc)"
```

Expected outputs:

```text
build/KOTOR2
build/KOTOR2.velf
build/eboot.bin
build/KOTOR2.vpk
```

The build validates the LiveArea assets and linked vitaGL policy automatically.
You can also run the checks directly:

```sh
./tools/check_livearea.sh .
./tools/check_vitagl_build.sh build/KOTOR2
```

Compile-time feature and benchmark switches are documented in
`loader/config.h`. These include `GL_WORKER_MODE`, `INI_NO_SHADOWS`,
`AI_BUDGET_US`, `AUDIO_UPDATE_SKIP`, `DXT_NATIVE`, `MIPGEN_GPU`, `FS_MISS_CACHE`
and `STALL_LOG_MS` (a per-frame stall breakdown in `log.txt`). Configuring with `-DKOTOR_AUTOTEST=ON`
builds a variant that drives the game with scripted input, for unattended
Vita3K smoke tests. The supported Android library and archive names are listed in
the installation section above.

## Credits

- **[ScoobyDouche](https://github.com/ScoobyDouche)**: creator of [VitaKotor](https://github.com/ScoobyDouche/VitaKotor). This KOTOR II port benefited extensively from that project's foundational loader, platform-integration, graphics, audio, reverse-engineering, and Vita adaptation work. VitaKotor made this companion KOTOR II effort possible.
- Andy Nguyen (TheFloW): the so-loader technique
- Rinnegatamante: vitaGL, vitashark, and reference ports
- lieff: [minimp3](https://github.com/lieff/minimp3) (CC0), the software MP3 decoder for streams beyond the hardware decoder's limit
- VitaSDK contributors: the Vita toolchain and libraries
- Aspyr Media, Obsidian Entertainment, BioWare, and Lucasfilm: the original game

Released under the MIT license. The license covers the loader only, never the
original game code or data.
