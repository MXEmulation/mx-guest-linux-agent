/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#define _GNU_SOURCE
#include "integration.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TITLE_AT (MXGA_INTEGRATION_STATUS_HEADER_BYTES + MXGA_INTEGRATION_WINDOW_BYTES)
#define SEGMENT_CAP (MXGA_INTEGRATION_MAX_WINDOWS * MXGA_INTEGRATION_MAX_SEGMENTS)

static uint8_t g_inventory[MXGUEST_SESSION_PAYLOAD_MAX];
static uint8_t g_status[MXGUEST_SESSION_PAYLOAD_MAX];
static struct mxga_integrated_window g_windows[MXGA_INTEGRATION_MAX_WINDOWS];
static struct mxga_integration_segment g_segments[MXGA_INTEGRATION_MAX_WINDOWS];
static struct mxga_integrated_window g_decoded[MXGA_INTEGRATION_MAX_WINDOWS];
static struct mxga_integration_segment g_decoded_segments[SEGMENT_CAP];

static const uint8_t g_icon[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

static uint32_t inventory(uint16_t flags, uint64_t generation, uint32_t count)
{
    struct mxga_integration_status status = {
        .generation = generation, .flags = flags, .window_count = count, .windows = g_windows};
    uint32_t len = 0, i;
    for (i = 0; i < count; i++) {
        g_segments[i] = (struct mxga_integration_segment){
            .scanout_id = 0, .x = 10, .y = 20, .width = 300, .height = 200,
            .destination_x = 0, .destination_y = 0, .destination_width = 300,
            .destination_height = 200};
        g_windows[i] = (struct mxga_integrated_window){
            .window_id = i + 1, .x = 10, .y = 20, .width = 300, .height = 200,
            .flags = MXGA_INTEGRATION_WINDOW_GEOMETRY_RELIABLE,
            .title = (const uint8_t *)"Term", .application_id = (const uint8_t *)"app.id",
            .icon_bgra = g_icon, .title_bytes = 4, .application_id_bytes = 6, .icon_width = 2,
            .icon_height = 2, .icon_bgra_bytes = 16, .scanout_id = 0, .segment_count = 1,
            .output_width = 300, .output_height = 200, .segments = &g_segments[i]};
    }
    assert(mxga_encode_integration_status(&status, g_inventory, sizeof g_inventory, &len) ==
           MXGA_OK);
    return len;
}

static void put16(uint8_t *at, uint16_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *at, uint32_t value)
{
    put16(at, (uint16_t)value);
    put16(at + 2, (uint16_t)(value >> 16));
}

static void expect_stored(const struct mxguest_integration *state, uint16_t system,
                          uint64_t generation)
{
    struct mxga_integration_status got;
    uint32_t len = 0;
    assert(mxguest_integration_build(state, system, g_status, sizeof g_status, &len) == 1);
    assert(mxga_decode_integration_status(g_status, len, &got, g_decoded,
                                          MXGA_INTEGRATION_MAX_WINDOWS, g_decoded_segments,
                                          SEGMENT_CAP) == MXGA_OK);
    assert(got.generation == generation);
}

static void test_status_flags(void)
{
    struct mxguest_integration state;
    struct mxga_integration_status got;
    uint32_t len = inventory(MXGUEST_INTEGRATION_BRIDGE_FLAGS, 7, 2), built = 0;
    assert(mxguest_integration_init(&state) == 0);
    assert(mxguest_integration_build(&state, 3, g_status, sizeof g_status, &built) == 0);
    assert(!mxguest_integration_due(&state, 3));
    assert(mxguest_integration_accept(&state, g_inventory, len) == 0);
    assert(mxguest_integration_due(&state, 3));
    assert(mxguest_integration_build(&state, MXGUEST_INTEGRATION_SYSTEM_FLAGS, g_status,
                                     sizeof g_status, &built) == 1);
    assert(mxga_decode_integration_status(g_status, built, &got, g_decoded,
                                          MXGA_INTEGRATION_MAX_WINDOWS, g_decoded_segments,
                                          SEGMENT_CAP) == MXGA_OK);
    assert(got.generation == 7 && got.window_count == 2);
    assert(got.flags == MXGA_INTEGRATION_REQUIRED_FLAGS);
    assert(g_decoded[1].window_id == 2 && g_decoded[1].title_bytes == 4 &&
           memcmp(g_decoded[1].title, "Term", 4) == 0);
    assert(g_decoded[0].icon_bgra_bytes == 16 && memcmp(g_decoded[0].icon_bgra, g_icon, 16) == 0);
    assert(g_decoded[0].segment_count == 1 && g_decoded[0].output_width == 300);
    assert(mxguest_integration_build(&state, MXGA_INTEGRATION_MXGPU_PRESENT, g_status,
                                     sizeof g_status, &built) == 1);
    assert(mxga_decode_integration_status(g_status, built, &got, g_decoded,
                                          MXGA_INTEGRATION_MAX_WINDOWS, g_decoded_segments,
                                          SEGMENT_CAP) == MXGA_OK);
    assert(got.flags == (MXGUEST_INTEGRATION_BRIDGE_FLAGS | MXGA_INTEGRATION_MXGPU_PRESENT));
    assert(mxguest_integration_build(&state, 0xffff, g_status, sizeof g_status, &built) == 1);
    assert(mxga_decode_integration_status(g_status, built, &got, g_decoded,
                                          MXGA_INTEGRATION_MAX_WINDOWS, g_decoded_segments,
                                          SEGMENT_CAP) == MXGA_OK);
    assert(got.flags == MXGA_INTEGRATION_REQUIRED_FLAGS);
    assert(mxguest_integration_build(&state, 3, g_status, 20, &built) == -1);

    mxguest_integration_mark_sent(&state, 3);
    assert(!mxguest_integration_due(&state, 3));
    assert(mxguest_integration_due(&state, 1));
    assert(mxguest_integration_due(&state, 0));
    assert(mxguest_integration_accept(&state, g_inventory, len) == 0);
    assert(mxguest_integration_due(&state, 3));
    mxguest_integration_mark_sent(&state, 3);
    mxguest_integration_reset(&state);
    assert(!mxguest_integration_due(&state, 3));
    assert(mxguest_integration_build(&state, 3, g_status, sizeof g_status, &built) == 0);
    mxguest_integration_free(&state);
}

static void test_inventory_validation(void)
{
    struct mxguest_integration state;
    uint8_t *big = calloc(1, MXGUEST_SESSION_PAYLOAD_MAX + 1u);
    uint32_t len = inventory(MXGUEST_INTEGRATION_BRIDGE_FLAGS, 9, 1);
    static uint8_t good[sizeof g_inventory];
    assert(big);
    memcpy(good, g_inventory, len);
    assert(mxguest_integration_init(&state) == 0);
    assert(mxguest_integration_accept(&state, good, len) == 0);

    assert(mxguest_integration_accept(&state, good, len - 1) == -1);
    assert(mxguest_integration_accept(&state, good, MXGA_INTEGRATION_STATUS_HEADER_BYTES - 1) == -1);
    assert(mxguest_integration_accept(&state, good, 0) == -1);
    assert(mxguest_integration_accept(&state, NULL, 0) == -1);
    assert(mxguest_integration_accept(&state, big, MXGUEST_SESSION_PAYLOAD_MAX + 1u) == -1);

    memcpy(g_inventory, good, len);
    g_inventory[TITLE_AT] = 0xff;
    assert(mxguest_integration_accept(&state, g_inventory, len) == -1);

    memcpy(g_inventory, good, len);
    put32(g_inventory + 12, MXGA_INTEGRATION_MAX_WINDOWS + 1u);
    assert(mxguest_integration_accept(&state, g_inventory, len) == -1);

    memcpy(g_inventory, good, len);
    put16(g_inventory + MXGA_INTEGRATION_STATUS_HEADER_BYTES + 42,
          MXGA_INTEGRATION_MAX_SEGMENTS + 1u);
    assert(mxguest_integration_accept(&state, g_inventory, len) == -1);

    memcpy(g_inventory, good, len);
    put16(g_inventory, 3);
    assert(mxguest_integration_accept(&state, g_inventory, len) == -1);

    memcpy(g_inventory, good, len);
    memset(g_inventory + 4, 0, 8);
    assert(mxguest_integration_accept(&state, g_inventory, len) == -1);

    memcpy(g_inventory, good, len);
    put16(g_inventory + MXGA_INTEGRATION_STATUS_HEADER_BYTES + 32, 3);
    assert(mxguest_integration_accept(&state, g_inventory, len) == -1);

    memcpy(g_inventory, good, len);
    put16(g_inventory + 2, 1u << 5);
    assert(mxguest_integration_accept(&state, g_inventory, len) == -1);

    memcpy(g_inventory, good, len);
    memcpy(g_inventory + len, good, 1);
    assert(mxguest_integration_accept(&state, g_inventory, len + 1) == -1);

    expect_stored(&state, 3, 9);
    mxguest_integration_free(&state);

    len = inventory(MXGA_INTEGRATION_MXGPU_PRESENT, 9, 1);
    assert(mxguest_integration_init(&state) == 0);
    assert(mxguest_integration_accept(&state, g_inventory, len) == -1);
    len = inventory(MXGUEST_INTEGRATION_BRIDGE_FLAGS, 1, 0);
    assert(mxguest_integration_accept(&state, g_inventory, len) == 0);
    len = inventory(MXGUEST_INTEGRATION_BRIDGE_FLAGS, 2, MXGA_INTEGRATION_MAX_WINDOWS);
    assert(mxguest_integration_accept(&state, g_inventory, len) == 0);
    expect_stored(&state, 3, 2);
    mxguest_integration_free(&state);
    free(big);
}

static void action_payload(uint8_t *out, uint16_t action, uint64_t generation, uint64_t window)
{
    struct mxga_integration_action value = {action, generation, window};
    uint32_t len = 0;
    assert(mxga_encode_integration_action(&value, out, MXGA_INTEGRATION_ACTION_BYTES, &len) ==
           MXGA_OK);
    assert(len == MXGA_INTEGRATION_ACTION_BYTES);
}

static void test_action_forwarding(void)
{
    struct mxguest_integration state;
    struct mxguest_session_out out;
    uint8_t action[MXGA_INTEGRATION_ACTION_BYTES], bad[MXGA_INTEGRATION_ACTION_BYTES];
    const uint8_t *pending;
    size_t pending_len;
    uint32_t len = inventory(MXGUEST_INTEGRATION_BRIDGE_FLAGS, 5, 2);
    uint8_t *text = calloc(1, MXGA_CLIPBOARD_MAX_TEXT_BYTES);
    assert(text);
    assert(mxguest_integration_init(&state) == 0 && mxguest_session_out_init(&out) == 0);
    action_payload(action, MXGA_INTEGRATION_ACTION_CLOSE, 5, 2);
    assert(mxguest_integration_forward(&state, &out, action, sizeof action) == 0);
    assert(mxguest_integration_accept(&state, g_inventory, len) == 0);
    assert(mxguest_integration_forward(&state, &out, action, sizeof action) == 0);
    mxguest_integration_mark_sent(&state, 3);
    mxguest_session_out_pending(&out, &pending_len);
    assert(pending_len == 0);

    assert(mxguest_integration_forward(&state, &out, action, sizeof action) == 1);
    pending = mxguest_session_out_pending(&out, &pending_len);
    assert(pending_len == 4u + 1u + MXGA_INTEGRATION_ACTION_BYTES);
    assert(pending[0] == 1 + MXGA_INTEGRATION_ACTION_BYTES && pending[4] == MXGUEST_SESSION_ACTION);
    assert(memcmp(pending + 5, action, sizeof action) == 0);
    mxguest_session_out_advance(&out, pending_len);

    action_payload(bad, MXGA_INTEGRATION_ACTION_ACTIVATE, 5, 1);
    assert(mxguest_integration_forward(&state, &out, bad, sizeof bad) == 1);
    mxguest_session_out_advance(&out, 4u + 1u + MXGA_INTEGRATION_ACTION_BYTES);
    action_payload(bad, MXGA_INTEGRATION_ACTION_ACTIVATE, 4, 1);
    assert(mxguest_integration_forward(&state, &out, bad, sizeof bad) == 0);
    action_payload(bad, MXGA_INTEGRATION_ACTION_ACTIVATE, 5, 3);
    assert(mxguest_integration_forward(&state, &out, bad, sizeof bad) == 0);
    mxguest_session_out_pending(&out, &pending_len);
    assert(pending_len == 0);

    assert(mxguest_integration_forward(&state, &out, action, sizeof action - 1) == -1);
    assert(mxguest_integration_forward(&state, &out, NULL, 0) == -1);
    memcpy(bad, action, sizeof bad);
    bad[2] = 3;
    assert(mxguest_integration_forward(&state, &out, bad, sizeof bad) == -1);
    memcpy(bad, action, sizeof bad);
    bad[0] = 2;
    assert(mxguest_integration_forward(&state, &out, bad, sizeof bad) == -1);
    memcpy(bad, action, sizeof bad);
    memset(bad + 12, 0, 8);
    assert(mxguest_integration_forward(&state, &out, bad, sizeof bad) == -1);

    assert(mxguest_session_out_push(&out, MXGUEST_SESSION_CLIPBOARD, text,
                                    MXGA_CLIPBOARD_MAX_TEXT_BYTES) == 0);
    assert(mxguest_session_out_push(&out, MXGUEST_SESSION_CLIPBOARD, text,
                                    MXGA_CLIPBOARD_MAX_TEXT_BYTES) == 0);
    assert(mxguest_session_out_push(&out, MXGUEST_SESSION_CLIPBOARD, text, 5) == 0);
    assert(mxguest_integration_forward(&state, &out, action, sizeof action) == -2);

    mxguest_integration_reset(&state);
    assert(mxguest_integration_forward(&state, &out, action, sizeof action) == 0);
    mxguest_session_out_free(&out);
    mxguest_integration_free(&state);
    free(text);
}

static void write_file(const char *dir, const char *entry, const char *name, const char *content)
{
    char path[512];
    FILE *out;
    snprintf(path, sizeof path, "%s/%s/%s", dir, entry, name);
    out = fopen(path, "w");
    assert(out && fputs(content, out) >= 0);
    assert(fclose(out) == 0);
}

static void make_device(const char *dir, const char *entry, const char *vendor, const char *device,
                        const char *driver)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, entry);
    assert(mkdir(path, 0755) == 0);
    write_file(dir, entry, "vendor", vendor);
    write_file(dir, entry, "device", device);
    if (driver) {
        char target[256];
        snprintf(target, sizeof target, "../../../bus/pci/drivers/%s", driver);
        snprintf(path, sizeof path, "%s/%s/driver", dir, entry);
        assert(symlink(target, path) == 0);
    }
}

static void remove_device(const char *dir, const char *entry)
{
    static const char *const names[] = {"vendor", "device", "driver"};
    char path[512];
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(path, sizeof path, "%s/%s/%s", dir, entry, names[i]);
        unlink(path);
    }
    snprintf(path, sizeof path, "%s/%s", dir, entry);
    assert(rmdir(path) == 0);
}

static void test_system_flags(void)
{
    char dir[] = "build/test-pci-XXXXXX";
    assert(mkdtemp(dir));
    assert(mxguest_integration_system_flags(dir) == 0);
    make_device(dir, "0000:00:02.0", "0x8086\n", "0x4750\n", "mxgpu");
    make_device(dir, "0000:00:03.0", "0x4d58\n", "0x0001\n", "mxgpu");
    assert(mxguest_integration_system_flags(dir) == 0);
    make_device(dir, "0000:00:04.0", "0x4d58\n", "0x4750\n", NULL);
    assert(mxguest_integration_system_flags(dir) == MXGA_INTEGRATION_MXGPU_PRESENT);
    remove_device(dir, "0000:00:04.0");
    make_device(dir, "0000:00:04.0", "0x4d58\n", "0x4750\n", "vfio-pci");
    assert(mxguest_integration_system_flags(dir) == MXGA_INTEGRATION_MXGPU_PRESENT);
    remove_device(dir, "0000:00:04.0");
    make_device(dir, "0000:00:04.0", "0x4d58\n", "0x4750\n", "mxgpu");
    assert(mxguest_integration_system_flags(dir) ==
           (MXGA_INTEGRATION_MXGPU_PRESENT | MXGA_INTEGRATION_MXGPU_DRIVER_READY));
    remove_device(dir, "0000:00:04.0");
    remove_device(dir, "0000:00:03.0");
    remove_device(dir, "0000:00:02.0");
    assert(rmdir(dir) == 0);
    assert(mxguest_integration_system_flags(dir) == 0);
}

static void test_capabilities(void)
{
    const uint64_t base = MXGA_CAP_SHUTDOWN | MXGA_CAP_RESTART | MXGA_CAP_SYSTEM_STATS;
    assert(mxguest_integration_caps(base, 0) == base);
    assert(mxguest_integration_caps(base, 1) == (base | MXGA_CAP_INTEGRATION));
    assert(mxguest_integration_caps(base | MXGA_CAP_INTEGRATION, 0) == base);
}

int main(void)
{
    test_status_flags();
    test_inventory_validation();
    test_action_forwarding();
    test_system_flags();
    test_capabilities();
    puts("integration logic ok");
    return 0;
}
