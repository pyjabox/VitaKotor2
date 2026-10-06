/* config.h -- compile-time constants for the KOTOR II Vita loader
 * Historical diagnostic comments are inherited from VitaKotor (KOTOR I)
 * unless explicitly marked as KOTOR II validation.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// Where the game's writable data / logs live
#define DATA_PATH        "ux0:data/kotor2"
#define LOG_PATH         DATA_PATH "/log.txt"

// The Android shared objects we so-load (copied out of the APK by the user)
#define SO_PATH          DATA_PATH "/libkotor2.so"
#define ANDROID_PORT_SO  DATA_PATH "/libObbVfs.so"
// Compression libs the game/companion NEED (mz_zip_reader_* / LzmaUncompress).
// Loaded before the companion so those imports resolve cross-module.
#define MINIZ_SO         DATA_PATH "/libminiz.so"
#define LZMA_SO          DATA_PATH "/libLzmaLib.so"
#define CXX_SO           DATA_PATH "/libc++_shared.so"

// OBB game-data archives (ZIPs read by the companion's ObbFile/miniz). The user
// copies the configured main/patch OBB archives here. The
// game normally mounts these via Java; we drive mountObb/mountPatchObb instead.
#define OBB_MAIN_PATH    DATA_PATH "/main.213.com.aspyr.swkotorii.obb"
#define OBB_PATCH_PATH   DATA_PATH "/patch.14.com.aspyr.swkotorii.obb"

// Base addresses the modules are mapped at (distinct so they don't overlap).
#define LOAD_ADDRESS              0x98000000  // libkotor2.so (main, ~9 MB)
#define ANDROID_PORT_LOAD_ADDRESS 0x90000000  // libObbVfs.so (~75 KB)
// In the proven-good high-user range (0x8Cxxxxxx collides with the 192MB heap);
// slot both between libandroid_port (0x90000000) and libKOTOR (0x98000000).
#define MINIZ_LOAD_ADDRESS        0x92000000  // libminiz.so (~63 KB)
#define LZMA_LOAD_ADDRESS         0x94000000  // libLzmaLib.so (~22 KB)
#define CXX_LOAD_ADDRESS          0x96000000  // Android libc++ shared runtime

// Font metrics (.txi) injection. Aspyr ships the override font TGAs without their
// companion .txi glyph metrics, so CAurFontInfo never populates -> fontInfo stays
// NULL -> every GUI text path null-derefs (WrapStrings, Draw, GetIdealPixelHeight).
// That is what makes the menu render with no text.
//
// We serve VPK-bundled .txi files, but the DELIVERY SHAPE matters. AurResGet has two
// exits that build the same 28-byte object with different layouts, and
// AurResGetNextLine dispatches on [+0]:
//
//   OBB hit  (+0x40e3dc): [+0]=0      [+12]=data [+16]=size [+20]=8192   *size=len
//   fallback (+0x40e4d0): [+0]=RWops* [+12]=0    [+16]=0    [+20]=500000 *size=1
//
//   [+0]==0 -> in-memory scanner, bounds-checked against [+16] (+0x40e8ca)
//   [+0]!=0 -> streaming scanner (+0x40e920), UNBOUNDED
//
// Merely enabling the loose-file fallback (flag=0) lands on the streaming scanner,
// which after the first line resumes at index bytes_read+1 inside a 500 KB buffer
// holding ~11 KB of file and walks uninitialised heap hunting CR/LF -> DATA_ABORT
// during engine init (log41 died at GL call #62 vs 4000+ in log36-40). So instead we
// let the game build AND REGISTER the fallback object, then convert it in place to
// the memory-backed shape. See AurResGet_hook in main.c.
#define FONT_TXI_MEMORY_INJECT 0

// Screen geometry
#define SCREEN_W         960
#define SCREEN_H         544

// Heap / memory budgets (MB). 192 MB fits within the extended-memory partition
// granted by ATTRIBUTE2=12 (see CMakeLists.txt). Requesting more (e.g. 256)
// without that grant makes the CRT heap init fail -> crash in _sbrk_r before main.
#define MEMORY_NEWLIB_MB          160
// RAM vitaGL must LEAVE UNCLAIMED. It takes everything else at vglInitExtended,
// which runs before the game does, so anything we mean to allocate later has to
// be inside this number or it will not be there. It was 16 MB when newlib held
// 192; newlib now holds 160 and bigalloc wants the difference, so this is
// 16 + BIGALLOC. Leave the two in step: shrinking newlib without raising this
// just hands the difference to vitaGL and the pool finds nothing.
//
// 48 -> 80 because vitaGL was not using it. Across log170-172 its RAM heap was
// 122 MB and its free figure never once fell below 73.8 MB -- it holds about
// 48 MB of a 122 MB claim, all session, in every session. Taking 32 MB of that
// leaves it ~42 MB of headroom against a working set it has never exceeded,
// and the pool commits segments lazily, so if the extra is never needed it
// simply stays unclaimed instead of sitting idle inside vitaGL.
#define MEMORY_VITAGL_THRESHOLD_MB 80

// Big-allocation pool (bigalloc.c). Everything at or above BIGALLOC_MIN_BYTES is
// served from segments of our own instead of newlib's arena, because mixing
// megabyte blocks with the game's thousands of small long-lived objects is what
// shreds the heap: log145 watched the largest servable block halve every ~150 s
// -- 32 MB down to 512 KB -- while 46 MB stayed free the whole time.
//
// The threshold is 256 KB, not 512 KB, because log142's fatal request was
// 286 KB. Traffic at that size is light (1458 in a 34-minute session), so the
// pool's best-fit walk costs nothing.
//
// This budget is TAKEN FROM newlib, not added: vitaGL claims everything except
// MEMORY_VITAGL_THRESHOLD_MB, so there is nothing spare.
//
// 32 -> 64 MB, and the [big] line is what says so. Peak live was 30, 31 and
// 31 MB in log170, log171 and log172 -- the pool ran pinned against a 32 MB
// ceiling for whole sessions, which means "peak 31" is the ceiling reporting
// itself rather than a measurement of demand. What it turned away went to
// newlib instead: 237, 622 and 648 fallbacks, of up to 4.3 MB each, mixed in
// among the game's thousands of small long-lived objects. That is precisely
// the mixing this pool exists to prevent, and log171 shows where it ends --
// operator new failing on 1 MB with 46.8 MB free and a largest servable block
// of 512 KB, bad_alloc, uncaught, dead.
//
// Segments are committed on demand, so this is a ceiling and not a reservation.
// The fallback line now carries total bytes and worst single request, which is
// what will say whether 64 is enough or still short.
#define BIGALLOC_MIN_BYTES (256u * 1024u)
#define BIGALLOC_SEG_MB    8
#define BIGALLOC_MAX_SEGS  8

// Multisampling. We shipped SCE_GXM_MULTISAMPLE_4X, which makes the GPU shade
// and resolve 4x the fragments at 960x544 -- on a Vita that is a luxury, and it
// costs most in exactly the geometry-heavy scenes that stutter (log118: slow
// windows averaged 30737 drawElements vs 7400 in fast ones).
//   SCE_GXM_MULTISAMPLE_NONE  fastest, aliased edges
//   SCE_GXM_MULTISAMPLE_2X    middle ground
//   SCE_GXM_MULTISAMPLE_4X    original, prettiest, slowest
#define GL_MSAA_MODE SCE_GXM_MULTISAMPLE_NONE

// ---- Archive mount speed & feedback ----------------------------------------
// Inherited from KOTOR I: mounting the main OBB reads a header per ZIP entry.
// scattered across 1.75 GB. Measured from the real archive: mean gap between
// consecutive headers 107 KB, median 38 KB. Read-ahead was tried and DISPROVED
// (log122): a 32 KB block buffer moved 373 MB to eliminate 30% of the reads and
// made the mount slower, 95s -> 103s. Sparse access defeats prefetching.
//
// What works instead is that the mount reads the same bytes every boot, so we
// record them once and replay them from a small file afterwards. See
// obb_index.h. First boot is unchanged; later boots should skip the scattered
// reads. Delete the generated main-OBB index under DATA_PATH to re-record it.
#define OBB_INDEX_CACHE      1
#define OBB_INDEX_BUDGET_KB  4096   // cap on recorded bytes (~735 KB expected)
#define OBB_INDEX_MAX_RANGE  4096   // ignore reads bigger than this: asset data,
                                    // not metadata, and it would bloat the cache

// Read archives with sceIoPread instead of fseek+fread. One syscall per read
// rather than two, and no newlib buffer to invalidate -- which matters because
// every archive read is preceded by a seek to the virtual handle's position.
#define OBB_USE_PREAD        1

// Progress bar for startup. The game draws NOTHING until its first draw call --
// log124 measured that at 69.3s, later even than the "Main Menu" analytics
// string at 56.1s -- so without this the console looks hung for over a minute.
// The bar spans loader start to that first draw, estimating from how long the
// previous boot took (saved under DATA_PATH). Warm and cold
// archive-cache timings are tracked separately; they differ by about a minute.
#define LOADSCREEN_ENABLE         0
#define LOADSCREEN_REDRAW_MS      100
#define LOADSCREEN_DEFAULT_WARM_S 70    // first-ever run with a built .idx cache
#define LOADSCREEN_DEFAULT_COLD_S 135   // first-ever run, building the cache

// Dress the boot screen in the game's own loading-screen art instead of a bare
// bar. The background comes from a load_*.tga inside the KOTOR II patch OBB,
// plain STORED zip entry and so readable before mount_obbs() runs. Set to 0 to
// go back to the untextured bar: the art path touches GL state (a texture, the
// fixed-function matrices) that the scissor-and-clear bar never did, so it is
// the first thing to rule out if a boot regresses.
#define LOADSCREEN_ART            1

// Bar geometry in the ART's own pixels, not loadscreen.gui's.
//
// The first attempt used PB_PROGRESS's extent from loadscreen.gui (LEFT 380,
// WIDTH 262 in its 1024x768 space) and sat visibly too wide. The groove the bar
// belongs in is painted into the load_*.tga itself, and measuring the two
// bright edge posts across all 97 of them puts it at x 437..586 -- 149 wide,
// not 262. Same centre, which is why it looked close but overhung by about 30
// pixels each side. Since we stretch the art over the whole framebuffer, the
// bar has to be anchored to the art, or the two cannot stay registered.
//
// Vertically the two sources agree, so this keeps loadscreen.gui's TOP 446 and
// HEIGHT 35 converted into art rows (x512/768).
#define ART_W                     1024
#define ART_H                     512
#define ART_BAR_X                 437
#define ART_BAR_W                 149
#define ART_BAR_Y                 297
#define ART_BAR_H                 23

// The rest of the screen, also in art pixels. loadscreen.gui's extents were the
// starting point but its horizontal figures do not survive the stretch (see the
// bar above), so these were set by composing the real assets against a capture
// of the game's own screen until they matched.
//
// The logo is width-anchored and sits on ART_LOGO_BOTTOM, which is just clear of
// the picture inset the art starts at row 164. Its file carries wide
// transparent margins, so the drawn quad uses the opaque bounding box.
#define ART_LOGO_W                300
#define ART_LOGO_BOTTOM           156
#define ART_LOAD_CX               517   // "LOADING", centred (LBL_LOADING)
#define ART_LOAD_Y                326
#define ART_HINT_CX               513   // the rotating line (LBL_HINT)
#define ART_HINT_Y                352
#define ART_HINT_W                590   // narrower than LBL_HINT's 748: matches
                                        // where the game's own screen wraps

// Seconds each line stays up. The screen freezes when the game takes over, at
// about 18s of a warm boot, so this is what decides how many are ever seen.
#define LOADSCREEN_HINT_SECONDS   6

// Assets read at boot. The .txi ships in the VPK for the game's own use; the
// atlas and the logo come out of the KOTOR II patch OBB.
#define FONT_TXI_PATH             "app0:fonts/dialogfont16x16b.txi"
#define FONT_TGA_ENTRY            "override/dialogfont16x16b.tga"
#define LOGO_TGA_ENTRY            "override/and_main_logo.tga"

// How many times to dump the game's GL state after it takes over. The art can
// only draw while the loader owns GL outright; once the game starts issuing GL
// the loadscreen stops for good (see loadscreen.h). These probes record what
// was bound at that point, so a future attempt to keep drawing for the whole
// boot starts from measurements rather than guesses.
#define LOADSCREEN_PROBE_MAX      24
#define LOADSCREEN_PROBE_MS       2000

// Per-call GL trace budget. Every traced call is one sceIoWrite to the memory
// card, and log125/126 measured what that costs: gaps after a log line sit at a
// flat ~8-11ms floor regardless of WHICH line it was, and the two slowest gaps
// mod 64 are residues 0 and 1 in both runs -- exactly log.c's close/reopen
// boundary. So the logger, not the engine, sets the pace. Before the first draw
// (70.3s) both runs wrote ~4-5.4k lines, ~60% of startup, of which the per-call
// GL trace alone was 1958 lines / 21.0s (log125) and 3148 / 24.7s (log126).
// The budget is never exhausted before the menu, so a shipping build pays all of
// it. 0 disables the trace (GLLOG still ticks the loadscreen and emits one
// "silenced" line); raise it to 4000 to get the bring-up trace back.
#define GL_TRACE_LIMIT 0

// Log write buffering. Every log line used to be its own sceIoWrite to the
// memory card, measured at 8-11ms, which put the logger on the critical path of
// both startup and frames (logs 127/128: ~3370 gameplay lines per session, ~34s
// of ~175s of frame time, 18-19%). Batch lines into one buffer instead and write
// when it fills or when LOG_FLUSH_MS has elapsed, so a burst costs one card
// write rather than hundreds while a quiet period still reaches disk promptly.
//
// The time-based flush is what bounds the risk: on a hard hang (not a CPU fault,
// which has its own path) at most LOG_FLUSH_MS of log is unwritten. This project
// has chased several stalls, so that ceiling matters more than the last few
// syscalls. Set LOG_BUFFER_KB to 0 for the old unbuffered line-at-a-time
// behaviour.
// Diagnostic probe logging. The probes are how every bug in this port has been
// found, and they are also what fills the card: in log162, twelve periodic tags
// accounted for about 85% of 17,599 lines across 28 minutes, and that write
// traffic lands on exactly the loading and combat bursts that stutter.
//
// SHIPPED BUILDS KEEP THIS ON. The log is how players report faults: they post
// LOG_PATH and that is what makes a bug fixable by someone who
// was not holding the console. A quiet release trades away the entire support
// path to buy back some card I/O, which is a bad trade and not one to make
// again without asking.
//
// Set to 0 only to measure what the logging itself costs, or for a build made
// for one person who is chasing framerate. When it is 0, lines are dropped
// before they are even formatted -- the tag is a literal prefix of the format
// string, so the test is a few byte compares and no vsnprintf, no lock, no
// card I/O -- and anything whose format mentions a failure, an error or a
// warning is kept regardless, as is everything written in panic mode.
#define LOG_DIAGNOSTICS 0

// Maximum-FPS benchmark logging: discard every routine line before formatting.
// Failure/error/warning lines and panic-mode crash dumps remain available.
#define LOG_BENCHMARK_MINIMAL 1

#define LOG_BUFFER_KB  8
#define LOG_FLUSH_MS   1000

// Heap tracing. gl_patch.c arms it at the last GL cap query (entering engine
// setup) to catch an UNBOUNDED alloc loop during bring-up, and never disarms it,
// so it runs for the whole session. MEM_TRACE is already throttled to every
// 1024th allocation -- but KOTOR allocates ~2.3M times in a 700s session, so the
// heartbeat alone emitted 2249 lines in log119, the single largest log source in
// gameplay (26% of all lines, ~54s of frame time). Off for release; set to 1 to
// hunt an allocation loop again.
#define MEM_TRACE_ENABLE 0

// glGet* value trace. The diagnostic value is all in init (~18 cap queries);
// steady-state values are noise, and log119 still spent 231 lines on them. The
// UNWRITTEN case stays unconditional either way -- it always signals a real
// vitaGL gap. Raise to 256 to restore the bring-up trace.
#define GLGET_TRACE_LIMIT 0

// Event-driven frame hitch trace. Timing is sampled once at swap with no
// per-draw clock calls; only frames slower than this threshold emit a line.
// Rate limiting prevents a sustained slow scene from turning logging into the
// cause of the slowdown. Set the threshold to 0 to disable individual lines;
// the existing 120-frame summary still reports average and worst frame time.
#define FRAME_HITCH_TRACE_MS   80
#define FRAME_HITCH_LOG_GAP_MS 500

// Bink integration modes. Production uses the embedded player and routes decoded
// PCM through the existing Vita mixer. The other modes retain isolated regression
// gates for shader, silent-video, and single audio/video playback.
#define BINK_MODE_SKIP        0
#define BINK_MODE_SHADER_TEST 1
#define BINK_MODE_LEGAL_VIDEO 2
#define BINK_MODE_OPENSL_TEST 3
#define BINK_MODE_PLAY        4
#ifndef BINK_MODE
#define BINK_MODE BINK_MODE_PLAY
#endif
#if BINK_MODE != BINK_MODE_SKIP && BINK_MODE != BINK_MODE_SHADER_TEST && \
    BINK_MODE != BINK_MODE_LEGAL_VIDEO && BINK_MODE != BINK_MODE_OPENSL_TEST && \
    BINK_MODE != BINK_MODE_PLAY
#error Unsupported BINK_MODE
#endif

// KOTOR's Android loop turns an AI update over 33/67/100/133 ms into
// 2/4/7/11 complete GameUpdate calls before the next presentation. On Vita the
// extra no-present updates amplify one slow frame into a stutter cascade. Clear
// the selected count before the primary update so every update can present.
// Hardware A/B: first-level median window time fell from ~61 ms to ~53 ms.
#define DISABLE_ADAPTIVE_RENDER_SKIP 1

// Skip glBindTexture calls that rebind what is already bound. log120 measured
// 1.008 texture binds per draw call, so nearly all of them are redundant.
// Set to 0 if textures ever look wrong, to rule this out.
#define GL_FILTER_REDUNDANT_BINDS 1

// Skip glUseProgram calls that re-select the program already current. vitaGL's
// glUseProgram does no GXM work but marks every uniform dirty, so a redundant
// one costs a full uniform re-upload (u_boneMatrices[51] included) on the next
// Inherited KOTOR I performance note; not revalidated for KOTOR II.
// MEASURED 0% IN log151: the game alternates programs genuinely and essentially
// never re-selects the current one, so this saves nothing in practice. Kept
// because it is one comparison and correct, not because it earned its place --
// the real cost is that ~2700 GENUINE switches per window each force a full
// uniform re-upload, and that cannot be filtered away from here.
// Set to 0 if lighting or skinning ever looks stale, to rule this out.
#define GL_FILTER_REDUNDANT_PROGS 1

// Pad NPOT texture widths to a multiple of 8 on upload. Added to prove the
// background-shear theory, and it did. The pad leaves the texture wider than
// the game believes, so a quad sampling u across [0,1] also covers the pad;
// it now RESAMPLES the row onto the padded width rather than extending the
// last column, so [0,1] still spans the whole picture at any size.
//
// That resample was aimed at the oversized minimap and the fog panel in the
// character screens, and it did NOT fix them -- log156 shows 81 resampled
// uploads (2x2, 4x4, 90x70, 756x106, 860x478) with both boxes unchanged on
// screen. Keep the resample anyway: it is strictly more correct than the smear
// it replaced, and the tiny gradients it was mangling were real. But the boxes
// are NOT the upload pad, so do not come back here for them. The real fix for
// the shear this exists to mask is strided texture init in vitaGL.
#define GL_NPOT_WIDTH_PAD 1

// Skinning bisect: force the ubershader's `#define USE_SKIN 1` to 0 in
// glShaderSource, bypassing kotor.vert's dynamic uniform-array read
// (u_boneMatrices[indices.x]) while leaving everything else identical.
// Diagnostic for the exploding-character bug -- characters render in BIND POSE
// (static but correctly shaped) while enabled. See gl_patch.c for the full
// rationale. Set to 0 to restore real skinning.
#define SKIN_BISECT_DISABLE 0

// Skinning bone-index rounding: rewrite kotor.vert's
//   ivec4 indices = ivec4(clamp(3.0 * a_matrixIndices, 0.0, 50.0));
// to round-to-nearest instead of truncating. DISPROVED (log79): the rewrite was
// confirmed applied to every skinned vertex shader ("SKIN FIX sh=N: 1 site") and
// the characters exploded exactly as before, so fp16 demotion of a_matrixIndices
// was never the problem. Left in place, off, because it costs nothing to re-arm.
// The real cause was vitaGL truncating >64 KB VBO attribute offsets into GXM's
// 16-bit SceGxmVertexAttribute::offset -- fixed in vitaGL, see
#define SKIN_INDEX_ROUND_FIX 0

#endif

// Scale GUI images that never went through ScaleExtentForResolution.
//
// log157 asked, per widget, whether it had been scaled, and every image at or
// above 200x200 came back NO -- while 138 other widgets in the same screen had
// been scaled normally. Two of the three are the pillarbox wings, which arrive
// at exactly the screen height (217x544 at x=-100 and x=843) and are already in
// device pixels; shrinking those would be wrong, so anything at or above the
// screen height is left alone. The third is a 238x238 image that is neither a
// texture size nor a device-space number, and 238 x 0.7083 is 169: an element
// left at authored size inside a frame the game scaled by 544/768 is 1.41x too
// big for it, which is exactly how far the minimap overflows its frame.
//
// TESTED AND DISPROVED (log158). The rescale fired exactly once, on precisely
// the widget it was aimed at -- "autoscaled self=0x85b42738 238x238 by x0.7083"
// -- and the minimap was unchanged on screen. So either that widget is not the
// minimap, or its size was never the problem. Either way the extent path is now
// finished as an explanation for these boxes: scaling is applied correctly to
// the 138 widgets that get it, and forcing it onto the ones that skip it fixes
// nothing.
//
// Left at 0. It is a speculative mutation of widget geometry with no evidence
// behind it any more, and shipping one of those is worse than the bug.
//
// Next suspect is blending, not geometry: the haze bands line up with the UI
// slots and read like additive overlays drawn opaque, and KOTOR stores
// per-texture blend modes in .txi files -- of which log157 shows a great many
// missing.
#define GUI_AUTOSCALE_UNSCALED_IMAGES 0

// Spatial audio. All four FMOD 3D entry points -- set3DAttributes,
// set3DMinMaxDistance, set3DListenerAttributes, set3DOcclusion -- were
// fmod_stub, so no positional sound ever attenuated with distance or panned:
// rushing water across the level played at the same level as water underfoot,
// and footsteps and dialogue sat under the ambience. log155 marks 418 of 617
// logged sounds FMOD_3D, so this is most of the mix, not an edge case.
//
// Implements FMOD Ex's default inverse rolloff (gain = mindistance/distance,
// flat inside mindistance, floored -- not silenced -- past maxdistance) plus a
// pan projected onto the listener's right vector. 2D sounds are untouched by
// design: music, UI and the ambient bed carry no position and play flat.
// Set to 0 to go back to every source at full volume, dead centre.
#define AUDIO_3D_ATTENUATION 1

// Handedness of the 3D listener basis. FMOD Ex is left-handed by default and
// only switches when the game passes FMOD_INIT_3D_RIGHTHANDED to System::init.
// The two conventions produce exactly opposite right vectors, i.e. a mirrored
// stereo image -- inaudible as a defect, wrong every time. Left-handed takes
// up x forward, right-handed forward x up. The first listener update prints the
// resulting basis so it can be checked against known geometry on hardware.
// VERIFIED, do not flip on a hunch: FModAudioSystem::InitSystem passes flags=0
// to FMOD::System::init (an immediate `movs r2, #0` before the call), so
// FMOD_INIT_3D_RIGHTHANDED is not set and the game is left-handed. If the
// stereo image ever sounds mirrored, the cause is elsewhere.
#define AUDIO_3D_RIGHTHANDED 0

// Stream long audio assets instead of replacing them with timed silence.
//
// Music and long ambience decode to 14-24 MB of PCM, against a heap shared with
// the game -- a 15 MB decode is what crashed a session mid-area once already.
// Above STREAM_PCM_MAX an asset is now streamed: its compressed bytes stay
// resident and a small ring of decoded PCM is refilled as it plays, so a track
// costs about 1.5 MB and starts with no decode stall.
//
// Set to 0 to restore the previous behaviour exactly, where every such asset
// became a correctly-timed silent placeholder and the score was never audible.
#define AUDIO_STREAM_LONG_ASSETS 1

// Diagnostic-only null backend. KOTOR II crashes when Disable Sound=1 leaves its
// audio object NULL, so this instead keeps valid FMOD handles while skipping all
// output, file reads, decoding, PCM allocation, and mixing. Production stays 0.
#define AUDIO_BENCHMARK_SILENT 0

// Experimental hook-side visibility cull (performance investigation, experiment
// 6). Gob::VisibilityCheck's size test is (2*radius/distance) > T with T read
// through the GOT slot at libkotor2.so+0x866d7c; on hardware EVERY checked gob
// passes (100% acceptance) and far gobs are confirmed by an engine LOS raycast,
// so the engine never size-culls the red-corridor vista. When a gob passes the
// engine's checks but its angular-size proxy falls below VIS_CULL_X_ENGINE_T * T,
// the hook returns not-visible instead and DoGobBuckets skips the whole
// submission (verified: caller branches away on rc==0 at libkotor2.so+0x537d70).
// The always-visible flag (byte at [gob+0x84]+0x50 bit 6) and the vtable+0x1dc
// predicate are replicated first, so flagged gobs are never culled.
// VIS_CULL_ENABLE 0 disables the hook entirely. Superseded by the engine-side
// test below for the current A/B; kept for a later comparison.
#define VIS_CULL_ENABLE 0
#define VIS_CULL_X_ENGINE_T 2.0f

// Character-model suppression A/B. This bypasses Gob::Render only for model
// names matching the character families recognized in main.c. It is a visual
// diagnostic to distinguish character submissions from room geometry, not a
// production optimization.
#define PERF_CULL_ALL_CHARACTERS 0

// Passive room/VIS telemetry. Reports the current room, its verified VIS links,
// and each linked room's Gob-list size without changing scene state.
#define PERF_ROOM_VIS_TELEMETRY 0

// Diagnostic VIS-edge A/B. Disabled for the active-list interception test
// below so the canonical VIS graph remains unchanged.
#define PERF_VIS_UNLINK_FROM_ROOM ""
#define PERF_VIS_UNLINK_ROOM      ""

// Emulator-only proof that filtering CollectActiveRooms' returned list is
// sufficient to suppress the expensive room workload. This reproduces the
// proven 001ebo15 -> 001ebo5 A/B without mutating Scene visibility state.
#define PERF_ACTIVE_ROOM_FILTER_FROM ""
#define PERF_ACTIVE_ROOM_FILTER_ROOM ""

// Dynamic closed-door visibility refinement. Whole-room removal is unsafe:
// several modules store visible framing, lighting, or shared render state in
// rooms beyond the current walkmesh room. Keep that aggressive mode disabled.
#define PERF_DYNAMIC_PORTAL_FILTER 0

// Deliberately aggressive distance-only benchmark. After the engine builds its
// active-room list, retain the current room unconditionally and retain another
// room only when the shortest distance from the camera/player position to that
// room's world-space AABB is at most this radius. Door state, VIS adjacency,
// force-retain flags, and portal topology are intentionally ignored. Invalid
// camera/bounds data fails open. This is expected to cause visible pop-in,
// missing vistas, and other graphical defects.
#define PERF_DISTANCE_ROOM_FILTER 0
#define PERF_DISTANCE_ROOM_RADIUS_M 10.0f

// Generic safer mode: retain every active room and all static room geometry,
// but reject Gobs owned exclusively by stable door-occluded candidate rooms.
// Gobs shared with any retained active room remain visible. Engine force-visible
// objects also remain protected. Door opening, uncertainty, or a room change
// clears the transient Gob set immediately.
#define PERF_DYNAMIC_PORTAL_GOB_FILTER 0
#define PERF_DYNAMIC_PORTAL_DELAY_US 1000000ULL

// Engine-side visibility test (performance investigation, experiment 6b).
// Hardware + relocations showed the float behind GOT slot libkotor2.so+0x866d7c
// is the named engine global `enablevisibilitytest` (0x904974, with
// `countvisibilityculls` at 0x904978), and this Android build leaves it at 0:
// Gob::VisibilityCheck's size test (2*radius/dist > T) passes unconditionally,
// so the engine's grace+line-of-sight cull path NEVER runs and every gob in a
// VIS-linked room is submitted, including everything behind walls. Writing a
// positive T re-enables the engine's own mechanism with its own fade/LOS
// semantics -- no reimplementation, no hook-side guessing. T is an angular-
// size cutoff: a gob is culled (after grace + LOS) when 2r/d < T, so T=0.02
// drops small props (r~0.25) beyond ~25 m and leaves walls and characters
// alone. The value is re-armed if anything resets it, and the engine's own
// cull counter is reported per census window.
#define VIS_ENGINE_ENABLE_TEST 1
#define VIS_ENGINE_TEST_T 0.02f

// Occlusion culling (loader/occlusion_cull.c). The engine renders every Gob
// (creature, placeable, door) that passes its frustum and size/line-of-sight
// tests, and in KOTOR II's corridors most of them are behind walls. Each
// Gob::Render is bracketed by a GL_ANY_SAMPLES_PASSED query, read a frame later
// without waiting on the GPU; a Gob whose last HIDE_AFTER results showed no
// visible sample is skipped through Gob::VisibilityCheck (the engine's own cull
// path) and re-rendered once every RETEST frames to re-test. Gobs that draw
// nothing inside Gob::Render, or that the engine flags IsAlwaysRendering, are
// never skipped. Hardware, 2026-10-01: at the heaviest spot of the test route
// the Gob pass halved (~30 -> 15 ms) and the frame went 92.8 -> 74.9 ms; no
// visible artefacts. A Gob stepping out from cover can appear up to RETEST
// frames late. A fail-safe turns culling off for the session when the queries
// look non-functional (Vita3K does not emulate them). Needs both Gob slots
// unhooked: builds whose telemetry or portal Gob filter owns them run without.
//   OCCLUSION_CULL_TEST  1 replaces query results with a moving pattern (a
//                        quarter of all Gobs "hidden") so Vita3K exercises the
//                        skip and re-test paths; objects visibly strobe. Crash
//                        tests only.
#ifndef OCCLUSION_CULL_ENABLE
#define OCCLUSION_CULL_ENABLE 1
#endif
#define OCCLUSION_CULL_QUERIES_PER_FRAME 60
#define OCCLUSION_CULL_HIDE_AFTER 2
#define OCCLUSION_CULL_RETEST 3
// [cull] statistics line period; 0 = never.
#define OCCLUSION_CULL_LOG_INTERVAL_US 10000000ULL
#ifndef OCCLUSION_CULL_TEST
#define OCCLUSION_CULL_TEST 0
#endif

// Visual-effects reduction A/B. These use the engine's exported feature
// globals, avoiding the unsafe render-function substitution experiment.
// Environment mapping and EMLM are the measured high-cost effects; emitters
// are the next material scene-stage cost. Shadows, grass and lens flares are
// also disabled for a clearly defined "effects low" comparison build.
#define EFFECTS_LOW_DISABLE_ENVIRONMENT_MAPPING 0
#define EFFECTS_LOW_DISABLE_EMLM 0
#define EFFECTS_LOW_DISABLE_EMITTERS 0
#define EFFECTS_LOW_DISABLE_SHADOWS 0
#define EFFECTS_LOW_DISABLE_ADVANCED_LIGHTING 0
#define EFFECTS_LOW_DISABLE_GRASS 0
#define EFFECTS_LOW_DISABLE_LENS_FLARES 0

// Extreme visual-reduction benchmark. Keep framebuffer objects allocated—the
// Android renderer crashes with a null pbuffer if doframebuffer is cleared—but
// ask the engine to skip dynamic/basic lighting, lightmap path selection, and
// optional post-processing work. Do not clear enablerenderlightmapped: like the
// environment render gates, that can suppress geometry instead of selecting a
// fallback. This build is diagnostic and may render surfaces flat or incorrectly.
#define EFFECTS_LOW_DISABLE_ALL_LIGHTING 0
#define EFFECTS_LOW_DISABLE_POSTPROCESSING 0

// Maximum-FPS benchmark: omit high-frequency scene/callee attribution, the
// pool lookup hook, draw-call clock sampling, signatures, periodic summaries,
// and watchdog telemetry. Crash handling and essential failure logs remain.
// Keep this disabled for save/load validation and production builds: the scene,
// render, and watchdog probes generate substantial work and have correlated with
// emulator host faults after otherwise successful module loads.
#define PERFORMANCE_TELEMETRY_ENABLE 0

// Lightweight maximum-FPS draws-per-frame benchmark. This adds one integer
// increment per GL draw and one frame increment per presentation. Every 10
// seconds it emits one buffered DRAW_FRAME line containing aggregate draws /
// presented frames. No FPS, frame-time, per-draw timing, classification, or
// signatures are enabled.
#define DRAW_FRAME_BENCHMARK_ENABLE 1
#define DRAW_FRAME_BENCHMARK_INTERVAL_US 10000000ULL

// Narrow post-module GUI resource trace. Uses only libkotor2 PLT-slot
// replacements: no function-entry trampolines and no behavior changes.
#define GUI_RESOURCE_TRACE_ENABLE 0

// Diagnostic-only save-list trace. Replaces two existing PLT slots rather than
// patching function entries: the main-menu directory enumeration and each
// CSWGuiSaveLoadEntry::LoadData call. It preserves behavior and records why a
// discovered slot is accepted or rejected. Disable after this investigation.
#define SAVE_LIST_TRACE_ENABLE 0

// HandleSaveButton temporarily registers OPTIONS: before opening OPTIONS:OPT.
// On the Android/Vita VFS boundary that pseudo-directory enumerates broad OBB
// contents and shadows valid GUI keys. Keep the file lookup, but do not create
// a resource key table for this one pseudo-directory.
#define OPTIONS_RESOURCE_DIRECTORY_FIX 1

// Upload textures as 16-bit instead of 32-bit.
//
// The live-texture census (log161) is emphatic that nothing leaks: 1,014,190 KB
// uploaded against 920,002 KB released over 28 minutes, balancing to the byte.
// The problem is that the working set does not FIT -- it peaks at 102,639 KB
// against the Vita's fixed 96 MB of CDRAM -- so vitaGL evicts and re-uploads
// continuously, runs at a few hundred KB free, and the picture starts tearing.
// VRAM cannot be raised to meet it; MEMORY_VITAGL_THRESHOLD_MB governs system
// RAM, not CDRAM. The only lever is making the textures smaller.
//
// RGBA8 -> RGBA4444 and RGB8 -> RGB565 halves the footprint to roughly 51 MB,
// which fits with room to spare. Ordered 4x4 Bayer dithering keeps the banding
// down; without it, gradients and skies posterise badly at 4 bits a channel.
//
// Render targets and cube maps are left alone: an FBO's attachment format is
// not ours to change, and the environment map is 64x64x6 and not worth the
// risk. Textures we convert are remembered per id so glTexSubImage2D can
// match, since a byte-typed sub-upload into a 4444 texture would corrupt it.
// Set to 0 to go back to full-precision uploads.
#define GL_TEX16_CONVERT 1

// ---- Hardware-validated optimisations (2026-10) ---------------------------
// The AI master's walk pass skips ids already seen as non-creatures this frame
// (loader/ai_list_cache.c): ~1 ms per frame.
#ifndef AI_LIST_CACHE_ENABLE
#define AI_LIST_CACHE_ENABLE      1
#endif
// CSWCCreature::CanSee results reused for a few frames per (viewer, target)
// (loader/cansee_cache.c): ~0.5 ms per frame.
#ifndef CANSEE_CACHE_ENABLE
#define CANSEE_CACHE_ENABLE       1
#endif
// Bloom: the mobile bloom chain (FrameBufferModificationsEndIos) costs ~13-15
// ms of GPU per frame; with its blur and add draws skipped (loader/bloom_ctl.c)
// no visible difference was noticed on hardware.
#ifndef BLOOM_DISABLE
#define BLOOM_DISABLE             1
#endif
// Engine resource-cache budget, via the sysinfo the loader reports to
// libkotor2's GlobalMemoryStatus (previously a ret0 stub: garbage). 0 reports
// zero memory, i.e. the engine's 16 MB fallback.
#ifndef RESMAN_BUDGET_MB
#define RESMAN_BUDGET_MB          64
#endif
// The talk table (dialog.tlk, ~10 MB) read once into RAM and served from there
// (loader/ramfile.c). Each string lookup otherwise flushes, seeks and tells on
// the card file, ~38 ms per lookup on hardware: the combat-message freezes.
#ifndef RAMFILE_TLK
#define RAMFILE_TLK               1
#endif
#define RAMFILE_MAX_BYTES         (16u * 1024u * 1024u)
// Repeated .obb reads kept in RAM (loader/obb_cache.c): the game re-reads each
// compiled script from the archive on every run, ~1 card read of 70-100 KB per
// frame (10-39 ms) in steady gameplay. A read is cached on its second sight;
// items up to OBB_CACHE_MAX_ITEM_KB, OBB_CACHE_MB in all (0 disables).
#ifndef OBB_CACHE_MB
#define OBB_CACHE_MB              16
#endif
#define OBB_CACHE_MAX_ITEM_KB     1024
// GL state calls that change nothing are skipped before ASLgl
// (loader/gl_state_filter.c): glEnable/glDisable and the active / client-
// active texture unit, plus the dead glGetIntegerv in ASLgl::glTexParameteri.
// 0 off, 1 on, 2 verify (never skips; counts disagreements).
#ifndef GL_STATE_FILTER
#define GL_STATE_FILTER           1
#endif
// FMOD System::update scans the channels only when an END may be owed
// (loader/audio_patch.c, g_end_dirty); 0 scans on every call.
#ifndef AUDIO_UPDATE_SKIP
#define AUDIO_UPDATE_SKIP         1
#endif
// Server AI master budget per frame in microseconds (loader/ai_budget.c); the
// engine's is 10000. 0 leaves the engine alone.
// No shadows: the loader sets the game's own [Graphics Options] Shadows=0 and
// Soft Shadows=0 in swkotor2.ini at every launch (loader/ini_defaults.c). The
// game then switches them off itself. 0 leaves the ini to the game.
#ifndef INI_NO_SHADOWS
#define INI_NO_SHADOWS            1
#endif
#ifndef AI_BUDGET_US
#define AI_BUDGET_US              3000
#endif
// vitaGL on a dedicated thread (loader/gl_worker.c): 0 off, 1 inline (record
// and replay on the game thread -- validates the command stream), 2 worker
// thread on core 2. The game may run GL_WORKER_FRAMES_AHEAD frames ahead.
#ifndef GL_WORKER_MODE
#define GL_WORKER_MODE            2
#endif
#define GL_WORKER_FRAMES_AHEAD    1
#define GL_WORKER_PRIORITY        158
#define GL_WORKER_STACK           (2 * 1024 * 1024)
#define GL_WORKER_CPU_MASK        SCE_KERNEL_CPU_MASK_USER_2
// Polls of an empty queue before the worker sleeps (~0.2-0.3 ms at 444 MHz).
#define GL_WORKER_SPIN            40000
// Commands published to the worker per barrier; GL_WORKER_BATCH_AB alternates
// it with 1 at each 10 s window (A/B).
#define GL_WORKER_BATCH           32
#ifndef GL_WORKER_BATCH_AB
#define GL_WORKER_BATCH_AB        0
#endif
// Skip GL state calls that repeat the recorder's state copy (binds, enables,
// attribute pointers, program, scissor/viewport, plain state setters).
// GL_WORKER_DEDUP_AB alternates it on/off at each 10 s window (A/B).
#define GL_WORKER_DEDUP           1
#ifndef GL_WORKER_DEDUP_AB
#define GL_WORKER_DEDUP_AB        0
#endif
// Diagnostic: sample where the game thread's frame goes (loader/glw_prof.c).
#ifndef GL_WORKER_PROFILE
#define GL_WORKER_PROFILE         0
#endif
