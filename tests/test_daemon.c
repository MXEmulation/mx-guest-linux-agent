/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#define main mxguest_daemon_main
#include "../src/mxguest_agentd.c"
#undef main

#include <assert.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int test_transport;

int __real_open(const char *path, int flags, ...);

int __wrap_open(const char *path, int flags, ...)
{
    if (!strcmp(path, AGENT_DEVICE)) {
        assert(flags == (O_RDWR | O_CLOEXEC));
        assert(fcntl(test_transport, F_SETFD, FD_CLOEXEC) == 0);
        return test_transport;
    }
    if (flags & O_CREAT) {
        va_list args;
        int mode;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
        return __real_open(path, flags, mode);
    }
    return __real_open(path, flags);
}

int main(void)
{
    int channels[2], status;
    pid_t child, finished = 0;
    uint8_t input[64], output[4096];
    uint32_t input_length;
    uint64_t start, now;
    unsigned hello = 0, stats = 0, heartbeat = 0, sent = 0;
    int sender_failed = 0;
    const struct timespec pause = {0, 20000000};
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channels) == 0);
    child = fork();
    assert(child >= 0);
    if (!child) {
        char *args[] = {"mxguest-agentd", NULL};
        close(channels[0]);
        test_transport = channels[1];
        _exit(mxguest_daemon_main(1, args));
    }
    assert(close(channels[1]) == 0);
    assert(report_clock(&start) == 0);
    do {
        struct mxga_frame frame;
        ssize_t length;
        assert(mxga_encode_frame(MXGA_PROTOCOL_MINOR, MXGA_OP_HEARTBEAT, sent + 1, NULL, 0, input,
                                 sizeof input, &input_length) == MXGA_OK);
        if (send(channels[0], input, input_length, MSG_NOSIGNAL | MSG_DONTWAIT) != input_length) {
            sender_failed = 1;
            break;
        }
        sent++;
        while ((length = recv(channels[0], output, sizeof output, MSG_DONTWAIT)) > 0) {
            assert(mxga_decode_frame(output, (uint32_t)length, &frame) == MXGA_OK);
            hello += frame.opcode == MXGA_OP_HELLO;
            stats += frame.opcode == MXGA_OP_SYSTEM_STATS;
            heartbeat += frame.opcode == MXGA_OP_HEARTBEAT;
        }
        assert(length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        nanosleep(&pause, NULL);
        assert(report_clock(&now) == 0);
    } while (now - start < 5600);
    assert(close(channels[0]) == 0);
    assert(report_clock(&start) == 0);
    do {
        finished = waitpid(child, &status, WNOHANG);
        assert(finished >= 0);
        if (finished)
            break;
        nanosleep(&pause, NULL);
        assert(report_clock(&now) == 0);
    } while (now - start < 500);
    if (!finished) {
        kill(child, SIGKILL);
        assert(waitpid(child, &status, 0) == child);
    }
    assert(finished == child && WIFEXITED(status) && WEXITSTATUS(status) == 1);
    assert(!sender_failed && hello == 1 && stats >= 2 && heartbeat >= 1 && sent > 100);
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    puts("daemon heartbeat survives continuous input; closed transport exits promptly");
    return 0;
}
