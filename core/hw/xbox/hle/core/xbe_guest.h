/*
 * hakuX HLE GPU — read the running title's XBE headers from guest memory.
 * vCPU thread only.
 */
#ifndef HW_XBOX_HLE_CORE_XBE_GUEST_H
#define HW_XBOX_HLE_CORE_XBE_GUEST_H

#include "hw/xbox/hle/core/xbe.h"

#define HLE_XBE_HDR_VA   0x00010000u
#define HLE_XBE_HDR_MAX  0x8000u

/* Copies sizeof_headers bytes from VA 0x10000 into buf (cap >= 0x178).
 * Returns bytes read, or 0 when no valid XBE header is mapped. */
uint32_t hle_xbe_read_guest_headers(uint8_t *buf, uint32_t cap);

/* (timedate << 32) | encoded entry of the mapped XBE, or 0 if none. */
uint64_t hle_xbe_guest_fingerprint(void);

#endif
