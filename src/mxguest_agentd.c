/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#define _GNU_SOURCE
#include "clipboard.h"
#include "mxga.h"
#include "power.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define AGENT_DEVICE "/dev/mxguest-agent"
#define AGENT_CAPS (MXGA_CAP_SHUTDOWN | MXGA_CAP_RESTART | MXGA_CAP_SYSTEM_STATS)

#ifndef SESSION_DIR
#define SESSION_DIR "/run/mxguest-agent"
#endif
#define SESSION_SOCKET SESSION_DIR "/session.sock"
#ifndef SEAT_STATE
#define SEAT_STATE "/run/systemd/seats/seat0"
#endif

struct session_client {
    int fd;
    struct mxguest_clip_in in;
    struct mxguest_clip_out out;
};

static uint64_t g_sequence = 1;
static uint8_t *g_send;
static uint8_t *g_payload;
static struct mxguest_clip g_clip;
static int g_clip_ready;
static int g_listen = -1;
static struct session_client g_client = {.fd = -1};

static int write_all(int fd, const uint8_t *buf, uint32_t len)
{
    uint32_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        off += (uint32_t)n;
    }
    return 0;
}

static int send_frame(int fd, uint16_t opcode, const uint8_t *payload, uint32_t payload_len)
{
    uint32_t len = 0;
    if (!g_send)
        g_send = malloc(MXGA_MAX_FRAME_BYTES);
    if (!g_send)
        return -1;
    if (mxga_encode_frame(MXGA_PROTOCOL_MINOR, opcode, g_sequence++, payload, payload_len, g_send,
                          MXGA_MAX_FRAME_BYTES, &len) != MXGA_OK)
        return -1;
    if (fd < 0)
        return 0;
    return write_all(fd, g_send, len);
}

static int send_hello(int fd, uint16_t opcode)
{
    uint8_t caps[8];
    uint32_t len = 0;
    uint64_t mask = mxguest_clip_caps(AGENT_CAPS, g_client.fd >= 0);
    if (mxga_encode_capabilities(mask, caps, sizeof caps, &len) != MXGA_OK)
        return -1;
    return send_frame(fd, opcode, caps, len);
}

static unsigned count_processes(void)
{
    DIR *dir = opendir("/proc");
    struct dirent *ent;
    unsigned count = 0;
    if (!dir)
        return 0;
    while ((ent = readdir(dir)) != NULL) {
        char *end = NULL;
        if (ent->d_name[0] < '1' || ent->d_name[0] > '9')
            continue;
        strtoul(ent->d_name, &end, 10);
        if (end && *end == '\0')
            count++;
    }
    closedir(dir);
    return count;
}

static uint64_t meminfo_kb(const char *key)
{
    FILE *in = fopen("/proc/meminfo", "r");
    char line[256];
    uint64_t value = 0;
    size_t key_len;
    if (!in)
        return 0;
    key_len = strlen(key);
    while (fgets(line, sizeof line, in)) {
        if (strncmp(line, key, key_len) == 0 && line[key_len] == ':') {
            value = strtoull(line + key_len + 1, NULL, 10);
            break;
        }
    }
    fclose(in);
    return value * 1024ull;
}

static int read_cpu_line(const char *name, unsigned long long *total, unsigned long long *idle)
{
    FILE *in = fopen("/proc/stat", "r");
    char line[512];
    int found = 0;
    if (!in)
        return -1;
    while (fgets(line, sizeof line, in)) {
        if (strncmp(line, name, strlen(name)) == 0 && line[strlen(name)] == ' ') {
            unsigned long long user = 0, nice = 0, system = 0, idle_v = 0;
            unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
            if (sscanf(line + strlen(name) + 1, "%llu %llu %llu %llu %llu %llu %llu %llu", &user,
                       &nice, &system, &idle_v, &iowait, &irq, &softirq, &steal) >= 4) {
                *idle = idle_v + iowait;
                *total = user + nice + system + *idle + irq + softirq + steal;
                found = 1;
            }
            break;
        }
    }
    fclose(in);
    return found ? 0 : -1;
}

static uint32_t permille_delta(unsigned long long total, unsigned long long idle,
                               unsigned long long prev_total, unsigned long long prev_idle)
{
    unsigned long long dt;
    unsigned long long di;
    if (total <= prev_total || idle < prev_idle)
        return 0;
    dt = total - prev_total;
    di = idle - prev_idle;
    if (dt == 0 || di > dt)
        return 0;
    return (uint32_t)(((dt - di) * 1000ull) / dt);
}

static int cpu_khz(unsigned index, struct mxga_cpu_stat *cpu)
{
    char path[160];
    FILE *in;
    unsigned value = 0;
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_cur_freq", index);
    in = fopen(path, "r");
    if (!in) {
        cpu->frequency_source = MXGA_CPU_FREQ_NONE;
        return 0;
    }
    if (fscanf(in, "%u", &value) == 1)
        cpu->current_khz = value;
    fclose(in);
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_min_freq", index);
    in = fopen(path, "r");
    if (in) {
        if (fscanf(in, "%u", &value) == 1)
            cpu->minimum_khz = value;
        fclose(in);
    }
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_max_freq", index);
    in = fopen(path, "r");
    if (in) {
        if (fscanf(in, "%u", &value) == 1)
            cpu->maximum_khz = value;
        fclose(in);
    }
    cpu->frequency_source = MXGA_CPU_FREQ_CPUFREQ;
    return 0;
}

static int collect_stats(struct mxga_system_stats *stats, struct mxga_cpu_stat *cpus, unsigned cap,
                         unsigned *used)
{
    FILE *in;
    double load1, load5, load15, uptime;
    unsigned i;
    unsigned long long total = 0, idle = 0;
    static unsigned long long last_total;
    static unsigned long long last_idle;
    static unsigned long long last_cpu_total[256];
    static unsigned long long last_cpu_idle[256];
    static int have_last;
    uint64_t total_mem;
    uint64_t available;
    if (!stats || !cpus || !used)
        return -1;
    memset(stats, 0, sizeof *stats);
    in = fopen("/proc/uptime", "r");
    if (in) {
        if (fscanf(in, "%lf", &uptime) == 1)
            stats->uptime_seconds = (uint64_t)uptime;
        fclose(in);
    }
    total_mem = meminfo_kb("MemTotal");
    available = meminfo_kb("MemAvailable");
    stats->memory_total_bytes = total_mem;
    stats->memory_available_bytes = available;
    stats->memory_used_bytes = total_mem > available ? total_mem - available : 0;
    stats->swap_total_bytes = meminfo_kb("SwapTotal");
    stats->swap_used_bytes = stats->swap_total_bytes > meminfo_kb("SwapFree")
                                 ? stats->swap_total_bytes - meminfo_kb("SwapFree")
                                 : 0;
    in = fopen("/proc/loadavg", "r");
    if (in) {
        if (fscanf(in, "%lf %lf %lf", &load1, &load5, &load15) == 3) {
            stats->load_average_permille[0] = (uint32_t)(load1 * 1000.0);
            stats->load_average_permille[1] = (uint32_t)(load5 * 1000.0);
            stats->load_average_permille[2] = (uint32_t)(load15 * 1000.0);
        }
        fclose(in);
    }
    stats->process_count = count_processes();
    if (read_cpu_line("cpu", &total, &idle) == 0 && have_last)
        stats->cpu_usage_permille = permille_delta(total, idle, last_total, last_idle);
    last_total = total;
    last_idle = idle;
    *used = 0;
    for (i = 0; i < cap; i++) {
        char name[32];
        unsigned long long ct = 0, ci = 0;
        snprintf(name, sizeof name, "cpu%u", i);
        if (read_cpu_line(name, &ct, &ci) != 0)
            break;
        memset(&cpus[i], 0, sizeof cpus[i]);
        if (have_last)
            cpus[i].usage_permille = permille_delta(ct, ci, last_cpu_total[i], last_cpu_idle[i]);
        last_cpu_total[i] = ct;
        last_cpu_idle[i] = ci;
        cpu_khz(i, &cpus[i]);
        (*used)++;
    }
    have_last = 1;
    stats->cpu_count = *used;
    stats->cpus = cpus;
    return 0;
}

static int send_stats(int fd)
{
    struct mxga_cpu_stat cpus[64];
    struct mxga_system_stats stats;
    unsigned used = 0;
    uint8_t payload[MXGA_STATS_HEADER_BYTES + 64 * MXGA_STATS_CPU_BYTES];
    uint32_t len = 0;
    if (collect_stats(&stats, cpus, 64, &used) != 0)
        return -1;
    if (mxga_encode_system_stats(&stats, payload, sizeof payload, &len) != MXGA_OK)
        return -1;
    return send_frame(fd, MXGA_OP_SYSTEM_STATS, payload, len);
}

static int power_command(int fd, uint16_t opcode, uint64_t sequence)
{
    uint8_t result[4];
    uint8_t frame[64];
    uint32_t result_len = 0;
    uint32_t frame_len = 0;
    const char *program = "/usr/bin/systemctl";
    int error;
    if (access(program, X_OK) != 0)
        program = "/bin/systemctl";
    error = mxguest_power_execute(opcode, program, 2000);
    if (error)
        fprintf(stderr, "power request 0x%x failed: %s\n", opcode, strerror(error));
    if (mxga_encode_command_result(opcode, (uint16_t)error, result, sizeof result, &result_len) !=
        MXGA_OK)
        return -1;
    if (mxga_encode_frame(MXGA_PROTOCOL_MINOR, MXGA_OP_COMMAND_RESULT, sequence, result, result_len,
                          frame, sizeof frame, &frame_len) != MXGA_OK)
        return -1;
    if (fd >= 0 && write_all(fd, frame, frame_len) != 0)
        return -1;
    return 0;
}

static int session_listen(void)
{
    struct sockaddr_un addr;
    int fd;
    if (strlen(SESSION_SOCKET) >= sizeof addr.sun_path)
        return -1;
    if (mkdir(SESSION_DIR, 0755) != 0 && errno != EEXIST)
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    if (fd >= FD_SETSIZE)
        goto fail;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SESSION_SOCKET);
    unlink(SESSION_SOCKET);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || chmod(SESSION_SOCKET, 0666) != 0 ||
        listen(fd, 4) != 0)
        goto fail;
    return fd;
fail:
    close(fd);
    return -1;
}

static void client_close(void)
{
    if (g_client.fd < 0)
        return;
    close(g_client.fd);
    g_client.fd = -1;
    mxguest_clip_in_free(&g_client.in);
    mxguest_clip_out_free(&g_client.out);
}

static int client_drop(int fd)
{
    client_close();
    return send_hello(fd, MXGA_OP_HEARTBEAT);
}

static int client_flush(void)
{
    for (;;) {
        size_t pending;
        const uint8_t *data = mxguest_clip_out_pending(&g_client.out, &pending);
        ssize_t n;
        if (!pending)
            return 0;
        n = send(g_client.fd, data, pending, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }
        if (n == 0)
            return -1;
        mxguest_clip_out_advance(&g_client.out, (size_t)n);
    }
}

static int client_pending(void)
{
    size_t pending = 0;
    if (g_client.fd >= 0)
        mxguest_clip_out_pending(&g_client.out, &pending);
    return pending != 0;
}

static int client_write_ready(int fd)
{
    if (g_client.fd >= 0 && client_flush() != 0)
        return client_drop(fd);
    return 0;
}

static int client_deliver(int fd, const uint8_t *text, uint32_t len)
{
    if (g_client.fd < 0)
        return 0;
    if (mxguest_clip_out_push(&g_client.out, text, len) != 0 || client_flush() != 0)
        return client_drop(fd);
    return 0;
}

static int client_text(int fd, const uint8_t *text, uint32_t len)
{
    uint32_t payload_len = 0;
    int result = mxguest_clip_build_changed(&g_clip, text, len, g_payload, MXGA_MAX_FRAME_BYTES,
                                            &payload_len);
    if (result < 0) {
        fprintf(stderr, "clipboard text not published\n");
        return 0;
    }
    if (result == 0)
        return 0;
    return send_frame(fd, MXGA_OP_CLIPBOARD_CHANGED, g_payload, payload_len);
}

static int client_read(int fd)
{
    uint8_t chunk[16384];
    size_t used = 0;
    ssize_t n = recv(g_client.fd, chunk, sizeof chunk, 0);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return client_drop(fd);
    }
    if (n == 0)
        return client_drop(fd);
    while (used < (size_t)n) {
        const uint8_t *text;
        uint32_t len;
        int state;
        size_t took = mxguest_clip_in_feed(&g_client.in, chunk + used, (size_t)n - used);
        used += took;
        while ((state = mxguest_clip_in_next(&g_client.in, &text, &len)) == 1) {
            if (client_text(fd, text, len) != 0)
                return -1;
        }
        if (state < 0 || took == 0)
            return client_drop(fd);
    }
    return 0;
}

static int session_accept(int fd)
{
    struct ucred cred;
    socklen_t cred_len = sizeof cred;
    struct session_client next = {.fd = -1};
    int sock = accept4(g_listen, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (sock < 0)
        return 0;
    if (sock >= FD_SETSIZE || getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0 ||
        !mxguest_clip_peer_allowed(SEAT_STATE, cred.uid) || mxguest_clip_in_init(&next.in) != 0) {
        close(sock);
        return 0;
    }
    if (mxguest_clip_out_init(&next.out) != 0) {
        mxguest_clip_in_free(&next.in);
        close(sock);
        return 0;
    }
    client_close();
    next.fd = sock;
    g_client = next;
    if (send_hello(fd, MXGA_OP_HEARTBEAT) != 0)
        return -1;
    if (g_clip.have_host)
        return client_deliver(fd, g_clip.host_text, g_clip.host_len);
    return 0;
}

static int host_clipboard(int fd, const uint8_t *payload, uint32_t len)
{
    const uint8_t *text;
    uint32_t text_len;
    int result;
    if (!g_clip_ready)
        return 0;
    result = mxguest_clip_host_write(&g_clip, payload, len, &text, &text_len);
    if (result < 0) {
        fprintf(stderr, "clipboard write ignored\n");
        return 0;
    }
    if (result == 0)
        return 0;
    return client_deliver(fd, text, text_len);
}

static int handle_frame(int fd, const uint8_t *buf, uint32_t len)
{
    struct mxga_frame frame;
    if (mxga_decode_frame(buf, len, &frame) != MXGA_OK)
        return -1;
    if (frame.opcode == MXGA_OP_CLIPBOARD_WRITE)
        return host_clipboard(fd, frame.payload, frame.payload_len);
    if (frame.opcode == MXGA_OP_SHUTDOWN)
        return frame.payload_len ? -1 : power_command(fd, MXGA_OP_SHUTDOWN, frame.sequence);
    if (frame.opcode == MXGA_OP_RESTART)
        return frame.payload_len ? -1 : power_command(fd, MXGA_OP_RESTART, frame.sequence);
    return 0;
}

static int self_check(void)
{
    struct mxga_cpu_stat cpus[8];
    struct mxga_system_stats stats;
    struct mxga_system_stats got;
    struct mxga_cpu_stat got_cpus[8];
    unsigned used = 0;
    uint8_t payload[MXGA_STATS_HEADER_BYTES + 8 * MXGA_STATS_CPU_BYTES];
    uint8_t caps[8];
    uint8_t frame[4096];
    uint32_t len = 0;
    uint32_t frame_len = 0;
    uint64_t capabilities = 0;
    struct mxga_frame decoded;
    if (collect_stats(&stats, cpus, 8, &used) != 0 || used == 0)
        return 1;
    if (mxga_encode_system_stats(&stats, payload, sizeof payload, &len) != MXGA_OK)
        return 1;
    if (mxga_encode_frame(MXGA_PROTOCOL_MINOR, MXGA_OP_SYSTEM_STATS, 1, payload, len, frame,
                          sizeof frame, &frame_len) != MXGA_OK)
        return 1;
    if (mxga_decode_frame(frame, frame_len, &decoded) != MXGA_OK ||
        decoded.opcode != MXGA_OP_SYSTEM_STATS)
        return 1;
    if (mxga_decode_system_stats(decoded.payload, decoded.payload_len, &got, got_cpus, 8) !=
        MXGA_OK)
        return 1;
    if (got.memory_total_bytes == 0 || got.cpu_count == 0)
        return 1;
    if (mxga_encode_capabilities(AGENT_CAPS, caps, sizeof caps, &len) != MXGA_OK)
        return 1;
    if (mxga_encode_frame(MXGA_PROTOCOL_MINOR, MXGA_OP_HELLO, 1, caps, len, frame, sizeof frame,
                          &frame_len) != MXGA_OK)
        return 1;
    if (mxga_decode_frame(frame, frame_len, &decoded) != MXGA_OK)
        return 1;
    if (mxga_decode_capabilities(decoded.payload, decoded.payload_len, &capabilities) != MXGA_OK)
        return 1;
    if ((capabilities & AGENT_CAPS) != AGENT_CAPS)
        return 1;
    printf("agent frames ok cpus %u mem %llu\n", got.cpu_count,
           (unsigned long long)got.memory_total_bytes);
    return 0;
}

static int report_clock(uint64_t *milliseconds)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    *milliseconds = (uint64_t)now.tv_sec * 1000u + now.tv_nsec / 1000000u;
    return 0;
}

int main(int argc, char **argv)
{
    int fd;
    uint64_t now, next_report, origin;
    uint8_t *buf;
    int check = argc > 1 && strcmp(argv[1], "--check") == 0;
    if (check)
        return self_check();
    buf = malloc(MXGA_MAX_FRAME_BYTES);
    g_payload = malloc(MXGA_MAX_FRAME_BYTES);
    if (!buf || !g_payload) {
        fprintf(stderr, "buffer allocation failed\n");
        return 1;
    }
    origin = mxguest_clip_random_origin();
    if (origin && mxguest_clip_init(&g_clip, origin) == 0) {
        g_listen = session_listen();
        if (g_listen >= 0)
            g_clip_ready = 1;
        else
            mxguest_clip_free(&g_clip);
    }
    if (!g_clip_ready)
        fprintf(stderr, "clipboard sharing unavailable\n");
    fd = open(AGENT_DEVICE, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s failed errno %d\n", AGENT_DEVICE, errno);
        return 1;
    }
    if (fd >= FD_SETSIZE) {
        fprintf(stderr, "device descriptor exceeds select capacity\n");
        close(fd);
        return 1;
    }
    if (send_hello(fd, MXGA_OP_HELLO) != 0 || send_stats(fd) != 0) {
        fprintf(stderr, "hello failed errno %d\n", errno);
        return 1;
    }
    if (report_clock(&now) != 0)
        return 1;
    next_report = now + 5000u;
    for (;;) {
        ssize_t n;
        fd_set read_set, write_set;
        struct timeval wait;
        uint64_t remaining;
        int maxfd = fd;
        if (report_clock(&now) != 0)
            return 1;
        remaining = next_report > now ? next_report - now : 0;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_SET(fd, &read_set);
        if (g_listen >= 0) {
            FD_SET(g_listen, &read_set);
            maxfd = g_listen > maxfd ? g_listen : maxfd;
        }
        if (g_client.fd >= 0) {
            FD_SET(g_client.fd, &read_set);
            if (client_pending())
                FD_SET(g_client.fd, &write_set);
            maxfd = g_client.fd > maxfd ? g_client.fd : maxfd;
        }
        wait.tv_sec = remaining / 1000u;
        wait.tv_usec = (remaining % 1000u) * 1000u;
        if (select(maxfd + 1, &read_set, &write_set, NULL, &wait) < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        if (FD_ISSET(fd, &read_set)) {
            n = read(fd, buf, MXGA_MAX_FRAME_BYTES);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return 1;
            }
            if (n == 0 || handle_frame(fd, buf, (uint32_t)n) != 0)
                return 1;
        }
        if (g_listen >= 0 && FD_ISSET(g_listen, &read_set) && session_accept(fd) != 0)
            return 1;
        if (g_client.fd >= 0 && FD_ISSET(g_client.fd, &write_set) && client_write_ready(fd) != 0)
            return 1;
        if (g_client.fd >= 0 && FD_ISSET(g_client.fd, &read_set) && client_read(fd) != 0)
            return 1;
        if (report_clock(&now) != 0)
            return 1;
        if (now >= next_report) {
            if (send_hello(fd, MXGA_OP_HEARTBEAT) != 0 || send_stats(fd) != 0)
                return 1;
            next_report = now + 5000u;
        }
    }
}
