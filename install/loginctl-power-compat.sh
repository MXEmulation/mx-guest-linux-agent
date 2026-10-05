#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke

if [ "$#" -eq 2 ] && [ "$2" = "--no-block" ]; then
    case "$1" in
        poweroff|reboot)
            printf 'power request: %s\n' "$1" >&2
            /usr/bin/timeout --signal=TERM --kill-after=1s 3s \
                /usr/bin/systemctl --no-block "$1"
            result=$?
            if [ "$result" -ne 0 ]; then
                printf 'power request failed: %s status=%s\n' "$1" "$result" >&2
            fi
            exit "$result"
            ;;
    esac
fi

exec /proc/1/root/usr/bin/loginctl "$@"
