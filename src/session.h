/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#ifndef MXGUEST_SESSION_H
#define MXGUEST_SESSION_H

#include "mxga.h"

#include <stddef.h>
#include <stdint.h>

/* A message is a u32 little-endian length N, then N bytes: a u8 type and the payload. */
#define MXGUEST_SESSION_CLIPBOARD 1u
#define MXGUEST_SESSION_WINDOWS 2u
#define MXGUEST_SESSION_ACTION 3u

#define MXGUEST_SESSION_PAYLOAD_MAX (MXGA_MAX_FRAME_BYTES - MXGA_HEADER_BYTES)
#define MXGUEST_SESSION_FRAME_MAX (4u + 1u + MXGUEST_SESSION_PAYLOAD_MAX)
#define MXGUEST_SESSION_OUT_CAP (2u * MXGUEST_SESSION_FRAME_MAX)

struct mxguest_session_in {
    uint8_t *buf;
    size_t have;
    size_t msg;
};

struct mxguest_session_out {
    uint8_t *buf;
    size_t len;
    size_t off;
};

int mxguest_session_in_init(struct mxguest_session_in *in);
void mxguest_session_in_free(struct mxguest_session_in *in);
size_t mxguest_session_in_feed(struct mxguest_session_in *in, const uint8_t *data, size_t len);
/* Returns 1 with the next client message (valid until the next feed or call), 0 when incomplete, -1 on violation. */
int mxguest_session_in_next(struct mxguest_session_in *in, uint8_t *type, const uint8_t **payload,
                            uint32_t *len);

int mxguest_session_out_init(struct mxguest_session_out *out);
void mxguest_session_out_free(struct mxguest_session_out *out);
/* Returns -1 when the payload is oversized for the type or the pending output would exceed two frames. */
int mxguest_session_out_push(struct mxguest_session_out *out, uint8_t type, const uint8_t *payload,
                             uint32_t len);
const uint8_t *mxguest_session_out_pending(const struct mxguest_session_out *out, size_t *len);
void mxguest_session_out_advance(struct mxguest_session_out *out, size_t len);

#endif
