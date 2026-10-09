/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#define _GNU_SOURCE
#include "clipboard.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_changed_frames(void)
{
    struct mxguest_clip clip;
    struct mxga_clipboard decoded;
    uint8_t payload[64];
    uint32_t len = 0;
    const uint8_t text[] = "hello";
    assert(mxguest_clip_init(&clip, 0) != 0);
    assert(mxguest_clip_init(&clip, 77) == 0);
    assert(mxguest_clip_build_changed(&clip, text, 5, payload, sizeof payload, &len) == 1);
    assert(mxga_decode_clipboard(payload, len, &decoded) == MXGA_OK);
    assert(decoded.origin == 77 && decoded.generation == 1);
    assert(decoded.text_bytes == 5 && memcmp(decoded.text, "hello", 5) == 0);
    assert(mxguest_clip_build_changed(&clip, text, 5, payload, sizeof payload, &len) == 1);
    assert(mxga_decode_clipboard(payload, len, &decoded) == MXGA_OK && decoded.generation == 2);
    assert(mxguest_clip_build_changed(&clip, NULL, 0, payload, sizeof payload, &len) == 1);
    assert(mxga_decode_clipboard(payload, len, &decoded) == MXGA_OK);
    assert(decoded.generation == 3 && decoded.text_bytes == 0);
    assert(mxguest_clip_build_changed(&clip, (const uint8_t *)"a\0b", 3, payload, sizeof payload,
                                      &len) == 0);
    assert(mxguest_clip_build_changed(&clip, (const uint8_t *)"\xff", 1, payload, sizeof payload,
                                      &len) == -1);
    assert(clip.generation == 3);
    assert(mxguest_clip_random_origin() != 0);
    mxguest_clip_free(&clip);
}

static void host_payload(uint8_t *payload, uint32_t *len, const char *text, uint32_t n)
{
    assert(mxga_encode_clipboard(5, 9, (const uint8_t *)text, n, payload, 64, len) == MXGA_OK);
}

static void test_echo_suppression(void)
{
    struct mxguest_clip clip;
    uint8_t in[64], out[64];
    uint32_t in_len, out_len = 0, text_len = 0;
    const uint8_t *text = NULL;
    assert(mxguest_clip_init(&clip, 1) == 0);
    assert(!clip.have_host);
    host_payload(in, &in_len, "from host", 9);
    assert(mxguest_clip_host_write(&clip, in, in_len, &text, &text_len) == 1);
    assert(text_len == 9 && memcmp(text, "from host", 9) == 0 && clip.have_host);
    assert(mxguest_clip_build_changed(&clip, (const uint8_t *)"from host", 9, out, sizeof out,
                                      &out_len) == 0);
    assert(clip.generation == 0);
    assert(mxguest_clip_build_changed(&clip, (const uint8_t *)"other", 5, out, sizeof out,
                                      &out_len) == 1);
    assert(clip.generation == 1 && !clip.have_host);
    assert(mxguest_clip_build_changed(&clip, (const uint8_t *)"from host", 9, out, sizeof out,
                                      &out_len) == 1);
    assert(clip.generation == 2);
    host_payload(in, &in_len, "", 0);
    assert(mxguest_clip_host_write(&clip, in, in_len, &text, &text_len) == 1 && text_len == 0);
    assert(mxguest_clip_build_changed(&clip, NULL, 0, out, sizeof out, &out_len) == 0);
    host_payload(in, &in_len, "a\0b", 3);
    assert(mxguest_clip_host_write(&clip, in, in_len, &text, &text_len) == 0);
    assert(mxguest_clip_host_write(&clip, in, 3, &text, &text_len) == -1);
    assert(mxguest_clip_host_write(&clip, NULL, 0, &text, &text_len) == -1);
    mxguest_clip_free(&clip);
}

static void write_file(const char *path, const char *content)
{
    FILE *out = fopen(path, "w");
    assert(out && fputs(content, out) >= 0);
    assert(fclose(out) == 0);
}

static void test_active_uid(void)
{
    char path[] = "build/test-seat-XXXXXX";
    int fd = mkstemp(path);
    uid_t uid = 0;
    assert(fd >= 0);
    close(fd);
    write_file(path, "# comment\nIS_SEAT0=1\nACTIVE=1\nACTIVE_UID=1000\nSESSIONS=1\n");
    assert(mxguest_clip_active_uid(path, &uid) == 0 && uid == 1000);
    assert(mxguest_clip_peer_allowed(path, 0));
    assert(mxguest_clip_peer_allowed(path, 1000));
    assert(!mxguest_clip_peer_allowed(path, 1001));
    write_file(path, "ACTIVE_UID=1000");
    assert(mxguest_clip_active_uid(path, &uid) == 0 && uid == 1000);
    write_file(path, "ACTIVE_UID=abc\n");
    assert(mxguest_clip_active_uid(path, &uid) == -1);
    write_file(path, "ACTIVE_UID=\n");
    assert(mxguest_clip_active_uid(path, &uid) == -1);
    write_file(path, "SESSIONS=1\n");
    assert(mxguest_clip_active_uid(path, &uid) == -1);
    assert(!mxguest_clip_peer_allowed(path, 1000) && mxguest_clip_peer_allowed(path, 0));
    assert(unlink(path) == 0);
    assert(mxguest_clip_active_uid(path, &uid) == -1);
    assert(!mxguest_clip_peer_allowed(path, 1000));
}

static void test_capabilities(void)
{
    const uint64_t base = MXGA_CAP_SHUTDOWN | MXGA_CAP_RESTART | MXGA_CAP_SYSTEM_STATS;
    const uint64_t both = MXGA_CAP_CLIPBOARD_READ | MXGA_CAP_CLIPBOARD_WRITE;
    assert(mxguest_clip_caps(base, 0) == base);
    assert(mxguest_clip_caps(base, 1) == (base | both));
    assert(mxguest_clip_caps(base | both, 0) == base);
}

int main(void)
{
    test_changed_frames();
    test_echo_suppression();
    test_active_uid();
    test_capabilities();
    puts("clipboard logic ok");
    return 0;
}
