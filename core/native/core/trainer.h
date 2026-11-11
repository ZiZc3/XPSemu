/*
 * XPSemu (XOG) native core: Xbox trainers (.etm, Evox Trainer Modules).
 * Our own code (patch 246). GPL-3.0-or-later.
 *
 * An .etm is a small block of x86 code made for one game: the dashboard put it in
 * memory and called its entry before the game's own, and it patched the game's code
 * for the options turned on (one byte each, 1 on). Here:
 *
 *   the start line   trainer=<file>  trainer_opts=<'0'/'1' per option>  (main_ps5.c)
 *   at boot          nc_trainer_install: the file read and checked, refused unless its
 *                    title IDs name the running XBE; copied into a contiguous block with
 *                    a stub before it; the boot thread starts at the stub, which calls the
 *                    trainer's entry (EDX = EBP = its base), reports on a port and goes on
 *                    to the title's entry
 *   order            the CPU layer's own code changes (title patches, the rdtsc/flag sweep,
 *                    the int3 hooks) wait for that report: they are made on the code as the
 *                    trainer left it (rx_cpu_after_trainer). Mods' XBE patches are in the
 *                    image before (they patch the disc's bytes as the XBE loads).
 *   savestates       not run again from a state: the state's memory has it
 *   live (252)       at the start each option's bytes are recorded (the entry run once per option on
 *                    the code as loaded, the memory put back between runs); the quick menu then
 *                    switches an option by writing its new or old bytes and its option byte, on the
 *                    guest CPU's thread while paused. Restart only: kernel use, a state load, or
 *                    options that change other bytes together than alone
 *
 * .xbtf (XBMC4Xbox's scrambled format with helper "toys") is not read yet.
 */
#ifndef NATIVE_CORE_TRAINER_H
#define NATIVE_CORE_TRAINER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_TRAINER_MAX_OPTIONS 100
#define NC_TRAINER_TEXT 88
#define NC_TRAINER_MAX_SIZE (64u * 1024u)
#define NC_TRAINER_MAX_DATES 8

typedef struct NcTrainerInfo {
    char name[NC_TRAINER_TEXT];
    char author[NC_TRAINER_TEXT];
    unsigned noptions;
    char options[NC_TRAINER_MAX_OPTIONS][NC_TRAINER_TEXT];
    unsigned nids;
    uint32_t title_ids[3];
    unsigned ndates;                        /* XBE dates its code compares 0x10114 with */
    uint32_t dates[NC_TRAINER_MAX_DATES];
    uint32_t size, sel, txt, ids, entry;    /* the file's size and header offsets */
} NcTrainerInfo;

/* Reads an .etm's header, texts and title IDs from buf[0..n). False with why
 * (a RAR renamed .etm, offsets past the end, ...). */
bool nc_trainer_parse(const uint8_t *buf, size_t n, NcTrainerInfo *info, char *why, size_t why_size);
/* The same from a file. */
bool nc_trainer_read(const char *path, NcTrainerInfo *info, char *why, size_t why_size);
/* One of its title IDs is title_id. */
bool nc_trainer_for_title(const NcTrainerInfo *info, uint32_t title_id);

/* The start line's (main_ps5.c), before the boot. */
void nc_trainer_set(const char *path);
void nc_trainer_set_options(const char *bits);
bool nc_trainer_wanted(void);

struct NcKernel;
struct NcVm;
struct HleXbeInfo;
/* After the XBE is loaded, before the boot thread is made: the eip the boot
 * thread starts at (the trainer's stub, or entry when none is run). */
uint32_t nc_trainer_install(struct NcKernel *k, struct NcVm *vm, const struct HleXbeInfo *xbe, uint32_t entry);
/* Installed and its entry not returned yet: the CPU layer holds its code changes. */
bool nc_trainer_pending(void);

/* patch 252: live switches. 1 when this run's options switch live; else 0 and why. */
int nc_trainer_live(char *why, size_t n);
/* The options as they are now ('0'/'1' each); the count, 0 when not live. */
int nc_trainer_live_bits(char *out, size_t n);
/* Options i and j change the same bytes (one on turns the other off). */
int nc_trainer_live_overlap(unsigned i, unsigned j);
/* The bytes option i's switch writes (0: only its option byte, read by the trainer's hooks). */
unsigned nc_trainer_live_bytes(unsigned i);
/* Option i on or off now (the game paused): 1 done, 0 not live, -1 failed; msg says what else changed or why. */
int nc_trainer_live_set(unsigned i, int on, char *msg, size_t n);
/* The guest CPU's thread at its pause point (kernel.c): a switch asked for is made. */
void nc_trainer_cpu_work(void);
/* A state was loaded: the recording isn't of this memory, live switches off. */
void nc_trainer_state_loaded(void);
/* Patch 254: once a second (the CPU layer's heartbeat): the bytes of options switched live in the last 15 s
 * read again; a change from what the switch wrote is logged (who puts a cheat back). */
void nc_trainer_live_watch(void);

/* Port I/O the trainer's stub and code make (native/dev/ports.c asks first). */
bool nc_trainer_port_out(uint16_t port, unsigned bytes, uint32_t value);
bool nc_trainer_port_in(uint16_t port, unsigned bytes, uint32_t *value);

#ifdef __cplusplus
}
#endif

#endif
