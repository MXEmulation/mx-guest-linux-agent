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

static void feed_all(struct mxguest_clip_in *in, const uint8_t *data, size_t len)
{
    assert(mxguest_clip_in_feed(in, data, len) == len);
}

static void test_local_parser(void)
{
    struct mxguest_clip_in in;
    const uint8_t *text = NULL;
    uint32_t len = 0;
    const uint8_t two[] = {3, 0, 0, 0, 'a', 'b', 'c', 0, 0, 0, 0};
    const uint8_t bad[] = {2, 0, 0, 0, 0xc3, 0x28};
    const uint8_t huge[] = {0xff, 0xff, 0xff, 0x7f};
    const uint32_t limit = MXGA_CLIPBOARD_MAX_TEXT_BYTES + 1;
    const uint8_t over[] = {(uint8_t)limit, (uint8_t)(limit >> 8), (uint8_t)(limit >> 16),
                            (uint8_t)(limit >> 24)};
    size_t i;
    assert(mxguest_clip_in_init(&in) == 0);
    for (i = 0; i < 6; i++) {
        feed_all(&in, two + i, 1);
        assert(mxguest_clip_in_next(&in, &text, &len) == 0);
    }
    feed_all(&in, two + 6, 1);
    assert(mxguest_clip_in_next(&in, &text, &len) == 1 && len == 3 && memcmp(text, "abc", 3) == 0);
    for (i = 7; i < 10; i++) {
        feed_all(&in, two + i, 1);
        assert(mxguest_clip_in_next(&in, &text, &len) == 0);
    }
    feed_all(&in, two + 10, 1);
    assert(mxguest_clip_in_next(&in, &text, &len) == 1 && len == 0);
    mxguest_clip_in_free(&in);

    assert(mxguest_clip_in_init(&in) == 0);
    feed_all(&in, two, sizeof two);
    assert(mxguest_clip_in_next(&in, &text, &len) == 1 && len == 3 && memcmp(text, "abc", 3) == 0);
    assert(mxguest_clip_in_next(&in, &text, &len) == 1 && len == 0);
    assert(mxguest_clip_in_next(&in, &text, &len) == 0);
    feed_all(&in, bad, sizeof bad);
    assert(mxguest_clip_in_next(&in, &text, &len) == -1);
    mxguest_clip_in_free(&in);

    assert(mxguest_clip_in_init(&in) == 0);
    feed_all(&in, huge, sizeof huge);
    assert(mxguest_clip_in_next(&in, &text, &len) == -1);
    mxguest_clip_in_free(&in);

    assert(mxguest_clip_in_init(&in) == 0);
    feed_all(&in, over, sizeof over);
    assert(mxguest_clip_in_next(&in, &text, &len) == -1);
    mxguest_clip_in_free(&in);
}

static void test_local_maximum(void)
{
    struct mxguest_clip_in in;
    struct mxguest_clip_out out;
    uint8_t *text = malloc(MXGA_CLIPBOARD_MAX_TEXT_BYTES);
    const uint8_t *got = NULL;
    const uint8_t *pending;
    size_t pending_len, taken = 0;
    uint32_t len = 0;
    int state = 0;
    assert(text);
    memset(text, 'x', MXGA_CLIPBOARD_MAX_TEXT_BYTES);
    assert(mxguest_clip_out_init(&out) == 0 && mxguest_clip_in_init(&in) == 0);
    assert(mxguest_clip_out_push(&out, text, MXGA_CLIPBOARD_MAX_TEXT_BYTES) == 0);
    assert(mxguest_clip_out_push(&out, text, MXGA_CLIPBOARD_MAX_TEXT_BYTES) == 0);
    assert(mxguest_clip_out_push(&out, text, 1) == -1);
    assert(mxguest_clip_out_push(&out, text, MXGA_CLIPBOARD_MAX_TEXT_BYTES + 1) == -1);
    pending = mxguest_clip_out_pending(&out, &pending_len);
    assert(pending_len == 2u * MXGUEST_CLIP_LOCAL_FRAME_MAX);
    while (pending_len) {
        size_t chunk = pending_len > 70000 ? 70000 : pending_len;
        size_t used = 0;
        while (used < chunk) {
            size_t n = mxguest_clip_in_feed(&in, pending + used, chunk - used);
            used += n;
            while ((state = mxguest_clip_in_next(&in, &got, &len)) == 1) {
                assert(len == MXGA_CLIPBOARD_MAX_TEXT_BYTES && got[len - 1] == 'x');
                taken++;
            }
            assert(state == 0 && n > 0);
        }
        mxguest_clip_out_advance(&out, chunk);
        pending = mxguest_clip_out_pending(&out, &pending_len);
    }
    assert(taken == 2 && pending_len == 0);
    assert(mxguest_clip_out_push(&out, (const uint8_t *)"hi", 2) == 0);
    pending = mxguest_clip_out_pending(&out, &pending_len);
    assert(pending_len == 6 && pending[0] == 2 && pending[1] == 0 && pending[4] == 'h');
    mxguest_clip_out_advance(&out, 4);
    assert(mxguest_clip_out_push(&out, (const uint8_t *)"j", 1) == 0);
    pending = mxguest_clip_out_pending(&out, &pending_len);
    assert(pending_len == 7 && pending[0] == 'h' && pending[2] == 1 && pending[6] == 'j');
    mxguest_clip_out_free(&out);
    mxguest_clip_in_free(&in);
    free(text);
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
    test_local_parser();
    test_local_maximum();
    test_active_uid();
    test_capabilities();
    puts("clipboard logic ok");
    return 0;
}
