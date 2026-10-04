/* occlusion_cull.h -- GPU occlusion culling of Gobs (see OCCLUSION_CULL_ENABLE
 * in config.h). */

#ifndef __OCCLUSION_CULL_H__
#define __OCCLUSION_CULL_H__

#include <stdint.h>

/* Main thread, after libkotor2 is relocated and after every other installer
 * that may own Gob::Render or Gob::VisibilityCheck: takes both PLT slots, or
 * neither if any of them is already hooked. */
void occlusion_cull_install(void);

/* SDL swap hook, game thread, once per presented frame. Creates the queries on
 * the first call (the GL context lives on this thread). */
void occlusion_cull_on_swap(uint64_t swap_end_us);

#endif
