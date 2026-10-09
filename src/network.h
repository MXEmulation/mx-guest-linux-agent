/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#ifndef MXGUEST_NETWORK_H
#define MXGUEST_NETWORK_H

#include "mxga.h"

#include <ifaddrs.h>
#include <stdint.h>

#define MXGUEST_NETWORK_PAYLOAD_MAX                                                                \
    (MXGA_NETWORK_HEADER_BYTES +                                                                   \
     MXGA_NETWORK_MAX_INTERFACES *                                                                 \
         (MXGA_NETWORK_INTERFACE_BYTES + MXGA_NETWORK_MAX_ADDRESSES * MXGA_NETWORK_ADDRESS_BYTES))

/* Builds a NETWORK_INFO payload from an interface address list in first-seen interface order.
 * Interfaces past the protocol bounds, addresses past the per-interface bound and names that
 * are not UTF-8 are omitted. Returns 0, or -1 when the payload cannot be encoded. */
int mxguest_network_build(const struct ifaddrs *list, uint8_t *out, uint32_t cap,
                          uint32_t *out_len);
/* Collects the current inventory with getifaddrs. */
int mxguest_network_collect(uint8_t *out, uint32_t cap, uint32_t *out_len);

#endif
