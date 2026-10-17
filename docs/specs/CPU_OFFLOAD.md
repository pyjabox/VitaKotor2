# Text widths cached, animated textures on core 1

`loader/cpu_offload.c` (`CPU_OFFLOAD`, `offload_mode.txt`): two cuts to the
game thread, chosen from the hardware PC profile of 2026-10-17. That profile
was taken on Telos, with ASLgl on the GL worker, at 54 ms frames with the
game thread 91–95% busy.

## What the profile showed

| Path | Game thread a frame |
|---|---:|
| `IosLabel::Draw` → `DrawTextWrapped` → `FTFont::Advance` | 8.7 ms |
| `AurTextureForceControllerUpdate` → `CAurTexture::Bind` → `Water` / `RingDistort` / `RandomTextureController::Control` | ~6 ms |

## Text widths

- **What the engine does:** `DrawTextWrapped` measures each candidate line,
  word by word, through the font's vtable (`+0x4c`, `FTFont::Advance`), every
  frame.
- **Why caching is safe:** each size is its own `FTTextureFont`, made once by
  `SetupTruetypeFont` and never freed.
- **The key:** a 64-bit hash of the font, `len`, the 24-byte spacing, and the
  bytes of `len + 1` characters. FTGL reads one character past `len`, for
  kerning.
- **How it hooks:** three slots — two vtable ABS32 entries and the PLT. The
  ABI is softfp: the `FTPoint` goes on the stack and the float comes back
  in r0.

## Animated textures

- **What `Control()` does:** it works on the controller's own buffers and ends
  with the texture's `glImage` (vtable `+0xb4`). That is its only GL; the
  rest calls the texture's size getters (`+0x4c` width, `+0x50` height,
  `+0x58` bytes per pixel), `Distort`, `Height2Channel`, `new[]`/`delete[]`
  and `rand`.
- **What runs where:** a core-1 pthread runs the engine's `Control()`, and its
  `glImage` is caught there as a copy. The game thread uploads that copy at
  the controller's first bind of the next frame, then queues it again. It
  queues each controller at most once a frame; every `Control()` returns at
  once when called again in the same frame, the frame being kept at
  `+8`.
- **Destruction:** the destructors (`D2`, `D0`) wait for the worker and drop
  the controller's slot.

## Results

### Vita3K, VERIFY

- On the Telos quicksave, every cached width matched FTGL.
- Captures look the same with the offload on and off.
- No texture update was missed.

### Hardware, Telos, 2026-10-17

The build had ASLgl on the GL worker and the AI thread on, with the offload
switching on and off every 10 s. Log:
`diagnostics/aithread-20261016/log-telos-offload-hw-1.txt`.

| | Offload on | Offload off |
|---|---:|---:|
| FPS | **21.5** | 17.5 |
| Frame time | **46.5 ms** | 57.2 ms |
| `DrawTextWrapped` | 1.36 ms | 3.85 ms (peaks 9.7–11.5) |
| Controllers on the game thread | 0.10 ms | 3.02 ms (5.6–13 ms with water busy) |

- About 90% of widths came from the cache; none were uncacheable.
- Across 18 adjacent pairs, the median ratio of frame times (off over on)
  is 1.22.
- In the heaviest water windows the worker missed up to 2.4 updates a frame;
  in steady play it missed none.
