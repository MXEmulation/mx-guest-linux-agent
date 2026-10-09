/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#define _GNU_SOURCE
#include "share.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define FRAMES 256

struct frame {
    uint16_t opcode;
    uint32_t len;
    uint8_t *payload;
};

static struct frame frames[FRAMES];
static unsigned frame_count, frame_seen;
static int peers[MXGA_SHARE_MAX], peer_count;
static char attached[8][MXGA_SHARE_PATH_BYTES + 1], detached[8][MXGA_SHARE_PATH_BYTES + 1];
static int attached_read_only[8], attach_count, detach_count, attach_error;
static uint64_t now = 1000000;
static uint8_t buffer[MXGUEST_SHARE_FUSE_BUFFER];

static int fake_attach(void *ctx, const char *mount_point, int read_only)
{
    int pair[2];
    (void)ctx;
    if (attach_error)
        return attach_error;
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
    assert(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
    snprintf(attached[attach_count], sizeof attached[0], "%s", mount_point);
    attached_read_only[attach_count++] = read_only;
    peers[peer_count++] = pair[1];
    return pair[0];
}

static void fake_detach(void *ctx, const char *mount_point, int fd)
{
    (void)ctx;
    snprintf(detached[detach_count++], sizeof detached[0], "%s", mount_point);
    close(fd);
}

static void fake_owner(void *ctx, uint32_t *uid, uint32_t *gid)
{
    (void)ctx;
    *uid = 1000;
    *gid = 1001;
}

static int fake_send(void *ctx, uint16_t opcode, const uint8_t *payload, uint32_t len)
{
    (void)ctx;
    assert(frame_count < FRAMES);
    frames[frame_count].opcode = opcode;
    frames[frame_count].len = len;
    frames[frame_count].payload = malloc(len ? len : 1);
    memcpy(frames[frame_count].payload, payload, len);
    frame_count++;
    return 0;
}

static const struct mxguest_share_system fake = {NULL, fake_attach, fake_detach, fake_owner,
                                                 fake_send};

static const struct frame *next_frame(uint16_t opcode)
{
    while (frame_seen < frame_count) {
        const struct frame *frame = &frames[frame_seen++];
        if (frame->opcode == opcode)
            return frame;
    }
    return NULL;
}

static struct mxga_fs_request next_request_at(int line)
{
    struct mxga_fs_request request;
    const struct frame *frame = next_frame(MXGA_OP_FS_REQUEST);
    if (!frame)
        fprintf(stderr, "no filesystem request at line %d\n", line);
    assert(frame);
    assert(mxga_decode_fs_request(frame->payload, frame->len, &request) == MXGA_OK);
    return request;
}

#define next_request() next_request_at(__LINE__)

static void no_request(void)
{
    assert(frame_seen == frame_count);
}

static int path_is(const struct mxga_fs_request *request, const char *path)
{
    return request->path_bytes == strlen(path) &&
           (!request->path_bytes || !memcmp(request->path, path, request->path_bytes));
}

static void service(struct mxguest_shares *shares)
{
    fd_set set;
    FD_ZERO(&set);
    mxguest_shares_watch(shares, &set, -1);
    assert(mxguest_shares_service(shares, &set, now) == 0);
}

static void fuse_send(struct mxguest_shares *shares, int peer, uint32_t opcode, uint64_t unique,
                      uint64_t nodeid, const void *body, uint32_t body_len,
                      const char *name, const char *name2)
{
    uint8_t message[8192];
    struct fuse_in_header in;
    uint32_t len = (uint32_t)sizeof in + body_len;
    memset(&in, 0, sizeof in);
    if (body_len)
        memcpy(message + sizeof in, body, body_len);
    if (name) {
        memcpy(message + len, name, strlen(name) + 1);
        len += (uint32_t)strlen(name) + 1;
    }
    if (name2) {
        memcpy(message + len, name2, strlen(name2) + 1);
        len += (uint32_t)strlen(name2) + 1;
    }
    in.len = len;
    in.opcode = opcode;
    in.unique = unique;
    in.nodeid = nodeid;
    memcpy(message, &in, sizeof in);
    assert(send(peer, message, len, 0) == (ssize_t)len);
    service(shares);
}

static int fuse_recv(int peer, uint64_t unique, void *body, uint32_t *body_len)
{
    struct fuse_out_header out;
    ssize_t n = recv(peer, buffer, sizeof buffer, MSG_DONTWAIT);
    assert(n >= (ssize_t)sizeof out);
    memcpy(&out, buffer, sizeof out);
    assert(out.len == (uint32_t)n && out.unique == unique);
    if (body)
        memcpy(body, buffer + sizeof out, (size_t)n - sizeof out);
    if (body_len)
        *body_len = (uint32_t)n - (uint32_t)sizeof out;
    return out.error;
}

static void fuse_silent(int peer)
{
    assert(recv(peer, buffer, sizeof buffer, MSG_DONTWAIT) < 0 && errno == EAGAIN);
}

static void respond(struct mxguest_shares *shares, const struct mxga_fs_request *request,
                    int32_t status, uint64_t handle, const void *data, uint32_t len)
{
    struct mxga_fs_response response;
    uint8_t payload[MXGA_FS_RESPONSE_HEADER_BYTES + 8192];
    uint32_t payload_len = 0;
    memset(&response, 0, sizeof response);
    response.request_id = request->request_id;
    response.handle = handle ? handle : request->handle;
    response.status = status;
    response.operation = request->operation;
    response.data = data;
    response.data_bytes = len;
    assert(mxga_encode_fs_response(&response, payload, sizeof payload, &payload_len) == MXGA_OK);
    assert(mxguest_shares_response(shares, payload, payload_len, now) == 0);
}

static void respond_attr(struct mxguest_shares *shares, const struct mxga_fs_request *request,
                         uint64_t ino, uint32_t mode, uint64_t size)
{
    struct mxga_fs_attributes attributes;
    uint8_t data[MXGA_FS_ATTRIBUTES_BYTES];
    uint32_t len = 0;
    memset(&attributes, 0, sizeof attributes);
    attributes.ino = ino;
    attributes.mode = mode;
    attributes.size = size;
    attributes.nlink = 1;
    attributes.uid = 501;
    attributes.gid = 20;
    attributes.mtime_s = 1700000000;
    assert(mxga_encode_fs_attributes(&attributes, data, sizeof data, &len) == MXGA_OK);
    respond(shares, request, 0, 0, data, len);
}

static void mount_share(struct mxguest_shares *shares, uint32_t id, const char *name,
                        const char *point, uint32_t flags)
{
    struct mxga_share_mount mount;
    uint8_t payload[MXGA_SHARE_MOUNT_BYTES];
    uint32_t len = 0;
    memset(&mount, 0, sizeof mount);
    mount.share_id = id;
    mount.flags = flags;
    mount.name_bytes = (uint32_t)strlen(name);
    memcpy(mount.name, name, mount.name_bytes);
    mount.mount_point_bytes = (uint32_t)strlen(point);
    memcpy(mount.mount_point, point, mount.mount_point_bytes);
    assert(mxga_encode_mount_share(&mount, payload, sizeof payload, &len) == MXGA_OK);
    assert(mxguest_shares_mount(shares, payload, len) == 0);
}

static uint32_t status_table(struct mxga_share_status *records)
{
    const struct frame *frame = NULL;
    uint32_t count = 0;
    unsigned i, last = frame_seen;
    for (i = frame_seen; i < frame_count; i++)
        if (frames[i].opcode == MXGA_OP_SHARE_STATUS) {
            frame = &frames[i];
            last = i + 1;
        }
    frame_seen = last;
    assert(frame);
    assert(mxga_decode_share_status(frame->payload, frame->len, records, MXGA_SHARE_MAX, &count) ==
           MXGA_OK);
    return count;
}

static int text_is(const uint8_t *text, uint32_t len, const char *expected)
{
    return len == strlen(expected) && !memcmp(text, expected, len);
}

static void test_mount_points(void)
{
    char out[MXGA_SHARE_PATH_BYTES + 1];
    assert(!mxguest_share_mount_point("/media/docs", "Docs", 1, 0, out, sizeof out) &&
           !strcmp(out, "/media/docs"));
    assert(!mxguest_share_mount_point("/mnt/a/b/", "x", 1, 0, out, sizeof out) &&
           !strcmp(out, "/mnt/a/b"));
    assert(!mxguest_share_mount_point("/run/media/user/Share", "x", 1, 0, out, sizeof out) &&
           !strcmp(out, "/run/media/user/Share"));
    assert(!mxguest_share_mount_point("/etc/passwd", "Docs", 1, 0, out, sizeof out) &&
           !strcmp(out, "/media/mxshare/Docs"));
    assert(!mxguest_share_mount_point("/media/../etc", "Docs", 1, 0, out, sizeof out) &&
           !strcmp(out, "/media/mxshare/Docs"));
    assert(!mxguest_share_mount_point("/media/a//b", "Docs", 1, 0, out, sizeof out) &&
           !strcmp(out, "/media/mxshare/Docs"));
    assert(!mxguest_share_mount_point("/media/", "Docs", 1, 0, out, sizeof out) &&
           !strcmp(out, "/media/mxshare/Docs"));
    assert(!mxguest_share_mount_point("/mediax/a", "Docs", 1, 0, out, sizeof out) &&
           !strcmp(out, "/media/mxshare/Docs"));
    assert(!mxguest_share_mount_point("media/a", "Docs", 1, 0, out, sizeof out) &&
           !strcmp(out, "/media/mxshare/Docs"));
    assert(!mxguest_share_mount_point("/media/a\nb", "Docs", 1, 0, out, sizeof out) &&
           !strcmp(out, "/media/mxshare/Docs"));
    assert(!mxguest_share_mount_point("", "../a/b\\c", 1, 0, out, sizeof out) &&
           !strcmp(out, "/media/mxshare/_._a_b_c"));
    assert(!mxguest_share_mount_point("", ".", 4, 0, out, sizeof out) &&
           !strcmp(out, "/media/mxshare/_"));
    assert(!mxguest_share_mount_point("/media/docs", "Docs", 9, 1, out, sizeof out) &&
           !strcmp(out, "/media/mxshare/Docs-9"));
    assert(!mxguest_share_mount_point(NULL, "", 9, 0, out, sizeof out) &&
           !strcmp(out, "/media/mxshare/share-9"));
    assert(mxguest_share_mount_point("/media/docs", "Docs", 1, 0, out, 5) == -1);
}

int main(void)
{
    struct mxguest_shares *shares;
    struct mxga_share_status records[MXGA_SHARE_MAX];
    struct mxga_fs_request request, requests[MXGUEST_SHARE_MAX_IN_FLIGHT];
    struct fuse_init_in init;
    struct fuse_init_out init_out;
    struct fuse_entry_out entry;
    struct fuse_attr_out attr_out;
    struct fuse_open_in open_in;
    struct fuse_open_out open_out;
    struct fuse_read_in read_in;
    struct fuse_write_out write_out;
    struct fuse_mkdir_in mkdir_in;
    struct fuse_create_in create_in;
    struct fuse_rename2_in rename_in;
    struct fuse_setattr_in setattr_in;
    struct fuse_statfs_out statfs_out;
    struct fuse_forget_in forget;
    struct fuse_release_in release_in;
    struct mxga_fs_statfs statfs = {4096, 1000, 500, 400, 100, 50, 255};
    uint8_t body[8192], dirents[64];
    uint32_t len = 0, used = 0, count, i;
    uint64_t file_node, created_node, lookup_id;
    int peer;
    fd_set set;

    test_mount_points();
    shares = mxguest_shares_new(&fake);
    assert(shares);
    assert(mxguest_shares_publish(shares) == 0);
    assert(status_table(records) == 0);

    mount_share(shares, 3, "Docs", "/etc/evil", MXGA_SHARE_FLAG_AUTOMOUNT);
    assert(attach_count == 1 && !strcmp(attached[0], "/media/mxshare/Docs") && !attached_read_only[0]);
    count = status_table(records);
    assert(count == 1 && records[0].share_id == 3 && records[0].state == MXGA_SHARE_STATE_MOUNTED &&
           records[0].flags == MXGA_SHARE_FLAG_AUTOMOUNT &&
           text_is(records[0].name, records[0].name_bytes, "Docs") &&
           text_is(records[0].mount_point, records[0].mount_point_bytes, "/media/mxshare/Docs"));
    peer = peers[0];

    memset(&init, 0, sizeof init);
    init.major = 7;
    init.minor = 45;
    init.max_readahead = 131072;
    init.flags = FUSE_ASYNC_READ | FUSE_MAX_PAGES | FUSE_POSIX_LOCKS;
    fuse_send(shares, peer, FUSE_GETATTR, 1, FUSE_ROOT_ID, NULL, 0, NULL, NULL);
    assert(fuse_recv(peer, 1, NULL, NULL) == -EIO);
    fuse_send(shares, peer, FUSE_INIT, 2, 0, &init, sizeof init, NULL, NULL);
    assert(fuse_recv(peer, 2, &init_out, &len) == 0 && len == sizeof init_out);
    assert(init_out.major == 7 && init_out.minor == 45 && init_out.max_write == MXGA_FS_MAX_IO_BYTES &&
           init_out.flags == (FUSE_ASYNC_READ | FUSE_MAX_PAGES) && init_out.max_pages >= 8);
    no_request();

    fuse_send(shares, peer, FUSE_LOOKUP, 3, FUSE_ROOT_ID, NULL, 0, "a.txt", NULL);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_GETATTR && request.share_id == 3 &&
           path_is(&request, "a.txt"));
    fuse_silent(peer);
    respond_attr(shares, &request, 41, S_IFREG | 0644, 5);
    assert(fuse_recv(peer, 3, &entry, &len) == 0 && len == sizeof entry);
    assert(entry.nodeid > FUSE_ROOT_ID && entry.attr.ino == 41 && entry.attr.size == 5 &&
           entry.attr.uid == 1000 && entry.attr.gid == 1001 && entry.attr.mode == (S_IFREG | 0644) &&
           entry.entry_valid == 1);
    file_node = entry.nodeid;
    fuse_send(shares, peer, FUSE_LOOKUP, 4, FUSE_ROOT_ID, NULL, 0, "a.txt", NULL);
    respond_attr(shares, (request = next_request(), &request), 41, S_IFREG | 0644, 5);
    assert(fuse_recv(peer, 4, &entry, NULL) == 0 && entry.nodeid == file_node);

    fuse_send(shares, peer, FUSE_GETATTR, 5, FUSE_ROOT_ID, NULL, 0, NULL, NULL);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_GETATTR && path_is(&request, ""));
    respond_attr(shares, &request, 2, S_IFDIR | 0640, 0);
    assert(fuse_recv(peer, 5, &attr_out, NULL) == 0 && attr_out.attr.mode == (S_IFDIR | 0750));

    memset(&open_in, 0, sizeof open_in);
    open_in.flags = O_RDWR | O_APPEND;
    fuse_send(shares, peer, FUSE_OPEN, 6, file_node, &open_in, sizeof open_in, NULL, NULL);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_OPEN && path_is(&request, "a.txt") &&
           request.flags == (MXGA_FS_OPEN_READ | MXGA_FS_OPEN_WRITE | MXGA_FS_OPEN_APPEND));
    respond(shares, &request, 0, 77, NULL, 0);
    assert(fuse_recv(peer, 6, &open_out, NULL) == 0 && open_out.fh == 77);

    memset(&read_in, 0, sizeof read_in);
    read_in.fh = 77;
    read_in.size = 10;
    fuse_send(shares, peer, FUSE_READ, 7, file_node, &read_in, sizeof read_in, NULL, NULL);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_READ && request.handle == 77 && request.length == 10 &&
           !request.path_bytes);
    respond(shares, &request, 0, 0, "abcd", 4);
    fuse_silent(peer);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_READ && request.offset == 4 && request.length == 6);
    respond(shares, &request, 0, 0, NULL, 0);
    assert(fuse_recv(peer, 7, body, &len) == 0 && len == 4 && !memcmp(body, "abcd", 4));
    read_in.size = 4;
    fuse_send(shares, peer, FUSE_READ, 8, file_node, &read_in, sizeof read_in, NULL, NULL);
    respond(shares, (request = next_request(), &request), 0, 0, "wxyz", 4);
    assert(fuse_recv(peer, 8, body, &len) == 0 && len == 4 && !memcmp(body, "wxyz", 4));

    {
        struct fuse_write_in write_in;
        memset(&write_in, 0, sizeof write_in);
        write_in.fh = 77;
        write_in.offset = 9;
        write_in.size = 3;
        memcpy(body, &write_in, sizeof write_in);
        memcpy(body + sizeof write_in, "xyz", 3);
        fuse_send(shares, peer, FUSE_WRITE, 9, file_node, body, sizeof write_in + 3, NULL, NULL);
    }
    request = next_request();
    assert(request.operation == MXGA_FS_OP_WRITE && request.handle == 77 && request.offset == 9 &&
           request.data_bytes == 3 && !memcmp(request.data, "xyz", 3));
    respond(shares, &request, 0, 0, NULL, 0);
    assert(fuse_recv(peer, 9, &write_out, NULL) == 0 && write_out.size == 3);

    memset(&mkdir_in, 0, sizeof mkdir_in);
    mkdir_in.mode = S_IFDIR | 0755;
    fuse_send(shares, peer, FUSE_MKDIR, 10, FUSE_ROOT_ID, &mkdir_in, sizeof mkdir_in, "d", NULL);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_MKDIR && path_is(&request, "d") && request.mode == 0755);
    respond(shares, &request, 0, 0, NULL, 0);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_GETATTR && path_is(&request, "d"));
    respond_attr(shares, &request, 50, S_IFDIR | 0755, 0);
    assert(fuse_recv(peer, 10, &entry, NULL) == 0);

    memset(&create_in, 0, sizeof create_in);
    create_in.flags = O_WRONLY | O_CREAT | O_TRUNC;
    create_in.mode = S_IFREG | 0600;
    fuse_send(shares, peer, FUSE_CREATE, 11, entry.nodeid, &create_in, sizeof create_in, "n", NULL);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_CREATE && path_is(&request, "d/n") && request.mode == 0600 &&
           request.flags == (MXGA_FS_OPEN_WRITE | MXGA_FS_OPEN_CREATE | MXGA_FS_OPEN_EXCLUSIVE));
    respond(shares, &request, -EEXIST, 0, NULL, 0);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_OPEN && path_is(&request, "d/n") &&
           request.flags == (MXGA_FS_OPEN_WRITE | MXGA_FS_OPEN_TRUNCATE));
    respond(shares, &request, 0, 88, NULL, 0);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_GETATTR && path_is(&request, "d/n"));
    respond_attr(shares, &request, 51, S_IFREG | 0600, 0);
    assert(fuse_recv(peer, 11, body, &len) == 0 && len == sizeof entry + sizeof open_out);
    memcpy(&entry, body, sizeof entry);
    memcpy(&open_out, body + sizeof entry, sizeof open_out);
    assert(open_out.fh == 88 && entry.attr.ino == 51);
    created_node = entry.nodeid;

    create_in.flags = O_WRONLY | O_CREAT | O_EXCL;
    fuse_send(shares, peer, FUSE_CREATE, 12, FUSE_ROOT_ID, &create_in, sizeof create_in, "e", NULL);
    respond(shares, (request = next_request(), &request), -EEXIST, 0, NULL, 0);
    assert(fuse_recv(peer, 12, NULL, NULL) == -EEXIST);
    no_request();

    memset(&rename_in, 0, sizeof rename_in);
    rename_in.newdir = FUSE_ROOT_ID;
    fuse_send(shares, peer, FUSE_RENAME2, 13, FUSE_ROOT_ID, NULL, 0, NULL, NULL);
    assert(fuse_recv(peer, 13, NULL, NULL) == -EINVAL);
    rename_in.flags = 1;
    fuse_send(shares, peer, FUSE_RENAME2, 14, FUSE_ROOT_ID, &rename_in, sizeof rename_in, "d", "z");
    assert(fuse_recv(peer, 14, NULL, NULL) == -EINVAL);
    no_request();
    rename_in.flags = 0;
    {
        uint64_t parent;
        fuse_send(shares, peer, FUSE_LOOKUP, 15, FUSE_ROOT_ID, NULL, 0, "d", NULL);
        respond_attr(shares, (request = next_request(), &request), 50, S_IFDIR | 0755, 0);
        assert(fuse_recv(peer, 15, &entry, NULL) == 0);
        parent = entry.nodeid;
        fuse_send(shares, peer, FUSE_RENAME2, 16, parent, &rename_in, sizeof rename_in, "n", "m");
    }
    request = next_request();
    assert(request.operation == MXGA_FS_OP_RENAME && path_is(&request, "d/n") &&
           request.second_path_bytes == 1 && request.second_path[0] == 'm');
    respond(shares, &request, 0, 0, NULL, 0);
    assert(fuse_recv(peer, 16, NULL, NULL) == 0);
    fuse_send(shares, peer, FUSE_GETATTR, 17, created_node, NULL, 0, NULL, NULL);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_GETATTR && path_is(&request, "m"));
    respond_attr(shares, &request, 51, S_IFREG | 0600, 0);
    assert(fuse_recv(peer, 17, NULL, NULL) == 0);

    memset(&read_in, 0, sizeof read_in);
    read_in.offset = 3;
    read_in.size = 4096;
    fuse_send(shares, peer, FUSE_READDIR, 18, FUSE_ROOT_ID, &read_in, sizeof read_in, NULL, NULL);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_READDIR && path_is(&request, "") && request.offset == 3 &&
           request.length == 4096 / 32);
    {
        struct mxga_fs_dirent dirent = {9, S_IFDIR | 0755, (const uint8_t *)"sub", 3};
        assert(mxga_encode_fs_dirent(&dirent, dirents, sizeof dirents, &len) == MXGA_OK);
        used = len;
        dirent.inode = 0;
        dirent.mode = S_IFLNK | 0777;
        dirent.name = (const uint8_t *)"link";
        dirent.name_bytes = 4;
        assert(mxga_encode_fs_dirent(&dirent, dirents + used, sizeof dirents - used, &len) == MXGA_OK);
        used += len;
    }
    respond(shares, &request, 0, 0, dirents, used);
    assert(fuse_recv(peer, 18, body, &len) == 0);
    {
        struct fuse_dirent first, second;
        memcpy(&first, body, sizeof first);
        assert(first.ino == 9 && first.off == 4 && first.namelen == 3 && first.type == DT_DIR &&
               !memcmp(body + FUSE_NAME_OFFSET, "sub", 3));
        memcpy(&second, body + FUSE_DIRENT_SIZE(&first), sizeof second);
        assert(second.ino == 6 && second.off == 5 && second.type == DT_LNK &&
               !memcmp(body + FUSE_DIRENT_SIZE(&first) + FUSE_NAME_OFFSET, "link", 4));
        assert(len == FUSE_DIRENT_SIZE(&first) + FUSE_DIRENT_SIZE(&second));
    }
    read_in.size = 40;
    fuse_send(shares, peer, FUSE_READDIR, 19, FUSE_ROOT_ID, &read_in, sizeof read_in, NULL, NULL);
    request = next_request();
    assert(request.length == 1);
    respond(shares, &request, 0, 0, dirents, used);
    assert(fuse_recv(peer, 19, body, &len) == 0 && len == 32);
    fuse_send(shares, peer, FUSE_READDIR, 20, FUSE_ROOT_ID, &read_in, sizeof read_in, NULL, NULL);
    respond(shares, (request = next_request(), &request), 0, 0, dirents, 5);
    assert(fuse_recv(peer, 20, NULL, NULL) == -EIO);

    memset(&setattr_in, 0, sizeof setattr_in);
    setattr_in.valid = FATTR_SIZE | FATTR_FH | FATTR_MODE | FATTR_MTIME | FATTR_MTIME_NOW;
    setattr_in.fh = 77;
    setattr_in.size = 2;
    setattr_in.mode = S_IFREG | 0640;
    fuse_send(shares, peer, FUSE_SETATTR, 21, file_node, &setattr_in, sizeof setattr_in, NULL, NULL);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_TRUNCATE && request.handle == 77 && request.offset == 2);
    respond(shares, &request, 0, 0, NULL, 0);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_SETATTR && path_is(&request, "a.txt") &&
           request.flags == (MXGA_FS_SETATTR_MODE | MXGA_FS_SETATTR_MTIME) &&
           request.data_bytes == MXGA_FS_ATTRIBUTES_BYTES);
    {
        struct mxga_fs_attributes sent;
        assert(mxga_decode_fs_attributes(request.data, request.data_bytes, &sent) == MXGA_OK);
        assert(sent.mode == 0640 && sent.mtime_s > 1600000000u);
    }
    respond(shares, &request, 0, 0, NULL, 0);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_GETATTR && path_is(&request, "a.txt"));
    respond_attr(shares, &request, 41, S_IFREG | 0640, 2);
    assert(fuse_recv(peer, 21, &attr_out, NULL) == 0 && attr_out.attr.size == 2);
    setattr_in.valid = FATTR_UID;
    fuse_send(shares, peer, FUSE_SETATTR, 22, file_node, &setattr_in, sizeof setattr_in, NULL, NULL);
    assert(fuse_recv(peer, 22, NULL, NULL) == -EPERM);
    setattr_in.valid = FATTR_SIZE;
    setattr_in.size = 0;
    fuse_send(shares, peer, FUSE_SETATTR, 23, file_node, &setattr_in, sizeof setattr_in, NULL, NULL);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_SETATTR && request.flags == MXGA_FS_SETATTR_SIZE);
    respond(shares, &request, -EACCES, 0, NULL, 0);
    assert(fuse_recv(peer, 23, NULL, NULL) == -EACCES);
    fuse_send(shares, peer, FUSE_SYMLINK, 24, FUSE_ROOT_ID, NULL, 0, "s", "t");
    assert(fuse_recv(peer, 24, NULL, NULL) == -ENOSYS);
    fuse_send(shares, peer, FUSE_FLUSH, 25, file_node, body, 24, NULL, NULL);
    assert(fuse_recv(peer, 25, NULL, NULL) == 0);
    fuse_send(shares, peer, FUSE_LOOKUP, 26, FUSE_ROOT_ID, NULL, 0, "back\\slash", NULL);
    assert(fuse_recv(peer, 26, NULL, NULL) == -EINVAL);
    no_request();

    fuse_send(shares, peer, FUSE_STATFS, 27, FUSE_ROOT_ID, NULL, 0, NULL, NULL);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_STATFS && path_is(&request, ""));
    assert(mxga_encode_fs_statfs(&statfs, body, sizeof body, &len) == MXGA_OK);
    respond(shares, &request, 0, 0, body, len);
    assert(fuse_recv(peer, 27, &statfs_out, NULL) == 0 && statfs_out.st.bsize == 4096 &&
           statfs_out.st.bavail == 400 && statfs_out.st.namelen == 255);

    fuse_send(shares, peer, FUSE_READLINK, 28, created_node, NULL, 0, NULL, NULL);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_READLINK && path_is(&request, "m"));
    respond(shares, &request, 0, 0, "target", 6);
    assert(fuse_recv(peer, 28, body, &len) == 0 && len == 6 && !memcmp(body, "target", 6));

    fuse_send(shares, peer, FUSE_UNLINK, 29, FUSE_ROOT_ID, NULL, 0, "a.txt", NULL);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_UNLINK && path_is(&request, "a.txt"));
    respond(shares, &request, 0, 0, NULL, 0);
    assert(fuse_recv(peer, 29, NULL, NULL) == 0);
    fuse_send(shares, peer, FUSE_GETATTR, 30, file_node, NULL, 0, NULL, NULL);
    assert(fuse_recv(peer, 30, NULL, NULL) == -ENOENT);
    fuse_send(shares, peer, FUSE_LOOKUP, 31, FUSE_ROOT_ID, NULL, 0, "a.txt", NULL);
    respond_attr(shares, (request = next_request(), &request), 60, S_IFREG | 0644, 0);
    assert(fuse_recv(peer, 31, &entry, NULL) == 0 && entry.nodeid != file_node);
    lookup_id = entry.nodeid;
    memset(&forget, 0, sizeof forget);
    forget.nlookup = 1;
    fuse_send(shares, peer, FUSE_FORGET, 32, lookup_id, &forget, sizeof forget, NULL, NULL);
    fuse_silent(peer);
    fuse_send(shares, peer, FUSE_LOOKUP, 33, FUSE_ROOT_ID, NULL, 0, "a.txt", NULL);
    respond_attr(shares, (request = next_request(), &request), 60, S_IFREG | 0644, 0);
    assert(fuse_recv(peer, 33, &entry, NULL) == 0 && entry.nodeid != lookup_id);

    fuse_send(shares, peer, FUSE_GETATTR, 34, FUSE_ROOT_ID, NULL, 0, NULL, NULL);
    request = next_request();
    assert(mxguest_shares_deadline(shares) == now + MXGUEST_SHARE_TIMEOUT_MS);
    now += MXGUEST_SHARE_TIMEOUT_MS;
    service(shares);
    assert(fuse_recv(peer, 34, NULL, NULL) == -EIO && mxguest_shares_in_flight(shares) == 0);
    respond_attr(shares, &request, 2, S_IFDIR | 0755, 0);
    fuse_silent(peer);
    open_in.flags = O_RDONLY;
    fuse_send(shares, peer, FUSE_OPEN, 35, created_node, &open_in, sizeof open_in, NULL, NULL);
    request = next_request();
    now += MXGUEST_SHARE_TIMEOUT_MS;
    service(shares);
    assert(fuse_recv(peer, 35, NULL, NULL) == -EIO);
    respond(shares, &request, 0, 99, NULL, 0);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_RELEASE && request.handle == 99 && request.share_id == 3);
    respond(shares, &request, 0, 0, NULL, 0);
    fuse_silent(peer);
    assert(mxguest_shares_in_flight(shares) == 0);

    for (i = 0; i < MXGUEST_SHARE_MAX_IN_FLIGHT; i++) {
        fuse_send(shares, peer, FUSE_GETATTR, 100 + i, FUSE_ROOT_ID, NULL, 0, NULL, NULL);
        requests[i] = next_request();
    }
    FD_ZERO(&set);
    assert(mxguest_shares_watch(shares, &set, -1) == -1 &&
           mxguest_shares_in_flight(shares) == MXGUEST_SHARE_MAX_IN_FLIGHT);
    fuse_send(shares, peer, FUSE_GETATTR, 200, FUSE_ROOT_ID, NULL, 0, NULL, NULL);
    no_request();
    for (i = 0; i < MXGUEST_SHARE_MAX_IN_FLIGHT; i++) {
        respond_attr(shares, &requests[i], 2, S_IFDIR | 0755, 0);
        assert(fuse_recv(peer, 100 + i, NULL, NULL) == 0);
    }
    service(shares);
    respond_attr(shares, (request = next_request(), &request), 2, S_IFDIR | 0755, 0);
    assert(fuse_recv(peer, 200, NULL, NULL) == 0);

    memset(&release_in, 0, sizeof release_in);
    release_in.fh = 77;
    fuse_send(shares, peer, FUSE_RELEASE, 201, file_node, &release_in, sizeof release_in, NULL, NULL);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_RELEASE && request.handle == 77);
    respond(shares, &request, 0, 0, NULL, 0);
    assert(fuse_recv(peer, 201, NULL, NULL) == 0);

    mount_share(shares, 4, "Same", "/mnt/same", MXGA_SHARE_FLAG_READ_ONLY);
    mount_share(shares, 5, "Other", "/mnt/same", 0);
    assert(attach_count == 3 && !strcmp(attached[1], "/mnt/same") && attached_read_only[1] &&
           !strcmp(attached[2], "/media/mxshare/Other-5"));
    attach_error = -EACCES;
    mount_share(shares, 6, "Bad", "/mnt/bad", 0);
    attach_error = 0;
    count = status_table(records);
    assert(count == 4 && records[3].share_id == 6 && records[3].state == MXGA_SHARE_STATE_FAILED &&
           records[3].error == -EACCES);

    {
        uint8_t payload[MXGA_SHARE_UNMOUNT_BYTES];
        assert(mxga_encode_unmount_share(3, payload, sizeof payload, &len) == MXGA_OK);
        assert(mxguest_shares_unmount(shares, payload, len) == 0);
    }
    assert(detach_count == 1 && !strcmp(detached[0], "/media/mxshare/Docs"));
    count = status_table(records);
    assert(count == 4 && records[0].state == MXGA_SHARE_STATE_UNMOUNTED && !records[0].error);
    service(shares);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_RELEASE && request.handle == 88 && request.share_id == 3);
    respond(shares, &request, 0, 0, NULL, 0);

    fuse_send(shares, peers[1], FUSE_INIT, 1, 0, &init, sizeof init, NULL, NULL);
    assert(fuse_recv(peers[1], 1, NULL, NULL) == 0);
    open_in.flags = O_RDONLY;
    fuse_send(shares, peers[1], FUSE_LOOKUP, 2, FUSE_ROOT_ID, NULL, 0, "f", NULL);
    respond_attr(shares, (request = next_request(), &request), 70, S_IFREG | 0444, 1);
    assert(fuse_recv(peers[1], 2, &entry, NULL) == 0);
    fuse_send(shares, peers[1], FUSE_OPEN, 3, entry.nodeid, &open_in, sizeof open_in, NULL, NULL);
    respond(shares, (request = next_request(), &request), 0, 123, NULL, 0);
    assert(fuse_recv(peers[1], 3, NULL, NULL) == 0);
    close(peers[1]);
    service(shares);
    assert(detach_count == 2 && !strcmp(detached[1], "/mnt/same"));
    count = status_table(records);
    assert(records[1].share_id == 4 && records[1].state == MXGA_SHARE_STATE_UNMOUNTED);
    request = next_request();
    assert(request.operation == MXGA_FS_OP_RELEASE && request.handle == 123 && request.share_id == 4);
    respond(shares, &request, 0, 0, NULL, 0);

    assert(mxguest_shares_close(shares) == 0);
    assert(detach_count == 3 && !strcmp(detached[2], "/media/mxshare/Other-5"));
    count = status_table(records);
    for (i = 0; i < count; i++)
        assert(records[i].state != MXGA_SHARE_STATE_MOUNTED);
    mxguest_shares_free(shares);
    for (i = 0; i < frame_count; i++)
        free(frames[i].payload);
    puts("shared folder translation ok");
    return 0;
}
