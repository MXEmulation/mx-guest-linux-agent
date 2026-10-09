/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#ifndef MXGUEST_INTEGRATION_H
#define MXGUEST_INTEGRATION_H

#include "mxga.h"
#include "session.h"

#include <stdint.h>

#define MXGUEST_INTEGRATION_BRIDGE_FLAGS                                                           \
    (MXGA_INTEGRATION_DESKTOP_BRIDGE_READY | MXGA_INTEGRATION_WINDOW_INVENTORY_READY |             \
     MXGA_INTEGRATION_ICON_RESOLUTION_READY)
#define MXGUEST_INTEGRATION_SYSTEM_FLAGS                                                           \
    (MXGA_INTEGRATION_MXGPU_PRESENT | MXGA_INTEGRATION_MXGPU_DRIVER_READY)
#define MXGUEST_INTEGRATION_DRIVER_NAME "mxgpu"

/* The bridge inventory is an MXGA integration status payload carrying only the bridge flags. */
struct mxguest_integration {
    uint8_t *inventory;
    uint32_t inventory_len;
    struct mxga_integrated_window *windows;
    struct mxga_integration_segment *segments;
    int have;
    int sent;
    uint16_t sent_flags;
};

int mxguest_integration_init(struct mxguest_integration *state);
void mxguest_integration_free(struct mxguest_integration *state);
void mxguest_integration_reset(struct mxguest_integration *state);

uint64_t mxguest_integration_caps(uint64_t base, int session_connected);
/* MXGPU_PRESENT and MXGPU_DRIVER_READY from a PCI devices directory such as /sys/bus/pci/devices. */
uint16_t mxguest_integration_system_flags(const char *devices_dir);

/* Validates and stores a bridge inventory. Returns 0, or -1 leaving the stored inventory unchanged. */
int mxguest_integration_accept(struct mxguest_integration *state, const uint8_t *payload,
                               uint32_t len);
/* Returns 1 with an INTEGRATION_STATUS payload built, 0 when there is no inventory, -1 on an encoding failure. */
int mxguest_integration_build(const struct mxguest_integration *state, uint16_t system_flags,
                              uint8_t *out, uint32_t cap, uint32_t *out_len);
/* Returns 1 when a status must be published for these system flags, 0 when the host is current. */
int mxguest_integration_due(const struct mxguest_integration *state, uint16_t system_flags);
void mxguest_integration_mark_sent(struct mxguest_integration *state, uint16_t system_flags);

/* Queues a host window action naming a published window: 1 queued, 0 not current, -1 malformed, -2 queue full. */
int mxguest_integration_forward(const struct mxguest_integration *state,
                                struct mxguest_session_out *out, const uint8_t *payload,
                                uint32_t len);

#endif
