# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror
CORE ?= deps/core
CORE_SOURCES := $(addprefix $(CORE)/,$(filter src/%.c,$(shell cat $(CORE)/sources.list)))
CORE_HEADERS := $(addprefix $(CORE)/,$(filter %.h,$(shell cat $(CORE)/sources.list)))
DAEMON_SOURCES := src/power.c src/clipboard.c src/session.c src/integration.c
DAEMON_HEADERS := src/power.h src/clipboard.h src/session.h src/integration.h
INCLUDES := -I$(CORE)/include -I$(CORE)/src -Isrc

.PHONY: all check clean

all: build/mxguest-agentd

build/mxguest-agentd: src/mxguest_agentd.c $(DAEMON_SOURCES) $(DAEMON_HEADERS) $(CORE_SOURCES) $(CORE_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ src/mxguest_agentd.c $(DAEMON_SOURCES) $(CORE_SOURCES)

build/test-clipboard: tests/test_clipboard.c src/clipboard.c src/clipboard.h $(CORE_SOURCES) $(CORE_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ tests/test_clipboard.c src/clipboard.c $(CORE_SOURCES)

build/test-session: tests/test_session.c src/session.c src/session.h $(CORE_SOURCES) $(CORE_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ tests/test_session.c src/session.c $(CORE_SOURCES)

build/test-integration: tests/test_integration.c src/integration.c src/integration.h src/session.c src/session.h $(CORE_SOURCES) $(CORE_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ tests/test_integration.c src/integration.c src/session.c $(CORE_SOURCES)

build/test-power: tests/test_power.c src/mxguest_agentd.c $(DAEMON_SOURCES) $(DAEMON_HEADERS) $(CORE_SOURCES) $(CORE_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) $(INCLUDES) -Wl,--wrap=execv -o $@ tests/test_power.c $(DAEMON_SOURCES) $(CORE_SOURCES)

build/test-daemon: tests/test_daemon.c src/mxguest_agentd.c $(DAEMON_SOURCES) $(DAEMON_HEADERS) $(CORE_SOURCES) $(CORE_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) $(INCLUDES) -Wl,--wrap=open -o $@ tests/test_daemon.c $(DAEMON_SOURCES) $(CORE_SOURCES)

check: build/mxguest-agentd build/test-power build/test-daemon build/test-clipboard build/test-session build/test-integration
	./build/mxguest-agentd --check
	./build/test-power
	./build/test-clipboard
	./build/test-session
	./build/test-integration
	./build/test-daemon

clean:
	rm -rf build
