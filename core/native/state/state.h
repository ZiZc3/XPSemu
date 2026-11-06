/*
 * hakuX native core — savestates (push-buffer mode).
 *
 * A state is a header and versioned sections. What a section stores is
 * described by field tables (the .def files in native/state, expanded by def.h in the
 * module that owns the structs), never copied as host structs: a field
 * added later loads from an older state with its default, and a change to
 * a saved struct stops the build until native/state/lock.h is updated by
 * tools/native/statelock.py, which makes the section's version go up with
 * it (native/state/CHANGELOG.md says what each version changed).
 */
#ifndef NATIVE_STATE_STATE_H
#define NATIVE_STATE_STATE_H

#include "native/core/nc.h"
#include "native/state/lock.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- field tables --------------------------------------------------------- */

enum NcFieldType {
    NC_FT_U8 = 1, NC_FT_U16, NC_FT_U32, NC_FT_U64,
    NC_FT_I8, NC_FT_I16, NC_FT_I32, NC_FT_I64,
    NC_FT_BOOL, NC_FT_F32, NC_FT_F64,
    NC_FT_REC,                  /* records of another table */
};

/* the element sizes the .def files' types name (def.h) */
#define NC_ESIZE_U8   1
#define NC_ESIZE_U16  2
#define NC_ESIZE_U32  4
#define NC_ESIZE_U64  8
#define NC_ESIZE_I8   1
#define NC_ESIZE_I16  2
#define NC_ESIZE_I32  4
#define NC_ESIZE_I64  8
#define NC_ESIZE_BOOL 1
#define NC_ESIZE_F32  4
#define NC_ESIZE_F64  8

typedef struct NcRecord NcRecord;

typedef struct NcField {
    const char *name;
    uint32_t offset;            /* in the host struct */
    uint8_t type;               /* NcFieldType */
    uint32_t esize;             /* an element's size (a record's: its host struct's) */
    uint32_t count;
    uint16_t since, until;      /* section versions: stored from since, up to before until (0: still) */
    const NcRecord *rec;        /* NC_FT_REC */
} NcField;

struct NcRecord {
    const char *name;
    const NcField *fields;
    unsigned nfields;
};

/* One field's share of a layout hash (def.h; tools/native/statelock.py
 * computes the same): what the stored bytes depend on, not where the host
 * keeps them. */
#define NC_FH_STEP(v, x) ((uint32_t)(v) * 131u + (uint32_t)(x))
#define NC_FH(t, e, n, s, u, l)                                                                    \
    (NC_FH_STEP(NC_FH_STEP(NC_FH_STEP(NC_FH_STEP(NC_FH_STEP(t, e), n), s), u), l) * 2654435761u)

/* ---- buffers -------------------------------------------------------------- */

typedef struct NcBuf {
    uint8_t *p;
    size_t len, cap;
    bool fail;                  /* out of memory: everything after is dropped */
} NcBuf;

typedef struct NcRd {
    const uint8_t *p;
    size_t len, pos;
    bool fail;                  /* read past the end: everything after reads 0 */
} NcRd;

void nc_buf_put(NcBuf *b, const void *p, size_t n);
void nc_buf_free(NcBuf *b);
static inline void nc_buf_u8(NcBuf *b, uint8_t v) { nc_buf_put(b, &v, 1); }
static inline void nc_buf_u32(NcBuf *b, uint32_t v) { nc_buf_put(b, &v, 4); }
static inline void nc_buf_u64(NcBuf *b, uint64_t v) { nc_buf_put(b, &v, 8); }

bool nc_rd_get(NcRd *r, void *out, size_t n);
/* n bytes in place (NULL past the end) */
const void *nc_rd_ptr(NcRd *r, size_t n);
static inline uint8_t nc_rd_u8(NcRd *r) { uint8_t v = 0; nc_rd_get(r, &v, 1); return v; }
static inline uint32_t nc_rd_u32(NcRd *r) { uint32_t v = 0; nc_rd_get(r, &v, 4); return v; }
static inline uint64_t nc_rd_u64(NcRd *r) { uint64_t v = 0; nc_rd_get(r, &v, 8); return v; }

/* A record's fields from obj (the section's current version). */
void nc_rec_put(NcBuf *b, const NcRecord *rec, const void *obj);
/* A record stored by a section of version v into obj: the fields v didn't
 * have yet keep obj's values (the caller's defaults); the ones it had and
 * this core dropped are read and dropped. */
void nc_rec_get(NcRd *r, const NcRecord *rec, void *obj, unsigned v);
/* n records of rec at obj, stride bytes apart */
void nc_recs_put(NcBuf *b, const NcRecord *rec, const void *obj, size_t stride, unsigned n);
void nc_recs_get(NcRd *r, const NcRecord *rec, void *obj, size_t stride, unsigned n, unsigned v);

/* ---- sections ------------------------------------------------------------- */

/* Section tags, as stored (four characters). */
#define NC_TAG(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

typedef struct NcStateSection {
    uint32_t tag;
    const char *name;
    unsigned version, min;      /* lock.h's: what this core writes, the oldest it reads */
    /* Appends the section; false fails the save (the reason logged). */
    bool (*save)(NcBuf *b);
    /* Restores the section stored by version v (min <= v <= version);
     * false fails the load. */
    bool (*load)(NcRd *r, unsigned v);
    /* A state from before the section existed loads without it. */
    bool optional;
    /* Such a load: the section's part as at power-on (NULL: as it is). */
    void (*absent)(void);
} NcStateSection;

/* ---- the state file ------------------------------------------------------- */

#define NC_STATE_MAGIC     NC_TAG('H', 'X', 'S', '1')

/* What a state names besides its sections (the header). */
typedef struct NcStateInfo {
    uint32_t title_id;
    char xbe_path[128];          /* the image running (relaunched titles: not default.xbe) */
    uint8_t xbe_sha1[20];        /* its header and certificate */
    uint64_t disc_size;
    uint32_t disc_hash;          /* its first sector's */
    uint32_t ram_size;
    uint64_t guest_ns;           /* the guest's time at the save */
    int64_t wall_time;           /* the save's (unix seconds) */
    uint8_t point;               /* NC_STATE_AT_* */
    char core_version[32];       /* the app's version name */
    char core_git[48];           /* its commit (+dirty): for diagnosis only */
} NcStateInfo;

enum { NC_STATE_AT_ENTRY = 1, NC_STATE_AT_IDLE = 2 };

/* Writes a state of the running emulation to path (atomically: a failure
 * keeps the file there). Only on the guest CPU's thread at a pause point,
 * with every device quiesced (save.c). NULL: saved; else why not. */
const char *nc_state_write(const char *path, const NcStateInfo *info, const NcStateSection *secs,
                           unsigned nsecs);

/* Reads the state at path: the header into *info, and each section through
 * secs (refused: a section this core doesn't know, or one older than its
 * min or newer than its version). NULL: loaded; else why not. */
const char *nc_state_read(const char *path, NcStateInfo *info, const NcStateSection *secs,
                          unsigned nsecs);
/* The same checks, nothing restored: NULL if it would load. */
const char *nc_state_verify(const char *path, NcStateInfo *info, const NcStateSection *secs,
                            unsigned nsecs);
/* One section of the state at path (tag), read by fn: NULL if it read (or
 * the state has none), else why not. */
const char *nc_state_read_section(const char *path, uint32_t tag, bool (*fn)(NcRd *r, unsigned v, void *ctx),
                                  void *ctx);
/* The header only (the app's slot list). */
bool nc_state_read_info(const char *path, NcStateInfo *info);

/* A section's load failing says why (shown to the user, after the
 * section's name); the first reason since the load began counts. */
void nc_state_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Field-level logs (NC_STATE_TRACE=1). */
bool nc_state_trace(void);

#ifdef __cplusplus
}
#endif

#endif
