/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#ifndef MXGUEST_CLIPBOARD_H
#define MXGUEST_CLIPBOARD_H

#include "mxga.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MXGUEST_CLIP_LOCAL_FRAME_MAX (4u + MXGA_CLIPBOARD_MAX_TEXT_BYTES)
#define MXGUEST_CLIP_OUT_CAP (2u * MXGUEST_CLIP_LOCAL_FRAME_MAX)

struct mxguest_clip {
    uint64_t origin;
    uint64_t generation;
    uint8_t *host_text;
    uint32_t host_len;
    int have_host;
    int echo;
};

struct mxguest_clip_in {
    uint8_t *buf;
    size_t have;
    size_t msg;
};

struct mxguest_clip_out {
    uint8_t *buf;
    size_t len;
    size_t off;
};

uint64_t mxguest_clip_random_origin(void);
uint64_t mxguest_clip_caps(uint64_t base, int session_connected);

int mxguest_clip_init(struct mxguest_clip *clip, uint64_t origin);
void mxguest_clip_free(struct mxguest_clip *clip);

/* Returns 1 with a CLIPBOARD_CHANGED payload built, 0 when the text is dropped or suppressed, -1 on error. */
int mxguest_clip_build_changed(struct mxguest_clip *clip, const uint8_t *text, uint32_t len,
                               uint8_t *payload, uint32_t cap, uint32_t *payload_len);
/* Returns 1 when the stored host text must be forwarded, 0 when ignored, -1 on a malformed payload. */
int mxguest_clip_host_write(struct mxguest_clip *clip, const uint8_t *payload, uint32_t len,
                            const uint8_t **text, uint32_t *text_len);

int mxguest_clip_in_init(struct mxguest_clip_in *in);
void mxguest_clip_in_free(struct mxguest_clip_in *in);
/* Appends up to the free space and returns the byte count accepted. */
size_t mxguest_clip_in_feed(struct mxguest_clip_in *in, const uint8_t *data, size_t len);
/* Returns 1 with the next message (valid until the next feed or next call), 0 when incomplete, -1 on violation. */
int mxguest_clip_in_next(struct mxguest_clip_in *in, const uint8_t **text, uint32_t *len);

int mxguest_clip_out_init(struct mxguest_clip_out *out);
void mxguest_clip_out_free(struct mxguest_clip_out *out);
/* Returns -1 when the message is oversized or the pending output would exceed two frames. */
int mxguest_clip_out_push(struct mxguest_clip_out *out, const uint8_t *text, uint32_t len);
const uint8_t *mxguest_clip_out_pending(const struct mxguest_clip_out *out, size_t *len);
void mxguest_clip_out_advance(struct mxguest_clip_out *out, size_t len);

/* Returns 0 and the uid of ACTIVE_UID= in a logind seat state file, or -1. */
int mxguest_clip_active_uid(const char *path, uid_t *uid);
int mxguest_clip_peer_allowed(const char *seat_path, uid_t uid);

#endif
