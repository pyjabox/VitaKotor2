/* bink_patch.h -- integrate KOTOR II's embedded Bink video path
 *
 * The Bink decode/draw entry points (MacPlayBinkGL, MacCreateBinkShaders) are
 * implemented inside libkotor2.so. Production enables both and routes the
 * player's OpenSL PCM queue through the existing Vita mixer. Diagnostic modes
 * retain isolated shader and one-movie playback gates.
 *
 * MacDecompress is NOT one of them despite the shared `Mac` prefix -- it is the
 * OBB's LZMA resource decompressor and must be left alone. See bink_patch.c.
 */

#ifndef __BINK_PATCH_H__
#define __BINK_PATCH_H__

#include <stdint.h>

#include "so_util.h"

// Apply the BINK_MODE policy to libkotor2.so's Bink exports.
void bink_patch(so_module *port_mod);

// Low-overhead counters driven by the existing GL and swap wrappers while the
// real player is active.
void bink_patch_note_texture_upload(unsigned width, unsigned height);
void bink_patch_on_swap(uint64_t swap_end_us);

// Refill KOTOR II's native Bink producer after an OpenSL buffer completes.
void bink_patch_on_audio_buffer_complete(void);

// Stop native producer feeding before Bink destroys its handle.
void bink_patch_stop_audio_pump(void);

#endif
