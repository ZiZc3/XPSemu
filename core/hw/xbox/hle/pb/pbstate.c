/*
 * hakuX HLE GPU — push-buffer mode in a savestate (pbstate.h; native core
 * only): the PBD section.
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/pb/pbmode.h"
#include "hw/xbox/hle/pb/pbstate.h"

#define NC_DEF_SECTION PBD
#define NC_DEF_FILE "native/state/pbd.def"
#include "native/state/def.h"
#include "native/state/sections.h"

bool nc_pbd_state_save(NcBuf *b)
{
    static HlePbState pb;
    static HlePbdState pd;
    static HleKvState kv;
    const float *imm;
    const uint32_t *inl, *idx;
    hle_pb_state_get(&pb);
    hle_pbd_state_get(&pd, &imm, &inl, &idx);
    hle_kelvin_state_get(&kv);
    nc_buf_u8(b, hle_pb_active());
    nc_rec_put(b, &nc_rec_HlePbState, &pb);
    nc_rec_put(b, &nc_rec_HlePbdState, &pd);
    nc_rec_put(b, &nc_rec_HleKvState, &kv);
    nc_buf_put(b, imm, (size_t)pd.nimm * 16 * 4 * sizeof(float));
    nc_buf_put(b, inl, (size_t)pd.ninl * sizeof(uint32_t));
    nc_buf_put(b, idx, (size_t)pd.nidx * sizeof(uint32_t));
    return true;
}

bool nc_pbd_state_load(NcRd *r, unsigned v)
{
    static HlePbState pb;
    static HlePbdState pd;
    static HleKvState kv;
    memset(&pb, 0, sizeof(pb));
    memset(&pd, 0, sizeof(pd));
    memset(&kv, 0, sizeof(kv));
    const bool active = nc_rd_u8(r);
    nc_rec_get(r, &nc_rec_HlePbState, &pb, v);
    nc_rec_get(r, &nc_rec_HlePbdState, &pd, v);
    nc_rec_get(r, &nc_rec_HleKvState, &kv, v);
    const void *imm = nc_rd_ptr(r, (size_t)pd.nimm * 16 * 4 * sizeof(float));
    const void *inl = nc_rd_ptr(r, (size_t)pd.ninl * sizeof(uint32_t));
    const void *idx = nc_rd_ptr(r, (size_t)pd.nidx * sizeof(uint32_t));
    if (r->fail) {
        return false;
    }
    if (active != hle_pb_active()) {
        hle_log("hakuX-hle-pb", "savestate: saved %s push-buffer mode, this run %s", active ? "in" : "out of",
                hle_pb_active() ? "in it" : "out of it");
        return false;
    }
    if (!active) {
        return true;
    }
    hle_pb_state_set(&pb);
    hle_kelvin_state_set(&kv);
    if (!hle_pbd_state_set(&pd, imm, inl, idx)) {
        return false;
    }
    hle_log("hakuX-hle-pb", "savestate: decoder at GET %08x%s%s, %llu swaps", pb.get,
            pb.flip_wait ? " (flip stall)" : "", pb.trap_wait ? " (trap)" : "", (unsigned long long)pd.swaps);
    return true;
}
