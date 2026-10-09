/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#ifndef MXGUEST_CLIPBOARD_H
#define MXGUEST_CLIPBOARD_H

#include "mxga.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct mxguest_clip {
    uint64_t origin;
    uint64_t generation;
    uint8_t *host_text;
    uint32_t host_len;
    int have_host;
    int echo;
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

/* Returns 0 and the uid of ACTIVE_UID= in a logind seat state file, or -1. */
int mxguest_clip_active_uid(const char *path, uid_t *uid);
int mxguest_clip_peer_allowed(const char *seat_path, uid_t uid);

#endif
