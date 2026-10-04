/* obb_index.h -- replay cache for scattered archive-mount reads.
 * Historical measurements below are inherited from VitaKotor (KOTOR I), not
 * KOTOR II validation. The implementation records repeated metadata reads and
 * replays them on later launches.
 *
 * Correctness rests on serving only byte ranges that were actually recorded
 * from this exact archive (validated by size). Anything not covered falls
 * through to a real read, so a stale or partial cache is slow, never wrong. */

#ifndef __OBB_INDEX_H__
#define __OBB_INDEX_H__

typedef struct ObbIndex ObbIndex;

/* Open the cache beside `archive_path`. If a valid one exists for an archive of
 * exactly `archive_size` bytes it is loaded and serving begins; otherwise the
 * returned index starts in recording mode. NULL means neither is possible, and
 * callers should just do normal reads. */
ObbIndex *obbidx_open(const char *archive_path, long archive_size);

/* 1 if the whole range was served from the cache, 0 if the caller must read. */
int obbidx_serve(ObbIndex *ix, long off, void *dst, long len);

/* Remember a range that was just read. No-op unless recording. */
void obbidx_record(ObbIndex *ix, long off, const void *src, long len);

/* Write the recording out. Call once the mount is done -- recording every read
 * for the whole session would grow without bound. No-op when serving. */
void obbidx_finish(ObbIndex *ix);

void obbidx_close(ObbIndex *ix);

/* 1 when this index is replaying a cache rather than recording one. */
int obbidx_is_serving(const ObbIndex *ix);

#endif
