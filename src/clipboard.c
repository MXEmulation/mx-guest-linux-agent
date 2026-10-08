/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#define _GNU_SOURCE
#include "clipboard.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

static int has_nul(const uint8_t *text, uint32_t len)
{
    return len && memchr(text, 0, len) != NULL;
}

static int same_text(const uint8_t *a, uint32_t a_len, const uint8_t *b, uint32_t b_len)
{
    return a_len == b_len && (a_len == 0 || memcmp(a, b, a_len) == 0);
}

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

uint64_t mxguest_clip_random_origin(void)
{
    for (;;) {
        uint64_t value = 0;
        ssize_t n = getrandom(&value, sizeof value, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n != (ssize_t)sizeof value)
            return 0;
        if (value)
            return value;
    }
}

uint64_t mxguest_clip_caps(uint64_t base, int session_connected)
{
    if (!session_connected)
        return base & ~(MXGA_CAP_CLIPBOARD_READ | MXGA_CAP_CLIPBOARD_WRITE);
    return base | MXGA_CAP_CLIPBOARD_READ | MXGA_CAP_CLIPBOARD_WRITE;
}

int mxguest_clip_init(struct mxguest_clip *clip, uint64_t origin)
{
    memset(clip, 0, sizeof *clip);
    if (!origin)
        return -1;
    clip->host_text = malloc(MXGA_CLIPBOARD_MAX_TEXT_BYTES);
    if (!clip->host_text)
        return -1;
    clip->origin = origin;
    return 0;
}

void mxguest_clip_free(struct mxguest_clip *clip)
{
    free(clip->host_text);
    memset(clip, 0, sizeof *clip);
}

int mxguest_clip_build_changed(struct mxguest_clip *clip, const uint8_t *text, uint32_t len,
                               uint8_t *payload, uint32_t cap, uint32_t *payload_len)
{
    if (has_nul(text, len))
        return 0;
    if (clip->echo && same_text(text, len, clip->host_text, clip->host_len))
        return 0;
    if (mxga_encode_clipboard(clip->origin, clip->generation + 1, text, len, payload, cap,
                              payload_len) != MXGA_OK)
        return -1;
    clip->generation++;
    clip->echo = 0;
    clip->have_host = 0;
    return 1;
}

int mxguest_clip_host_write(struct mxguest_clip *clip, const uint8_t *payload, uint32_t len,
                            const uint8_t **text, uint32_t *text_len)
{
    struct mxga_clipboard decoded;
    if (mxga_decode_clipboard(payload, len, &decoded) != MXGA_OK)
        return -1;
    if (has_nul(decoded.text, decoded.text_bytes))
        return 0;
    if (decoded.text_bytes)
        memcpy(clip->host_text, decoded.text, decoded.text_bytes);
    clip->host_len = decoded.text_bytes;
    clip->have_host = 1;
    clip->echo = 1;
    *text = clip->host_text;
    *text_len = clip->host_len;
    return 1;
}

int mxguest_clip_in_init(struct mxguest_clip_in *in)
{
    memset(in, 0, sizeof *in);
    in->buf = malloc(MXGUEST_CLIP_LOCAL_FRAME_MAX);
    return in->buf ? 0 : -1;
}

void mxguest_clip_in_free(struct mxguest_clip_in *in)
{
    free(in->buf);
    memset(in, 0, sizeof *in);
}

static void in_release(struct mxguest_clip_in *in)
{
    if (!in->msg)
        return;
    memmove(in->buf, in->buf + in->msg, in->have - in->msg);
    in->have -= in->msg;
    in->msg = 0;
}

size_t mxguest_clip_in_feed(struct mxguest_clip_in *in, const uint8_t *data, size_t len)
{
    size_t room;
    in_release(in);
    room = MXGUEST_CLIP_LOCAL_FRAME_MAX - in->have;
    if (len > room)
        len = room;
    if (len)
        memcpy(in->buf + in->have, data, len);
    in->have += len;
    return len;
}

int mxguest_clip_in_next(struct mxguest_clip_in *in, const uint8_t **text, uint32_t *len)
{
    uint32_t size;
    in_release(in);
    if (in->have < 4)
        return 0;
    size = (uint32_t)in->buf[0] | ((uint32_t)in->buf[1] << 8) | ((uint32_t)in->buf[2] << 16) |
           ((uint32_t)in->buf[3] << 24);
    if (size > MXGA_CLIPBOARD_MAX_TEXT_BYTES)
        return -1;
    if (in->have - 4 < size)
        return 0;
    if (!utf8_valid(in->buf + 4, size))
        return -1;
    *text = in->buf + 4;
    *len = size;
    in->msg = 4 + (size_t)size;
    return 1;
}

int mxguest_clip_out_init(struct mxguest_clip_out *out)
{
    memset(out, 0, sizeof *out);
    out->buf = malloc(MXGUEST_CLIP_OUT_CAP);
    return out->buf ? 0 : -1;
}

void mxguest_clip_out_free(struct mxguest_clip_out *out)
{
    free(out->buf);
    memset(out, 0, sizeof *out);
}

int mxguest_clip_out_push(struct mxguest_clip_out *out, const uint8_t *text, uint32_t len)
{
    size_t pending = out->len - out->off;
    if (len > MXGA_CLIPBOARD_MAX_TEXT_BYTES || pending + 4 + len > MXGUEST_CLIP_OUT_CAP)
        return -1;
    if (out->off) {
        memmove(out->buf, out->buf + out->off, pending);
        out->len = pending;
        out->off = 0;
    }
    out->buf[out->len] = (uint8_t)len;
    out->buf[out->len + 1] = (uint8_t)(len >> 8);
    out->buf[out->len + 2] = (uint8_t)(len >> 16);
    out->buf[out->len + 3] = (uint8_t)(len >> 24);
    if (len)
        memcpy(out->buf + out->len + 4, text, len);
    out->len += 4 + (size_t)len;
    return 0;
}

const uint8_t *mxguest_clip_out_pending(const struct mxguest_clip_out *out, size_t *len)
{
    *len = out->len - out->off;
    return out->buf + out->off;
}

void mxguest_clip_out_advance(struct mxguest_clip_out *out, size_t len)
{
    out->off += len;
    if (out->off >= out->len)
        out->off = out->len = 0;
}

int mxguest_clip_active_uid(const char *path, uid_t *uid)
{
    static const char key[] = "ACTIVE_UID=";
    FILE *in = fopen(path, "r");
    char line[256];
    int found = -1;
    if (!in)
        return -1;
    while (fgets(line, sizeof line, in)) {
        size_t len = strlen(line);
        int complete = len && line[len - 1] == '\n';
        if (!complete && !feof(in)) {
            int c;
            while ((c = getc(in)) != EOF && c != '\n')
                ;
            continue;
        }
        if (strncmp(line, key, sizeof key - 1) == 0) {
            char *end = NULL;
            unsigned long value;
            errno = 0;
            value = strtoul(line + sizeof key - 1, &end, 10);
            if (errno == 0 && end != line + sizeof key - 1 && (*end == '\n' || *end == '\0') &&
                (uid_t)value == value) {
                *uid = (uid_t)value;
                found = 0;
            }
            break;
        }
    }
    fclose(in);
    return found;
}

int mxguest_clip_peer_allowed(const char *seat_path, uid_t uid)
{
    uid_t active;
    if (uid == 0)
        return 1;
    return mxguest_clip_active_uid(seat_path, &active) == 0 && active == uid;
}
