# cbot

A modular, high-performance bot framework written in C (C11) for building bots across Telegram, IRC, Discord, and Console.

## Features

- **Single Public Header:** Everything public lives in [`include/cbot.h`](include/cbot.h). Internal structures are kept strictly opaque to preserve ABI stability.
- **Normalized Events:** Handlers receive a clean `CbotEvent` regardless of platform, with a `raw` pointer escape hatch for platform-specific features.
- **VTable Adapter Interface:** Adapters plug in through a simple struct of function pointers (`CbotAdapterVTable`), making new platforms easy to add without touching the core.
- **Explicit Memory Ownership:** The framework owns every event string for the duration of the handler; handlers copy anything they need to keep.
- **Router & Fallbacks:** Direct command routing (`/start`, `/weather <city>`), button callback prefix matching, and text fallback handlers.
- **Middleware Chain:** Extensible middleware for logging, rate limiting, authentication, and deduplication.
- **Threadpool Concurrency:** Worker thread pool for concurrent event dispatching, with zero-thread synchronous mode for deterministic testing.
- **Per-User State Storage:** Thread-safe key/value conversation memory that guarantees string lifetimes for the duration of the handler.
- **Zero-Network Testing:** Test your bot completely offline using the built-in Console adapter.

## Quickstart

```c
#include <cbot.h>

static void hello(CbotCtx *ctx, const CbotEvent *ev) {
    cbot_replyf(ctx, "Hi %s!", ev->user_name ? ev->user_name : "there");
}

int main(void) {
    Cbot *bot = cbot_new(NULL);
    cbot_add_adapter(bot, cbot_adapter_console());
    cbot_command(bot, "start", hello);
    return cbot_run(bot);
}
```

## Building & Testing

```bash
# Build the static library, tests, and example
make all

# Run test suite
make test

# Run test suite under AddressSanitizer and UndefinedBehaviorSanitizer
make ASAN=1 test
```

## Running the Example

```bash
./examples/echo_bot
```
Or interact via piped commands:
```bash
printf "/start\n/setname Alice\n/whoami\nHello!\n" | ./examples/echo_bot
```

## Project Structure

```
cbot/
├── include/
│   └── cbot.h               # Public API header
├── src/
│   ├── cbot_internal.h      # Private internals & definitions
│   ├── core.c               # Lifecycle, deduplication, event dispatch
│   ├── router.c             # Command, callback, & text routing
│   ├── middleware.c         # Middleware chain & built-in handlers
│   ├── state.c              # Thread-safe in-memory key/value store
│   └── threadpool.c         # POSIX worker thread pool
├── adapters/
│   ├── console.c            # Built-in stdin/stdout console adapter
│   ├── telegram.c           # Telegram adapter (slice 2)
│   └── irc.c                # IRC adapter (slice 3)
├── examples/
│   └── echo_bot.c           # Interactive console bot demonstration
├── tests/
│   ├── test_core.c          # Core lifecycle, dedupe & concurrency tests
│   ├── test_router.c        # Command, prefix, and fallback tests
│   ├── test_state.c         # Per-user state storage & isolation tests
│   ├── test_middleware.c     # Chain ordering & short-circuit tests
│   └── test_console.c       # Console adapter pipe integration test
├── cbot.pc                  # pkg-config definition
└── Makefile
```
