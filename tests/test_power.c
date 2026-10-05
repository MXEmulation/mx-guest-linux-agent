/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#define main mxguest_daemon_main
#include "../src/mxguest_agentd.c"
#undef main

#include <assert.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

static char executable[4096];
static char marker[4096];

int __real_execv(const char *path, char *const arguments[]);

int __wrap_execv(const char *path, char *const arguments[])
{
    if (!strcmp(path, "/usr/bin/systemctl") || !strcmp(path, "/bin/systemctl"))
        return __real_execv(executable, arguments);
    return __real_execv(path, arguments);
}

static int helper(int argc, char **argv)
{
    const char *mode = getenv("MX_POWER_TEST_MODE");
    const char *expected = getenv("MX_POWER_TEST_ACTION");
    const char *file = getenv("MX_POWER_TEST_MARKER");
    const char *inherited = getenv("MX_POWER_TEST_FD");
    const struct timespec delay = {0, 60000000};
    int fd;
    if (argc != 3 || strcmp(argv[1], "--no-block") || !expected || strcmp(argv[2], expected) ||
        !mode || !file)
        return 90;
    if (inherited && (fcntl(atoi(inherited), F_GETFD) != -1 || errno != EBADF))
        return 94;
    if (!strcmp(mode, "timeout-tree")) {
        pid_t descendant = fork();
        if (descendant < 0)
            return 95;
        if (!descendant) {
            char process[32];
            const struct timespec sleep_time = {5, 0};
            int bytes = snprintf(process, sizeof process, "%ld", (long)getpid());
            if (signal(SIGTERM, SIG_IGN) == SIG_ERR)
                return 96;
            fd = open(file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (fd < 0 || write(fd, process, (size_t)bytes) != bytes || close(fd) != 0)
                return 97;
            for (;;)
                nanosleep(&sleep_time, NULL);
        }
    }
    if (!strcmp(mode, "timeout") || !strcmp(mode, "timeout-tree")) {
        const struct timespec sleep_time = {5, 0};
        nanosleep(&sleep_time, NULL);
        return 91;
    }
    if (!strcmp(mode, "signal")) {
        raise(SIGTERM);
        return 92;
    }
    nanosleep(&delay, NULL);
    fd = open(file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0 || write(fd, "executed", 8) != 8 || close(fd) != 0)
        return 93;
    return !strcmp(mode, "failure") ? 23 : 0;
}

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return (uint64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static void configure(const char *mode, const char *action)
{
    assert(setenv("MX_POWER_TEST_MODE", mode, 1) == 0);
    assert(setenv("MX_POWER_TEST_ACTION", action, 1) == 0);
    assert(setenv("MX_POWER_TEST_MARKER", marker, 1) == 0);
    assert(unlink(marker) == 0 || errno == ENOENT);
}

static void no_children(void)
{
    int status;
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
}

static void response(uint16_t opcode, uint64_t sequence, uint16_t expected)
{
    int descriptors[2];
    uint8_t wire[128];
    struct mxga_frame frame;
    uint16_t actual_opcode, actual_status;
    ssize_t length;
    pid_t observer;
    int status;
    assert(pipe(descriptors) == 0);
    observer = fork();
    assert(observer >= 0);
    if (!observer) {
        assert(close(descriptors[1]) == 0);
        length = read(descriptors[0], wire, sizeof wire);
        assert(length > 0);
        assert(access(marker, R_OK) == 0);
        assert(close(descriptors[0]) == 0);
        assert(mxga_decode_frame(wire, (uint32_t)length, &frame) == MXGA_OK);
        assert(frame.sequence == sequence && frame.opcode == MXGA_OP_COMMAND_RESULT);
        assert(frame.payload_len == 4);
        actual_opcode = frame.payload[0] | (uint16_t)frame.payload[1] << 8;
        actual_status = frame.payload[2] | (uint16_t)frame.payload[3] << 8;
        assert(actual_opcode == opcode && actual_status == expected);
        _exit(0);
    }
    assert(close(descriptors[0]) == 0);
    assert(power_command(descriptors[1], opcode, sequence) == 0);
    assert(close(descriptors[1]) == 0);
    assert(waitpid(observer, &status, 0) == observer);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    no_children();
}

int main(int argc, char **argv)
{
    char directory[4096];
    char working_directory[2048];
    char unavailable[4096];
    uint64_t started;
    ssize_t length;
    uint8_t wire[128], invalid = 1;
    uint32_t wire_length;
    int inherited_fd;
    if (argc > 1 && !strcmp(argv[1], "--no-block"))
        return helper(argc, argv);
    length = readlink("/proc/self/exe", executable, sizeof executable - 1);
    assert(length > 0 && (size_t)length < sizeof executable - 1);
    executable[length] = '\0';
    assert(getcwd(working_directory, sizeof working_directory));
    assert(snprintf(directory, sizeof directory, "%s/build/power-test-XXXXXX", working_directory) >
           0);
    assert(mkdtemp(directory));
    assert(snprintf(marker, sizeof marker, "%s/finished", directory) > 0);
    assert(snprintf(unavailable, sizeof unavailable, "%s/missing", directory) > 0);

    configure("success", "poweroff");
    inherited_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(inherited_fd >= 0);
    {
        char descriptor[32];
        assert(snprintf(descriptor, sizeof descriptor, "%d", inherited_fd) > 0);
        assert(setenv("MX_POWER_TEST_FD", descriptor, 1) == 0);
    }
    assert(mxguest_power_execute(MXGA_OP_SHUTDOWN, executable, 1000) == 0);
    assert(close(inherited_fd) == 0);
    assert(unsetenv("MX_POWER_TEST_FD") == 0);
    assert(access(marker, R_OK) == 0);
    no_children();
    configure("timeout-tree", "reboot");
    assert(prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0);
    started = monotonic_ms();
    assert(mxguest_power_execute(MXGA_OP_RESTART, executable, 500) == ETIMEDOUT);
    {
        FILE *process = fopen(marker, "r");
        uint64_t execution_ms = monotonic_ms() - started;
        long descendant;
        int status;
        pid_t found = 0;
        const struct timespec pause = {0, 10000000};
        assert(process && fscanf(process, "%ld", &descendant) == 1 && descendant > 0);
        assert(fclose(process) == 0);
        started = monotonic_ms();
        while (!(found = waitpid((pid_t)descendant, &status, WNOHANG)) &&
               monotonic_ms() - started < 1000)
            nanosleep(&pause, NULL);
        if (!found) {
            assert(kill((pid_t)descendant, SIGKILL) == 0);
            assert(waitpid((pid_t)descendant, &status, 0) == (pid_t)descendant);
        }
        assert(found == (pid_t)descendant);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        assert(execution_ms < 1300);
    }
    assert(prctl(PR_SET_CHILD_SUBREAPER, 0, 0, 0, 0) == 0);
    no_children();
    configure("success", "reboot");
    assert(mxguest_power_execute(MXGA_OP_RESTART, executable, 1000) == 0);
    no_children();
    configure("failure", "poweroff");
    assert(mxguest_power_execute(MXGA_OP_SHUTDOWN, executable, 1000) == EIO);
    no_children();
    configure("signal", "reboot");
    assert(mxguest_power_execute(MXGA_OP_RESTART, executable, 1000) == EIO);
    no_children();
    configure("timeout", "poweroff");
    started = monotonic_ms();
    assert(mxguest_power_execute(MXGA_OP_SHUTDOWN, executable, 40) == ETIMEDOUT);
    assert(monotonic_ms() - started < 800);
    assert(access(marker, R_OK) != 0);
    no_children();
    assert(mxguest_power_execute(MXGA_OP_SHUTDOWN, unavailable, 100) == ENOENT);
    assert(mxguest_power_execute(MXGA_OP_SHUTDOWN, NULL, 100) == EINVAL);
    assert(mxguest_power_execute(MXGA_OP_RESTART, "relative", 100) == EINVAL);
    assert(mxguest_power_execute(MXGA_OP_HEARTBEAT, executable, 100) == EINVAL);
    assert(mxguest_power_execute(MXGA_OP_SHUTDOWN, executable, 0) == EINVAL);
    assert(mxguest_power_execute(MXGA_OP_SHUTDOWN, executable, 5001) == EINVAL);
    assert(mkdir(unavailable, 0700) == 0);
    assert(mxguest_power_execute(MXGA_OP_SHUTDOWN, unavailable, 100) == EIO);
    assert(rmdir(unavailable) == 0);
    no_children();

    configure("success", "poweroff");
    response(MXGA_OP_SHUTDOWN, 0x1122334455667788ull, 0);
    configure("failure", "reboot");
    response(MXGA_OP_RESTART, 0x8877665544332211ull, EIO);
    assert(mxga_encode_frame(MXGA_PROTOCOL_MINOR, MXGA_OP_SHUTDOWN, 27, &invalid, 1, wire,
                             sizeof wire, &wire_length) == MXGA_OK);
    assert(handle_frame(-1, wire, wire_length) == -1);
    no_children();
    assert(unlink(marker) == 0);
    assert(rmdir(directory) == 0);
    puts(
        "power execution, failure, timeout, descriptor closure, descendants and reply ordering passed");
    return 0;
}
