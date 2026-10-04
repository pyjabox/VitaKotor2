/* opensl_patch.h -- minimal OpenSL ES ABI used by the embedded Bink player. */
#ifndef OPENSL_PATCH_H
#define OPENSL_PATCH_H

#include <stdint.h>
#include <SLES/OpenSLES.h>

SLresult bink_slCreateEngine(SLObjectItf *engine, SLuint32 num_options,
                             const SLEngineOption *options,
                             SLuint32 num_interfaces,
                             const SLInterfaceID *interface_ids,
                             const SLboolean *interface_required);

extern const SLInterfaceID bink_sl_iid_engine;
extern const SLInterfaceID bink_sl_iid_play;
extern const SLInterfaceID bink_sl_iid_volume;
extern const SLInterfaceID bink_sl_iid_bufferqueue;

void bink_opensl_mix(int32_t *acc, unsigned frames, unsigned out_rate);
void bink_opensl_log_stats(void);

#endif
