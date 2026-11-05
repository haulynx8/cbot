CC ?= gcc
AR ?= ar
PREFIX ?= /usr/local

CFLAGS ?= -Wall -Wextra -Werror -std=c11 -pthread -Iinclude -Isrc -D_POSIX_C_SOURCE=200809L -g -O2
LIB_CFLAGS = $(CFLAGS) -fPIC

ifeq ($(ASAN),1)
    CFLAGS += -fsanitize=address,undefined
    LDFLAGS += -fsanitize=address,undefined
endif

# Optional cweb framework location (default: ../cweb)
CWEB_DIR ?= ../cweb
CWEB_INC = -I$(CWEB_DIR)/include
CWEB_LIB = -L$(CWEB_DIR) -lcweb

# Optional libcurl
CURL_CFLAGS = $(shell pkg-config --cflags libcurl 2>/dev/null || echo "")
CURL_LIBS = $(shell pkg-config --libs libcurl 2>/dev/null || echo "-lcurl")

# Core framework (Zero external dependencies)
CORE_SRCS = src/core.c src/router.c src/middleware.c src/state.c src/threadpool.c \
            adapters/console.c adapters/irc.c
CORE_OBJS = $(CORE_SRCS:.c=.o)
CORE_HEADERS = include/cbot.h src/cbot_internal.h

# Telegram module (Requires libcurl and libcweb)
TG_SRCS = adapters/telegram.c
TG_OBJS = $(TG_SRCS:.c=.o)
TG_HEADERS = include/cbot.h include/cbot_telegram.h

STATIC_LIBS = libcbot.a libcbot_telegram.a

# Test executables
CORE_TEST_SRCS = tests/test_core.c tests/test_router.c tests/test_state.c \
                 tests/test_middleware.c tests/test_console.c
CORE_TEST_BINS = $(CORE_TEST_SRCS:.c=)

TG_TEST_BIN = tests/test_telegram

ALL_TESTS = $(CORE_TEST_BINS) $(TG_TEST_BIN)

# Examples
EXAMPLES = examples/echo_bot examples/telegram_bot

.PHONY: all test clean install

all: $(STATIC_LIBS) $(EXAMPLES) $(ALL_TESTS)

src/%.o: src/%.c $(CORE_HEADERS)
	$(CC) $(LIB_CFLAGS) -c $< -o $@

adapters/console.o: adapters/console.c $(CORE_HEADERS)
	$(CC) $(LIB_CFLAGS) -c $< -o $@

adapters/irc.o: adapters/irc.c $(CORE_HEADERS)
	$(CC) $(LIB_CFLAGS) -c $< -o $@

adapters/telegram.o: adapters/telegram.c $(TG_HEADERS)
	$(CC) $(LIB_CFLAGS) $(CWEB_INC) $(CURL_CFLAGS) -c $< -o $@

libcbot.a: $(CORE_OBJS)
	$(AR) rcs $@ $^

libcbot_telegram.a: $(TG_OBJS)
	$(AR) rcs $@ $^

examples/echo_bot: examples/echo_bot.c libcbot.a
	$(CC) $(CFLAGS) $< -L. -lcbot $(LDFLAGS) -o $@

examples/telegram_bot: examples/telegram_bot.c libcbot.a libcbot_telegram.a
	$(CC) $(CFLAGS) $(CWEB_INC) $(CURL_CFLAGS) $< -L. -lcbot_telegram -lcbot $(CWEB_LIB) $(CURL_LIBS) $(LDFLAGS) -o $@

tests/test_%: tests/test_%.c libcbot.a
	$(CC) $(CFLAGS) $< -L. -lcbot $(LDFLAGS) -o $@

tests/test_telegram: tests/test_telegram.c libcbot.a libcbot_telegram.a
	$(CC) $(CFLAGS) $(CWEB_INC) $(CURL_CFLAGS) $< -L. -lcbot_telegram -lcbot $(CWEB_LIB) $(CURL_LIBS) $(LDFLAGS) -o $@

test: $(ALL_TESTS)
	@echo "=== Running cbot full test suite ==="
	@for t in $(ALL_TESTS); do \
		echo "Running $$t..."; \
		./$$t || exit 1; \
	done
	@echo "=== All tests passed successfully! ==="

clean:
	rm -f $(CORE_OBJS) $(TG_OBJS) $(STATIC_LIBS) $(ALL_TESTS) $(EXAMPLES)

install: $(STATIC_LIBS)
	install -d $(PREFIX)/include $(PREFIX)/lib $(PREFIX)/lib/pkgconfig
	install -m 644 include/cbot.h $(PREFIX)/include/
	install -m 644 include/cbot_telegram.h $(PREFIX)/include/
	install -m 644 libcbot.a $(PREFIX)/lib/
	install -m 644 libcbot_telegram.a $(PREFIX)/lib/
	install -m 644 cbot.pc $(PREFIX)/lib/pkgconfig/
