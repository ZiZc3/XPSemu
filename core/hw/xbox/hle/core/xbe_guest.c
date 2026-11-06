/*
 * hakuX HLE GPU — read the running title's XBE headers from guest memory.
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/core/xbe_guest.h"

uint32_t hle_xbe_read_guest_headers(uint8_t *buf, uint32_t cap)
{
    if (cap < 0x178) {
        return 0;
    }
    if (hle_va_rw(HLE_XBE_HDR_VA, buf, 0x178, false) != 0) {
        return 0;
    }
    if (hle_rd32(buf) != HLE_XBE_MAGIC) {
        return 0;
    }
    uint32_t soh = hle_rd32(buf + 0x108);
    if (soh < 0x178 || soh > cap || soh > HLE_XBE_HDR_MAX) {
        return 0;
    }
    if (hle_va_rw(HLE_XBE_HDR_VA, buf, soh, false) != 0) {
        return 0;
    }
    return soh;
}

uint64_t hle_xbe_guest_fingerprint(void)
{
    uint8_t h[0x178];
    if (hle_va_rw(HLE_XBE_HDR_VA, h, sizeof(h), false)) {
        return 0;
    }
    if (hle_rd32(h) != HLE_XBE_MAGIC) {
        return 0;
    }
    return ((uint64_t)hle_rd32(h + 0x114) << 32) | hle_rd32(h + 0x128);
}
