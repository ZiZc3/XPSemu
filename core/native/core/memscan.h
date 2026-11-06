/*
 * hakuX native core — debug: guest memory scans (debug.hakux.memscan) for
 * writing mods and cheats; memscan.c lists the commands.
 */
#ifndef NATIVE_CORE_MEMSCAN_H
#define NATIVE_CORE_MEMSCAN_H

#include "native/core/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The guest CPU's thread, once a vblank (mods.c). */
void nc_memscan_poll(const NcVm *vm);

#ifdef __cplusplus
}
#endif

#endif
