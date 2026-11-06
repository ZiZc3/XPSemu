/*
 * hakuX HLE GPU — the title: its XBE read from guest memory when it starts,
 * then push-buffer mode for it (pb/pbmode.h).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/core/image.h"
#include "hw/xbox/hle/core/probe.h"
#include "hw/xbox/hle/core/xbe_guest.h"
#include "hw/xbox/hle/pb/pbmode.h"

#define MAX_IMAGE (64u << 20)

static uint8_t g_hdr[HLE_XBE_HDR_MAX];

bool hle_title_in_lib(uint32_t va)
{
    const HleTitle *t = &g_hle.title;
    for (unsigned i = 0; i < t->nlib; i++) {
        if (va >= t->lib_lo[i] && va < t->lib_hi[i]) {
            return true;
        }
    }
    return false;
}

void hle_title_stop(void)
{
    hle_pb_stop();
    memset(&g_hle.title, 0, sizeof(g_hle.title));
}

/* The D3D library's sections (diagnostics: where the guest CPU is). */
static void lib_ranges(HleTitle *t)
{
    for (unsigned i = 0; i < t->xbe.nsections && t->nlib < HLE_MAX_LIB_RANGES; i++) {
        const HleXbeSection *s = &t->xbe.sections[i];
        uint32_t end;
        if (hle_is_lib_section_name(s->name) &&
            hle_img_section_end(t->xbe.base, t->xbe.sizeof_image, s->va, s->vsize, &end)) {
            t->lib_lo[t->nlib] = s->va;
            t->lib_hi[t->nlib++] = end;
        }
    }
}

void hle_title_start(void)
{
    uint64_t fp = hle_xbe_guest_fingerprint();
    if (fp == 0 || fp == g_hle.title.fp) {
        return;
    }
    hle_title_stop();
    HleTitle *t = &g_hle.title;
    t->fp = fp;
    uint32_t hl = hle_xbe_read_guest_headers(g_hdr, sizeof(g_hdr));
    if (!hl || hle_xbe_parse(g_hdr, hl, &t->xbe) != HLE_XBE_OK ||
        t->xbe.sizeof_image > MAX_IMAGE) {
        hle_log("hakuX-hle", "fingerprint %016llx: unusable XBE headers; HLE idle",
                (unsigned long long)fp);
        return;
    }
    lib_ranges(t);
    hle_log("hakuX-hle-mode", "title %08x: push-buffer mode", t->xbe.title_id);
    hle_probe_register();                   /* debug: HAKUX_HLE_PROBE */
    hle_pb_start();
    hle_host_code_changed();
    hle_host_kick();
}
