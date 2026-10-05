/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#define _POSIX_C_SOURCE 200809L
#include "power.h"
#include "mxga.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t retired_child;

static int clock_ms(uint64_t *value)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return errno;
    *value = (uint64_t)now.tv_sec * 1000u + (unsigned long)now.tv_nsec / 1000000u;
    return 0;
}

static int wait_until(pid_t child, uint64_t deadline, int *status)
{
    const struct timespec pause = {0, 10000000};
    for (;;) {
        uint64_t now;
        pid_t found = waitpid(child, status, WNOHANG);
        int error;
        if (found == child)
            return 0;
        if (found < 0 && errno != EINTR)
            return errno;
        error = clock_ms(&now);
        if (error)
            return error;
        if (now >= deadline)
            return ETIMEDOUT;
        if (nanosleep(&pause, NULL) != 0 && errno != EINTR)
            return errno;
    }
}

int mxguest_power_execute(uint16_t opcode, const char *program, unsigned timeout_ms)
{
    const char *action;
    uint64_t now;
    pid_t child;
    int status = 0;
    int error;

    if (opcode != MXGA_OP_SHUTDOWN && opcode != MXGA_OP_RESTART)
        return EINVAL;
    if (!program || program[0] != '/' || !timeout_ms || timeout_ms > 5000)
        return EINVAL;
    if (retired_child) {
        pid_t found = waitpid(retired_child, &status, WNOHANG);
        if (!found || (found < 0 && errno != ECHILD))
            return EBUSY;
        retired_child = 0;
    }
    if (access(program, X_OK) != 0)
        return errno;
    error = clock_ms(&now);
    if (error)
        return error;
    action = opcode == MXGA_OP_SHUTDOWN ? "poweroff" : "reboot";
    child = fork();
    if (child < 0)
        return errno;
    if (!child) {
        char *const arguments[] = {(char *)program, "--no-block", (char *)action, NULL};
        if (setpgid(0, 0) != 0)
            _exit(126);
        execv(program, arguments);
        _exit(127);
    }
    if (setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH) {
        error = errno;
        kill(child, SIGKILL);
    } else {
        error = wait_until(child, now + timeout_ms, &status);
    }
    if (error) {
        kill(-child, SIGKILL);
        kill(child, SIGKILL);
        if (clock_ms(&now) != 0 || wait_until(child, now + 500, &status) != 0)
            retired_child = child;
        return error;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "power command %s failed: exit=%d signal=%d\n", action,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        return EIO;
    }
    return 0;
}
