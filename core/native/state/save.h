/*
 * hakuX native core — savestates: what the core and the app call (save.c).
 */
#ifndef NATIVE_STATE_SAVE_H
#define NATIVE_STATE_SAVE_H

#include "native/state/state.h"

#ifdef __cplusplus
extern "C" {
#endif

struct NcCore;
struct NcKernel;
struct NcFrame;

/* The core whose state is saved and loaded (nc_core_boot). */
void nc_state_attach(struct NcCore *c);
/* The app's version name, for the states' headers. */
void nc_state_set_core_version(const char *name);

/* Saves the running game to path (not on the guest CPU's thread): NULL, or
 * why it couldn't. Takes about a frame and the write. */
const char *nc_state_save(const char *path);

/* After nc_core_boot booted the state's XBE (the header's xbe_path): the
 * state replaces it. NULL, or why not (then the core must stop: what the
 * boot made is partly replaced). */
const char *nc_state_load(const char *path);

/* Whether the state at path can load into the running game: NULL, or why
 * not (another game or disc, a section this core can't read). */
const char *nc_state_compat(const char *path);

/* The state file being saved or loaded (its folder holds the game's blob
 * store, cache.c). */
const char *nc_state_path(void);
/* After a save: the blob store keeps what the folder's states use. */
void nc_cache_gc(const char *state_path);

/* The guest CPU at its pause point (nc_kernel_pause_point): a save waiting
 * for it is taken now. */
void nc_state_cpu_work(struct NcKernel *k, const struct NcFrame *f, bool idle);

#ifdef __cplusplus
}
#endif

#endif
