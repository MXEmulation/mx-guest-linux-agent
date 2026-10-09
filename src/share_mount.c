/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#define _GNU_SOURCE
#include "share.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define SHARE_MOUNTINFO "/proc/self/mountinfo"

static int open_directories(const char *path)
{
    char part[MXGA_SHARE_PATH_BYTES + 1];
    const char *at = path;
    int dir = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0 || path[0] != '/')
        goto fail;
    while (*at) {
        size_t len;
        int next;
        while (*at == '/')
            at++;
        len = strcspn(at, "/");
        if (!len)
            break;
        if (len >= sizeof part) {
            errno = ENAMETOOLONG;
            goto fail;
        }
        memcpy(part, at, len);
        part[len] = 0;
        at += len;
        if (!strcmp(part, ".") || !strcmp(part, "..")) {
            errno = EINVAL;
            goto fail;
        }
        if (mkdirat(dir, part, 0755) != 0 && errno != EEXIST)
            goto fail;
        next = openat(dir, part, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0)
            goto fail;
        close(dir);
        dir = next;
    }
    return dir;
fail:
    if (dir >= 0) {
        int saved = errno;
        close(dir);
        errno = saved;
    }
    return -1;
}

int mxguest_share_attach(void *ctx, const char *mount_point, int read_only)
{
    char options[160], target[64];
    int dir, fuse, error;
    (void)ctx;
    dir = open_directories(mount_point);
    if (dir < 0)
        return -errno;
    fuse = open("/dev/fuse", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fuse < 0 || fuse >= FD_SETSIZE) {
        error = fuse < 0 ? errno : EMFILE;
        if (fuse >= 0)
            close(fuse);
        close(dir);
        return -error;
    }
    snprintf(options, sizeof options,
             "fd=%d,rootmode=40000,user_id=0,group_id=0,allow_other,default_permissions,"
             "max_read=%u",
             fuse, MXGA_FS_MAX_IO_BYTES);
    snprintf(target, sizeof target, "/proc/self/fd/%d", dir);
    if (mount("mxshare", target, MXGUEST_SHARE_FS_TYPE,
              MS_NOSUID | MS_NODEV | (read_only ? MS_RDONLY : 0), options) != 0) {
        error = errno;
        close(fuse);
        close(dir);
        return -error;
    }
    close(dir);
    return fuse;
}

static void unescape(char *text)
{
    char *out = text;
    while (*text) {
        if (text[0] == '\\' && text[1] >= '0' && text[1] <= '3' && text[2] >= '0' &&
            text[2] <= '7' && text[3] >= '0' && text[3] <= '7') {
            *out++ = (char)((text[1] - '0') * 64 + (text[2] - '0') * 8 + (text[3] - '0'));
            text += 4;
        } else {
            *out++ = *text++;
        }
    }
    *out = 0;
}

static int mountinfo_entry(char *line, char **point)
{
    char *field = line, *separator = strstr(line, " - ");
    unsigned i;
    if (!separator)
        return 0;
    for (i = 0; i < 4 && field; i++) {
        field = strchr(field, ' ');
        if (field)
            field++;
    }
    if (!field)
        return 0;
    *point = field;
    field = strchr(field, ' ');
    if (!field)
        return 0;
    *field = 0;
    unescape(*point);
    separator += 3;
    return !strncmp(separator, MXGUEST_SHARE_FS_TYPE " ", strlen(MXGUEST_SHARE_FS_TYPE) + 1);
}

static int share_mounted_at(const char *mountinfo, const char *mount_point)
{
    FILE *in = fopen(mountinfo, "r");
    char *line = NULL, *point;
    size_t cap = 0;
    int found = 0;
    if (!in)
        return 0;
    while (!found && getline(&line, &cap, in) > 0)
        found = mountinfo_entry(line, &point) && !strcmp(point, mount_point);
    free(line);
    fclose(in);
    return found;
}

void mxguest_share_detach(void *ctx, const char *mount_point, int fuse_fd)
{
    (void)ctx;
    while (share_mounted_at(SHARE_MOUNTINFO, mount_point))
        if (umount2(mount_point, MNT_DETACH | UMOUNT_NOFOLLOW) != 0)
            break;
    if (fuse_fd >= 0)
        close(fuse_fd);
}

void mxguest_share_sweep(const char *mountinfo)
{
    FILE *in = fopen(mountinfo, "r");
    char *line = NULL, *point;
    size_t cap = 0;
    if (!in)
        return;
    while (getline(&line, &cap, in) > 0)
        if (mountinfo_entry(line, &point) && umount2(point, MNT_DETACH | UMOUNT_NOFOLLOW) != 0)
            fprintf(stderr, "stale shared folder at %s not removed: %s\n", point, strerror(errno));
    free(line);
    fclose(in);
}

int mxguest_share_available(void)
{
    int fd;
    if (geteuid() != 0)
        return 0;
    fd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}
