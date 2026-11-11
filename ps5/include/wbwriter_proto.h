/*
 * wbwriter_proto.h - what XPSemu and the persistent write-helper say to each other.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The helper (ps5/wbwriter/wbwriter.c) is a payload the app streams to the PS5
 * payload loader (elfldr, loopback port 9021). The loader runs it in its own
 * process - OUTSIDE the app sandbox, where the console does NOT throttle file
 * writes - and hands it the remainder of that same connection as its standard
 * input and output, over which it speaks this protocol and stays alive.
 *
 * The framing (stream the ELF, then keep the same socket for a line/piece
 * conversation) and the "helper runs outside the sandbox, un-throttled" idea
 * are adapted from ProsperoEden (BlackBearReloaded)'s self-update kit,
 * GPL-3.0-or-later - see headless/self_update_helper and
 * headless/update_check/self_update_protocol.h. Our frames are binary (many
 * small writes) rather than its text lines, but the mechanism is theirs.
 *
 * All little-endian (both sides are the same console). The app sends a request
 * frame (32 bytes + payload); the helper always answers with a reply frame (16
 * bytes). Requests and replies are 1:1 and in order on the single connection.
 */
#ifndef XOG_WBWRITER_PROTO_H
#define XOG_WBWRITER_PROTO_H

#include <stdint.h>

#define WBW_MAGIC 0x31525857u /* bytes 'W' 'X' 'R' '1' */
#define WBW_PROTO_VERSION 1u
#define WBW_CHUNK (1u << 20) /* largest data payload in one WRITE frame */
#define WBW_MAX_PATH 960u    /* longest host path either side accepts (cache restore paths are long) */
#define WBW_LOADER_PORT 9021

enum {
    WBW_HELLO = 1,    /* app -> helper, first; helper echoes with result = version */
    WBW_OPEN = 2,     /* payload = path (dlen); result = handle (>=1) */
    WBW_WRITE = 3,    /* handle, off, payload = data (dlen); result = bytes written */
    WBW_FSYNC = 4,    /* handle */
    WBW_TRUNCATE = 5, /* handle, off = new size */
    WBW_CLOSE = 6,    /* handle */
    WBW_DELETE = 7,   /* payload = path (dlen) */
    WBW_RENAME = 8,   /* payload = oldpath (dlen) then newpath (dlen2) */
    WBW_QUIT = 9      /* helper replies then exits */
};

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;  /* WBW_MAGIC */
    uint16_t op;     /* WBW_* */
    uint16_t flags;  /* reserved, 0 */
    uint32_t seq;    /* request id, echoed */
    uint32_t handle; /* file handle for handle ops, else 0 */
    uint64_t off;    /* WRITE offset / TRUNCATE size */
    uint32_t dlen;   /* payload bytes: data (WRITE) or first path */
    uint32_t dlen2;  /* RENAME: second path length, else 0 */
} WbwReq;            /* 32 bytes */

typedef struct {
    uint32_t magic;  /* WBW_MAGIC */
    uint16_t op;     /* echoed */
    int16_t status;  /* 0 ok, else -errno (clamped) */
    uint32_t seq;    /* echoed */
    uint32_t result; /* OPEN: handle; WRITE: bytes written; HELLO: version; else 0 */
} WbwRep;            /* 16 bytes */
#pragma pack(pop)

#endif
