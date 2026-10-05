/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#ifndef MXGUEST_POWER_H
#define MXGUEST_POWER_H

#include <stdint.h>

int mxguest_power_execute(uint16_t opcode, const char *program, unsigned timeout_ms);

#endif
