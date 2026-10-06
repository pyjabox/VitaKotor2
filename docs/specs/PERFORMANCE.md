# KOTOR II Vita performance

**Last updated:** 2026-10-04
**Status:** Canonical performance document

This is the single source of truth for KOTOR II Vita performance work. It consolidates the former gameplay-performance, optimization-research, and dynamic-room-culling documents. Update this file instead of creating another performance spec.

## 1. Executive summary

KOTOR II is predominantly **CPU-bound in engine-side scene submission**, not GPU-bound in the final `glDraw*` calls. Heavy views expose many room-owned Gobs and mesh parts. Each part incurs engine traversal, material selection, uniform setup, and render dispatch before a comparatively cheap GL submission.

The largest confirmed improvements are:

1. **Audio-lock contention removal:** clear views improved from about 66–70 ms to 33 ms per frame; a heavy red-corridor view improved from 244–270 ms to 82–97 ms.
2. **Reduced active-room workload:** the scoped `001ebo15 -> 001ebo5` test improved physical-Vita performance from about 14 FPS to 25 FPS.
3. **Experimental 10 m distance-only room filtering:** in the best matched final three hardware windows, draws fell from 364.82 to 263.19 per presented frame (−27.86%), while the presentation-rate proxy rose from 11.45 to 14.75/s (+28.81%). The tested visual result resembled old-school distance fog/pop-in and was considered tolerable, but it is not production-safe.
4. **Persistent custom-shader caching:** removes major first-use doorway stalls when it works. Two uncached links previously cost about 606.6 ms and 608.2 ms, roughly 1.2 seconds combined. It remains experimental because an older warm-cache hardware run produced a separate GXM fault/black screen.
5. **Validated v0.2.0 geometry-safe profile:** the reconstructed build runs at about 13 FPS in the tested second level with no observed graphical glitches. It combines the custom shader cache, sampler/texture speedhacks, 16-bit texture conversion, redundant-bind filtering, adaptive-render-skip suppression, and the engine visibility test at `0.02`.
6. **GPU occlusion culling of Gobs (section 6.6):** at the heaviest spot of the test route the Gob pass halved (about 30 to 15 ms) and the frame went from 92.8 to 74.9 ms (10.8 to 13.4 FPS), with no visible artefacts.
7. **Combat stutter fixed: talk table in RAM (section 5.2):** the 0.2–0.6 s freezes when attacking were string lookups on `dialog.tlk`, about 38 ms each on the memory card. Per 10 s of combat, frames of 200 ms or more fell from 6.0 to 1.0, and frames of 500 ms or more from 2.2 to 0.1.
8. **Mobile bloom skipped (section 9.1):** GPU time per frame went from 36 to 22 ms at the test spot, with no visible difference.
9. **Simulation fixes (sections 5.3, 5.4):** the engine's resource cache now gets a real budget, which halved the combat frames over 200 ms. Two caches save about 1.1 ms (AI walk pass) and 0.5 ms (`CanSee`) per frame.
10. **Archive read cache (section 5.5):** scripts were re-read from the memory card on every run, blocking about 10 ms per frame. Keeping repeated reads in RAM took the mean frame from 75.0 to 60.2 ms in a matched A/B. The release build now averages about 53 ms per frame (about 19 FPS) in the tested gameplay.
11. **GL state filter (section 5.6):** repeated `glEnable`/`glDisable` and texture-unit calls are dropped before Aspyr's translation layer, and a dead query in `glTexParameteri` is removed. In matched A/B windows the frame was about 5 ms faster (1–12 ms).
12. **GL worker thread (section 5.7):** vitaGL runs on core 2. The game thread records commands instead, at about 4.5 ms per frame instead of about 8 ms. Frame at the busy spot: 61.7–63.7 → 58.5 ms.
13. **No shadows (section 9.2):** about 4.4 ms and 80 draws per frame in a busy scene. The loader sets the game's own `Shadows=0` in `swkotor2.ini`.
14. **FMOD update skip (section 5.8):** the game calls `System::update` about 50 times per frame; the loader now scans its channels only when a sound has ended. About 1 ms per frame.
15. **Server AI budget 3 ms (section 5.9):** the AI master spent its full 10 ms budget every frame. At 3 ms it takes 3.4–5.3 ms.
16. **Release candidate 1, released as v0.3.0 (2026-10-04)**, all of the above on top of item 11:
    - the busy spot runs at 49–53 ms per frame (~19–20 FPS), against ~62 ms before;
    - light scenes (140–155 draws) run at 27–30 ms (34–37 FPS);
    - the gameplay average over an 8-minute session was about 40 ms (~25 FPS).
17. **Boot and loading screens (section 5.14, v0.4.1):** the archive replay index is written at last: first frame 33.9 → 24.3 s from the second launch on. Large non-power-of-two images are rescaled with NEON and get their mips on the GPU: 2–6 times faster per image (1920x1200: 837 → 354 ms).

Generic door/portal filtering did not reproduce the scoped VIS gain:

- Gob-only filtering changed stable draw throughput by only −0.42% in the mean, which is noise, while adding room/Gob scans.
- Door-aware whole-room filtering produced no measurable reduction on the tested Peragus route.
- The 10 m filter reduced draws/frame because it deliberately ignores doors, portals, adjacency, and force-retain semantics and removes entire distant rooms.

## 2. Benchmark configurations

### 2.1 Geometry-safe maximum-throughput baseline

| Setting | Value | Rationale |
|---|---:|---|
| `PERF_DISTANCE_ROOM_FILTER` | `0` | Full room rendering; valid control |
| `PERF_DYNAMIC_PORTAL_FILTER` | `0` | Ineffective on tested route and can glitch |
| `PERF_DYNAMIC_PORTAL_GOB_FILTER` | `0` | No measurable reduction |
| `VIS_ENGINE_ENABLE_TEST` | `1` | Restores the engine visibility mechanism |
| `VIS_ENGINE_TEST_T` | `0.02f` | Conservative tested threshold |
| `GL_TEX16_CONVERT` | `1` | Helps the texture set fit Vita CDRAM |
| `DISABLE_ADAPTIVE_RENDER_SKIP` | `1` | Prevents slow-update cascades |
| `GL_FILTER_REDUNDANT_BINDS` | `1` | Avoids redundant texture work |
| `GL_FILTER_REDUNDANT_PROGS` | `1` | Correct and cheap; measured benefit is small |
| `PERFORMANCE_TELEMETRY_ENABLE` | `0` | Full attribution has observer cost |
| `LOG_DIAGNOSTICS` | `0` | Avoid diagnostic work on hot paths |
| `LOG_BENCHMARK_MINIMAL` | `1` | Keep failures and panic dumps only |
| `DRAW_FRAME_BENCHMARK_ENABLE` | `1` | Lightweight 10-second workload sample; may be disabled after validation |

The validated v0.2.0 vitaGL recipe is:

```sh
git checkout 38d2f9704b6b241965ed7086aeaacd69f044f2c5
git apply --unidiff-zero patches/vitaGL-packed-vbo-offset.patch
make clean
make NO_SPLASHSCREEN=1 HAVE_SHADER_CACHE=1 \
  SAMPLERS_SPEEDHACK=1 TEXTURES_SPEEDHACK=1 -j4
make install
```

Important details:

- `HAVE_SHADER_CACHE=1` is required by the currently validated fast profile;
- `SAMPLERS_SPEEDHACK=1` and `TEXTURES_SPEEDHACK=1` are enabled;
- `LOG_ERRORS` is deliberately omitted;
- `DRAW_SPEEDHACK` is deliberately omitted because it locked physical hardware;
- the packed-VBO offset patch and KOTOR II uniform-capacity clamp are retained;
- all room/Gob filters remain disabled.

The cache and omission of `LOG_ERRORS` changed together in the successful build. Their individual sustained-performance contributions have not been isolated, so the complete recipe above—not either option alone—is the known-good configuration.

### 2.2 v0.2.0 hardware validation

The archived known-fast source and vitaGL recipe were reconstructed from a clean
build directory rather than reusing the historical executable. The reconstructed
build was uploaded to physical Vita, downloaded again byte-for-byte, and tested
in gameplay. It ran at about **13 FPS in the tested second level with no observed
graphical glitches**.

The result confirms that the complete configuration is reproducible. It does
not prove that shader caching alone produces the gain: cache enablement and
omitting `LOG_ERRORS` remain coupled in the validated recipe.


### 2.3 Experimental 10 m profile

The distance-filter experiment changes only:

- `PERF_DISTANCE_ROOM_FILTER=1`;
- `PERF_DISTANCE_ROOM_RADIUS_M=10.0f`.

After the engine creates the transient `CollectActiveRooms` list, the loader:

1. reads the live camera position as the available player-position proxy;
2. always retains the current room;
3. measures the shortest distance to every other room’s world-space AABB;
4. removes a room when that distance exceeds 10 m;
5. fails open for invalid camera or room bounds.

It intentionally ignores doors, portals, VIS adjacency, and force-retain flags. Removing a room can suppress static geometry, room-owned Gobs, characters, doors, interactables, emitters, lights, and related render state. It does not independently distance-test each object.

Expected defects include room pop-in, missing vistas, disappearing objects, incorrect lighting, transition glitches, and black views in some modules.

## 3. Measurement rules

### 3.1 Use draws per presented frame

`draws/sec` is misleading: an optimization can lower draws/frame while raising FPS, leaving draws/sec unchanged or higher. The canonical workload metric is:

`draws per presented frame = (glDrawArrays + glDrawElements) / presented frames`

The lightweight `DRAW_FRAME` benchmark adds one integer increment per GL draw, one frame increment and interval check per presentation, and one buffered log line every 10 seconds. It does not enable FPS logging, frame-time timing, per-draw clocks, classification, signatures, watchdog telemetry, or full performance telemetry.

### 3.2 Controlled A/B procedure

1. Use the same save, physical position, camera direction, and door state.
2. Remain stationary for at least 30 seconds before the doorway.
3. Traverse the same route.
4. Remain stationary for at least 30 seconds afterward.
5. Compare matching stable windows, not whole-run averages containing movies, loading, transitions, or shader compilation.
6. Record and round-trip verify the deployed `eboot.bin` SHA-256.
7. Treat physical Vita as authoritative; use an emulator first when a safety or visual smoke test is useful.

The final-three-window comparison below was the best matched stable portion. The five-window comparison mixed more route/ramp-up content and is weaker.

## 4. Confirmed workload diagnosis

### 4.1 Scene-stage attribution

A controlled `001ebo` capture compared a clear view with the red corridor:

| Metric per frame | Clear | Red | Delta |
|---|---:|---:|---:|
| Frame time | 70.7 ms | 244.5 ms | +173.8 ms |
| All draws | 102.6 | 333.9 | +231.4 |
| `glDrawElements` | 70.6 | 301.9 | +231.4 |
| `DoMeshBuckets` draws | 18.0 | 48.0 | +30.0 |
| `DoGobBuckets` draws | 48.6 | 228.6 | +180.0 |
| `DoEmitterBucket` draws | 3.0 | 24.3 | +21.4 |

`DoGobBuckets` accounted for about 78% of added draws and 82% of added frame time. The increase consisted mainly of ordinary indexed submissions rather than one pathological shader.

### 4.2 GL submission is not dominant

In a roughly 100-draw clear window, complete timed GL draw calls cost about 2.9 ms/frame. In an intermediate 222-draw heavy window, they cost about 4.9 ms/frame. Approximately 96% of timed bucket cost occurred before or between GL draws in engine traversal and setup.

Dominant mesh/Gob indexed draws used bound element buffers. Client-index speedhacks therefore do not address this workload. Conservative signatures also showed that most dominant draws were unique.

The measured path is:

`Scene::DoGobBuckets -> Gob::VisibilityCheck -> Gob::Render -> Gob::PartDraw -> PartTriMesh::Draw -> material render function -> GLRender::DrawElements`

The VBO index-pool lookup contributed only about 0.20 ms/frame and 2.43% of the engine draw wrapper, so it is ruled out.

### 4.3 GL layer share

The engine reaches the GPU through three layers:
- Aspyr's `ASLgl`: desktop-GL and D3D emulation, 420 functions, 246 of them called by the engine;
- gles2-bc (`OpenGLES::`);
- vitaGL: 153 functions.

A 2026-10-03 measurement marked the layer the game thread was in at every call into them. A sampler thread on core 2 read the mark and the thread's state every 200 µs. Results for gameplay at 290–450 draws per frame, after removing the markers' own cost (about 10–15 ms per frame):

| Share of the frame | ms per frame |
|---|---:|
| Engine, running | 45–60 |
| Engine, waiting (card reads, section 5.5) | 7–27 |
| Translation (`ASLgl` + gles2-bc) | 14–21 |
| vitaGL | 8–12 |
| Swap and GPU waits | ~0.1 |

The GPU was busy 17–26% of the time, and the game thread was never preempted.

Per frame the engine made 37–56k translation-layer calls and 8–12k vitaGL calls. The busiest translation calls were:
- `ASLgl::glProgramEnvParameter4fARB`: 4–6k;
- `glEnable`/`glDisable`: 3.5–5k;
- `glActiveTextureARB`: 1.3–2k.

640–910 vitaGL calls per frame return a value: `glGetIntegerv` 420–590, `glIsProgram` 180–275.

A follow-up run recorded, at each sample, which translation entry point and which vitaGL function the game thread was in. These are per-frame figures for heavy gameplay at 320–410 draws, including the vitaGL calls under each entry point and the markers' own cost:

| Entry point | ms per frame | Calls per frame | Note |
|---|---:|---:|---|
| `ASLgl::glDrawElements` | 13–16 | 320–410 | about 39 µs per draw: ~21 µs in gles2-bc (shader and state preparation), ~15 µs in vitaGL's `glDrawElements` |
| `glDisable` + `glEnable` | 3–4.6 | 3.8–5.3k | gles2-bc's `OpenGLES20Context::glDisable` is a switch with no redundancy check |
| `glBindProgramARB` | 1.8–2.6 | 1.1–1.5k | already skips a re-bind internally; the program depends on fog/alpha flags |
| `glActiveTextureARB` | 1.7–2.4 | 1.5–2.1k | |
| `glTexParameteri` | 1.7–2.4 | 440–600 | about 4 µs each; it ends with a `glGetIntegerv` whose result it never reads |
| `glProgramEnvParameter4fARB` | 1.2–1.7 | 4.7–6.4k | only stores four floats; this is the markers' cost |

Every other vitaGL entry point costs 0.6 ms per frame or less. While loading, `ASLgl::glCompressedTexImage2DARB` (CPU DXT decode) and `gluBuild2DMipmaps` take up to 225 and 82 ms per frame.

A GL worker thread at the vitaGL boundary would therefore save at most 8–12 ms per frame. Some of that goes on recording about 10k calls, and on answering the value-returning calls from a client-side copy of the state. A boundary at `ASLgl` would cover 22–33 ms, but it means taking over 420 desktop-GL functions that pass client memory pointers. Removing the card waits (section 5.5) was cheaper and came first.

### 4.4 Engine profile and subtree timers

`loader/pc_prof.c` (diagnostic, not in releases) is a statistical PC profiler of the game thread:
- A user thread cannot read another thread's registers on the Vita. kubridge can, however, deliver a prefetch abort to a user handler with the full register context, and resume the thread when the handler returns.
- A sampler thread removes execute permission from libkotor2's `.text` (not the whole segment: `.dynsym`/`.dynstr`, the unwind tables and `.rodata` are read at run time). The first engine instruction fetched afterwards is the sample. The handler records the PC and the r7 frame chain, restores the permission and returns.
- A protection change briefly unmaps the range, so faults in it while the change is in flight are retried. The handler must realign the stack, because kubridge runs it on the faulting thread's stack, which is not always 8-byte aligned.
- Samples go to `ux0:data/kotor2/pcprof.bin`; `tools/pcprof_report.py` (engine-prof worktree) analyses them against the frame log.

The samples are biased. The protection change takes effect on the game thread's core only at its next translation miss, so a sample lands at the next call into another page. 31.5% of samples fell exactly on a function's first instruction. Code outside `.text` (vitaGL, the loader, libc) is not protected, so its time lands on the next engine page fetched. The `ProgramType::ProgramType` entry collected about 15% of samples this way, which was vitaGL draw time. Self figures are therefore unreliable; figures inclusive of the call chain match exact timers at the subtree level.

The exact timers (`loader/fx_ab.c`, diagnostic) patch the entry of a dozen subtrees. Busy spot, ~375 draws, shadows on, profiled frame 66.9 ms:

| Subtree | ms per frame |
|---|---:|
| `Scene::Render` | 42.3 |
| └ `RenderStaticGeometry` | 15.5 |
| └ `DoGobBuckets` (incl. `Gob::Animate` 4.1, ~250 calls) | 15.6 |
| └ `RenderShadows` | 4.4 |
| └ `DoEmitterBucket` | 0.7 |
| `CServerExoAppInternal::MainLoop` (`CServerAIMaster::UpdateState` 9.7) | 11.3 |
| `CClientAIMaster::UpdateState` outside rendering | ~6.5 |
| `ManageSceneBSP` (every object re-sorted into rooms each frame) | 3.7 |
| `CSWGuiManager::Draw` | 2.3 |
| `FModAudioSystem::UpdateSystem` | 1.3 |

## 5. CPU and stall fixes

### 5.1 Audio contention: largest completed CPU fix

The original mixer held a shared lock while mixing, while `Sys_update` acquired it once per channel. Heavy scenes amplified this into thousands of acquisitions per frame.

| Metric | Before | After |
|---|---:|---:|
| Clear-view frame | 66–70 ms | about 33 ms |
| Red-corridor frame | 244–270 ms | 82–97 ms |
| `Sys_update` average | 2,537 us | 28 us |
| Game-thread acquisitions | about 11,200/frame | about 82/frame |
| Cumulative lock wait | about 93 ms/frame | 0.85 ms/frame |
| Waits over 1 ms | 3,449/session | 0 |
| Mixer hold | 2.3–3.8 ms/grain | snapshot 28–44 us + write-back 16–20 us |

The retained design snapshots channels under a short lock, mixes outside it, and performs generation-guarded write-back under another short lock. Cache frees are deferred while a snapshot is active.

### 5.2 Combat stutter: talk table in RAM

In fights the frame rate dropped to about 5 FPS, with single frames of 0.2–0.6 s. It was not a regression: an A/B on 2026-10-02 showed v0.2.0 with 8.2 frames of 200 ms or more per 10 s of combat. Probe builds with culling, the bloom skip and the caches of section 5.4 showed 3.0–4.7.

Hardware stall breakdowns (timing hooks, frames of 200 ms or more) put the worst frames in `CSWCMessage::HandleServerToPlayerCCMessage`, which builds the combat feedback text:

- Every slow call was a whole multiple of about 38.3 ms (38, 76, 114, 153, 191 ms), in bursts of 3–4 messages per attack (subtypes 18, 20, 22, 23).
- All of that time was in `CTlkTable::FetchInternal`, one call per string looked up: 14 calls took 535 ms in one frame. Fast calls cost almost nothing.
- `ParseStr` was fast, and the `fread`s on `dialog.tlk` took 0.2 ms per frame. The time was in the other stdio calls made on the card file:
  - each fetch calls `CExoFile::GetSize` twice, and each `GetSize` is `fflush`, `ftell`, `fseek` to the end, `ftell`, `fseek` back;
  - then the fetch seeks and reads twice, once for the entry and once for the text;
  - newlib turns `fflush` on a read stream into `lseek`s and drops its buffer.

`loader/ramfile.c` (`RAMFILE_TLK`, on by default) reads `dialog.tlk` (or `dialogf.tlk`) into memory on the first open: 9.9 MB, about 1 s at boot. The buffer is kept for the session, and each open gets an `fmemopen` stream over it. The stream is a real newlib `FILE`, so every stdio call the engine makes behaves as before, but nothing reaches the card.

Hardware, same save and fights, per 10 s of gameplay (probe builds):

| `dialog.tlk` | Frame | Frames ≥200 ms | Frames ≥500 ms |
|---|---:|---:|---:|
| On the card | 86.6 ms | 6.0 | 2.2 |
| In RAM | 70.6 ms | 1.0 | 0.1 |

A string lookup now costs about 0.08 ms. Seeks, tells and flushes on every other ordinary file add at most a few ms per frame, and only while loading.

### 5.3 Resource-cache budget

The loader exported `sysinfo` as `ret0`, so libkotor2's `GlobalMemoryStatus` read stack garbage. `CExoResMan` sizes its cache from it: half of total physical memory above 32 MB, else 16 MB. On hardware it got the 16 MB fallback while holding 32 MB, so it evicted constantly. Combat scripts were re-read from the card on every run.

The loader now reports `2 × RESMAN_BUDGET_MB` (64), a 64 MB budget. In matched combat runs, frames over 200 ms went from 56 to 28, and the time spent in them from 20.3 to 11.3 s.

Some script reads remain: `CExoResFile::ReadResource` still takes 100–150 ms in some combat frames (section 13).

### 5.4 AI walk pass and `CanSee` caches

- **AI walk pass** (`loader/ai_list_cache.c`, `AI_LIST_CACHE_ENABLE`): `CServerAIMaster::UpdateState` re-runs its walk pass over the whole AI list after every object it updates. That is about 130 passes over ~255 entries, or ~33k `GetObjectAtPosition` lookups per frame, and only creatures are ever acted on. Ids already seen as non-creatures this frame now return NULL at that one call site. This saves about 1.1 ms per frame.
- **`CanSee`** (`loader/cansee_cache.c`, `CANSEE_CACHE_ENABLE`): line-of-sight raycasts of about 0.6 ms each, asked about five times per frame by UI code. A result is reused for 3 frames per (viewer, target). This saves about 0.5 ms per frame.

Both are on by default.

### 5.5 Archive read cache

The layer measurement (section 4.3) showed the game thread blocked in the engine for 7–27 ms per frame in ordinary gameplay. `[PERF:io]` showed about one 70–100 KB read of `main.obb` per frame, taking 10–39 ms. Most came from `CVirtualMachineInternal::RunScript`: each run of a script reads the compiled script from the archive again, and `k_ai_master` runs about 8 times a second. The engine's resource cache held 33 of its 64 MB, so this was not eviction.

`loader/obb_cache.c` (`OBB_CACHE_MB`, 16 by default) caches reads of the shared `.obb` handles by exact (archive, offset, length). It sits after the mount replay cache.
- A read is admitted the second time it is seen, so one-off reads while loading do not churn it.
- Items are up to 1 MB, and the least recently used go first.
- The archives are read-only, so nothing needs invalidating.

Hardware A/B, alternating 10 s windows in one session:

| Cache | Frame (mean) | Game thread waiting |
|---|---:|---:|
| Off (6 windows) | 75.0 ms | 10.2 ms/frame |
| On (5 windows) | 60.2 ms | 1.4 ms/frame |

Each on window was 7–26 ms faster than the mean of its two neighbouring off windows (median about 18 ms). The cache held 12 MB without evicting.

In a later session with the release build, the cache filled its 16 MB and evicted some items. That build averaged about 53 ms per frame (43–66 ms) in gameplay at 200–420 draws per frame. 9 of 12 windows had no frame of 200 ms or more.

### 5.6 GL state filter

The profile in section 4.3 showed thousands of state calls per frame running gles2-bc's full path, which has no redundancy check. `loader/gl_state_filter.c` (`GL_STATE_FILTER`, 1 by default) replaces the PLT slots of `ASLgl::glEnable`/`glDisable`, `glActiveTexture(ARB)` and `glClientActiveTexture(ARB)`. A call that sets the value last passed through the filter is dropped. Texture caps are keyed by texture unit.

Nothing else changes these states:
- `ASLgl::glPushAttrib`/`glPopAttrib` and the client variants are empty.
- The engine's direct vitaGL calls are framebuffer binds and `glBlendEquation`.
- `ASL_GLBlitter` saves and restores through the same `ASLgl` entry points.
- gles2-bc's enable setters are called only from `OpenGLES20Context::glEnable`/`glDisable`, which only these `ASLgl` entry points reach.
- `ASLgl` keeps alpha test (`s_useAlphaTestShader`) and fog itself, and its special cases are idempotent.

The loader's own vitaGL drawing does change state behind gles2-bc, so the filter forgets its copy after long frames and whenever the loading screen or its font draws. `GL_SCISSOR_TEST`, which a loader GUI fix sets directly, is never filtered.

`ASLgl::glTexParameteri` ends with a `glGetIntegerv(GL_TEXTURE_BINDING_2D)` whose result it never reads, a synchronous query into vitaGL 440–600 times per frame. That instruction is replaced by NOPs, after its bytes are checked.

Two hardware runs cycled OFF / VERIFY / ON every 10 s. VERIFY skips nothing; it checks each call the filter would skip against `ASLgl`'s answer.

| Run | ON (mean frame) | OFF (mean frame) | ON vs neighbouring OFF |
|---|---:|---:|---:|
| 1 | 56.9 ms | 63.9 ms | 2–12 ms faster, median ~7.6 |
| 2 | 60.7 ms | 65.9 ms | 1–9 ms faster, median ~3.6 |

About two-thirds of `glEnable`/`glDisable` calls and about 30% of texture-unit calls were repeats, so 2.0–3.1k calls per frame were dropped. VERIFY found no disagreement on the active or client-active unit, or on any cap vitaGL keeps (blend, depth, cull, stencil, …).

The only flagged caps were `GL_LIGHTING` and `GL_ALPHA_TEST`. The check cannot read either: gles2-bc forwards `glIsEnabled` for lighting to vitaGL, which does not track it, and `ASLgl` keeps alpha test outside gles2-bc.

### 5.7 GL worker thread

`loader/gl_worker.c` (`GL_WORKER_MODE`, 2 by default) moves vitaGL to a worker thread on core 2:
- Every vitaGL function the loader uses is wrapped with `-Wl,--wrap`. The 145 wrappers are generated by `tools/gen_glw.py`.
- Calls are recorded into a 1 MB ring. The worker initialises vitaGL, since GXM binds to the thread that does, and replays the ring.
- The game thread moves to cores 0–1 and may run one frame ahead.
- Value-returning calls (`glIsProgram`, `glIsEnabled`, `glGetIntegerv`, `glGetError`, occlusion query results) are answered from a game-side copy of the state, so steady gameplay makes no blocking call.
- Draws from client memory copy their vertex and index data into the ring (about 100 KB per frame).

What it took to get a gain:
- The worker polls an empty ring for about 0.2 ms before sleeping. Waking it for each command had cost about 10 ms per frame.
- Commands are published 32 at a time.
- Repeated state calls are dropped before recording (2,000–2,700 per frame). This shortens the worker's time but not the frame.
- Cross-thread variables sit in cache lines written by one thread only, and the worker times runs of commands rather than each command. This halved the worker's time, from about 18.6 to 8 ms per frame.

The same eboot was played twice at the same spot, with `ux0:data/kotor2/glw_mode.txt` switching the worker off (0) and on (2). At ~400 draws per frame:
- vitaGL on the game thread cost about 8 ms (2.2 ms state calls, ~6 ms draws);
- recording costs about 4.5 ms (~0.4 µs per state call, ~1.7 µs per draw);
- engine time was unchanged at about 49 ms, so the cores do not slow each other down;
- the frame went from 61.7–63.7 to 58.5 ms.

### 5.8 FMOD update skip

The game calls `FMOD::System::update` about 50 times per frame, from `messagepump()`. The loader's update took the mutex and scanned every channel each time, almost always with nothing to deliver: 1.3 ms per frame.

A dirty flag (`AUDIO_UPDATE_SKIP`, 1 by default) is now set under the lock wherever an END may become deliverable: a channel finishes, an END moves to the retirement ring, or a callback is installed. The update returns at once while the flag is clear. Timed in-session, the update went from 0.5–1.1 ms per frame to 0.0 ms.

### 5.9 Server AI budget

`CServerAIMaster::UpdateState` updates AI objects until a fixed budget is spent: `movw r4, #10000` (microseconds) at libkotor2+0x4a7e00, shared out over five AI levels. Objects it does not reach wait for a later frame. On the Vita it took 7.0–9.2 ms per frame. The `AIUpdate`s themselves were about 2.5–3 ms; the rest is per-object loop work: about 1,000 high-resolution timer reads and 4,400 object lookups per frame, plus events.

In-session A/B (`loader/fx_ab.c`, busy scenes):

| Budget | Master time | Objects updated per frame |
|---|---:|---|
| 10 ms (engine) | 7.0–9.2 ms | ~250, every object |
| 5 ms | 5.3–6.4 ms | ~65% |
| 3 ms | 3.4–5.3 ms | ~35%; creatures about every third frame |

The user played the 3 ms windows, noticed nothing, and chose 3 ms. `loader/ai_budget.c` (`AI_BUDGET_US`, 3000 by default) checks the instruction bytes before rewriting the immediate.

### 5.10 Native DXT texture uploads

A stall profile (user-mode PC sampler, every frame of 80 ms or more logged) put CPU texture work at the top of the stalls. `ASLgl::glCompressedTexImage2D` decodes DXT1/3/5 to RGBA8 on the CPU for every mip level (`DecompressDXT*_8888`). For level 0 it also rebuilds the whole chain with `gluBuild2DMipmaps`, which the engine's own level uploads then overwrite. That was 35% of loading stall time, texture creation another 15%, and texture creation was the largest named cause of gameplay hitches.

`loader/dxt_native.c` (`DXT_NATIVE`, 1 by default) hooks `glCompressedTexImage2D` and its ARB alias. A power-of-two 2D DXT upload goes to vitaGL compressed (UBC1/2/3). The hook first records `GL_RGBA` as the bound texture's format in gles2-bc (`OpenGLESState::setBoundTextureFormat` + `setTextureFormat`), as the decoded upload does, because the uber shader reads it. The chain ends at the last full 4x4 block. Every other upload goes through ASLgl as before.

The hook places the blocks itself instead of through vitaGL's compressed upload. vitaGL 38d2f97 swizzles a compressed level with an asynchronous `sceGxmTransferCopy`, a path added upstream on 2026-05-09. It also grows the texture at each new level with `vgl_realloc`, which frees a moved block at once while that copy can still be writing into it.
- In Vita3K this corrupted the heap: a host crash during the first area load.
- On hardware, with the copies fenced (`sceGxmTransferFinish`), some combat effect sprites still came out scrambled. A startup self-test showed the copy's layout itself is right: every level of DXT1/DXT5 chains from 4x4 to 512x512, square or not, matched the CPU swizzle when each copy was waited for. What breaks is the copy running asynchronously.

So each level is allocated without data on the GL thread (a new GL worker op, `glw_call`), and the blocks are swizzled in with vitaGL's exported CPU swizzler (`SwizzleTexData64Bpp`/`128Bpp`), its pre-2026-05-09 path. At a chain's first level after 0, the block is grown once to the chain's last level. No GPU copy is involved.

Hardware result in the same scenes: texture building in stalls fell from 8.9 s to 3.3 s of excess time, and the CPU DXT decode left the profile. `gluBuild2DMipmaps` remains for uncompressed images (GUI art, the minimap) through `GLRender::CreateTexture`. `ux0:data/kotor2/dxt_mode.txt` containing `0` sends every upload back through ASLgl.

### 5.11 Music and voice streams

After the textures, every gameplay hitch of 300 ms or more fell on a music start or change (`PlayStinger`, `PlayBattleMusic`, `PlayMusic` -> `CExoStreamingSoundSourceInternal::InitializeSource`). The profiler caught almost no samples in them, so the time was outside the engine: in the loader's FMOD replacement, `FMOD::System::createSound`.
- A stream under 6 MB of PCM was decoded whole on the game thread at open. A battle stinger (10-26 s, 1.8-2.2 MB of PCM) took a 620 ms frame; a 44 KB cue took 306 ms.
- Every stream read its whole file first, at about 10 MB/s from the OBB: 319 ms for a 3.2 MB track.

Now:
- Every MP3 stream streams, whatever its length. RIFF sounds (plain PCM and the IMA-ADPCM area beds) cost no decode and keep the whole-asset path and its cache.
- A stream reads its first 128 KB on open (5-8 s of music). `System::update` reads the rest, 32 KB per 12 ms, while it plays, and the decoder waits at the loaded edge. Hardware: 3.2 MB tracks loaded in about 2.2 s with no underruns.
- `FMOD_LOOP_NORMAL` loops inside the stream, seamlessly and without an END, as FMOD does. The menu theme and area beds are created that way. The game itself replays some `LOOP_OFF` cues at intervals; around the Kreia conversation a 10.6 s cue returns about every 30 s.
- The hardware decoder allows 6 MP3 instances (`SCE_AUDIODEC_MP3_MAX_NSTREAMS`): 5 for streams, 1 spare. The game keeps stopped and finished streams open (sound objects hold theirs between plays), so all 5 can be taken while few are audible. A 13 s voice line then fell back to a whole decode on the game thread: 547 ms. A stream that finds no free hardware decoder now decodes with minimp3 on the audio thread instead (`loader/minimp3.h`, lieff/minimp3 ea99364, CC0, NEON paths on).

minimp3 on hardware costs 0.78-0.85 ms per 44.1 kHz stereo frame (26 ms of audio, about 3% of a core per stream) and 0.52-0.64 ms per 32 kHz mono voice frame. In a 9-minute test with half of all MP3 streams forced onto it (`AUDIO_SW_STREAM_TEST`), there were no underruns and no whole decode on the game thread. The user heard no distortion.

A whole-asset MP3 decode on the game thread runs at about 0.65 ms per KB of PCM, roughly 50 times slower per frame than the same hardware decoder on the audio thread. The cause is not known. Thread placement (below) did not change it.

### 5.12 Thread placement: no effect

The game thread runs on cores 0-1, the audio thread on any core, both at the default priority, with the GL worker on core 2. An in-session A/B rotated three layouts every 30 s: as is; game on core 0 and audio on core 1; game on core 1 and audio on core 0. Window means swung from 34 to 57 ms with the scene within each layout (layout averages 42.7-46.4 ms), in no consistent order, and slow frames did not change. Audio never underran in any layout. The layout stays as it is.

### 5.13 First-use GUI and conversation stalls

With textures and streams fixed, the remaining gameplay hitches (about 150-270 ms) were the minimap's first draw, dialog text and the start of a conversation. The PC profiler caught little of them (23-39 samples in 255-428 ms frames): the time was in the loader's file layer, libObbVfs, miniz and the card. `loader/stall_parts.c` (with `STALL_LOG_MS`) appends to each slow frame's `[stall]` line the game thread's time in file opens, reads (with KB) and seeks, GL worker waits, texture uploads and the 16-bit conversion, mip builds and `createSound`.

Three fixes followed:
- **Mip chains on the GPU** (`loader/mip_gpu.c`, `MIPGEN_GPU`). The engine builds the mip chain of every uncompressed image with its own `gluBuild2DMipmaps`, shrinking each level on the CPU. vitaGL ignores the pixels of a level above 0 of an uncompressed texture and downsamples level 0 on the GPU instead (`_glTexImage2D_FlatIMPL` -> `gpu_alloc_mipmaps`). For a power-of-two image the hook uploads level 0 through ASLgl and calls `glGenerateMipmap`. Hardware A/B, alternate images: during loading the engine path averaged 332 ms per image (2.85 s worst), the vitaGL path 76 ms (430 ms worst); in gameplay 36 ms against 2.8 ms.
- **Loose-file misses** (`FS_MISS_CACHE`). The engine looks for a loose copy of nearly every resource before the archives, opening `texturepacks/swpc_tex_*.{nwm,mod,sav,erf}` by relative path on every texture load, then `override/`, `streamsounds/`, `dlc/` and others: 1828 card misses in a 67 s Vita3K run. In those read-only folders a listing made once per folder answers the misses (`stat`, `access`, `SDL_RWFromFile`, `fopen` with relative paths resolved through `getcwd`); a write into a listed folder drops its listing. Vita3K: 197 misses left per run, all in folders the game writes. Hardware: the conversation start's 36-40 file opens went from 23-24 ms to 8 ms.
- **Cached held sounds**. A RIFF stream held decoded (voice, ambience) was read whole from the card before the decoded-PCM cache was checked, 9-19 ms for a sound already cached. The cache key only reads the first and last 256 bytes and the length, so a 4 KB head and a 256-byte tail now find it.

`fs_stat` tells the save list's caller by its return address (libkotor2+0x59d780, which needs the bionic stat layout). A timing wrapper around it emptied the Load Game list in testing: hooks that read `__builtin_return_address` must not be wrapped.

What remains is mostly first-time card reads at about 10 MB/s: the minimap's 714 KB image (60 ms), scripts spawning creatures mid-play (one read 3.6 MB in a 368 ms frame), dialog entries and their voice lines. Large non-power-of-two images still take the engine's CPU mip path during loading (up to 2.86 s for one image).

### 5.14 Boot and loading time

**The replay index was never written.** `loader/obb_index.c` records the archives' small reads (up to 4 KB) during the mount and serves them from one file at later boots. KOTOR I's loader mounts the archives itself and then calls `io_obb_mount_done()`; in KOTOR II the engine mounts them and nothing made that call, so every boot made about 60,760 small card reads. The loader now calls it at the first presented frame. Hardware, two boots: first frame 33.9 s, then 24.3 s; card time before the first frame 11.7 s, then 3.5 s (52 reads, 60,768 served from the index). The index is written once, at the first launch of a build.

**Non-power-of-two images.** GLU scales such an image to the power of two nearest in its top two bits and shrinks every level on the CPU (up to 2.86 s for one loading screen). `loader/mip_gpu.c` now rescales level 0 to the same size (bilinear, `image_rescale` in `loader/pixel_ops.c`), uploads it and lets vitaGL make the chain. The 16-bit conversion (`tex16_pack`) is NEON too, byte-identical to the scalar loop (`tools/test_pixel_ops.c`). Hardware, game thread per image:

| Image | Before | After |
|---|---|---|
| 2048x2048 RGBA | 429 ms | 187 ms |
| 1920x1200 RGBA -> 2048x1024 | 837 ms | 354 ms |
| 1024x767 RGBA -> 1024x512 | 208 ms | 105 ms |
| 1100x655 RGBA -> 1024x512 | 565 ms (engine) | 91 ms |
| 1023x1024 RGB -> 1024x1024 | 338 ms (801 ms engine) | 194 ms |

These passes run at about 150 MB/s, so they appear to wait on memory more than on arithmetic.

**Card throughput.** A test build (`CARD_BENCH`) read fresh regions of the main archive at the first frame: 16 KB reads 9.1 MB/s, 64 KB 9.8 MB/s, 512 KB 9.9 MB/s, random 64 KB 9.7 MB/s. Larger or read-ahead reads would not be faster; about 10 MB/s is the card.

**Where the time goes now** (PC profiler, each sample weighted by the time since the previous one, because time outside the engine lands on the engine instruction it returns to):
- Boot, 23.9 s to the first frame. The library loads, hooks and vitaGL take 0-5.3 s. libObbVfs's file tables take a few seconds: each OBB ends with a zlib-compressed `std::set` of its files (361 and 18,447 entries), whose comparator calls `tolower` four times per character. Key tables, the DLC folder scan and the sound-option ini writes follow (5.3-15.2 s together). `dialog.tlk` into RAM takes 1.0 s and the rules tables 1.0 s. GUI setup is about 6.6 s: 3.5 s reading about 28 MB of large images, the rest CPU on them (`ImageGetAlphaMean`, a float division per pixel, 0.8 s; red/blue swaps in `CResTGA::OnResourceServiced` and back in `CAuroraTexture::Unload`, 0.9 s; `glGenTextures` waiting for the GL worker, 0.7 s; the loader's conversion and rescale, 0.7 s). The legal screen then sleeps 2.5 s.
- Save load, 24 s. 13.3 s are archive reads. The rest:
  - `CopyFileA`, 2.2 s: it copies the module into `currentgame/` through a C++ stream, in 4 KB card writes;
  - loose files under `dlc/`, about 2 s: newlib's stdio buffer is always 1 KB, so they are read 1 KB per card access;
  - the copied module's key list, 0.8 s: 5,469 reads of about 8 bytes, each after a seek that drops the buffer;
  - GFF field lookups, about 1 s: `CResGFF::ByteSwap` is a protected wrapper around an empty function;
  - whole decodes of short sounds, 0.7 s;
  - GL name round trips, 0.35 s.
  None of these is changed yet.

**An intermittent slow boot.** About one boot in three spends 12.1-12.7 s instead of about 0.5 s scanning `dlc/mods_english/override` (477 files). This adds about 11.5 s, with or without the profiler and the replay index. `_findfirst` stats every entry while the directory is open. In a slow boot each call takes longer than the one before, from about 7 ms to about 50 ms (+0.1 ms per entry), on the same curve in every slow run. The directory's sectors then seem to be re-read from the card at each lookup, never kept. The trigger is not known: not the build, the profiler, the replay index, the GL worker, thread placement or saving. Answering these `stat` calls from the engine's own `readdir` results would avoid it, but that change is parked.

The profiler itself slows boot code outside the engine (libObbVfs, `stat`) about 2.3 times. Its self samples also cluster at instruction-cache-line boundaries, so the inclusive chains are the reliable figures.

## 6. Room and visibility experiments

### 6.1 Scoped VIS edge: successful proof

While `001ebo15` was current, excluding `001ebo5` reduced stable windows from roughly 228–298 draws/frame to 138–140 draws/frame. Physical-Vita performance reportedly improved from about 14 FPS to 25 FPS.

This proved room-level workload reduction is high value and `CollectActiveRooms` is a viable transient interception point. It was not retained as a general solution because it hard-coded a module/room relationship.

### 6.2 Generic door-aware whole-room filter: rejected

The portal algorithm compared geometry-only and closed-door-aware traversals, delayed changes for one second, and restored on uncertainty. In Peragus it could produce broad candidate sets; removing dependent rooms caused black screens, missing geometry, or doorway defects.

A later aggressive A/B remained around 4,050–4,081 draws/sec, effectively equal to the unfiltered roughly 4,013 draws/sec baseline. The tested route did not yield meaningful removals.

### 6.3 Gob-only ownership filter: rejected

The safer variant retained rooms/static geometry and rejected only Gobs owned exclusively by stable door-occluded rooms. Shared and force-visible Gobs were protected.

| Build | Mean draws/sec | Median |
|---|---:|---:|
| Unfiltered | 4,012.64 | 4,076.02 |
| Gob-only | 3,995.70 | 4,093.81 |
| Difference | −0.42% | +0.44% |

This is noise. Rebuilding ownership sets also adds CPU work, so keep `PERF_DYNAMIC_PORTAL_GOB_FILTER=0`.

### 6.4 Distance-only 10 m filter: successful experiment

Physical-Vita hashes:

- filtered: `d906561dc581a0305ce7a33e7f382a098a12e0e0821e89ce8990da2a2ce39e0a`;
- unfiltered control: `d9121c5f3f300a7a10fa69e4118d30585ec0cff36b96e3c264c13510630d9b9e`.

Best matched final three windows:

| Build | Draws/frame | Presentation-rate proxy |
|---|---:|---:|
| 10 m | 263.19 | 14.75/s |
| Unfiltered | 364.82 | 11.45/s |
| Difference | **−27.86%** | **+28.81%** |

Broader final five windows:

| Build | Draws/frame | Presentation-rate proxy |
|---|---:|---:|
| 10 m | 267.55 | 14.81/s |
| Unfiltered | 288.57 | 13.99/s |
| Difference | −7.28% | +5.83% |

The user could feel the slowdown after returning to the unfiltered control and considered the 10 m visual result tolerable in the tested route, describing it as an old-school game with distance fog.

This is a promising optional profile, not a globally validated default. It needs testing across module types, cutscenes, combat, transitions, elevators, large outdoor areas, and scripts involving visible room-owned objects.

### 6.5 Engine visibility threshold

The Android binary initializes `enablevisibilitytest` to zero, making its size test pass unconditionally. The loader restores the engine mechanism at `0.02`.

Increasing it to `0.08` improved rejection counts by only about 2–4% in heavy windows and left a representative frame around 98 ms while increasing pop-in risk. Keep `0.02`.

### 6.6 GPU occlusion culling of Gobs: adopted

The engine submits every Gob (creature, placeable, door) that passes its frustum and size/line-of-sight tests. A 2026-10-01 hardware census bracketed each outermost `Gob::Render` with a `GL_ANY_SAMPLES_PASSED` query, read a frame later without waiting. In the heavy windows of the test route, 61–80% of rendered Gobs (76–86% of Gob draws) produced no visible sample, and their `Gob::Render` CPU cost 7–19 ms per frame, about 0.55 ms per hidden Gob. The GPU was busy for about 35–39 ms of a 70–100 ms frame, so the queries have room.

`loader/occlusion_cull.c` (`OCCLUSION_CULL_ENABLE`, on by default) acts on those results:

- It uses only two PLT slots. `Gob::Render` carries the query and a draw count. `Gob::VisibilityCheck` runs the engine's own check first, then returns 0 for a Gob whose last two results were hidden, so `DoGobBuckets` skips it exactly like an engine cull.
- A skipped Gob is re-rendered once every 3 frames to re-test. It is back as soon as a re-test shows a sample.
- Queries are double-buffered (60 per frame, within vitaGL's 128 slots). Results are collected at the start of the next Gob pass if the GPU has finished, so decisions apply one frame after the query.
- Never skipped:
  - Gobs that issue no draws inside `Gob::Render`, since their parts may be drawn by a later pass;
  - Gobs flagged `IsAlwaysRendering`.
- The state table is cleared after long frames (loads).
- A fail-safe turns culling off for the session when nearly every result reads hidden. That is what Vita3K produces, because it does not emulate visibility queries.

Hardware A/B at the heaviest spot (about 44 Gobs per frame, room-mesh pass about 16 ms), using windows in which the scene rendered every frame:

| Culling | Frame | FPS | Render | Gob pass |
|---|---:|---:|---:|---:|
| Off | 92.8 ms | 10.8 | 57.5 ms | 30.6 ms |
| On | 74.9 ms | 13.4 | 41.9 ms | 15.2 ms |

Across every window of the run, the ratio of Gob pass to room-mesh pass (independent of menu frames) had a median of 1.01 with culling on (12 windows) and 1.92 with it off (5 windows). At a lighter spot (mesh pass about 9.5 ms) the Gob pass went from about 15 to 9 ms. All other passes were unchanged. No artefacts were seen while playing. A Gob stepping out from cover can appear up to 3 frames late, about 0.2 s at 15 FPS, which was not noticeable.

With culling on, about 6 ms per frame at the heavy spot still goes to rendering Gobs that are hidden: the re-tests, plus the two frames before a Gob is first skipped.

## 7. Shader compilation and cache

### 7.1 Doorway hitch root cause

The doorway stutter remained with portal/Gob filtering disabled. Logs identified synchronous first-use shader creation:

- program 14 link: 606.6 ms;
- program 15 link: 608.2 ms;
- combined stall: about 1.2 seconds.

The variants involved skinning, then skinning plus cube mapping and bump mapping.

### 7.2 Persistent cache behavior

A hardened diagnostic run showed 16 warm menu cache hits, zero misses, and successful GXM registration. At the doorway, cached programs 14 and 15 appeared within roughly 20 ms rather than consuming 1.2 seconds.

KOTOR II calls `glUniform4fv(..., 3, ...)` even for `u_fragmentShaderParams[2]`. The loader records linked capacity and clamps the upload, preventing a 16-byte overwrite of adjacent vitaGL metadata.

This fixes one proven corruption path. An older warm-cache run recorded a separate abort/black screen, so cache recovery remains documented. However, the exact cache-enabled/no-`LOG_ERRORS` recipe was reconstructed from the archived source, deployed, and confirmed to run well on physical Vita. It is therefore the v0.2.0 performance profile, while still requiring broader cold/warm and module-transition validation.

### 7.3 Diagnostic trace observer effect

A cache-diagnostic vitaGL checkout opened, wrote, and closed `shader-cache-trace.log` on every matrix update. One short menu run generated 938 `UNIFORM_M4` records and fell below 1 FPS. Cache lookup was healthy; synchronous diagnostic I/O caused the slowdown.

Benchmark vitaGL must contain none of `cache_diag`, `shader-cache-trace`, `UNIFORM_M4`, or `UNIFORM_LOC` render-path tracing.

## 8. vitaGL and texture findings

### Retained

- Packed-VBO offset patch: offsets above 64 KB remain in the stream base because GXM’s attribute offset is 16-bit. This fixed exploding/skewed characters.
- `SAMPLERS_SPEEDHACK=1`.
- `TEXTURES_SPEEDHACK=1`.
- `GL_TEX16_CONVERT=1`: converts RGBA8 to RGBA4444 and RGB8 to RGB565, excluding render targets and cube maps. The texture working set peaked near 102.6 MB against 96 MB CDRAM.
- Redundant texture/program bind filtering.

### Rejected or low value

- `DRAW_SPEEDHACK=1`: physical-Vita GPU/driver lockup.
- Client-index direct use: dominant draws are EBO-backed.
- Index-pool optimization: measured ceiling about 0.20 ms/frame.
- Exact vertex-layout/state caching: lower priority because complete GL cost is much smaller than engine submission.
- Raising system-memory thresholds cannot enlarge fixed CDRAM.

## 9. Effects experiments

| Experiment | Result | Decision |
|---|---|---|
| Disable force glow | No meaningful gain | Reject |
| Disable framebuffer allocation | Black output/null-pbuffer crash | Never use |
| Disable environment mapping/EMLM globals | Core geometry/characters vanish | Reject |
| Redirect env/EMLM to flat render | Later allocator corruption | Reject |
| Disable emitters | Fog disappears; small possible gain | Optional only |
| Disable shadows/bump/specular | Geometry-safe in tested route | Shadows off by default (section 9.2); bump/specular optional |
| Disable grass/lens flares | Expected visual loss; low ceiling | Optional |
| Disable all lighting/lightmaps | Characters/path disappear | Reject |
| `disablepostprocessing=1` alone | Not isolated cleanly | Open |

### 9.1 Mobile bloom: skipped

Aspyr's `FrameBufferModificationsEndIos` runs a bloom chain every frame:

- a downsample into a half-resolution buffer;
- a horizontal and a vertical blur (`kotorbloom.frag`, 17 taps with dependent reads), about 6 ms of GPU each;
- an additive quad.

`loader/bloom_ctl.c` (`BLOOM_DISABLE`, default 1) hooks `RenderModificationQuad`'s GOT and PLT slots and skips those four draws by return address. Each call site's bytes are checked at install. The scene copy and the final composite still run. GPU time per frame went from 36 to 22 ms at the test spot, and no visual difference was noticed in play. The `visualizepass2` option does not control this chain: it only gates desktop paths this build never calls.

The graphics-menu “Framerate” checkbox toggles a limiter (`g_bFrameRateLocked`), not a performance mode. Leaving it unticked is correct for throughput.

### 9.2 Creature shadows, emitters, environment maps

The renderer reads its effect switches every frame:
- `enableshadows` in `Scene::RenderSinglePass`;
- `enableemitters` in `Scene::DoEmitterBucket`;
- `enableenvmap` in `RenderEnvironmentMapped`. Clearing it takes the engine's own no-env-map path. Clearing `enablerenderenv` instead would skip those surfaces entirely.

One hardware session cycled base / shadows off / emitters off / env-map off every 10 s, at the same spot (~375 draws):
- shadows off: frame 66.9 → 63.5 / 62.5 ms, about 80 fewer draws; `RenderShadows` was timed at 4.4 ms;
- emitters off: 0.6–1.2 ms;
- env-map off: no measurable change.

Shadows are off by default (the user's call), through the game's own option. At start-up `CClientOptions::LoadOptions` reads `[Graphics Options] Shadows` from `swkotor2.ini` and calls `SetShadows`, which branches through two veneers to `AurEnableShadows` or `AurDisableShadows`. `loader/ini_defaults.c` (`INI_NO_SHADOWS`, 1 by default) writes `Shadows=0` and `Soft Shadows=0` into the ini at every launch, before the game reads it. The rest of the file is left as it is.

In Vita3K, with `Shadows=0` and no loader override, `Scene::RenderShadows` never ran (neither the projected nor the stencil pass). The earlier override zeroed `enableshadows` in engine memory and re-zeroed it at every swap, because the game applied the ini's `Shadows=1` on top of it; it was removed.

Other shadow knobs:
- `LightManager::m_nMaxShadowLights` is already 1.
- `maxshadowdist` (35 m) is read by `Scene::DoGobShadows` per frame.
- Shadow blobs are part of creature appearance data (`CSWCCreature::ApplyShadowBlob`); no ini option controls them.

No game option controls the mobile bloom:
- `IosBloomEnabled()` returns 1.
- `Frame Buffer=0` (`SetFrameBuffer` → `AurDisableFrameBufferEffects`) left draws per frame identical on the same route, so the bloom chain still runs.
- The bloom skip (section 9.1) stays.

## 10. Logging and telemetry policy

Fine-grained instrumentation can add roughly 2–3 ms/frame in heavy scenes. Historical line-at-a-time logging measured about 8–11 ms per line in some captures.

Use three levels:

1. **Production/support:** buffered logs and crash diagnostics.
2. **Maximum-FPS benchmark:** `LOG_DIAGNOSTICS=0`, `LOG_BENCHMARK_MINIMAL=1`, full telemetry off; only failures, warnings, errors, and panic dumps survive.
3. **Minimal draw A/B:** level 2 plus `DRAW_FRAME_BENCHMARK_ENABLE=1`. Each `[DRAW_FRAME]` window also logs:
   - average and maximum frame time;
   - counts of frames of 100, 200 and 500 ms or more and of 1 s or more;
   - the resource-cache budget and use.

   Use these counts to compare stutter.

Always check for stale files before attributing trace data to a new build.

## 11. Optional hardware clock profile

The application requests the public `444/222/222/166` MHz profile. A separate PSVshellPlus 500 MHz test reportedly improved about 13 FPS to 15 FPS. This is consistent with CPU-bound submission but remains a single-device anecdotal A/B. Kernel plugin, heat, and battery trade-offs must remain optional.

## 12. Do not repeat without new evidence

- Hard-code more room names or VIS edges as a general solution.
- Mutate canonical VIS state for transient rendering.
- Treat every walkmesh opening as a door.
- Assume broad closed-portal room ownership matches all render dependencies.
- Suppress all characters as a production optimization.
- Raise visibility thresholds aggressively and assume rejection counts imply frame-time gains.
- Replace environment/EMLM functions with incompatible flat wrappers.
- Disable framebuffer allocation.
- Enable `DRAW_SPEEDHACK` on hardware.
- Re-enable synchronous per-uniform/cache trace writes.
- Compare draws/sec when the question is workload per frame.
- Compare whole-run averages containing different rooms, loading, or movies.
- Use `visualizepass2` to switch bloom off: it gates only dead desktop paths (section 9.1).
- Hold SELECT as a runtime A/B toggle: it opens the game's main menu and contaminates the windows.
- Upload compressed textures through vitaGL's own compressed path (38d2f97): its asynchronous GPU copy scrambles textures in game and races its own realloc (section 5.10).
- Make looping streams play once and raise END: it only puts a gap into seamless loops. The game's own interval replays are expected (section 5.11).
- Pin the game and audio threads to separate cores without new evidence (section 5.12).
- Trust Vita3K for texture-upload or GPU-copy correctness: it emulates the copy synchronously, and none of the hardware faults above showed in it.
- Wrap a hook that reads `__builtin_return_address(0)` (`fs_stat`, `ai_list_cache.c`, `bloom_ctl.c`, several in `main.c`): the wrapper becomes the caller (section 5.13).
- Expect larger or read-ahead card reads to load faster: 16 KB to 512 KB reads all run at 9-10 MB/s (section 5.14).
- Read unweighted PC-profiler counts as time: time outside the engine (card waits, libObbVfs, vitaGL) counts as one sample however long it lasts (section 5.14).

## 13. Recommended next work

1. Validate the 10 m profile across representative modules, combat, party members, cutscenes, transitions, and outdoor areas.
2. Sweep 10, 15, 20, and 30 m with matched controls. A larger radius may preserve most gain with less pop-in.
3. Use player position if a stable symbol/layout is verified; current code uses camera position as a proxy.
4. Add enter/leave hysteresis to reduce room-edge popping.
5. Test a hybrid profile: always retain current and directly adjacent rooms, then radius-filter all others.
6. Identify the remaining historical warm shader-cache abort and add version/size/register validation with safe fallback.
7. If disk cache remains unsafe, prewarm known shader variants during loading.
8. Investigate safe static batching/material grouping.
9. Isolate post-processing disable while retaining all geometry/lighting gates.
10. Keep every hardware experiment reversible with exact hashes.
11. Make occlusion re-tests cheap: query a hidden Gob's bounding box with color and depth writes off instead of re-rendering it. This would recover most of the remaining ~6 ms at the heavy spot and cut pop-in to one frame. It requires restoring exactly the GL state that gles2-bc caches.
12. Done (sections 5.10, 5.11, 5.13 and 5.14): stream opens no longer read or decode on the game thread, DXT textures are no longer decoded on the CPU, uncompressed images get their mips on the GPU (non-power-of-two ones rescaled with NEON), loose-file misses no longer touch the card, and the archive replay index is written. Left:
    - loading the minimap image when the area loads instead of at its first draw;
    - the intermittent slow boot (section 5.14), parked;
    - the loading costs measured in section 5.14 (module copy in 4 KB writes, 1 KB loose reads, GFF lookups, image passes), each worth 0.3-2 s.
13. NWScript: `k_ai_master` costs about 17 ms per run and is interpreter-bound (4.5–7 ms per frame, about 18 ms in combat).
14. The draw path is the rest of the translation cost: about 39 µs per draw, ~21 µs in gles2-bc's shader and state preparation and ~15 µs in vitaGL's `glDrawElements` (section 4.3). `glBindTexture` and `glBindProgramARB` are not safe to filter simply. Some engine code binds textures in vitaGL directly, and the program depends on the fog and alpha flags.
15. Done (section 5.7): a GL worker thread at the vitaGL boundary.
16. The archive cache filled its 16 MB in a longer session. Check how much it evicts before raising it.
17. Some sounds cut in and out in release candidate 1. Suspects: the FMOD update skip (section 5.8) and the 3 ms AI budget (section 5.9). Each has its own switch.
18. Engine targets from section 4.4:
    - the per-draw mesh path (`RenderStaticGeometry` and `DoGobBuckets`, ~13–15 ms each);
    - `ManageSceneBSP` re-sorting every object into rooms each frame (3.2–3.7 ms);
    - animating Gobs that occlusion culling hides (`Gob::Animate`, ~4 ms at ~250 calls per frame);
    - the AI master's per-object overhead (timer reads and object lookups).
19. GL recording still costs about 0.4 µs per state call, more than vitaGL's own setter. The cache-line fix did not change that; the cause is not yet known.

## 14. Reproducibility hashes

The following executable hashes identify the physical-hardware A/B builds used
for the summarized measurements. Private runtime data and machine-specific test
logs are intentionally not part of the source repository.

The v0.2.0 candidate was reconstructed from source snapshot SHA-256
`9b781fed3ca485159f94a945269449cef577baad7c6b4322904dcd0bc00eacff`
using the exact vitaGL recipe in section 2.1. It was round-trip verified over FTP
and confirmed at about 13 FPS in the tested second level without observed
rendering defects.

| Build | SHA-256 |
|---|---|
| Maximum-FPS quiet | `9b4d9ddd726767fe8da0e74405940896d427a6763c5e38d892ee37ff42465a46` |
| Draws/sec baseline | `38becce0a4a7d04cc1d145c76626f975986655deaea93b35fb8dd0e53caafd3d` |
| Gob-only A/B | `7172a44eb8b576d7c63f26f8b382ba09839fc27d65c839eb03dd4bf179b643dd` |
| Aggressive door-aware | `dd60eac723693c45b6c565f6489b208d9eef463b577f31664d2d231f1939eb88` |
| 10 m distance filter | `d906561dc581a0305ce7a33e7f382a098a12e0e0821e89ce8990da2a2ce39e0a` |
| Unfiltered draws/frame | `d9121c5f3f300a7a10fa69e4118d30585ec0cff36b96e3c264c13510630d9b9e` |
| Reconstructed v0.2.0 eboot | `e38ba46401c59abd826828e9e4db1134566fdac3c434226f4a3447ec9bb47727` |
| Reconstructed v0.2.0 VPK | `46844badc040e0000268dfc9bb9199c9920fc8d134db61028ef328a4f234789d` |
| v0.2.0 + slow-frame counter (stutter A/B) | `0c04193b1ead521d419e02414adc19037bdb0a69f525a86b57e9fbd772053d5c` |
| Talk table in RAM + probes (section 5.2) | `7812522fb34d5cfb55fb5edf3cb68fe5467d667db0e790172f6be5d485f162c6` |
| Archive read cache A/B + probes (section 5.5) | `28b0dd04589ac4cb7b735fef3bf8dfbd961a5a96947248feb82200ca09d49aac` |
| Release build, `perf/ab-b-optimizations` with the archive cache | `dc282fc796bef033cf057bc20b370f080ba1f1e452bf7664962cafeb1fd632f3` |
| GL state filter A/B + probes, run 2 (section 5.6) | `700b738b963607a7a3ce81e2323353696a82064b963dbb797262c777126c7ebe` |
| GL worker profiling build, modes 2 and 0 (section 5.7) | `9a1359817c6fc3c08e24d95b4d89e41f91cf9e4d26b4b5518bc9a35af17a6ea1` |
| Effect-switch A/B + subtree timers (sections 4.4, 9.2) | `9ddbe45a9c86fb780292fde9dbd05e68b0262ee07d2b3e2a7be52d6789ccb3bd` |
| Fix A/B: FMOD skip, AI budget (sections 5.8, 5.9) | `3c3e22c83417f3580bc4bdd57e453776aa6d7c2416ea3621124af69f4884498c` |
| Release candidate 1 as played (item 16; source of v0.3.0) | `aa72f280d8bc7a6341e8bd704f00a4c6072fb4df8c7ba4fd492d9435452e0ad3` |
| Stall profiler build (sections 5.10, 5.11) | `fd4da8b522755ec2a9387140f4b46603f8de980dece8e4f039209a907cc8a8ab` |
| Native DXT, CPU swizzle, with self-test (section 5.10) | `82035ed494a28f5443da52aaef6be314e1fbae4b736940fa44bec8e4d09670ac` |
| Streams with FMOD loop semantics, 5 hardware streams (section 5.11) | `7a2b0bc1b198bb8327f68f939f042ea45ae8d10e2a798097bb8327145e210e28` |
| Thread placement A/B (section 5.12) | `aa36ee33e3ed16fbcd54b3ffa0c0ae7f2e9d1a4597d5f463169599c1fb03772a` |
| minimp3 overflow, every other stream forced (section 5.11) | `3d2198050ac0103415900927d2174b9f715d833e512099a5afee4796f40bf7fe` |
| Stall breakdown + mip A/B (section 5.13) | `76ecb0d4b94ee192bb7f7b5486824c920d00564f624b7724d5939cd37ef52ef8` |
| Stall breakdown + misses, GPU mips, cached sounds (section 5.13) | `79b6d45d0c511d75b049faac637e3dd29b662060bedd3af9cc9c9f96b8e83dec` |
| Replay index, NEON image paths, card benchmark (section 5.14) | `e61feee1a0a488938bc570922cf2811ea560b186cd3276ffc75a36e7295b341c` |
| The same with the PC profiler (section 5.14) | `9495a28cb0f6fa201ac4842f14ff692ea9f94dde0e998596e4c56ae836b8efaf` |
| v0.4.1 eboot | `24feb8be0fa03e62dee2300a296421ccfbd82df53b60c10b393f6dbd257b5540` |

## 15. Current decision

v0.4.1 is v0.4.0 plus section 5.14: the archive replay index is written (first frame about 24 s instead of 34 s from the second launch on), and loading-screen images whose sides are not powers of two are rescaled with NEON and get their mips on the GPU. Frame rate is as in v0.4.0.

v0.4.0 is v0.3.0 plus the stall fixes of sections 5.10, 5.11 and 5.13: native DXT uploads, streamed music and voice with minimp3 overflow, GPU mip chains for uncompressed images, loose-file misses answered from folder listings, and cached held sounds found from their head and tail. Average frame rate is as in v0.3.0. The 0.3-0.65 s hitches at music starts and changes, voice lines and first-time textures are gone; first-time content loads still reach 0.12-0.23 s, occasionally more (a script spawning content mid-play took 368 ms).

v0.3.0 is release candidate 1 (summary item 16): the section 2.1 geometry-safe profile of v0.2.0 plus every default listed below. In the tested gameplay it averaged about 25 FPS, and 19–20 FPS in the busiest scenes. v0.2.0 delivered about 13 FPS in the tested second level.

Occlusion culling of Gobs (section 6.6) is on by default. It works through the engine's own cull path, and it fails safe when the queries do not work.

These are also on by default:
- the talk table in RAM (section 5.2);
- the resource-cache budget (section 5.3);
- the AI walk-pass and `CanSee` caches (section 5.4);
- the archive read cache (section 5.5);
- the GL state filter (section 5.6);
- the GL worker thread (section 5.7);
- the FMOD update skip (section 5.8);
- the 3 ms server AI budget (section 5.9);
- native DXT texture uploads (section 5.10, v0.4.0);
- streamed music and voice, with minimp3 for overflow streams (section 5.11, v0.4.0);
- mip chains of power-of-two uncompressed images made on the GPU, and loose-file misses answered from folder listings (section 5.13, v0.4.0);
- the bloom skip (section 9.1);
- creature shadows off (section 9.2).

Each has a `config.h` switch.

Keep all room filters disabled in releases. The 10 m distance filter can reduce matched-window draws/frame by about 28%, but it removes complete room-owned render state and remains too experimental for general use. Maintain it only as an opt-in research preset until it passes broad module, combat, cutscene, and transition testing.

The long-term target remains reducing engine-side per-room/per-Gob submission while preserving gameplay dependencies.
