/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#define _GNU_SOURCE
#include "share.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define SHARE_TOMBSTONES 64u
#define SHARE_TTL_SECONDS 1u
#define SHARE_READS_PER_SERVICE 16u
#define SHARE_REQUEST_BYTES                                                                        \
    (MXGA_FS_REQUEST_HEADER_BYTES + 2u * MXGA_FS_MAX_PATH_BYTES + MXGA_FS_MAX_IO_BYTES)
#define SHARE_REPLY_BYTES (MXGA_FS_MAX_IO_BYTES + 4096u)
#define SHARE_MIN_DIRENT ((uint32_t)FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + 8u))
#define SHARE_INITIAL_BUCKETS 256u

struct node {
    uint64_t id, parent, lookups;
    uint32_t children;
    int linked;
    char *name;
    struct node *id_next, *name_next;
};

struct share {
    uint32_t id, flags, state;
    int32_t error;
    char name[MXGA_SHARE_NAME_BYTES + 1];
    char mount_point[MXGA_SHARE_PATH_BYTES + 1];
    int fd;
    int ready;
    struct node **by_id, **by_name;
    size_t buckets, nodes;
    uint64_t next_node;
    uint64_t *handles;
    size_t handle_count, handle_cap;
};

struct pending {
    int used;
    uint64_t request_id, unique, nodeid, newdir, fh, offset, deadline;
    uint32_t share_id, opcode, stage, flags, mode, size, got, fuse_flags;
    struct mxga_fs_attributes attributes;
    uint8_t *buffer;
    const uint8_t *data;
    char name[MXGA_FS_MAX_NAME_BYTES + 1], name2[MXGA_FS_MAX_NAME_BYTES + 1];
};

struct release {
    uint32_t share_id;
    uint64_t handle;
};

struct mxguest_shares {
    struct mxguest_share_system system;
    struct share *shares[MXGA_SHARE_MAX];
    struct pending pending[MXGUEST_SHARE_MAX_IN_FLIGHT];
    uint32_t in_flight;
    uint64_t tombstone_id[SHARE_TOMBSTONES];
    uint32_t tombstone_share[SHARE_TOMBSTONES];
    uint32_t tombstone_next;
    struct release *releases;
    size_t release_count, release_cap;
    uint64_t next_request;
    int send_failed, send_errno;
    uint8_t *fuse_buf, *request_buf, *reply_buf;
    char path[MXGA_FS_MAX_PATH_BYTES + 1], path2[MXGA_FS_MAX_PATH_BYTES + 1];
    const char *parts[MXGA_FS_MAX_PATH_BYTES / 2 + 1];
};

static size_t bucket_id(const struct share *sh, uint64_t id)
{
    return (size_t)(id * 0x9e3779b97f4a7c15ull >> 17) & (sh->buckets - 1);
}

static size_t bucket_name(const struct share *sh, uint64_t parent, const char *name)
{
    uint64_t hash = 1469598103934665603ull ^ parent;
    while (*name)
        hash = (hash ^ (uint8_t)*name++) * 1099511628211ull;
    return (size_t)(hash ^ hash >> 29) & (sh->buckets - 1);
}

static struct node *node_by_id(const struct share *sh, uint64_t id)
{
    struct node *node = sh->by_id ? sh->by_id[bucket_id(sh, id)] : NULL;
    while (node && node->id != id)
        node = node->id_next;
    return node;
}

static struct node *node_by_name(const struct share *sh, uint64_t parent, const char *name)
{
    struct node *node = sh->by_name[bucket_name(sh, parent, name)];
    while (node && (node->parent != parent || strcmp(node->name, name)))
        node = node->name_next;
    return node;
}

static void name_link(struct share *sh, struct node *node)
{
    size_t at = bucket_name(sh, node->parent, node->name);
    node->name_next = sh->by_name[at];
    sh->by_name[at] = node;
    node->linked = 1;
}

static void name_unlink(struct share *sh, struct node *node)
{
    struct node **link;
    if (!node->linked)
        return;
    link = &sh->by_name[bucket_name(sh, node->parent, node->name)];
    while (*link && *link != node)
        link = &(*link)->name_next;
    if (*link)
        *link = node->name_next;
    node->linked = 0;
}

static int nodes_rehash(struct share *sh)
{
    size_t old = sh->buckets, i;
    struct node **ids = sh->by_id, **names = sh->by_name;
    size_t grown = old * 2;
    struct node **next_ids = calloc(grown, sizeof *next_ids);
    struct node **next_names = calloc(grown, sizeof *next_names);
    if (!next_ids || !next_names) {
        free(next_ids);
        free(next_names);
        return -1;
    }
    sh->by_id = next_ids;
    sh->by_name = next_names;
    sh->buckets = grown;
    for (i = 0; i < old; i++) {
        struct node *node = ids[i];
        while (node) {
            struct node *next = node->id_next;
            size_t at = bucket_id(sh, node->id);
            node->id_next = sh->by_id[at];
            sh->by_id[at] = node;
            node = next;
        }
        node = names[i];
        while (node) {
            struct node *next = node->name_next;
            name_link(sh, node);
            node = next;
        }
    }
    free(ids);
    free(names);
    return 0;
}

static void nodes_clear(struct share *sh)
{
    size_t i;
    for (i = 0; sh->by_id && i < sh->buckets; i++) {
        struct node *node = sh->by_id[i];
        while (node) {
            struct node *next = node->id_next;
            free(node->name);
            free(node);
            node = next;
        }
    }
    free(sh->by_id);
    free(sh->by_name);
    sh->by_id = sh->by_name = NULL;
    sh->buckets = sh->nodes = 0;
}

static struct node *node_add(struct share *sh, uint64_t id, uint64_t parent, const char *name)
{
    struct node *node = calloc(1, sizeof *node);
    size_t at;
    if (!node || !(node->name = strdup(name))) {
        free(node);
        return NULL;
    }
    node->id = id;
    node->parent = parent;
    at = bucket_id(sh, id);
    node->id_next = sh->by_id[at];
    sh->by_id[at] = node;
    sh->nodes++;
    if (parent) {
        struct node *up = node_by_id(sh, parent);
        if (up)
            up->children++;
        name_link(sh, node);
    }
    return node;
}

static int nodes_init(struct share *sh)
{
    sh->buckets = SHARE_INITIAL_BUCKETS;
    sh->by_id = calloc(sh->buckets, sizeof *sh->by_id);
    sh->by_name = calloc(sh->buckets, sizeof *sh->by_name);
    sh->next_node = FUSE_ROOT_ID + 1;
    if (!sh->by_id || !sh->by_name || !node_add(sh, FUSE_ROOT_ID, 0, "")) {
        nodes_clear(sh);
        return -1;
    }
    node_by_id(sh, FUSE_ROOT_ID)->lookups = 1;
    return 0;
}

static void node_drop(struct share *sh, struct node *node)
{
    while (node && node->id != FUSE_ROOT_ID && !node->lookups && !node->children) {
        struct node **link = &sh->by_id[bucket_id(sh, node->id)];
        struct node *up = node_by_id(sh, node->parent);
        name_unlink(sh, node);
        while (*link && *link != node)
            link = &(*link)->id_next;
        if (*link)
            *link = node->id_next;
        free(node->name);
        free(node);
        sh->nodes--;
        if (up && up->children)
            up->children--;
        node = up;
    }
}

static struct node *node_get(struct share *sh, uint64_t parent, const char *name)
{
    struct node *node = node_by_name(sh, parent, name);
    if (node)
        return node;
    if (sh->nodes >= sh->buckets * 2 && nodes_rehash(sh) != 0)
        return NULL;
    return node_add(sh, sh->next_node++, parent, name);
}

static void node_move(struct share *sh, struct node *node, uint64_t parent, const char *name)
{
    char *copy = strdup(name);
    struct node *from, *to;
    if (!copy) {
        name_unlink(sh, node);
        return;
    }
    name_unlink(sh, node);
    if (node->parent != parent) {
        from = node_by_id(sh, node->parent);
        to = node_by_id(sh, parent);
        if (to)
            to->children++;
        node->parent = parent;
        if (from && from->children) {
            from->children--;
            node_drop(sh, from);
        }
    }
    free(node->name);
    node->name = copy;
    name_link(sh, node);
}

static int node_path(struct mxguest_shares *s, const struct share *sh, uint64_t id,
                     const char *leaf, char *out, uint32_t *out_len)
{
    uint32_t depth = 0, total = 0, at = 0;
    struct node *node = node_by_id(sh, id);
    if (leaf) {
        size_t size = strlen(leaf);
        if (size > MXGA_FS_MAX_NAME_BYTES)
            return -ENAMETOOLONG;
        s->parts[depth++] = leaf;
        total = (uint32_t)size;
    }
    while (node && node->id != FUSE_ROOT_ID) {
        if (!node->linked)
            return -ENOENT;
        if (depth == sizeof s->parts / sizeof s->parts[0])
            return -ENAMETOOLONG;
        s->parts[depth++] = node->name;
        total += (uint32_t)strlen(node->name) + (total ? 1u : 0u);
        if (total > MXGA_FS_MAX_PATH_BYTES)
            return -ENAMETOOLONG;
        node = node_by_id(sh, node->parent);
    }
    if (!node)
        return -ENOENT;
    while (depth--) {
        size_t size = strlen(s->parts[depth]);
        if (at)
            out[at++] = '/';
        memcpy(out + at, s->parts[depth], size);
        at += (uint32_t)size;
    }
    out[at] = 0;
    if (mxga_fs_path_valid((const uint8_t *)out, at) != MXGA_OK)
        return -EINVAL;
    *out_len = at;
    return 0;
}

static struct share *share_find(const struct mxguest_shares *s, uint32_t id)
{
    uint32_t i;
    for (i = 0; i < MXGA_SHARE_MAX; i++)
        if (s->shares[i] && s->shares[i]->id == id)
            return s->shares[i];
    return NULL;
}

static int handle_track(struct share *sh, uint64_t handle)
{
    if (sh->handle_count == sh->handle_cap) {
        size_t grown = sh->handle_cap ? sh->handle_cap * 2 : 16;
        uint64_t *next = realloc(sh->handles, grown * sizeof *next);
        if (!next)
            return -1;
        sh->handles = next;
        sh->handle_cap = grown;
    }
    sh->handles[sh->handle_count++] = handle;
    return 0;
}

static void handle_untrack(struct share *sh, uint64_t handle)
{
    size_t i;
    for (i = 0; i < sh->handle_count; i++)
        if (sh->handles[i] == handle) {
            sh->handles[i] = sh->handles[--sh->handle_count];
            return;
        }
}

static void release_queue(struct mxguest_shares *s, uint32_t share_id, uint64_t handle)
{
    if (s->release_count == s->release_cap) {
        size_t grown = s->release_cap ? s->release_cap * 2 : 16;
        struct release *next = realloc(s->releases, grown * sizeof *next);
        if (!next) {
            fprintf(stderr, "shared folder handle %llu not released\n", (unsigned long long)handle);
            return;
        }
        s->releases = next;
        s->release_cap = grown;
    }
    s->releases[s->release_count].share_id = share_id;
    s->releases[s->release_count++].handle = handle;
}

static void fuse_reply(const struct share *sh, uint64_t unique, int error, const void *data,
                       size_t len)
{
    struct fuse_out_header header;
    struct iovec parts[2];
    if (!sh || sh->fd < 0 || !unique)
        return;
    header.len = (uint32_t)(sizeof header + (error ? 0 : len));
    header.error = error;
    header.unique = unique;
    parts[0].iov_base = &header;
    parts[0].iov_len = sizeof header;
    parts[1].iov_base = (void *)data;
    parts[1].iov_len = error ? 0 : len;
    while (writev(sh->fd, parts, error || !len ? 1 : 2) < 0 && errno == EINTR)
        ;
}

static void pending_free(struct mxguest_shares *s, struct pending *p)
{
    free(p->buffer);
    memset(p, 0, sizeof *p);
    s->in_flight--;
}

static void pending_abandon(struct mxguest_shares *s, struct pending *p)
{
    s->tombstone_id[s->tombstone_next] = p->request_id;
    s->tombstone_share[s->tombstone_next] = p->share_id;
    s->tombstone_next = (s->tombstone_next + 1) % SHARE_TOMBSTONES;
    pending_free(s, p);
}

static struct pending *pending_new(struct mxguest_shares *s, const struct share *sh,
                                   const struct fuse_in_header *in)
{
    uint32_t i;
    for (i = 0; i < MXGUEST_SHARE_MAX_IN_FLIGHT; i++) {
        struct pending *p = &s->pending[i];
        if (p->used)
            continue;
        memset(p, 0, sizeof *p);
        p->used = 1;
        p->share_id = sh->id;
        if (in) {
            p->unique = in->unique;
            p->opcode = in->opcode;
            p->nodeid = in->nodeid;
        }
        s->in_flight++;
        return p;
    }
    return NULL;
}

static uint64_t realtime_now(uint32_t *nanoseconds)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    *nanoseconds = (uint32_t)now.tv_nsec;
    return now.tv_sec > 0 ? (uint64_t)now.tv_sec : 0;
}

static int pending_issue(struct mxguest_shares *s, struct pending *p, uint64_t now_ms)
{
    struct mxga_fs_request request;
    const struct share *sh = share_find(s, p->share_id);
    uint8_t attributes[MXGA_FS_ATTRIBUTES_BYTES];
    uint32_t len = 0, path_len = 0, path2_len = 0;
    int named = p->name[0] != 0, result = 0;
    memset(&request, 0, sizeof request);
    if (!sh)
        return -ENODEV;
    request.share_id = p->share_id;
    request.operation = p->stage;
    switch (p->stage) {
    case MXGA_FS_OP_READ:
        request.handle = p->fh;
        request.offset = p->offset + p->got;
        request.length = p->size - p->got;
        break;
    case MXGA_FS_OP_WRITE:
        request.handle = p->fh;
        request.offset = p->offset;
        request.data = p->data;
        request.data_bytes = p->size;
        break;
    case MXGA_FS_OP_TRUNCATE:
        request.handle = p->fh;
        request.offset = p->attributes.size;
        break;
    case MXGA_FS_OP_RELEASE:
    case MXGA_FS_OP_FSYNC:
        request.handle = p->fh;
        break;
    case MXGA_FS_OP_STATFS:
        break;
    case MXGA_FS_OP_RENAME:
        result = node_path(s, sh, p->newdir, p->name2, s->path2, &path2_len);
        request.second_path = (const uint8_t *)s->path2;
        request.second_path_bytes = path2_len;
        /* fall through */
    default:
        if (!result)
            result = node_path(s, sh, p->nodeid, named ? p->name : NULL, s->path, &path_len);
        request.path = (const uint8_t *)s->path;
        request.path_bytes = path_len;
        break;
    }
    if (result)
        return result;
    if (p->stage == MXGA_FS_OP_READDIR) {
        request.offset = p->offset;
        request.length = p->size / SHARE_MIN_DIRENT ? p->size / SHARE_MIN_DIRENT : 1u;
    } else if (p->stage == MXGA_FS_OP_OPEN || p->stage == MXGA_FS_OP_CREATE) {
        request.flags = p->flags;
        request.mode = p->stage == MXGA_FS_OP_CREATE ? p->mode : 0u;
    } else if (p->stage == MXGA_FS_OP_MKDIR) {
        request.mode = p->mode;
    } else if (p->stage == MXGA_FS_OP_SETATTR) {
        request.flags = p->flags;
        if (mxga_encode_fs_attributes(&p->attributes, attributes, sizeof attributes, &len) !=
            MXGA_OK)
            return -EINVAL;
        request.data = attributes;
        request.data_bytes = len;
    }
    request.request_id = s->next_request;
    if (mxga_encode_fs_request(&request, s->request_buf, SHARE_REQUEST_BYTES, &len) != MXGA_OK)
        return -EINVAL;
    if (s->system.send(s->system.ctx, MXGA_OP_FS_REQUEST, s->request_buf, len) != 0) {
        s->send_failed = 1;
        s->send_errno = errno;
        return -EIO;
    }
    p->request_id = s->next_request++;
    p->deadline = now_ms + MXGUEST_SHARE_TIMEOUT_MS;
    return 0;
}

static void pending_start(struct mxguest_shares *s, struct share *sh, struct pending *p,
                          uint32_t stage, uint64_t now_ms)
{
    int result;
    p->stage = stage;
    result = pending_issue(s, p, now_ms);
    if (result) {
        fuse_reply(sh, p->unique, result, NULL, 0);
        pending_free(s, p);
    }
}

static void fill_attr(const struct mxguest_shares *s, uint64_t nodeid,
                      const struct mxga_fs_attributes *in, struct fuse_attr *out)
{
    uint32_t mode = in->mode;
    memset(out, 0, sizeof *out);
    if (S_ISDIR(mode))
        mode |= (mode & 0444u) >> 2;
    out->ino = in->ino ? in->ino : nodeid;
    out->size = in->size;
    out->blocks = in->blocks;
    out->atime = in->atime_s;
    out->mtime = in->mtime_s;
    out->ctime = in->ctime_s;
    out->atimensec = in->atime_ns < 1000000000u ? in->atime_ns : 0u;
    out->mtimensec = in->mtime_ns < 1000000000u ? in->mtime_ns : 0u;
    out->ctimensec = in->ctime_ns < 1000000000u ? in->ctime_ns : 0u;
    out->mode = mode;
    out->nlink = in->nlink ? in->nlink : 1u;
    s->system.owner(s->system.ctx, &out->uid, &out->gid);
}

static int reply_entry(struct mxguest_shares *s, struct share *sh, struct pending *p,
                       const struct mxga_fs_response *response, const struct fuse_open_out *open)
{
    struct mxga_fs_attributes attributes;
    struct fuse_entry_out entry;
    struct node *node;
    uint8_t reply[sizeof entry + sizeof *open];
    if (mxga_decode_fs_attributes(response->data, response->data_bytes, &attributes) != MXGA_OK)
        return -EIO;
    node = node_get(sh, p->nodeid, p->name);
    if (!node)
        return -ENOMEM;
    node->lookups++;
    memset(&entry, 0, sizeof entry);
    entry.nodeid = node->id;
    entry.entry_valid = SHARE_TTL_SECONDS;
    entry.attr_valid = SHARE_TTL_SECONDS;
    fill_attr(s, node->id, &attributes, &entry.attr);
    memcpy(reply, &entry, sizeof entry);
    if (open)
        memcpy(reply + sizeof entry, open, sizeof *open);
    fuse_reply(sh, p->unique, 0, reply, sizeof entry + (open ? sizeof *open : 0));
    return 0;
}

static int reply_attr(struct mxguest_shares *s, struct share *sh, struct pending *p,
                      const struct mxga_fs_response *response)
{
    struct mxga_fs_attributes attributes;
    struct fuse_attr_out out;
    if (mxga_decode_fs_attributes(response->data, response->data_bytes, &attributes) != MXGA_OK)
        return -EIO;
    memset(&out, 0, sizeof out);
    out.attr_valid = SHARE_TTL_SECONDS;
    fill_attr(s, p->nodeid, &attributes, &out.attr);
    fuse_reply(sh, p->unique, 0, &out, sizeof out);
    return 0;
}

static int reply_dir(struct mxguest_shares *s, struct share *sh, struct pending *p,
                     const struct mxga_fs_response *response)
{
    uint32_t cursor = 0, used = 0;
    uint64_t index = p->offset;
    while (cursor < response->data_bytes) {
        struct mxga_fs_dirent entry;
        struct fuse_dirent *out = (struct fuse_dirent *)(s->reply_buf + used);
        uint32_t record;
        if (mxga_decode_fs_dirent(response->data, response->data_bytes, &cursor, &entry) !=
            MXGA_OK)
            return -EIO;
        record = (uint32_t)FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + entry.name_bytes);
        if (used + record > p->size || used + record > SHARE_REPLY_BYTES)
            break;
        memset(out, 0, record);
        out->ino = entry.inode ? entry.inode : index + 2u;
        out->off = index + 1u;
        out->namelen = entry.name_bytes;
        out->type = (entry.mode & S_IFMT) >> 12;
        memcpy(out->name, entry.name, entry.name_bytes);
        used += record;
        index++;
    }
    fuse_reply(sh, p->unique, 0, s->reply_buf, used);
    return 0;
}

static int read_progress(struct mxguest_shares *s, struct share *sh, struct pending *p,
                         const struct mxga_fs_response *response, uint64_t now_ms)
{
    if (response->data_bytes > p->size - p->got)
        return -EIO;
    if (!p->got && (response->data_bytes == p->size || !response->data_bytes)) {
        fuse_reply(sh, p->unique, 0, response->data, response->data_bytes);
        return 0;
    }
    if (response->data_bytes) {
        if (!p->buffer && !(p->buffer = malloc(p->size)))
            return -ENOMEM;
        memcpy(p->buffer + p->got, response->data, response->data_bytes);
        p->got += response->data_bytes;
        if (p->got < p->size) {
            int result = pending_issue(s, p, now_ms);
            return result ? result : 1;
        }
    }
    fuse_reply(sh, p->unique, 0, p->buffer, p->got);
    return 0;
}

static int share_responds(struct mxguest_shares *s, struct share *sh, struct pending *p,
                          const struct mxga_fs_response *response, uint64_t now_ms)
{
    int status = response->status;
    struct fuse_open_out open;
    struct fuse_write_out written;
    struct mxga_fs_statfs statfs;
    struct fuse_statfs_out stat_out;
    struct node *node;
    if (status)
        return status;
    switch (p->opcode) {
    case FUSE_LOOKUP:
        return reply_entry(s, sh, p, response, NULL);
    case FUSE_GETATTR:
        return reply_attr(s, sh, p, response);
    case FUSE_SETATTR:
        if (p->stage == MXGA_FS_OP_GETATTR)
            return reply_attr(s, sh, p, response);
        p->stage = p->stage == MXGA_FS_OP_TRUNCATE && p->flags ? MXGA_FS_OP_SETATTR
                                                                : MXGA_FS_OP_GETATTR;
        status = pending_issue(s, p, now_ms);
        return status ? status : 1;
    case FUSE_READDIR:
        return reply_dir(s, sh, p, response);
    case FUSE_OPEN:
        if (handle_track(sh, response->handle) != 0) {
            release_queue(s, sh->id, response->handle);
            return -ENOMEM;
        }
        memset(&open, 0, sizeof open);
        open.fh = response->handle;
        fuse_reply(sh, p->unique, 0, &open, sizeof open);
        return 0;
    case FUSE_READ:
        return read_progress(s, sh, p, response, now_ms);
    case FUSE_WRITE:
        memset(&written, 0, sizeof written);
        written.size = p->size;
        fuse_reply(sh, p->unique, 0, &written, sizeof written);
        return 0;
    case FUSE_CREATE:
        if (p->stage != MXGA_FS_OP_GETATTR) {
            if (handle_track(sh, response->handle) != 0) {
                release_queue(s, sh->id, response->handle);
                return -ENOMEM;
            }
            p->fh = response->handle;
            p->stage = MXGA_FS_OP_GETATTR;
            status = pending_issue(s, p, now_ms);
            return status ? status : 1;
        }
        memset(&open, 0, sizeof open);
        open.fh = p->fh;
        status = reply_entry(s, sh, p, response, &open);
        if (!status)
            p->fh = 0;
        return status;
    case FUSE_MKDIR:
        if (p->stage == MXGA_FS_OP_GETATTR)
            return reply_entry(s, sh, p, response, NULL);
        p->stage = MXGA_FS_OP_GETATTR;
        status = pending_issue(s, p, now_ms);
        return status ? status : 1;
    case FUSE_UNLINK:
    case FUSE_RMDIR:
        if ((node = node_by_name(sh, p->nodeid, p->name)))
            name_unlink(sh, node);
        break;
    case FUSE_RENAME:
    case FUSE_RENAME2:
        if ((node = node_by_name(sh, p->newdir, p->name2)))
            name_unlink(sh, node);
        if ((node = node_by_name(sh, p->nodeid, p->name)))
            node_move(sh, node, p->newdir, p->name2);
        break;
    case FUSE_READLINK:
        fuse_reply(sh, p->unique, 0, response->data, response->data_bytes);
        return 0;
    case FUSE_STATFS:
        if (mxga_decode_fs_statfs(response->data, response->data_bytes, &statfs) != MXGA_OK)
            return -EIO;
        memset(&stat_out, 0, sizeof stat_out);
        stat_out.st.blocks = statfs.blocks;
        stat_out.st.bfree = statfs.blocks_free;
        stat_out.st.bavail = statfs.blocks_available;
        stat_out.st.files = statfs.files;
        stat_out.st.ffree = statfs.files_free;
        stat_out.st.bsize = (uint32_t)statfs.block_size;
        stat_out.st.frsize = (uint32_t)statfs.block_size;
        stat_out.st.namelen = (uint32_t)(statfs.name_max ? statfs.name_max : MXGA_FS_MAX_NAME_BYTES);
        fuse_reply(sh, p->unique, 0, &stat_out, sizeof stat_out);
        return 0;
    default:
        break;
    }
    fuse_reply(sh, p->unique, 0, NULL, 0);
    return 0;
}

static int share_create_retry(struct mxguest_shares *s, struct pending *p, int status,
                              uint64_t now_ms)
{
    if (p->opcode != FUSE_CREATE || p->stage != MXGA_FS_OP_CREATE || status != -EEXIST ||
        (p->fuse_flags & O_EXCL))
        return 0;
    p->stage = MXGA_FS_OP_OPEN;
    p->flags &= ~(MXGA_FS_OPEN_CREATE | MXGA_FS_OPEN_EXCLUSIVE);
    if (p->fuse_flags & O_TRUNC)
        p->flags |= MXGA_FS_OPEN_TRUNCATE;
    return pending_issue(s, p, now_ms) ? 0 : 1;
}

int mxguest_shares_response(struct mxguest_shares *s, const uint8_t *payload, uint32_t len,
                            uint64_t now_ms)
{
    struct mxga_fs_response response;
    struct pending *p = NULL;
    struct share *sh;
    uint32_t i;
    int status;
    if (mxga_decode_fs_response(payload, len, &response) != MXGA_OK) {
        fprintf(stderr, "shared folder response ignored\n");
        return 0;
    }
    for (i = 0; i < MXGUEST_SHARE_MAX_IN_FLIGHT && !p; i++)
        if (s->pending[i].used && s->pending[i].request_id == response.request_id)
            p = &s->pending[i];
    if (!p) {
        for (i = 0; i < SHARE_TOMBSTONES; i++)
            if (s->tombstone_id[i] == response.request_id && response.request_id) {
                if (!response.status && response.handle &&
                    (response.operation == MXGA_FS_OP_OPEN ||
                     response.operation == MXGA_FS_OP_CREATE))
                    release_queue(s, s->tombstone_share[i], response.handle);
                s->tombstone_id[i] = 0;
            }
        return mxguest_shares_service(s, NULL, now_ms);
    }
    sh = share_find(s, p->share_id);
    status = response.operation == p->stage && response.status >= -4095 ? response.status : -EIO;
    response.status = status;
    if (!p->unique) {
        pending_free(s, p);
        return mxguest_shares_service(s, NULL, now_ms);
    }
    if (status && share_create_retry(s, p, status, now_ms))
        return s->send_failed ? -1 : 0;
    status = sh ? share_responds(s, sh, p, &response, now_ms) : -ENODEV;
    if (status < 0) {
        if (p->opcode == FUSE_CREATE && p->fh && sh) {
            handle_untrack(sh, p->fh);
            release_queue(s, sh->id, p->fh);
        }
        fuse_reply(sh, p->unique, status, NULL, 0);
    }
    if (status != 1)
        pending_free(s, p);
    if (s->send_failed) {
        errno = s->send_errno;
        return -1;
    }
    return mxguest_shares_service(s, NULL, now_ms);
}

static void fuse_init(struct mxguest_shares *s, struct share *sh, const struct fuse_in_header *in,
                      const uint8_t *body, uint32_t size)
{
    struct fuse_init_in request;
    struct fuse_init_out reply;
    long page = sysconf(_SC_PAGESIZE);
    (void)s;
    memset(&request, 0, sizeof request);
    memcpy(&request, body, size < sizeof request ? size : sizeof request);
    if (size < 16 || request.major != FUSE_KERNEL_VERSION || request.minor < 23) {
        fuse_reply(sh, in->unique, -EPROTO, NULL, 0);
        return;
    }
    memset(&reply, 0, sizeof reply);
    reply.major = FUSE_KERNEL_VERSION;
    reply.minor = request.minor < FUSE_KERNEL_MINOR_VERSION ? request.minor
                                                            : FUSE_KERNEL_MINOR_VERSION;
    reply.max_readahead = request.max_readahead;
    reply.flags = request.flags & (FUSE_ASYNC_READ | FUSE_BIG_WRITES | FUSE_MAX_PAGES);
    reply.max_background = 16;
    reply.congestion_threshold = 12;
    reply.max_write = MXGA_FS_MAX_IO_BYTES;
    reply.time_gran = 1;
    reply.max_pages = (uint16_t)(MXGA_FS_MAX_IO_BYTES / (page > 0 ? (unsigned long)page : 4096u));
    sh->ready = 1;
    fuse_reply(sh, in->unique, 0, &reply, sizeof reply);
}

static void fuse_forget(struct share *sh, uint64_t nodeid, uint64_t count)
{
    struct node *node = node_by_id(sh, nodeid);
    if (!node || nodeid == FUSE_ROOT_ID)
        return;
    node->lookups = node->lookups > count ? node->lookups - count : 0;
    node_drop(sh, node);
}

static uint32_t open_flags(uint32_t flags)
{
    uint32_t mapped = 0, access = flags & O_ACCMODE;
    if (access == O_RDONLY || access == O_RDWR)
        mapped |= MXGA_FS_OPEN_READ;
    if (access == O_WRONLY || access == O_RDWR)
        mapped |= MXGA_FS_OPEN_WRITE;
    if (flags & O_APPEND)
        mapped |= MXGA_FS_OPEN_APPEND;
    if (flags & O_TRUNC)
        mapped |= MXGA_FS_OPEN_TRUNCATE;
    return mapped;
}

static int copy_name(char *out, const uint8_t *body, uint32_t size, uint32_t *used)
{
    const uint8_t *end = memchr(body, 0, size);
    size_t len;
    if (!end)
        return -EINVAL;
    len = (size_t)(end - body);
    if (!len)
        return -EINVAL;
    if (len > MXGA_FS_MAX_NAME_BYTES)
        return -ENAMETOOLONG;
    memcpy(out, body, len + 1);
    if (used)
        *used = (uint32_t)len + 1;
    return 0;
}

static int fuse_setattr(struct pending *p, const struct fuse_setattr_in *in)
{
    uint32_t flags = 0, nanoseconds = 0;
    uint64_t now = 0;
    if (in->valid & (FATTR_UID | FATTR_GID))
        return -EPERM;
    if (in->valid & (FATTR_ATIME_NOW | FATTR_MTIME_NOW))
        now = realtime_now(&nanoseconds);
    if (in->valid & FATTR_MODE) {
        flags |= MXGA_FS_SETATTR_MODE;
        p->attributes.mode = in->mode & 07777u;
    }
    if (in->valid & FATTR_SIZE) {
        flags |= MXGA_FS_SETATTR_SIZE;
        p->attributes.size = in->size;
    }
    if (in->valid & FATTR_ATIME) {
        flags |= MXGA_FS_SETATTR_ATIME;
        p->attributes.atime_s = in->valid & FATTR_ATIME_NOW ? now : in->atime;
        p->attributes.atime_ns = in->valid & FATTR_ATIME_NOW ? nanoseconds : in->atimensec;
    }
    if (in->valid & FATTR_MTIME) {
        flags |= MXGA_FS_SETATTR_MTIME;
        p->attributes.mtime_s = in->valid & FATTR_MTIME_NOW ? now : in->mtime;
        p->attributes.mtime_ns = in->valid & FATTR_MTIME_NOW ? nanoseconds : in->mtimensec;
    }
    if (p->attributes.atime_ns >= 1000000000u || p->attributes.mtime_ns >= 1000000000u)
        return -EINVAL;
    if ((flags & MXGA_FS_SETATTR_SIZE) && (in->valid & FATTR_FH) && in->fh) {
        p->fh = in->fh;
        p->flags = flags & ~MXGA_FS_SETATTR_SIZE;
        return MXGA_FS_OP_TRUNCATE;
    }
    p->flags = flags;
    return flags ? MXGA_FS_OP_SETATTR : MXGA_FS_OP_GETATTR;
}

static void fuse_request(struct mxguest_shares *s, struct share *sh, const uint8_t *message,
                         uint32_t len, uint64_t now_ms)
{
    struct fuse_in_header in;
    const uint8_t *body;
    struct pending *p;
    uint32_t size, used = 0;
    int stage = 0, result = 0;
    if (len < sizeof in)
        return;
    memcpy(&in, message, sizeof in);
    if (in.len != len || (uint32_t)in.total_extlen * 8u > len - sizeof in)
        return;
    body = message + sizeof in;
    size = len - (uint32_t)sizeof in - (uint32_t)in.total_extlen * 8u;
    switch (in.opcode) {
    case FUSE_INIT:
        fuse_init(s, sh, &in, body, size);
        return;
    case FUSE_FORGET:
        if (size >= sizeof(struct fuse_forget_in))
            fuse_forget(sh, in.nodeid, ((const struct fuse_forget_in *)body)->nlookup);
        return;
    case FUSE_BATCH_FORGET: {
        struct fuse_batch_forget_in batch;
        uint32_t i;
        if (size < sizeof batch)
            return;
        memcpy(&batch, body, sizeof batch);
        for (i = 0; i < batch.count &&
                    sizeof batch + (i + 1u) * sizeof(struct fuse_forget_one) <= size;
             i++) {
            struct fuse_forget_one one;
            memcpy(&one, body + sizeof batch + i * sizeof one, sizeof one);
            fuse_forget(sh, one.nodeid, one.nlookup);
        }
        return;
    }
    case FUSE_INTERRUPT:
        return;
    case FUSE_OPENDIR: {
        struct fuse_open_out open;
        memset(&open, 0, sizeof open);
        fuse_reply(sh, in.unique, 0, &open, sizeof open);
        return;
    }
    case FUSE_RELEASEDIR:
    case FUSE_FSYNCDIR:
    case FUSE_FLUSH:
    case FUSE_DESTROY:
        fuse_reply(sh, in.unique, 0, NULL, 0);
        return;
    default:
        break;
    }
    if (!sh->ready) {
        fuse_reply(sh, in.unique, -EIO, NULL, 0);
        return;
    }
    p = pending_new(s, sh, &in);
    if (!p) {
        fuse_reply(sh, in.unique, -EAGAIN, NULL, 0);
        return;
    }
    switch (in.opcode) {
    case FUSE_LOOKUP:
        result = copy_name(p->name, body, size, NULL);
        stage = MXGA_FS_OP_GETATTR;
        break;
    case FUSE_GETATTR:
        stage = MXGA_FS_OP_GETATTR;
        break;
    case FUSE_SETATTR: {
        struct fuse_setattr_in attr;
        if (size < sizeof attr) {
            result = -EINVAL;
            break;
        }
        memcpy(&attr, body, sizeof attr);
        stage = fuse_setattr(p, &attr);
        if (stage < 0)
            result = stage;
        break;
    }
    case FUSE_READDIR: {
        struct fuse_read_in read;
        if (size < sizeof read) {
            result = -EINVAL;
            break;
        }
        memcpy(&read, body, sizeof read);
        p->offset = read.offset;
        p->size = read.size < SHARE_REPLY_BYTES ? read.size : SHARE_REPLY_BYTES;
        stage = MXGA_FS_OP_READDIR;
        break;
    }
    case FUSE_OPEN: {
        struct fuse_open_in open;
        if (size < sizeof open) {
            result = -EINVAL;
            break;
        }
        memcpy(&open, body, sizeof open);
        p->flags = open_flags(open.flags);
        stage = MXGA_FS_OP_OPEN;
        break;
    }
    case FUSE_READ: {
        struct fuse_read_in read;
        if (size < sizeof read) {
            result = -EINVAL;
            break;
        }
        memcpy(&read, body, sizeof read);
        p->fh = read.fh;
        p->offset = read.offset;
        p->size = read.size < MXGA_FS_MAX_IO_BYTES ? read.size : MXGA_FS_MAX_IO_BYTES;
        if (!p->size) {
            fuse_reply(sh, in.unique, 0, NULL, 0);
            pending_free(s, p);
            return;
        }
        stage = MXGA_FS_OP_READ;
        break;
    }
    case FUSE_WRITE: {
        struct fuse_write_in write;
        if (size < sizeof write) {
            result = -EINVAL;
            break;
        }
        memcpy(&write, body, sizeof write);
        if (write.size > size - sizeof write || write.size > MXGA_FS_MAX_IO_BYTES) {
            result = -EINVAL;
            break;
        }
        p->fh = write.fh;
        p->offset = write.offset;
        p->size = write.size;
        p->data = body + sizeof write;
        stage = MXGA_FS_OP_WRITE;
        break;
    }
    case FUSE_RELEASE: {
        struct fuse_release_in release;
        if (size < sizeof release) {
            result = -EINVAL;
            break;
        }
        memcpy(&release, body, sizeof release);
        p->fh = release.fh;
        handle_untrack(sh, release.fh);
        stage = MXGA_FS_OP_RELEASE;
        break;
    }
    case FUSE_FSYNC: {
        struct fuse_fsync_in sync;
        if (size < sizeof sync) {
            result = -EINVAL;
            break;
        }
        memcpy(&sync, body, sizeof sync);
        p->fh = sync.fh;
        stage = MXGA_FS_OP_FSYNC;
        break;
    }
    case FUSE_CREATE: {
        struct fuse_create_in create;
        if (size < sizeof create) {
            result = -EINVAL;
            break;
        }
        memcpy(&create, body, sizeof create);
        result = copy_name(p->name, body + sizeof create, size - (uint32_t)sizeof create, NULL);
        p->fuse_flags = create.flags;
        p->flags = open_flags(create.flags) | MXGA_FS_OPEN_CREATE | MXGA_FS_OPEN_EXCLUSIVE;
        p->flags &= ~MXGA_FS_OPEN_TRUNCATE;
        p->mode = create.mode & 07777u;
        stage = MXGA_FS_OP_CREATE;
        break;
    }
    case FUSE_MKDIR: {
        struct fuse_mkdir_in mkdir;
        if (size < sizeof mkdir) {
            result = -EINVAL;
            break;
        }
        memcpy(&mkdir, body, sizeof mkdir);
        result = copy_name(p->name, body + sizeof mkdir, size - (uint32_t)sizeof mkdir, NULL);
        p->mode = mkdir.mode & 07777u;
        stage = MXGA_FS_OP_MKDIR;
        break;
    }
    case FUSE_UNLINK:
    case FUSE_RMDIR:
        result = copy_name(p->name, body, size, NULL);
        stage = in.opcode == FUSE_UNLINK ? MXGA_FS_OP_UNLINK : MXGA_FS_OP_RMDIR;
        break;
    case FUSE_RENAME:
    case FUSE_RENAME2: {
        uint32_t header = in.opcode == FUSE_RENAME ? (uint32_t)sizeof(struct fuse_rename_in)
                                                    : (uint32_t)sizeof(struct fuse_rename2_in);
        struct fuse_rename2_in rename;
        memset(&rename, 0, sizeof rename);
        if (size < header) {
            result = -EINVAL;
            break;
        }
        memcpy(&rename, body, header);
        if (rename.flags) {
            result = -EINVAL;
            break;
        }
        p->newdir = rename.newdir;
        result = copy_name(p->name, body + header, size - header, &used);
        if (!result)
            result = copy_name(p->name2, body + header + used, size - header - used, NULL);
        stage = MXGA_FS_OP_RENAME;
        break;
    }
    case FUSE_READLINK:
        stage = MXGA_FS_OP_READLINK;
        break;
    case FUSE_STATFS:
        stage = MXGA_FS_OP_STATFS;
        break;
    default:
        result = -ENOSYS;
        break;
    }
    if (!result && (in.opcode == FUSE_FSYNC || in.opcode == FUSE_RELEASE) && !p->fh) {
        fuse_reply(sh, in.unique, 0, NULL, 0);
        pending_free(s, p);
        return;
    }
    if (result) {
        fuse_reply(sh, in.unique, result, NULL, 0);
        pending_free(s, p);
        return;
    }
    pending_start(s, sh, p, (uint32_t)stage, now_ms);
}

static void share_disconnect(struct mxguest_shares *s, struct share *sh)
{
    uint32_t i;
    size_t h;
    if (sh->fd >= 0)
        s->system.detach(s->system.ctx, sh->mount_point, sh->fd);
    sh->fd = -1;
    sh->ready = 0;
    for (i = 0; i < MXGUEST_SHARE_MAX_IN_FLIGHT; i++)
        if (s->pending[i].used && s->pending[i].share_id == sh->id) {
            if (s->pending[i].opcode == FUSE_CREATE && s->pending[i].fh)
                release_queue(s, sh->id, s->pending[i].fh);
            pending_abandon(s, &s->pending[i]);
        }
    for (h = 0; h < sh->handle_count; h++)
        release_queue(s, sh->id, sh->handles[h]);
    sh->handle_count = 0;
    nodes_clear(sh);
}

static int send_releases(struct mxguest_shares *s, uint64_t now_ms)
{
    while (s->release_count && s->in_flight < MXGUEST_SHARE_MAX_IN_FLIGHT && !s->send_failed) {
        struct release next = s->releases[0];
        struct share holder;
        struct pending *p;
        memset(&holder, 0, sizeof holder);
        holder.id = next.share_id;
        memmove(s->releases, s->releases + 1, --s->release_count * sizeof *s->releases);
        p = pending_new(s, &holder, NULL);
        if (!p)
            break;
        p->fh = next.handle;
        p->stage = MXGA_FS_OP_RELEASE;
        if (!share_find(s, next.share_id)) {
            pending_free(s, p);
            continue;
        }
        if (pending_issue(s, p, now_ms))
            pending_free(s, p);
    }
    if (s->send_failed) {
        errno = s->send_errno;
        return -1;
    }
    return 0;
}

int mxguest_shares_publish(struct mxguest_shares *s)
{
    struct mxga_share_status records[MXGA_SHARE_MAX];
    uint8_t payload[MXGA_SHARE_STATUS_HEADER_BYTES + MXGA_SHARE_MAX * MXGA_SHARE_STATUS_RECORD_BYTES];
    uint32_t count = 0, len = 0, i;
    for (i = 0; i < MXGA_SHARE_MAX; i++) {
        const struct share *sh = s->shares[i];
        if (!sh)
            continue;
        memset(&records[count], 0, sizeof records[count]);
        records[count].share_id = sh->id;
        records[count].state = sh->state;
        records[count].error = sh->state == MXGA_SHARE_STATE_FAILED ? sh->error : 0;
        records[count].flags = sh->flags;
        records[count].name = (const uint8_t *)sh->name;
        records[count].name_bytes = (uint32_t)strlen(sh->name);
        records[count].mount_point = (const uint8_t *)sh->mount_point;
        records[count].mount_point_bytes = (uint32_t)strlen(sh->mount_point);
        count++;
    }
    if (mxga_encode_share_status(records, count, payload, sizeof payload, &len) != MXGA_OK) {
        fprintf(stderr, "shared folder status not published\n");
        return 0;
    }
    return s->system.send(s->system.ctx, MXGA_OP_SHARE_STATUS, payload, len);
}

static int point_in_use(const struct mxguest_shares *s, const struct share *self, const char *point)
{
    uint32_t i;
    for (i = 0; i < MXGA_SHARE_MAX; i++)
        if (s->shares[i] && s->shares[i] != self && s->shares[i]->fd >= 0 &&
            !strcmp(s->shares[i]->mount_point, point))
            return 1;
    return 0;
}

static struct share *share_slot(struct mxguest_shares *s, uint32_t id)
{
    struct share *sh = share_find(s, id);
    uint32_t i, spare = MXGA_SHARE_MAX;
    if (sh)
        return sh;
    for (i = 0; i < MXGA_SHARE_MAX; i++) {
        if (!s->shares[i]) {
            spare = i;
            break;
        }
        if (s->shares[i]->fd < 0 && spare == MXGA_SHARE_MAX)
            spare = i;
    }
    if (spare == MXGA_SHARE_MAX)
        return NULL;
    if (!s->shares[spare] && !(s->shares[spare] = calloc(1, sizeof *s->shares[spare])))
        return NULL;
    sh = s->shares[spare];
    free(sh->handles);
    memset(sh, 0, sizeof *sh);
    sh->id = id;
    sh->fd = -1;
    return sh;
}

int mxguest_shares_mount(struct mxguest_shares *s, const uint8_t *payload, uint32_t len)
{
    struct mxga_share_mount mount;
    char point[MXGA_SHARE_PATH_BYTES + 1];
    struct share *sh;
    int fd;
    if (mxga_decode_mount_share(payload, len, &mount) != MXGA_OK) {
        fprintf(stderr, "shared folder mount request ignored\n");
        return 0;
    }
    sh = share_find(s, mount.share_id);
    if (mxguest_share_mount_point(mount.mount_point, mount.name, mount.share_id, 0, point,
                                  sizeof point) != 0 ||
        (point_in_use(s, sh, point) &&
         mxguest_share_mount_point(NULL, mount.name, mount.share_id, 1, point, sizeof point) != 0)) {
        fprintf(stderr, "shared folder %u has no usable mount point\n", mount.share_id);
        return 0;
    }
    if (sh && sh->fd >= 0) {
        if (sh->flags == mount.flags && !strcmp(sh->mount_point, point) &&
            !strcmp(sh->name, mount.name))
            return mxguest_shares_publish(s);
        share_disconnect(s, sh);
    }
    sh = share_slot(s, mount.share_id);
    if (!sh) {
        fprintf(stderr, "shared folder table full\n");
        return 0;
    }
    sh->flags = mount.flags;
    memcpy(sh->name, mount.name, sizeof sh->name);
    memcpy(sh->mount_point, point, sizeof sh->mount_point);
    sh->error = 0;
    if (nodes_init(sh) != 0) {
        sh->state = MXGA_SHARE_STATE_FAILED;
        sh->error = -ENOMEM;
        return mxguest_shares_publish(s);
    }
    fd = s->system.attach(s->system.ctx, point, (mount.flags & MXGA_SHARE_FLAG_READ_ONLY) != 0);
    if (fd < 0) {
        nodes_clear(sh);
        sh->state = MXGA_SHARE_STATE_FAILED;
        sh->error = fd;
        fprintf(stderr, "shared folder %u mount at %s failed: %s\n", sh->id, point, strerror(-fd));
    } else {
        sh->fd = fd;
        sh->state = MXGA_SHARE_STATE_MOUNTED;
    }
    return mxguest_shares_publish(s);
}

int mxguest_shares_unmount(struct mxguest_shares *s, const uint8_t *payload, uint32_t len)
{
    uint32_t id;
    struct share *sh;
    if (mxga_decode_unmount_share(payload, len, &id) != MXGA_OK) {
        fprintf(stderr, "shared folder unmount request ignored\n");
        return 0;
    }
    sh = share_find(s, id);
    if (sh) {
        share_disconnect(s, sh);
        sh->state = MXGA_SHARE_STATE_UNMOUNTED;
        sh->error = 0;
    }
    return mxguest_shares_publish(s);
}

static int share_read(struct mxguest_shares *s, struct share *sh, uint64_t now_ms)
{
    uint32_t reads = 0;
    while (sh->fd >= 0 && s->in_flight < MXGUEST_SHARE_MAX_IN_FLIGHT &&
           reads++ < SHARE_READS_PER_SERVICE && !s->send_failed) {
        ssize_t n = read(sh->fd, s->fuse_buf, MXGUEST_SHARE_FUSE_BUFFER);
        if (n < 0) {
            if (errno == EINTR || errno == ENOENT)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            share_disconnect(s, sh);
            sh->state = MXGA_SHARE_STATE_UNMOUNTED;
            return 1;
        }
        if (n == 0) {
            share_disconnect(s, sh);
            sh->state = MXGA_SHARE_STATE_UNMOUNTED;
            return 1;
        }
        fuse_request(s, sh, s->fuse_buf, (uint32_t)n, now_ms);
    }
    return 0;
}

int mxguest_shares_service(struct mxguest_shares *s, const fd_set *readable, uint64_t now_ms)
{
    uint32_t i;
    int changed = 0;
    for (i = 0; i < MXGUEST_SHARE_MAX_IN_FLIGHT; i++) {
        struct pending *p = &s->pending[i];
        if (p->used && p->deadline && p->deadline <= now_ms) {
            struct share *sh = share_find(s, p->share_id);
            if (p->opcode == FUSE_CREATE && p->fh && sh) {
                handle_untrack(sh, p->fh);
                release_queue(s, sh->id, p->fh);
            }
            fuse_reply(sh, p->unique, -EIO, NULL, 0);
            pending_abandon(s, p);
        }
    }
    for (i = 0; readable && i < MXGA_SHARE_MAX; i++) {
        struct share *sh = s->shares[i];
        if (sh && sh->fd >= 0 && FD_ISSET(sh->fd, readable))
            changed |= share_read(s, sh, now_ms);
    }
    if (s->send_failed) {
        errno = s->send_errno;
        return -1;
    }
    if (changed && mxguest_shares_publish(s) != 0)
        return -1;
    return send_releases(s, now_ms);
}

int mxguest_shares_watch(const struct mxguest_shares *s, fd_set *readable, int maxfd)
{
    uint32_t i;
    if (s->in_flight >= MXGUEST_SHARE_MAX_IN_FLIGHT)
        return maxfd;
    for (i = 0; i < MXGA_SHARE_MAX; i++) {
        const struct share *sh = s->shares[i];
        if (sh && sh->fd >= 0 && sh->fd < FD_SETSIZE) {
            FD_SET(sh->fd, readable);
            if (sh->fd > maxfd)
                maxfd = sh->fd;
        }
    }
    return maxfd;
}

uint64_t mxguest_shares_deadline(const struct mxguest_shares *s)
{
    uint64_t next = UINT64_MAX;
    uint32_t i;
    for (i = 0; i < MXGUEST_SHARE_MAX_IN_FLIGHT; i++)
        if (s->pending[i].used && s->pending[i].deadline && s->pending[i].deadline < next)
            next = s->pending[i].deadline;
    return next;
}

uint32_t mxguest_shares_in_flight(const struct mxguest_shares *s)
{
    return s->in_flight;
}

struct mxguest_shares *mxguest_shares_new(const struct mxguest_share_system *system)
{
    struct mxguest_shares *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->system = *system;
    s->next_request = 1;
    s->fuse_buf = malloc(MXGUEST_SHARE_FUSE_BUFFER);
    s->request_buf = malloc(SHARE_REQUEST_BYTES);
    s->reply_buf = malloc(SHARE_REPLY_BYTES);
    if (!s->fuse_buf || !s->request_buf || !s->reply_buf) {
        mxguest_shares_free(s);
        return NULL;
    }
    return s;
}

int mxguest_shares_close(struct mxguest_shares *s)
{
    uint32_t i;
    size_t r;
    int result = 0;
    for (i = 0; i < MXGA_SHARE_MAX; i++)
        if (s->shares[i] && s->shares[i]->fd >= 0) {
            share_disconnect(s, s->shares[i]);
            s->shares[i]->state = MXGA_SHARE_STATE_UNMOUNTED;
        }
    for (i = 0; i < MXGUEST_SHARE_MAX_IN_FLIGHT; i++)
        if (s->pending[i].used)
            pending_abandon(s, &s->pending[i]);
    for (r = 0; r < s->release_count && !result; r++) {
        struct mxga_fs_request request;
        uint32_t len = 0;
        memset(&request, 0, sizeof request);
        request.request_id = s->next_request++;
        request.share_id = s->releases[r].share_id;
        request.operation = MXGA_FS_OP_RELEASE;
        request.handle = s->releases[r].handle;
        if (mxga_encode_fs_request(&request, s->request_buf, SHARE_REQUEST_BYTES, &len) ==
            MXGA_OK)
            result = s->system.send(s->system.ctx, MXGA_OP_FS_REQUEST, s->request_buf, len);
    }
    s->release_count = 0;
    return result ? result : mxguest_shares_publish(s);
}

void mxguest_shares_free(struct mxguest_shares *s)
{
    uint32_t i;
    if (!s)
        return;
    for (i = 0; i < MXGA_SHARE_MAX; i++)
        if (s->shares[i]) {
            share_disconnect(s, s->shares[i]);
            free(s->shares[i]->handles);
            free(s->shares[i]);
        }
    for (i = 0; i < MXGUEST_SHARE_MAX_IN_FLIGHT; i++)
        free(s->pending[i].buffer);
    free(s->releases);
    free(s->fuse_buf);
    free(s->request_buf);
    free(s->reply_buf);
    free(s);
}

static int point_component_ok(const char *start, size_t len)
{
    size_t i;
    if (!len || (len == 1 && start[0] == '.') || (len == 2 && start[0] == '.' && start[1] == '.'))
        return 0;
    for (i = 0; i < len; i++)
        if ((unsigned char)start[i] < 0x20u || start[i] == 0x7f || start[i] == '\\')
            return 0;
    return 1;
}

static int point_allowed(const char *requested)
{
    static const char *roots[] = {"/media/", "/mnt/", "/run/media/"};
    size_t len, i, root = 0;
    const char *at;
    if (!requested)
        return 0;
    len = strlen(requested);
    while (len > 1 && requested[len - 1] == '/')
        len--;
    for (i = 0; i < sizeof roots / sizeof roots[0] && !root; i++)
        if (!strncmp(requested, roots[i], strlen(roots[i])))
            root = strlen(roots[i]);
    if (!root || len <= root || len > MXGA_SHARE_PATH_BYTES - 1)
        return 0;
    at = requested + root;
    while (at < requested + len) {
        const char *slash = memchr(at, '/', (size_t)(requested + len - at));
        size_t part = slash ? (size_t)(slash - at) : (size_t)(requested + len - at);
        if (!point_component_ok(at, part))
            return 0;
        at += part + 1;
    }
    return (int)len;
}

int mxguest_share_mount_point(const char *requested, const char *name, uint32_t share_id,
                              int distinct, char *out, size_t cap)
{
    char clean[MXGA_SHARE_NAME_BYTES + 1];
    size_t i, len = name ? strlen(name) : 0;
    int allowed = distinct ? 0 : point_allowed(requested), written;
    if (allowed) {
        if ((size_t)allowed >= cap)
            return -1;
        memcpy(out, requested, (size_t)allowed);
        out[allowed] = 0;
        return 0;
    }
    if (len > MXGA_SHARE_NAME_BYTES)
        len = MXGA_SHARE_NAME_BYTES;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        clean[i] = c < 0x20u || c == 0x7fu || c == '/' || c == '\\' ? '_' : (char)c;
    }
    clean[len] = 0;
    if (clean[0] == '.')
        clean[0] = '_';
    if (distinct || !len)
        written = snprintf(out, cap, "%s/%s-%u", MXGUEST_SHARE_FALLBACK_ROOT,
                           len ? clean : "share", share_id);
    else
        written = snprintf(out, cap, "%s/%s", MXGUEST_SHARE_FALLBACK_ROOT, clean);
    return written < 0 || (size_t)written >= cap ? -1 : 0;
}
