/* room_dist.h -- optional: rooms beyond a distance are not drawn (room_dist.c). */

#ifndef __ROOM_DIST_H__
#define __ROOM_DIST_H__

#include <stdint.h>

/* Reads ux0:data/kotor2/room_distance.txt; hooks CollectActiveRooms only if
 * it sets a distance (or in ROOM_AB test builds). */
void room_dist_install(void);
/* Test builds (ROOM_AB), every presented frame: the running count of draws. */
void room_dist_frame(uint64_t swap_end_us, uint64_t draws_total);

#endif
