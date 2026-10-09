/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#define _GNU_SOURCE
#include "integration.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SEGMENT_CAP (MXGA_INTEGRATION_MAX_WINDOWS * MXGA_INTEGRATION_MAX_SEGMENTS)

int mxguest_integration_init(struct mxguest_integration *state)
{
    memset(state, 0, sizeof *state);
    state->inventory = malloc(MXGUEST_SESSION_PAYLOAD_MAX);
    state->windows = calloc(MXGA_INTEGRATION_MAX_WINDOWS, sizeof *state->windows);
    state->segments = calloc(SEGMENT_CAP, sizeof *state->segments);
    if (state->inventory && state->windows && state->segments)
        return 0;
    mxguest_integration_free(state);
    return -1;
}

void mxguest_integration_free(struct mxguest_integration *state)
{
    free(state->inventory);
    free(state->windows);
    free(state->segments);
    memset(state, 0, sizeof *state);
}

void mxguest_integration_reset(struct mxguest_integration *state)
{
    state->inventory_len = 0;
    state->have = 0;
    state->sent = 0;
    state->sent_flags = 0;
}

uint64_t mxguest_integration_caps(uint64_t base, int session_connected)
{
    if (!session_connected)
        return base & ~MXGA_CAP_INTEGRATION;
    return base | MXGA_CAP_INTEGRATION;
}

static int read_id(const char *devices_dir, const char *entry, const char *name, unsigned long *id)
{
    char path[PATH_MAX];
    char line[32];
    char *end = NULL;
    FILE *in;
    int ok = 0;
    if (snprintf(path, sizeof path, "%s/%s/%s", devices_dir, entry, name) >= (int)sizeof path)
        return 0;
    in = fopen(path, "r");
    if (!in)
        return 0;
    if (fgets(line, sizeof line, in)) {
        *id = strtoul(line, &end, 16);
        ok = end != line && (*end == '\n' || *end == '\0');
    }
    fclose(in);
    return ok;
}

static int driver_bound(const char *devices_dir, const char *entry)
{
    char path[PATH_MAX];
    char target[PATH_MAX];
    const char *name;
    ssize_t n;
    if (snprintf(path, sizeof path, "%s/%s/driver", devices_dir, entry) >= (int)sizeof path)
        return 0;
    n = readlink(path, target, sizeof target - 1);
    if (n <= 0)
        return 0;
    target[n] = '\0';
    name = strrchr(target, '/');
    name = name ? name + 1 : target;
    return strcmp(name, MXGUEST_INTEGRATION_DRIVER_NAME) == 0;
}

uint16_t mxguest_integration_system_flags(const char *devices_dir)
{
    DIR *dir = opendir(devices_dir);
    struct dirent *ent;
    uint16_t flags = 0;
    if (!dir)
        return 0;
    while ((ent = readdir(dir)) != NULL) {
        unsigned long vendor = 0, device = 0;
        if (ent->d_name[0] == '.')
            continue;
        if (!read_id(devices_dir, ent->d_name, "vendor", &vendor) ||
            !read_id(devices_dir, ent->d_name, "device", &device))
            continue;
        if (vendor != MX_PCI_VENDOR_ID || device != MXGPU_PCI_DEVICE_ID)
            continue;
        flags |= MXGA_INTEGRATION_MXGPU_PRESENT;
        if (driver_bound(devices_dir, ent->d_name))
            flags |= MXGA_INTEGRATION_MXGPU_DRIVER_READY;
    }
    closedir(dir);
    return flags;
}

static int decode(const struct mxguest_integration *state, const uint8_t *payload, uint32_t len,
                  struct mxga_integration_status *status)
{
    return mxga_decode_integration_status(payload, len, status, state->windows,
                                          MXGA_INTEGRATION_MAX_WINDOWS, state->segments,
                                          SEGMENT_CAP) == MXGA_OK
               ? 0
               : -1;
}

int mxguest_integration_accept(struct mxguest_integration *state, const uint8_t *payload,
                               uint32_t len)
{
    struct mxga_integration_status status;
    if (!payload || len > MXGUEST_SESSION_PAYLOAD_MAX || decode(state, payload, len, &status) != 0)
        return -1;
    if (status.flags & ~MXGUEST_INTEGRATION_BRIDGE_FLAGS)
        return -1;
    memcpy(state->inventory, payload, len);
    state->inventory_len = len;
    state->have = 1;
    state->sent = 0;
    return 0;
}

int mxguest_integration_build(const struct mxguest_integration *state, uint16_t system_flags,
                              uint8_t *out, uint32_t cap, uint32_t *out_len)
{
    struct mxga_integration_status status;
    if (!state->have)
        return 0;
    if (decode(state, state->inventory, state->inventory_len, &status) != 0)
        return -1;
    status.flags = (uint16_t)((status.flags & MXGUEST_INTEGRATION_BRIDGE_FLAGS) |
                              (system_flags & MXGUEST_INTEGRATION_SYSTEM_FLAGS));
    return mxga_encode_integration_status(&status, out, cap, out_len) == MXGA_OK ? 1 : -1;
}

int mxguest_integration_due(const struct mxguest_integration *state, uint16_t system_flags)
{
    return state->have &&
           (!state->sent || state->sent_flags != (system_flags & MXGUEST_INTEGRATION_SYSTEM_FLAGS));
}

void mxguest_integration_mark_sent(struct mxguest_integration *state, uint16_t system_flags)
{
    state->sent = 1;
    state->sent_flags = system_flags & MXGUEST_INTEGRATION_SYSTEM_FLAGS;
}

int mxguest_integration_forward(const struct mxguest_integration *state,
                                struct mxguest_session_out *out, const uint8_t *payload,
                                uint32_t len)
{
    struct mxga_integration_action action;
    struct mxga_integration_status status;
    uint32_t i;
    int current = 0;
    if (mxga_decode_integration_action(payload, len, &action) != MXGA_OK)
        return -1;
    if (!state->have || !state->sent || decode(state, state->inventory, state->inventory_len, &status))
        return 0;
    for (i = 0; i < status.window_count && action.generation == status.generation; i++)
        current |= status.windows[i].window_id == action.window_id;
    if (!current)
        return 0;
    return mxguest_session_out_push(out, MXGUEST_SESSION_ACTION, payload, len) == 0 ? 1 : -2;
}
