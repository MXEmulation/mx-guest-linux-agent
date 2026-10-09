/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#define _GNU_SOURCE
#include "network.h"

#include <net/if.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

struct network_entry {
    char name[IFNAMSIZ];
    struct mxga_network_interface record;
    struct mxga_network_address addresses[MXGA_NETWORK_MAX_ADDRESSES];
};

static uint16_t network_flags(unsigned flags)
{
    return (uint16_t)((flags & IFF_UP ? MXGA_NETWORK_FLAG_UP : 0u) |
                      (flags & IFF_RUNNING ? MXGA_NETWORK_FLAG_RUNNING : 0u) |
                      (flags & IFF_LOOPBACK ? MXGA_NETWORK_FLAG_LOOPBACK : 0u) |
                      (flags & IFF_POINTOPOINT ? MXGA_NETWORK_FLAG_POINT_TO_POINT : 0u));
}

static uint8_t prefix_bits(const uint8_t *mask, unsigned bytes)
{
    unsigned i, bits = 0;
    for (i = 0; i < bytes; i++) {
        uint8_t value = mask[i];
        while (value & 0x80u) {
            bits++;
            value = (uint8_t)(value << 1);
        }
        if (mask[i] != 0xffu)
            break;
    }
    return (uint8_t)bits;
}

static struct network_entry *network_find(struct network_entry *entries, uint32_t *count,
                                          const char *name)
{
    struct mxga_network_interface probe;
    size_t length = strlen(name);
    uint32_t i;
    for (i = 0; i < *count; i++)
        if (!strcmp(entries[i].name, name))
            return &entries[i];
    if (*count == MXGA_NETWORK_MAX_INTERFACES || length > MXGA_NETWORK_NAME_BYTES ||
        length >= IFNAMSIZ)
        return NULL;
    memset(&probe, 0, sizeof probe);
    memcpy(probe.name, name, length);
    if (mxga_encode_network_info(&probe, 1, NULL, 0, NULL) == MXGA_ERR_UTF8)
        return NULL;
    memset(&entries[*count], 0, sizeof entries[*count]);
    memcpy(entries[*count].name, name, length);
    entries[*count].record = probe;
    return &entries[(*count)++];
}

int mxguest_network_build(const struct ifaddrs *list, uint8_t *out, uint32_t cap,
                          uint32_t *out_len)
{
    struct network_entry *entries = calloc(MXGA_NETWORK_MAX_INTERFACES, sizeof *entries);
    struct mxga_network_interface records[MXGA_NETWORK_MAX_INTERFACES];
    const struct ifaddrs *item;
    uint32_t count = 0, i;
    int result;
    if (!entries)
        return -1;
    for (item = list; item; item = item->ifa_next) {
        struct network_entry *entry;
        struct mxga_network_address *address;
        int family;
        if (!item->ifa_name || !(entry = network_find(entries, &count, item->ifa_name)))
            continue;
        entry->record.flags = network_flags(item->ifa_flags);
        family = item->ifa_addr ? item->ifa_addr->sa_family : AF_UNSPEC;
        if (family == AF_PACKET) {
            const struct sockaddr_ll *link = (const struct sockaddr_ll *)item->ifa_addr;
            if (link->sll_halen == MXGA_NETWORK_MAC_BYTES)
                memcpy(entry->record.mac, link->sll_addr, MXGA_NETWORK_MAC_BYTES);
            continue;
        }
        if ((family != AF_INET && family != AF_INET6) ||
            entry->record.address_count == MXGA_NETWORK_MAX_ADDRESSES)
            continue;
        address = &entry->addresses[entry->record.address_count++];
        if (family == AF_INET) {
            const struct sockaddr_in *ip = (const struct sockaddr_in *)item->ifa_addr;
            const struct sockaddr_in *mask = (const struct sockaddr_in *)item->ifa_netmask;
            address->family = MXGA_NETWORK_FAMILY_IPV4;
            memcpy(address->address, &ip->sin_addr, 4);
            address->prefix_length =
                mask ? prefix_bits((const uint8_t *)&mask->sin_addr, 4) : 32u;
        } else {
            const struct sockaddr_in6 *ip = (const struct sockaddr_in6 *)item->ifa_addr;
            const struct sockaddr_in6 *mask = (const struct sockaddr_in6 *)item->ifa_netmask;
            address->family = MXGA_NETWORK_FAMILY_IPV6;
            memcpy(address->address, &ip->sin6_addr, 16);
            address->prefix_length =
                mask ? prefix_bits((const uint8_t *)&mask->sin6_addr, 16) : 128u;
        }
    }
    for (i = 0; i < count; i++) {
        records[i] = entries[i].record;
        records[i].addresses = entries[i].addresses;
    }
    result = mxga_encode_network_info(records, count, out, cap, out_len) == MXGA_OK ? 0 : -1;
    free(entries);
    return result;
}

int mxguest_network_collect(uint8_t *out, uint32_t cap, uint32_t *out_len)
{
    struct ifaddrs *list = NULL;
    int result;
    if (getifaddrs(&list) != 0)
        return -1;
    result = mxguest_network_build(list, out, cap, out_len);
    freeifaddrs(list);
    return result;
}
