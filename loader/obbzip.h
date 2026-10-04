/* obbzip.h -- read one file out of an .obb, without the game's help.
 *
 * The OBBs are plain zip archives, and the game mounts them through miniz
 * inside libandroid_port. That mount is the slowest part of startup, so
 * anything the boot screen wants to draw has to come from somewhere else:
 * loadscreen_begin() runs BEFORE mount_obbs(), and the art it needs lives
 * inside patch.obb.
 *
 * Every one of patch.obb's 565 entries is STORED -- compression method 0,
 * verified across the whole archive -- so "extracting" a file is a seek and a
 * read. That makes a private reader small enough to be worth having, and it
 * costs the mount nothing because it closes before the mount begins.
 *
 * Deliberately not a general zip library: STORED only, no zip64, no CRC check
 * (a corrupt archive fails check_obb() long before it reaches us). Anything it
 * cannot handle returns NULL and the caller falls back to drawing nothing --
 * the boot screen is cosmetic and must never be able to fail a boot. */

#ifndef __OBBZIP_H__
#define __OBBZIP_H__

typedef struct ObbZip ObbZip;

/* Open an archive and read its central directory into memory. NULL if the file
 * is missing, unreadable, or not a zip we understand. */
ObbZip *obbzip_open(const char *path);

void obbzip_close(ObbZip *z);

/* Read a whole entry by exact name. Returns a malloc'd buffer the caller owns
 * and writes its length to *size_out; NULL if absent or not STORED. */
void *obbzip_read(ObbZip *z, const char *name, unsigned *size_out);

/* Locate an entry's bytes without reading them: the absolute offset of its data
 * within the archive file, and its length. Lets a caller pull a handful of
 * scattered records out of a large member -- dialog.tlk is 5.4 MB and we want
 * about 128 short strings from it -- instead of loading the whole thing. 1 on
 * success, 0 if absent or not STORED. */
int obbzip_locate(ObbZip *z, const char *name, unsigned long long *off, unsigned *size);

/* Read `len` bytes from an absolute offset in the archive file. Pair with
 * obbzip_locate(). 1 on success. */
int obbzip_pread(ObbZip *z, unsigned long long off, void *dst, unsigned len);

/* Count entries whose name starts with `prefix` and ends with `suffix`. With
 * `pick` in [0, count), the pick'th such name is copied into `out` instead.
 * Pass pick < 0 to only count. Lets a caller choose at random from a family of
 * files without a hardcoded name list. */
int obbzip_match(ObbZip *z, const char *prefix, const char *suffix,
                 int pick, char *out, unsigned out_size);

#endif
