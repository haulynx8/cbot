CC ?= gcc
AR ?= ar
PREFIX ?= /usr/local

CFLAGS ?= -Wall -Wextra -Werror -std=c11 -pthread -Iinclude -D_POSIX_C_SOURCE=200809L -g -O2
LIB_CFLAGS = $(CFLAGS) -fPIC

ifeq ($(ASAN),1)
    CFLAGS += -fsanitize=address,undefined
    LDFLAGS += -fsanitize=address,undefined
endif

# Core framework & built-in adapters
CORE_SRCS = src/core.c src/router.c src/middleware.c src/state.c src/threadpool.c \
            adapters/console.c adapters/telegram.c adapters/irc.c
CORE_OBJS = $(CORE_SRCS:.c=.o)
CORE_HEADERS = include/cbot.h src/cbot_internal.h

STATIC_LIB = libcbot.a

# Test executables
TEST_SRCS = tests/test_core.c tests/test_router.c tests/test_state.c \
            tests/test_middleware.c tests/test_console.c
TEST_BINS = $(TEST_SRCS:.c=)

# Examples
EXAMPLES = examples/echo_bot

.PHONY: all test clean install

all: $(STATIC_LIB) $(EXAMPLES) $(TEST_BINS)

%.o: %.c $(CORE_HEADERS)
	$(CC) $(LIB_CFLAGS) -c $< -o $@

$(STATIC_LIB): $(CORE_OBJS)
	$(AR) rcs $@ $^

examples/echo_bot: examples/echo_bot.c $(STATIC_LIB)
	$(CC) $(CFLAGS) $< -L. -lcbot $(LDFLAGS) -o $@

tests/test_%: tests/test_%.c $(STATIC_LIB)
	$(CC) $(CFLAGS) $< -L. -lcbot $(LDFLAGS) -o $@

test: $(TEST_BINS)
	@echo "=== Running cbot test suite ==="
	@for t in $(TEST_BINS); do \
		echo "Running $$t..."; \
		./$$t || exit 1; \
	done
	@echo "=== All tests passed successfully! ==="

clean:
	rm -f $(CORE_OBJS) $(STATIC_LIB) $(TEST_BINS) $(EXAMPLES)

install: $(STATIC_LIB)
	install -d $(PREFIX)/include $(PREFIX)/lib $(PREFIX)/lib/pkgconfig
	install -m 644 include/cbot.h $(PREFIX)/include/
	install -m 644 $(STATIC_LIB) $(PREFIX)/lib/
	install -m 644 cbot.pc $(PREFIX)/lib/pkgconfig/
