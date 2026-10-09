/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#ifndef MXGUEST_SHARE_H
#define MXGUEST_SHARE_H

#include "mxga.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/select.h>

#define MXGUEST_SHARE_MAX_IN_FLIGHT 32u
#define MXGUEST_SHARE_TIMEOUT_MS 60000u
#define MXGUEST_SHARE_FALLBACK_ROOT "/media/mxshare"
#define MXGUEST_SHARE_FUSE_BUFFER (MXGA_FS_MAX_IO_BYTES + 65536u)
#define MXGUEST_SHARE_FS_TYPE "fuse.mxshare"

struct mxguest_share_system {
    void *ctx;
    /* Mounts a FUSE file system at mount_point: its connection descriptor, or a negated errno. */
    int (*attach)(void *ctx, const char *mount_point, int read_only);
    void (*detach)(void *ctx, const char *mount_point, int fuse_fd);
    /* The owner presented for every shared file. */
    void (*owner)(void *ctx, uint32_t *uid, uint32_t *gid);
    /* Sends a guest frame to the host: 0, or -1 with errno set. */
    int (*send)(void *ctx, uint16_t opcode, const uint8_t *payload, uint32_t len);
};

struct mxguest_shares;

struct mxguest_shares *mxguest_shares_new(const struct mxguest_share_system *system);
/* Detaches every mount, releases open host handles without awaiting replies and publishes. */
int mxguest_shares_close(struct mxguest_shares *shares);
/* Detaches every mount and abandons pending requests without contacting the host. */
void mxguest_shares_free(struct mxguest_shares *shares);

/* Each returns 0, or -1 when sending to the host failed (errno preserved). */
int mxguest_shares_publish(struct mxguest_shares *shares);
int mxguest_shares_mount(struct mxguest_shares *shares, const uint8_t *payload, uint32_t len);
int mxguest_shares_unmount(struct mxguest_shares *shares, const uint8_t *payload, uint32_t len);
int mxguest_shares_response(struct mxguest_shares *shares, const uint8_t *payload, uint32_t len,
                            uint64_t now_ms);
/* Serves readable FUSE connections, expires requests and sends deferred handle releases. */
int mxguest_shares_service(struct mxguest_shares *shares, const fd_set *readable,
                           uint64_t now_ms);

/* Adds connections to watch while the in-flight limit has room; returns the new maximum fd. */
int mxguest_shares_watch(const struct mxguest_shares *shares, fd_set *readable, int maxfd);
/* Earliest pending deadline, or UINT64_MAX. */
uint64_t mxguest_shares_deadline(const struct mxguest_shares *shares);
uint32_t mxguest_shares_in_flight(const struct mxguest_shares *shares);

/* Host mount points are used only when absolute under /media, /mnt or /run/media with no empty,
 * "." or ".." component; otherwise MXGUEST_SHARE_FALLBACK_ROOT/<sanitised name>, with
 * "-<share id>" appended when distinct is set. Returns 0, or -1 when out is too small. */
int mxguest_share_mount_point(const char *requested, const char *name, uint32_t share_id,
                              int distinct, char *out, size_t cap);

/* Kernel side: mount, unmount and stale-mount removal for MXGUEST_SHARE_FS_TYPE. */
int mxguest_share_attach(void *ctx, const char *mount_point, int read_only);
void mxguest_share_detach(void *ctx, const char *mount_point, int fuse_fd);
void mxguest_share_sweep(const char *mountinfo);
int mxguest_share_available(void);

#endif
