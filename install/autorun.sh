#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
set -eu
media=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
installer="$media/linux/install/install.sh"
manifest="$media/linux/install/sources.tsv"
if [ ! -f "$installer" ] || [ ! -f "$manifest" ]; then
    printf '%s\n' 'MX installation media is incomplete.' >&2
    exit 1
fi
if [ "$(id -u)" = 0 ]; then
    exec bash "$installer" --sources "$manifest"
fi
if [ -t 0 ]; then
    exec sudo -- bash "$installer" --sources "$manifest"
fi
if command -v gnome-terminal >/dev/null 2>&1; then
    exec gnome-terminal --wait -- sh -c 'sudo -- bash "$1" --sources "$2"; status=$?; printf "\nPress Enter to close.\n"; read answer; exit "$status"' sh "$installer" "$manifest"
fi
if command -v konsole >/dev/null 2>&1; then
    exec konsole --hold -e sudo -- bash "$installer" --sources "$manifest"
fi
if command -v xterm >/dev/null 2>&1; then
    exec xterm -hold -e sudo -- bash "$installer" --sources "$manifest"
fi
printf '%s\n' 'Open a terminal and run this launcher to install MX guest additions.' >&2
exit 1
