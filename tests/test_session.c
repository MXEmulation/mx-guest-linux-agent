/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#include "session.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t message(uint8_t *buf, uint32_t size, uint8_t type, const void *payload)
{
    buf[0] = (uint8_t)size;
    buf[1] = (uint8_t)(size >> 8);
    buf[2] = (uint8_t)(size >> 16);
    buf[3] = (uint8_t)(size >> 24);
    if (size) {
        buf[4] = type;
        if (size > 1 && payload != buf + 5)
            memcpy(buf + 5, payload, size - 1);
    }
    return 4 + (size_t)size;
}

static void feed_all(struct mxguest_session_in *in, const uint8_t *data, size_t len)
{
    assert(mxguest_session_in_feed(in, data, len) == len);
}

static void test_parser(void)
{
    struct mxguest_session_in in;
    const uint8_t *payload = NULL;
    uint8_t wire[64], type = 0;
    uint32_t len = 0;
    size_t used = 0, i;
    assert(mxguest_session_in_init(&in) == 0);
    used += message(wire + used, 4, MXGUEST_SESSION_CLIPBOARD, "abc");
    used += message(wire + used, 1, MXGUEST_SESSION_CLIPBOARD, NULL);
    used += message(wire + used, 3, MXGUEST_SESSION_WINDOWS, "\xff\xfe");
    assert(used == 20);
    for (i = 0; i < 7; i++) {
        feed_all(&in, wire + i, 1);
        assert(mxguest_session_in_next(&in, &type, &payload, &len) == 0);
    }
    feed_all(&in, wire + 7, 1);
    assert(mxguest_session_in_next(&in, &type, &payload, &len) == 1);
    assert(type == MXGUEST_SESSION_CLIPBOARD && len == 3 && memcmp(payload, "abc", 3) == 0);
    feed_all(&in, wire + 8, used - 8);
    assert(mxguest_session_in_next(&in, &type, &payload, &len) == 1);
    assert(type == MXGUEST_SESSION_CLIPBOARD && len == 0);
    assert(mxguest_session_in_next(&in, &type, &payload, &len) == 1);
    assert(type == MXGUEST_SESSION_WINDOWS && len == 2 && payload[0] == 0xff);
    assert(mxguest_session_in_next(&in, &type, &payload, &len) == 0);
    mxguest_session_in_free(&in);
}

static void expect_violation(const uint8_t *wire, size_t len)
{
    struct mxguest_session_in in;
    const uint8_t *payload = NULL;
    uint8_t type = 0;
    uint32_t size = 0;
    assert(mxguest_session_in_init(&in) == 0);
    feed_all(&in, wire, len);
    assert(mxguest_session_in_next(&in, &type, &payload, &size) == -1);
    mxguest_session_in_free(&in);
}

static void test_violations(void)
{
    uint8_t wire[64];
    const uint8_t huge[] = {0xff, 0xff, 0xff, 0x7f};
    const uint32_t big = 1u + MXGUEST_SESSION_PAYLOAD_MAX + 1u;
    const uint8_t over_all[4] = {(uint8_t)big, (uint8_t)(big >> 8), (uint8_t)(big >> 16),
                                 (uint8_t)(big >> 24)};
    uint8_t *text = malloc(MXGUEST_SESSION_FRAME_MAX);
    assert(text);
    expect_violation(wire, message(wire, 0, 0, NULL));
    expect_violation(wire, message(wire, 1, 0, NULL));
    expect_violation(wire, message(wire, 1, 9, NULL));
    expect_violation(wire, message(wire, 1, MXGUEST_SESSION_ACTION, NULL));
    expect_violation(wire, message(wire, 3, MXGUEST_SESSION_CLIPBOARD, "\xc3\x28"));
    expect_violation(huge, sizeof huge);
    expect_violation(over_all, sizeof over_all);
    memset(text, 'x', MXGUEST_SESSION_FRAME_MAX);
    message(text, 1u + MXGA_CLIPBOARD_MAX_TEXT_BYTES + 1u, MXGUEST_SESSION_CLIPBOARD,
            text + 5);
    expect_violation(text, 4u + 1u + MXGA_CLIPBOARD_MAX_TEXT_BYTES + 1u);
    free(text);
}

static void test_maximum(void)
{
    struct mxguest_session_in in;
    struct mxguest_session_out out;
    uint8_t *text = malloc(MXGA_CLIPBOARD_MAX_TEXT_BYTES);
    uint8_t *windows = malloc(MXGUEST_SESSION_FRAME_MAX);
    const uint8_t *got = NULL;
    const uint8_t *pending;
    uint8_t action[MXGA_INTEGRATION_ACTION_BYTES] = {1};
    uint8_t type = 0;
    size_t pending_len, taken = 0, used = 0;
    uint32_t len = 0;
    int state = 0;
    assert(text && windows);
    memset(text, 'x', MXGA_CLIPBOARD_MAX_TEXT_BYTES);
    assert(mxguest_session_out_init(&out) == 0 && mxguest_session_in_init(&in) == 0);
    assert(mxguest_session_out_push(&out, MXGUEST_SESSION_CLIPBOARD, text,
                                    MXGA_CLIPBOARD_MAX_TEXT_BYTES) == 0);
    assert(mxguest_session_out_push(&out, MXGUEST_SESSION_CLIPBOARD, text,
                                    MXGA_CLIPBOARD_MAX_TEXT_BYTES) == 0);
    assert(mxguest_session_out_push(&out, MXGUEST_SESSION_CLIPBOARD, text, 100) == -1);
    assert(mxguest_session_out_push(&out, MXGUEST_SESSION_CLIPBOARD, text,
                                    MXGA_CLIPBOARD_MAX_TEXT_BYTES + 1) == -1);
    assert(mxguest_session_out_push(&out, MXGUEST_SESSION_WINDOWS, text, 1) == -1);
    pending = mxguest_session_out_pending(&out, &pending_len);
    assert(pending_len == 2u * (5u + MXGA_CLIPBOARD_MAX_TEXT_BYTES));
    while (pending_len) {
        size_t chunk = pending_len > 70000 ? 70000 : pending_len;
        used = 0;
        while (used < chunk) {
            size_t n = mxguest_session_in_feed(&in, pending + used, chunk - used);
            used += n;
            while ((state = mxguest_session_in_next(&in, &type, &got, &len)) == 1) {
                assert(type == MXGUEST_SESSION_CLIPBOARD);
                assert(len == MXGA_CLIPBOARD_MAX_TEXT_BYTES && got[len - 1] == 'x');
                taken++;
            }
            assert(state == 0 && n > 0);
        }
        mxguest_session_out_advance(&out, chunk);
        pending = mxguest_session_out_pending(&out, &pending_len);
    }
    assert(taken == 2 && pending_len == 0);

    assert(mxguest_session_out_push(&out, MXGUEST_SESSION_CLIPBOARD, (const uint8_t *)"hi", 2) == 0);
    pending = mxguest_session_out_pending(&out, &pending_len);
    assert(pending_len == 7 && pending[0] == 3 && pending[1] == 0 && pending[4] == 1 &&
           pending[5] == 'h');
    mxguest_session_out_advance(&out, 5);
    assert(mxguest_session_out_push(&out, MXGUEST_SESSION_ACTION, action, sizeof action) == 0);
    pending = mxguest_session_out_pending(&out, &pending_len);
    assert(pending_len == 2u + 4u + 1u + MXGA_INTEGRATION_ACTION_BYTES && pending[0] == 'h');
    assert(pending[2] == 1 + MXGA_INTEGRATION_ACTION_BYTES && pending[6] == MXGUEST_SESSION_ACTION);
    assert(mxguest_session_out_push(&out, MXGUEST_SESSION_ACTION, action, 19) == -1);
    mxguest_session_out_free(&out);

    memset(windows + 5, 'w', MXGUEST_SESSION_PAYLOAD_MAX);
    message(windows, 1u + MXGUEST_SESSION_PAYLOAD_MAX, MXGUEST_SESSION_WINDOWS, windows + 5);
    used = 0;
    while (used < MXGUEST_SESSION_FRAME_MAX) {
        size_t want = MXGUEST_SESSION_FRAME_MAX - used;
        size_t n;
        if (want > 65536)
            want = 65536;
        n = mxguest_session_in_feed(&in, windows + used, want);
        assert(n == want);
        used += n;
        state = mxguest_session_in_next(&in, &type, &got, &len);
        assert(state == (used == MXGUEST_SESSION_FRAME_MAX ? 1 : 0));
    }
    assert(type == MXGUEST_SESSION_WINDOWS && len == MXGUEST_SESSION_PAYLOAD_MAX &&
           got[len - 1] == 'w');
    mxguest_session_in_free(&in);
    free(windows);
    free(text);
}

int main(void)
{
    test_parser();
    test_violations();
    test_maximum();
    puts("session framing ok");
    return 0;
}
