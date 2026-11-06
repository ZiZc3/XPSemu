/*
 * hakuX native core — the APU's savestate section.
 *
 * The fields are stored self-describing, as xemu's VMState descriptions
 * named them: a description's field count, then each field's name, kind
 * (1 bytes, 2 structs, 3 a pointed-to struct), size and count, and its data
 * (a struct's: its own description's fields, nested). A load matches fields
 * by name, so a field this core no longer has is skipped, and refuses a
 * state whose field changed or that misses one (but an optional one, added
 * later: older states don't have it). The tables below are the
 * xemu descriptions' fields in their order; states saved over them load
 * here.
 */
#include "native/dev/apu/apu_int.h"

typedef struct NcApuState {
    int32_t version_id;
} NcApuState;

#define NC_DEF_SECTION APU
#define NC_DEF_FILE "native/state/apu.def"
#include "native/state/def.h"
#include "native/state/sections.h"

enum { F_END, F_DATA, F_STRUCT, F_PTR };

typedef struct ApuField ApuField;
typedef struct ApuDesc {
    const char *name;
    const ApuField *fields;
} ApuDesc;
struct ApuField {
    const char *name;
    uint8_t kind;
    uint32_t offset, size, num;
    const ApuDesc *sub;
    bool opt;                   /* added later: a state without it loads, the field left as it is */
};

#define DATA(s, m) {#m, F_DATA, offsetof(s, m), sizeof(((s *)0)->m), 1, NULL}
#define OPT_DATA(s, m) {#m, F_DATA, offsetof(s, m), sizeof(((s *)0)->m), 1, NULL, true}
#define STRUCT(s, m, n, t, desc) {#m, F_STRUCT, offsetof(s, m), sizeof(t), (n), &(desc)}
#define PTR(s, m, t, desc) {#m, F_PTR, offsetof(s, m), sizeof(t), 1, &(desc)}
#define END {NULL, F_END, 0, 0, 0, NULL}

/* the version of the field layout ("mcpx-apu" 1) */
#define APU_STATE_VERSION 1

static const ApuField k_dma_fields[] = {
    DATA(DSPDMAState, configuration),
    DATA(DSPDMAState, control),
    DATA(DSPDMAState, start_block),
    DATA(DSPDMAState, next_block),
    DATA(DSPDMAState, error),
    DATA(DSPDMAState, eol),
    END,
};
static const ApuDesc k_dma = {"mcpx-apu/dsp-state/dma", k_dma_fields};

static const ApuField k_core_fields[] = {
    DATA(dsp_core_t, instr_cycle),
    DATA(dsp_core_t, pc),
    DATA(dsp_core_t, registers),
    DATA(dsp_core_t, stack),
    DATA(dsp_core_t, xram),
    DATA(dsp_core_t, yram),
    DATA(dsp_core_t, pram),
    DATA(dsp_core_t, mixbuffer),
    DATA(dsp_core_t, periph),
    DATA(dsp_core_t, loop_rep),
    DATA(dsp_core_t, pc_on_rep),
    DATA(dsp_core_t, interrupt_state),
    DATA(dsp_core_t, interrupt_instr_fetch),
    DATA(dsp_core_t, interrupt_save_pc),
    DATA(dsp_core_t, interrupt_counter),
    DATA(dsp_core_t, interrupt_ipl_to_raise),
    DATA(dsp_core_t, interrupt_pipeline_count),
    DATA(dsp_core_t, interrupt_ipl),
    DATA(dsp_core_t, interrupt_is_pending),
    DATA(dsp_core_t, num_inst),
    DATA(dsp_core_t, cur_inst_len),
    DATA(dsp_core_t, cur_inst),
    DATA(dsp_core_t, disasm_memory_ptr),
    DATA(dsp_core_t, exception_debugging),
    DATA(dsp_core_t, disasm_prev_inst_pc),
    DATA(dsp_core_t, disasm_is_looping),
    DATA(dsp_core_t, disasm_cur_inst),
    DATA(dsp_core_t, disasm_cur_inst_len),
    DATA(dsp_core_t, disasm_registers_save),
    END,
};
static const ApuDesc k_core = {"mcpx-apu/dsp-state/core", k_core_fields};

static const ApuField k_dsp_fields[] = {
    STRUCT(DSPState, core, 1, dsp_core_t, k_core),
    STRUCT(DSPState, dma, 1, DSPDMAState, k_dma),
    DATA(DSPState, save_cycles),
    DATA(DSPState, interrupts),
    END,
};
static const ApuDesc k_dsp = {"mcpx-apu/dsp-state", k_dsp_fields};

static const ApuField k_ssl_fields[] = {
    DATA(MCPXAPUVPSSLData, base),
    DATA(MCPXAPUVPSSLData, count),
    DATA(MCPXAPUVPSSLData, ssl_index),
    DATA(MCPXAPUVPSSLData, ssl_seg),
    END,
};
static const ApuDesc k_ssl = {"mcpx_apu_voice_data", k_ssl_fields};

static const ApuField k_apu_fields[] = {
    PTR(MCPXAPUState, gp.dsp, DSPState, k_dsp),
    DATA(MCPXAPUState, gp.regs),
    PTR(MCPXAPUState, ep.dsp, DSPState, k_dsp),
    DATA(MCPXAPUState, ep.regs),
    DATA(MCPXAPUState, regs),
    DATA(MCPXAPUState, vp.inbuf_sge_handle),
    DATA(MCPXAPUState, vp.outbuf_sge_handle),
    STRUCT(MCPXAPUState, vp.ssl, MCPX_HW_MAX_VOICES, MCPXAPUVPSSLData, k_ssl),
    DATA(MCPXAPUState, vp.ssl_base_page),
    DATA(MCPXAPUState, vp.hrtf_submix),
    DATA(MCPXAPUState, vp.hrtf_headroom),
    DATA(MCPXAPUState, vp.submix_headroom),
    DATA(MCPXAPUState, vp.voice_locked),
    /* the HRTF entries the 3D voices' filters take: without them a voice
     * the title didn't commit again was silent after a load
     * (mcpx_apu_vp_restore_hrtf) */
    OPT_DATA(MCPXAPUState, vp.hrtf),
    END,
};
static const ApuDesc k_apu = {"mcpx-apu", k_apu_fields};

static void put(NcBuf *b, const ApuDesc *desc, void *base)
{
    uint32_t n = 0;
    for (const ApuField *f = desc->fields; f->kind != F_END; f++) {
        n++;
    }
    nc_buf_u32(b, n);
    for (const ApuField *f = desc->fields; f->kind != F_END; f++) {
        const uint8_t len = (uint8_t)strlen(f->name);
        nc_buf_u8(b, len);
        nc_buf_put(b, f->name, len);
        nc_buf_u8(b, f->kind);
        nc_buf_u32(b, f->size);
        nc_buf_u32(b, f->num);
        uint8_t *p = (uint8_t *)base + f->offset;
        if (f->kind == F_DATA) {
            nc_buf_put(b, p, f->size);
        } else if (f->kind == F_STRUCT) {
            for (uint32_t i = 0; i < f->num; i++) {
                put(b, f->sub, p + i * f->size);
            }
        } else {
            put(b, f->sub, *(void **)p);
        }
    }
}

static bool skip(NcRd *r);

/* A stored field this core doesn't have: its data passed over. */
static bool skip_field(NcRd *r, uint8_t kind, uint32_t size, uint32_t num)
{
    if (kind == F_DATA) {
        return nc_rd_ptr(r, size) != NULL;
    }
    const uint32_t k = kind == F_STRUCT ? num : 1;
    for (uint32_t i = 0; i < k; i++) {
        if (!skip(r)) {
            return false;
        }
    }
    return true;
}

static bool skip(NcRd *r)
{
    const uint32_t n = nc_rd_u32(r);
    for (uint32_t i = 0; i < n && !r->fail; i++) {
        const uint8_t len = nc_rd_u8(r);
        nc_rd_ptr(r, len);
        const uint8_t kind = nc_rd_u8(r);
        const uint32_t size = nc_rd_u32(r), num = nc_rd_u32(r);
        if (!skip_field(r, kind, size, num)) {
            return false;
        }
    }
    return !r->fail;
}

/* A struct's stored size is its host layout's, not the data's (that is the
 * nested fields'); only the bytes fields' sizes have to agree. */
static bool get(NcRd *r, const ApuDesc *desc, void *base, const char *path)
{
    bool ok = true;
    unsigned have = 0, want = 0;
    for (const ApuField *f = desc->fields; f->kind != F_END; f++) {
        want += !f->opt;
    }
    const uint32_t n = nc_rd_u32(r);
    for (uint32_t i = 0; ok && i < n && !r->fail; i++) {
        char name[256];
        const uint8_t len = nc_rd_u8(r);
        nc_rd_get(r, name, len);
        name[len] = 0;
        const uint8_t kind = nc_rd_u8(r);
        const uint32_t size = nc_rd_u32(r), num = nc_rd_u32(r);
        const ApuField *f = desc->fields;
        while (f->kind != F_END && strcmp(f->name, name) != 0) {
            f++;
        }
        if (f->kind == F_END) {
            nc_log("hakuX-native-state", "apu: %s.%s is no longer stored: skipped", path, name);
            ok = skip_field(r, kind, size, num);
            continue;
        }
        if (f->kind != kind || f->num != num || (kind == F_DATA && f->size != size)) {
            nc_log("hakuX-native-state", "apu: %s.%s changed (kind %u size %u x%u, now %u size %u x%u)", path,
                   name, kind, size, num, f->kind, f->size, f->num);
            ok = false;
            break;
        }
        have += !f->opt;
        uint8_t *p = (uint8_t *)base + f->offset;
        if (kind == F_DATA) {
            ok = nc_rd_get(r, p, size);
        } else if (kind == F_STRUCT) {
            for (uint32_t k = 0; ok && k < num; k++) {
                ok = get(r, f->sub, p + k * f->size, f->name);
            }
        } else {
            ok = get(r, f->sub, *(void **)p, f->name);
        }
    }
    if (ok && have != want) {
        nc_log("hakuX-native-state", "apu: %s: the state has %u of this core's %u fields", path, have, want);
        ok = false;
    }
    return ok && !r->fail;
}

/* The frame thread is held off (d->lock) while the device's state is read
 * or written. */
bool nc_apu_state_save(NcBuf *b)
{
    MCPXAPUState *d = g_state;
    const NcApuState st = {APU_STATE_VERSION};
    nc_rec_put(b, &nc_rec_NcApuState, &st);
    pthread_mutex_lock(&d->lock);
    put(b, &k_apu, d);
    pthread_cond_signal(&d->cond);
    pthread_mutex_unlock(&d->lock);
    return true;
}

bool nc_apu_state_load(NcRd *r, unsigned v)
{
    MCPXAPUState *d = g_state;
    NcApuState st = {0};
    nc_rec_get(r, &nc_rec_NcApuState, &st, v);
    if (st.version_id != APU_STATE_VERSION) {
        nc_log("hakuX-native-state", "apu: state version %d, this core reads %d", st.version_id,
               APU_STATE_VERSION);
        return false;
    }
    mcpx_apu_reset(d);
    pthread_mutex_lock(&d->lock);
    const bool ok = get(r, &k_apu, d, k_apu.name);
    mcpx_apu_post_load(d);
    pthread_cond_signal(&d->cond);
    pthread_mutex_unlock(&d->lock);
    return ok;
}
