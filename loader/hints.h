/* hints.h -- the game's own loading-screen hints, for the boot screen.
 *
 * These are the lines the real loading screen shows under the progress bar.
 * Getting at them means walking the resource system by hand, because the boot
 * screen draws long before the game has mounted anything:
 *
 *   chitin.key      names every resource and says which archive holds it
 *   data/2da.bzf    a BIF whose entries are LZMA blobs; holds loadscreenhints
 *   loadscreenhints a binary 2DA of StrRefs, two columns: gameplay and story
 *   dialog.tlk      maps a StrRef to the actual text
 *
 * All four live in main.obb as STORED zip entries, so obbzip reads them
 * directly. The decompressor is passed in rather than looked up here: it is the
 * game's own libLzmaLib, already loaded by the time the boot screen runs, so
 * nothing new is linked in, and keeping the lookup at the call site leaves this
 * file pure parsing that can be exercised off the Vita.
 *
 * Two details that cost time to work out, both encoded here: a .bzf entry's
 * size field is the UNCOMPRESSED size, with the packed bytes running to the
 * next entry's offset; and its LZMA stream is five bytes of properties with the
 * usual eight-byte length field omitted.
 *
 * Reading the language-specific dialog.tlk means the hints come out in
 * whatever language the installed game data uses, with no table to maintain.
 *
 * Every failure is silent and total: no hints, and the boot screen simply shows
 * its own lines instead. */

#ifndef __HINTS_H__
#define __HINTS_H__

#include <stddef.h>

/* libLzmaLib's entry point: a raw stream, explicit property bytes, and an
 * output size the caller already knows. */
typedef int (*LzmaUncompressFn)(unsigned char *dest, size_t *destLen,
                                const unsigned char *src, size_t *srcLen,
                                const unsigned char *props, size_t propsSize);

/* Read the hints out of main.obb, decompressing with `lzma`. Returns how many
 * were loaded, 0 on any failure. Call once, early; costs a few hundred
 * milliseconds of card I/O. */
int hints_load(LzmaUncompressFn lzma);

/* How many hints are available, and one of them. hints_get() returns NULL for
 * an out-of-range index. */
int hints_count(void);
const char *hints_get(int i);

/* Drop the text. Every pointer from hints_get() dangles afterwards. */
void hints_free(void);

#endif
