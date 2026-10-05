#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
set -eu
media=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$media/linux/install/autorun.sh" "$@"
