/* k2res.h -- read-only access to KOTOR II's data before the engine runs
 * (k2res.c): the OBB file table, ERF and BIF members, TPC textures, 2DA
 * columns and TLK strings. Used by the boot loading screen. Every function
 * fails soft (0 or NULL) on anything it does not recognise. */

#ifndef __K2RES_H__
#define __K2RES_H__

#include <stdint.h>
#include <psp2/types.h>

/* Aspyr's OBB: a zlib-compressed std::set<FileMetadata> at the end, every
 * member stored. Calls fn(name, name_len, offset, size, ctx) for each member;
 * fn returns nonzero to stop. Returns the number of members visited, -1 if
 * the table could not be read. */
typedef int (*k2obb_fn)(const char *name, unsigned len, uint64_t off, uint64_t size, void *ctx);
int k2obb_scan(SceUID fd, k2obb_fn fn, void *ctx);

/* An ERF (MOD/ERF/SAV) at byte `base` of fd: fn(resref, type, abs_off, size, ctx)
 * for each resource, resref lowercased. */
typedef int (*k2erf_fn)(const char *resref, unsigned type, uint64_t off, uint32_t size, void *ctx);
int k2erf_scan(SceUID fd, uint64_t base, k2erf_fn fn, void *ctx);

/* len bytes at off, malloc'd; NULL on a short read. */
void *k2_read(SceUID fd, uint64_t off, uint32_t len);

/* A TPC (DXT1/DXT5, or 32-bit RGBA) to top-down RGBA8, malloc'd. */
uint8_t *k2tpc_rgba(const uint8_t *tpc, uint32_t len, int *w, int *h);

/* chitin.key: the BIF file name ("data\\2da.bif" -> "data/2da.bif") and index
 * of resref/type. */
int k2key_find(const uint8_t *key, uint32_t len, const char *resref, unsigned type, char *bif, int bif_sz,
               uint32_t *index);

/* Resource `index` of the BIF at byte `base` of fd, malloc'd. */
uint8_t *k2bif_read(SceUID fd, uint64_t base, uint32_t index, uint32_t *len);

/* The integer cells of one column of a binary 2DA (V2.b); empty cells
 * skipped. Returns how many were written. */
int k2_2da_ints(const uint8_t *d, uint32_t len, const char *column, int *out, int max);

/* String `strref` of the TLK at byte `base` of fd, as UTF-8 (the file is
 * cp1252). Returns its length, 0 if absent. */
int k2tlk_get(SceUID fd, uint64_t base, int strref, char *out, int out_sz);

#endif
