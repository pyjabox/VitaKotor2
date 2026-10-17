# Moving ASLgl onto the GL worker

## Goal

Take the GL translation work off the game thread. Today the GL worker
(`loader/gl_worker.c`) runs only vitaGL on core 2. Everything above vitaGL
still runs on the game thread:

- Aspyr's `ASLgl`, the desktop-GL emulation layer;
- its KOTOR shader path (`GlesKotorSetupShader`, `KotorProgram`);
- gles2-bc (`OpenGLES::OpenGLES2::OpenGLES20Context`, GLES 1.1 on GLES 2);
- the recording of each vitaGL call for the worker.

The hardware profiles put that work at about 15–18 ms of a 45–60 ms frame in
busy scenes (PERFORMANCE.md 4.3 and 4.4).

## Phase 0: reverse engineering (libkotor2 2.0.4, 2026-10-16)

### Where the time goes

- **The GL work is in ASLgl's KOTOR shader path, not gles2-bc.** In the PC
  samples (pcprof4, 90 ms frames), the `KotorProgram` family's self time is
  about 10 ms a frame:
  - `SetAttributes` 4.3 ms;
  - `SetAttributeShadowOnly` 1.25 ms;
  - `Unuse` 1.1 ms;
  - `SetMatrix` 1.0 ms;
  - `Use` 0.8 ms;
  - lights, camera and skinning about 1.7 ms together.

  No `OpenGLES20Context` function is in the top entries. The 12.3 ms charged to
  `ProgramType::ProgramType` is vitaGL draw time, attributed there by the
  sampler's bias.
- **Per draw, `ASLgl::glDrawElements` calls `GlesKotorSetupShader`.** That
  function:
  - reads gles2-bc's matrix stacks directly (`MatrixStack::getProjectionMatrix`
    and `getModelViewMatrix`) and multiplies them;
  - uploads matrices, camera, shader parameters, lights and material,
    skinning bones, fog and vertex attributes through `KotorProgram`, which
    calls vitaGL directly.

### The boundary

- **ASLgl reaches gles2-bc through one context object.** It's a C++ object,
  found at a fixed offset (`+0x180c`) in a global structure. Calls are
  virtual, for example `glEnable` at vtable `+0x80`, `glMatrixMode` at `+0x108`
  and `glVertexPointer` at `+0x1a8`. The class is
  `OpenGLES::OpenGLES2::OpenGLES20Context`, with 244 vtable slots.
- **ASLgl also calls vitaGL directly:** uniforms, framebuffers and
  `glIsProgram`.
- **It reads gles2-bc state directly:** `OpenGLESState::getClientActiveTexture`
  as well as the matrix stacks.
- **So ASLgl and gles2-bc cannot be split across threads.** The boundary is
  the engine's calls into ASLgl. ASLgl, `KotorProgram`, gles2-bc and vitaGL
  all run on the worker; the game thread records.

### The interface

- **241 ASLgl functions are called from outside the GL layers,** at 3,259
  call sites. 268 ASLgl symbols have relocation slots (316 slots), including a
  few data symbols such as `s_boneMatrix`, `s_lightData` and `s_cameraMatrix`.
  Every function slot is taken over:
  - a call on the game thread is recorded;
  - a call on the worker (ASLgl calling itself) runs directly.

  Tail calls reach the PLT through Thumb-to-ARM veneers. The one function
  pointer the engine takes, `glEnableClientState` in `GLRender::RenderObjectsVTC`,
  sits in a relocation slot too.
- **Nothing outside the GL layers calls gles2-bc or `KotorProgram` directly,**
  apart from pure matrix math. Aspyr's D3D emulation (`IDirect3D*_Mac`), the
  GLSL translator and the GL blitter call through ASLgl. They stay on the game
  thread and are recorded like the engine.
- **Query-like calls:** 44 return a value or query state. Traced through direct
  calls from the per-frame code, 14 are reachable:
  - Bink movies (shader creation and logs);
  - first-use texture creation (`glGenTextures` from `AurTextureMakeCurrent`);
  - soft shadows (off);
  - save screenshots (`glReadPixels`);
  - the loading screen's clear colour;
  - **the GUI's viewport, `glGetIntegerv(GL_VIEWPORT)` from
    `AurGUIStartLayer`: the only steady per-frame one.** It's answered from a
    copy kept by the recorder.

  A query that has to wait makes the game thread drain the worker: up to a
  frame. Queries must not happen in steady frames.
- **Pointer arguments: 67 functions.** All sizes are known: matrices,
  uniforms, small vectors, name arrays, buffer and texture data.
  - Large uploads happen at load time and can wait for the worker.
  - Vertex arrays and indices in client memory must be copied when the draw is
    recorded, as the GL worker does today at the vitaGL level.

### Threading and shared state

- **Thread assumptions:** ASLgl and gles2-bc touch nothing outside GL except
  SDL window and context creation at start-up. Their only `pthread_self` call
  is a cached `ASL::GetOsVersion`.
- **The worker side already works:** the GL worker's vitaGL wrappers run
  calls made on the worker directly (`glw_direct`, keyed by TLS).
- **Shared globals:** the engine writes four globals that ASLgl reads at
  program bind and draw time. Each would be read late on the worker, so
  `glBindProgramARB` and `GlesKotorSetupShader` must read private copies
  (byte-checked patches of their address constants). The recorder sends the
  engine's current values with each bind and draw.

  | Global | Size | Written by | Read by |
  |---|---:|---|---|
  | `g_useDistortionOutput` | 1 B | `Scene::DoDistortionBuckets`, `PartEmitter::RenderFunc`, `RenderInterface::SetDepthMask` | `ASLgl::glBindProgramARB` |
  | `g_illumShader` | 1 B | `GLRender::EnableFragmentShadersARB` | `glBindProgramARB`, `GlesKotorSetupShader` |
  | `g_illumShaderValues` | 16 B | `GLRender::EnableFragmentShadersARB` | `GlesKotorSetupShader` |
  | `s_vertexShaderParams` | 16 B | `SetBumpedOutShellIos` | `GlesKotorSetupShader` |

### The deciding cost

The engine makes 37–56k ASLgl calls a frame, measured with hooks on
2026-10-03. Most are tiny: `glProgramEnvParameter4fARB` alone accounts for
4.7–6.4k, and ASLgl only stores its four floats. Recording at today's
~0.4 µs a call would cost about as much as it saves; a lean path for tiny
calls is the condition.

| Recording cost per call | Net game-thread gain |
|---|---|
| 0.4 µs (today's recorder) | about zero |
| 0.2 µs | 6–9 ms a frame |
| 0.1 µs | 10–13 ms a frame |

## Plan

**Phase 1 — measurement (`loader/asl_probe.c`, `ASL_PROBE`).** Counting stubs
on every ASLgl function slot. Per frame:
- calls to each function from outside the GL layers;
- queries, with their pname;
- client-memory draws and their bytes;
- `ASLgl::glDrawElements` and `GlesKotorSetupShader` time per call;
- a lean recorder micro-benchmark.

These pick the fast paths and the state copies.

**Phase 2 — recorder, inline mode.**
- **Generator:** a generator in the style of `tools/gen_glw.py` over the ASLgl
  signatures. It emits a recorder and a replayer for each function.
  - Floats travel as integer carriers (softfp), doubles as 64-bit pairs.
  - Unclassified functions wait for the worker, which is always correct.
  - Classified ones record without waiting.
- **Recording paths:**
  - **Fast path:** for scalar-only calls.
  - **Copy rules:** for pointer arguments.
  - **Draws:** snapshot client arrays. Element-buffer data is shadowed on the
    CPU, so index ranges can be read.
  - **State copies:** answer the per-frame queries (viewport, enabled caps,
    errors) without waiting.
- **Loader hooks:** the GL state filter moves in front of the recorder. The
  DXT and mipmap hooks run on the worker.
- **Validation:** in "inline" mode the recorder records, then replays at once
  on the game thread. That checks the codec in Vita3K with no threading.

**Phase 3 — worker mode.**
- ASLgl runs on core 2, with the ring sized for a frame of ASLgl calls (2–4 MB).
- The four globals get their patched private copies.
- A/B switch in game, and hardware sessions.

## Phase 2 and 3 findings (Vita3K, 2026-10-17)

### Answers the recorder gives
- **`glIsEnabled` follows ASLgl, not what was set.** ASLgl keeps alpha test and
  fog itself, and gles2-bc asks vitaGL, which does not track fixed-function
  enables. For those caps the recorder answers what the worker first said.
  Only the caps vitaGL keeps (`gl_state_filter.c`'s list, plus scissor) are
  answered from what the engine set.
- **Result:** VERIFY checked about 481,000 answers over a 150 s run, inline and
  in worker mode, with 0 disagreements.

### Hazards found once ASLgl ran on another thread
1. **Loader code that calls ASLgl by address skips the recorder.**
   `mip_gpu.c` uploaded through `ASLgl::glTexImage2D` directly. It now goes
   through `aslw_entry()`.
2. **Loader state that gles2-bc's shims read.** `gl_patch.c`'s `glViewport`
   shim reads the GUI clipping scope, which `main.c`'s `AurGUISetupViewport`
   hook changes. The save, scope change and restore now run in the stream
   (`aslw_run()`).
3. **ASLgl calls itself through its slots.** Its immediate mode sets client
   arrays and draws. With ASLgl on the game thread, those inner calls reached
   the recorder's copies of the engine's state. The next worker phase then
   saw stale client arrays: 72–149 waited-for draws a frame, and arrays
   re-pointed at stale memory. Generated recorders now count call depth on
   the game thread (`aslw_depth`), and only depth-0 calls update the copies.
4. **Client-array copies with a vertex buffer bound.** A pointer given while
   the draw's array buffer is bound is an offset into it. The worker now binds
   buffer 0 around the copy's pointer calls.
5. **The GL worker's own state copy (`gl_worker.c`).** While ASLgl runs on the
   worker, its vitaGL calls pass that copy by. The fixes:
   - The producer drops no repeated calls in this phase (`glw_aslw_active`).
   - The recorder notes viewport, scissor, enables, framebuffer and unpack
     alignment into the copy, but only in this phase.
   - When ASLgl comes back to the game thread, the copy is read from vitaGL in
     a waited-for op (`glw_resync`). It covers buffer bindings, the current
     program, every attribute's array state and live program names; caps,
     rectangles and repeat groups are marked unknown.

   Noting in the other phase was the GUI bug: the note came first, so the
   real vitaGL call was dropped as a repeat. GUI sub-viewports and scissors
   never reached the GPU, the minimap's clear spilled past its frame, the map
   went missing, and menu art drew at the wrong size.
6. **Pixel sizes at the ASLgl level.** Uploads use desktop formats (BGRA, BGR,
   packed types) and the engine's unpack state. `aslw_pixels_size()` covers
   those. Any other layout uploads with the game thread waiting, and is
   counted in the `[aslw]` line; none were seen.

### Tooling
- **Window captures.** `tools/vita3k_shots.sh` captures the Vita3K window for
  each `[autotest] SHOT` the loader logs: main menu, save list, loading
  screen, then 5 s into each A/B window. File names carry the phase. Reading
  the frame back in the guest gives black in Vita3K.

## Hardware A/B (Telos, 2026-10-17)

Build: release plus `ASLW=1` (A/B every 10 s), with the AI thread on. Log:
`diagnostics/aithread-20261016/log-telos-aslw-hw-1.txt`. Steady play is
everything after 70 s, minus the module load.

| | ASLgl on the GL worker | ASLgl on the game thread |
|---|---:|---:|
| FPS | **18.4** | 14.6 |
| Frame time | **54.4 ms** | 68.5 ms |
| `Scene::Render` on the game thread | **34.2 ms** | 47.6 ms |
| GL worker busy | 25–40 ms | 7–13 ms |

- **Gain:** 26% more FPS. `Scene::Render` drops by 13.4 ms, inside the 11–18 ms
  Phase 0 estimated for ASLgl, `KotorProgram` and gles2-bc. Across 24
  adjacent pairs, the median ratio of frame times (game-thread half over
  worker half) is 1.28.
- **Clean run:** there were no waited-for draws or uploads of unknown layout,
  and the worker was asked about 0.01 queries a frame while answering
  250–530 itself. No dropped calls and no faults, over about 5 minutes and
  30 switches.
- **Headroom:** the worker is busy at most about 40 ms of a 67 ms frame in the
  heaviest windows. The game thread's own `Scene::Render` (about 34 ms) is
  now the limit.
