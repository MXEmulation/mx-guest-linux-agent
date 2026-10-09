/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#include "session.h"

#include <stdlib.h>
#include <string.h>

static int utf8_valid(const uint8_t *text, uint32_t len)
{
    uint32_t i = 0;
    while (i < len) {
        uint32_t scalar, minimum, more;
        uint8_t lead = text[i++];
        if (lead < 0x80u)
            continue;
        if (lead >= 0xc2u && lead <= 0xdfu) {
            scalar = lead & 0x1fu;
            minimum = 0x80u;
            more = 1;
        } else if (lead >= 0xe0u && lead <= 0xefu) {
            scalar = lead & 0x0fu;
            minimum = 0x800u;
            more = 2;
        } else if (lead >= 0xf0u && lead <= 0xf4u) {
            scalar = lead & 7u;
            minimum = 0x10000u;
            more = 3;
        } else {
            return 0;
        }
        if (more > len - i)
            return 0;
        while (more--) {
            uint8_t next = text[i++];
            if ((next & 0xc0u) != 0x80u)
                return 0;
            scalar = (scalar << 6) | (next & 0x3fu);
        }
        if (scalar < minimum || scalar > 0x10ffffu || (scalar >= 0xd800u && scalar <= 0xdfffu))
            return 0;
    }
    return 1;
}

int mxguest_session_in_init(struct mxguest_session_in *in)
{
    memset(in, 0, sizeof *in);
    in->buf = malloc(MXGUEST_SESSION_FRAME_MAX);
    return in->buf ? 0 : -1;
}

void mxguest_session_in_free(struct mxguest_session_in *in)
{
    free(in->buf);
    memset(in, 0, sizeof *in);
}

static void in_release(struct mxguest_session_in *in)
{
    if (!in->msg)
        return;
    memmove(in->buf, in->buf + in->msg, in->have - in->msg);
    in->have -= in->msg;
    in->msg = 0;
}

size_t mxguest_session_in_feed(struct mxguest_session_in *in, const uint8_t *data, size_t len)
{
    size_t room;
    in_release(in);
    room = MXGUEST_SESSION_FRAME_MAX - in->have;
    if (len > room)
        len = room;
    if (len)
        memcpy(in->buf + in->have, data, len);
    in->have += len;
    return len;
}

int mxguest_session_in_next(struct mxguest_session_in *in, uint8_t *type, const uint8_t **payload,
                            uint32_t *len)
{
    uint32_t size, body;
    uint8_t kind;
    in_release(in);
    if (in->have < 4)
        return 0;
    size = (uint32_t)in->buf[0] | ((uint32_t)in->buf[1] << 8) | ((uint32_t)in->buf[2] << 16) |
           ((uint32_t)in->buf[3] << 24);
    if (size == 0 || size > 1u + MXGUEST_SESSION_PAYLOAD_MAX)
        return -1;
    if (in->have - 4 < size)
        return 0;
    kind = in->buf[4];
    body = size - 1u;
    if (kind == MXGUEST_SESSION_CLIPBOARD) {
        if (body > MXGA_CLIPBOARD_MAX_TEXT_BYTES || !utf8_valid(in->buf + 5, body))
            return -1;
    } else if (kind != MXGUEST_SESSION_WINDOWS) {
        return -1;
    }
    *type = kind;
    *payload = in->buf + 5;
    *len = body;
    in->msg = 4 + (size_t)size;
    return 1;
}

int mxguest_session_out_init(struct mxguest_session_out *out)
{
    memset(out, 0, sizeof *out);
    out->buf = malloc(MXGUEST_SESSION_OUT_CAP);
    return out->buf ? 0 : -1;
}

void mxguest_session_out_free(struct mxguest_session_out *out)
{
    free(out->buf);
    memset(out, 0, sizeof *out);
}

int mxguest_session_out_push(struct mxguest_session_out *out, uint8_t type, const uint8_t *payload,
                             uint32_t len)
{
    size_t pending = out->len - out->off;
    uint32_t size = len + 1u;
    if (type == MXGUEST_SESSION_CLIPBOARD ? len > MXGA_CLIPBOARD_MAX_TEXT_BYTES
                                          : type != MXGUEST_SESSION_ACTION ||
                                                len != MXGA_INTEGRATION_ACTION_BYTES)
        return -1;
    if (pending + 4 + size > MXGUEST_SESSION_OUT_CAP)
        return -1;
    if (out->off) {
        memmove(out->buf, out->buf + out->off, pending);
        out->len = pending;
        out->off = 0;
    }
    out->buf[out->len] = (uint8_t)size;
    out->buf[out->len + 1] = (uint8_t)(size >> 8);
    out->buf[out->len + 2] = (uint8_t)(size >> 16);
    out->buf[out->len + 3] = (uint8_t)(size >> 24);
    out->buf[out->len + 4] = type;
    if (len)
        memcpy(out->buf + out->len + 5, payload, len);
    out->len += 4 + (size_t)size;
    return 0;
}

const uint8_t *mxguest_session_out_pending(const struct mxguest_session_out *out, size_t *len)
{
    *len = out->len - out->off;
    return out->buf + out->off;
}

void mxguest_session_out_advance(struct mxguest_session_out *out, size_t len)
{
    out->off += len;
    if (out->off >= out->len)
        out->off = out->len = 0;
}
