# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror
CORE ?= deps/core
CORE_SOURCES := $(addprefix $(CORE)/,$(filter src/%.c,$(shell cat $(CORE)/sources.list)))
CORE_HEADERS := $(addprefix $(CORE)/,$(filter %.h,$(shell cat $(CORE)/sources.list)))

.PHONY: all check clean

all: build/mxguest-agentd

build/mxguest-agentd: src/mxguest_agentd.c src/power.c src/power.h src/clipboard.c src/clipboard.h $(CORE_SOURCES) $(CORE_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) -I$(CORE)/include -I$(CORE)/src -o $@ src/mxguest_agentd.c src/power.c src/clipboard.c $(CORE_SOURCES)

build/test-clipboard: tests/test_clipboard.c src/clipboard.c src/clipboard.h $(CORE_SOURCES) $(CORE_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) -I$(CORE)/include -I$(CORE)/src -Isrc -o $@ tests/test_clipboard.c src/clipboard.c $(CORE_SOURCES)

build/test-power: tests/test_power.c src/mxguest_agentd.c src/power.c src/power.h src/clipboard.c src/clipboard.h $(CORE_SOURCES) $(CORE_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) -I$(CORE)/include -I$(CORE)/src -Isrc -Wl,--wrap=execv -o $@ tests/test_power.c src/power.c src/clipboard.c $(CORE_SOURCES)

build/test-daemon: tests/test_daemon.c src/mxguest_agentd.c src/power.c src/power.h src/clipboard.c src/clipboard.h $(CORE_SOURCES) $(CORE_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) -I$(CORE)/include -I$(CORE)/src -Isrc -Wl,--wrap=open -o $@ tests/test_daemon.c src/power.c src/clipboard.c $(CORE_SOURCES)

check: build/mxguest-agentd build/test-power build/test-daemon build/test-clipboard
	./build/mxguest-agentd --check
	./build/test-power
	./build/test-clipboard
	./build/test-daemon

clean:
	rm -rf build
