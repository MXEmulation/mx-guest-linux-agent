/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#define _GNU_SOURCE
#include "network.h"

#include <arpa/inet.h>
#include <assert.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <string.h>

static struct ifaddrs items[48];
static struct sockaddr_ll links[4];
static struct sockaddr_in v4[40], v4_masks[40];
static struct sockaddr_in6 v6[2], v6_masks[2];
static unsigned used;

static struct ifaddrs *item(const char *name, unsigned flags, struct sockaddr *address,
                            struct sockaddr *mask)
{
    struct ifaddrs *entry = &items[used];
    memset(entry, 0, sizeof *entry);
    entry->ifa_name = (char *)name;
    entry->ifa_flags = flags;
    entry->ifa_addr = address;
    entry->ifa_netmask = mask;
    if (used)
        items[used - 1].ifa_next = entry;
    used++;
    return entry;
}

static void ipv4(unsigned index, const char *address, const char *mask)
{
    v4[index].sin_family = AF_INET;
    v4_masks[index].sin_family = AF_INET;
    assert(inet_pton(AF_INET, address, &v4[index].sin_addr) == 1);
    assert(inet_pton(AF_INET, mask, &v4_masks[index].sin_addr) == 1);
}

int main(void)
{
    static uint8_t payload[MXGUEST_NETWORK_PAYLOAD_MAX];
    static struct mxga_network_interface interfaces[MXGA_NETWORK_MAX_INTERFACES];
    static struct mxga_network_address addresses[MXGA_NETWORK_MAX_INTERFACES * MXGA_NETWORK_MAX_ADDRESSES];
    uint32_t len = 0, count = 0;
    unsigned i;
    const uint8_t mac[6] = {0x02, 0xd0, 0x5f, 0x2c, 0x0a, 0xc1};
    links[0].sll_family = AF_PACKET;
    links[0].sll_halen = 6;
    memcpy(links[0].sll_addr, mac, 6);
    links[1].sll_family = AF_PACKET;
    links[1].sll_halen = 0;
    ipv4(0, "10.0.0.15", "255.255.255.0");
    ipv4(1, "127.0.0.1", "255.0.0.0");
    v6[0].sin6_family = AF_INET6;
    v6_masks[0].sin6_family = AF_INET6;
    assert(inet_pton(AF_INET6, "fe80::d05f:2cff:fe0a:c130", &v6[0].sin6_addr) == 1);
    assert(inet_pton(AF_INET6, "ffff:ffff:ffff:ffff::", &v6_masks[0].sin6_addr) == 1);
    item("lo", IFF_UP | IFF_LOOPBACK | IFF_RUNNING, (struct sockaddr *)&links[1], NULL);
    item("enp0s4", IFF_UP | IFF_RUNNING | IFF_BROADCAST | IFF_MULTICAST,
         (struct sockaddr *)&links[0], NULL);
    item("lo", IFF_UP | IFF_LOOPBACK | IFF_RUNNING, (struct sockaddr *)&v4[1],
         (struct sockaddr *)&v4_masks[1]);
    item("enp0s4", IFF_UP | IFF_RUNNING, (struct sockaddr *)&v4[0], (struct sockaddr *)&v4_masks[0]);
    item("enp0s4", IFF_UP | IFF_RUNNING, (struct sockaddr *)&v6[0], (struct sockaddr *)&v6_masks[0]);
    item("tun\xff", IFF_UP | IFF_POINTOPOINT, NULL, NULL);
    item("wg0", IFF_POINTOPOINT, NULL, NULL);
    item("maximum1234567", IFF_UP, NULL, NULL);
    for (i = 2; i < 36; i++) {
        char text[32];
        snprintf(text, sizeof text, "192.168.%u.1", i);
        ipv4(i, text, "255.255.255.255");
        item("many", IFF_UP, (struct sockaddr *)&v4[i], (struct sockaddr *)&v4_masks[i]);
    }
    assert(mxguest_network_build(items, payload, sizeof payload, &len) == 0);
    assert(mxga_decode_network_info(payload, len, interfaces, MXGA_NETWORK_MAX_INTERFACES,
                                    addresses, sizeof addresses / sizeof addresses[0],
                                    &count) == MXGA_OK);
    assert(count == 5);
    assert(!strcmp((const char *)interfaces[0].name, "lo"));
    assert(interfaces[0].flags ==
           (MXGA_NETWORK_FLAG_UP | MXGA_NETWORK_FLAG_RUNNING | MXGA_NETWORK_FLAG_LOOPBACK));
    assert(interfaces[0].address_count == 1 && interfaces[0].addresses[0].prefix_length == 8);
    assert(!memcmp(interfaces[0].mac, "\0\0\0\0\0\0", 6));
    assert(!strcmp((const char *)interfaces[1].name, "enp0s4"));
    assert(!memcmp(interfaces[1].mac, mac, 6));
    assert(interfaces[1].flags == (MXGA_NETWORK_FLAG_UP | MXGA_NETWORK_FLAG_RUNNING));
    assert(interfaces[1].address_count == 2);
    assert(interfaces[1].addresses[0].family == MXGA_NETWORK_FAMILY_IPV4 &&
           interfaces[1].addresses[0].prefix_length == 24 &&
           !memcmp(interfaces[1].addresses[0].address, "\x0a\x00\x00\x0f", 4));
    assert(interfaces[1].addresses[1].family == MXGA_NETWORK_FAMILY_IPV6 &&
           interfaces[1].addresses[1].prefix_length == 64 &&
           interfaces[1].addresses[1].address[0] == 0xfe &&
           interfaces[1].addresses[1].address[15] == 0x30);
    assert(!strcmp((const char *)interfaces[2].name, "wg0") &&
           interfaces[2].flags == MXGA_NETWORK_FLAG_POINT_TO_POINT &&
           !interfaces[2].address_count);
    assert(!strcmp((const char *)interfaces[3].name, "maximum1234567"));
    assert(!strcmp((const char *)interfaces[4].name, "many") &&
           interfaces[4].address_count == MXGA_NETWORK_MAX_ADDRESSES &&
           interfaces[4].addresses[0].prefix_length == 32);
    assert(mxguest_network_build(items, payload, 8, &len) == -1);
    assert(mxguest_network_build(NULL, payload, sizeof payload, &len) == 0 && len == 8);
    assert(mxguest_network_collect(payload, sizeof payload, &len) == 0 && len >= 8);
    puts("network inventory records ok");
    return 0;
}
