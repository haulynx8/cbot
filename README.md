# cbot

A modular, high-performance bot framework written in C (C11) for building bots across Telegram, IRC, Discord, and Console.

## Features

- **Single Public Header:** Everything public lives in [`include/cbot.h`](include/cbot.h) (and optional [`include/cbot_telegram.h`](include/cbot_telegram.h)). Internal structures are kept strictly opaque to preserve ABI stability.
- **Normalized Events:** Handlers receive a clean `CbotEvent` regardless of platform, with a `raw` pointer escape hatch for platform-specific features.
- **VTable Adapter Interface:** Adapters plug in through a struct of function pointers (`CbotAdapterVTable`), making new platforms easy to add without touching the core.
- **Explicit Memory Ownership:** The framework owns every event string for the duration of the handler; handlers copy anything they need to keep.
- **Router & Fallbacks:** Direct command routing (`/start`, `/weather <city>`), button callback prefix matching, and text fallback handlers.
- **Middleware Chain:** Extensible middleware for logging, rate limiting, authentication, and deduplication.
- **Threadpool Concurrency:** Worker thread pool for concurrent event dispatching, with zero-thread synchronous mode for deterministic testing.
- **Per-User State Storage:** Thread-safe key/value conversation memory that guarantees string lifetimes for the duration of the handler.
- **Telegram Adapter:**
  - **Long Polling:** Polls `getUpdates` with offset tracking over HTTPS via `libcurl`.
  - **Webhook Mode:** Directly attaches to a `cweb` HTTP route via `cbot_telegram_attach_cweb()`.
  - **Outbound Client:** Robust `sendMessage` with automatic HTTP 429 rate limit backoff and retry (`parameters.retry_after`), 5xx exponential backoff, JSON escaping, and token masking.
- **Zero-Network Testing:** Test your bot completely offline using the built-in Console adapter.

## Quickstart (Console)

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

## Quickstart (Telegram with cweb Webhook)

```c
#include <cbot.h>
#include <cbot_telegram.h>
#include <cweb.h>

static void hello(CbotCtx *ctx, const CbotEvent *ev) {
    cbot_replyf(ctx, "Hello %s!", ev->user_name ? ev->user_name : "there");
}

int main(void) {
    Cbot *bot = cbot_new(NULL);
    CbotAdapter *tg = cbot_adapter_telegram_webhook("YOUR_BOT_TOKEN");
    cbot_add_adapter(bot, tg);

    /* Attach incoming Telegram webhook POST requests to cweb */
    CwebApp app = {0};
    cbot_telegram_attach_cweb(tg, &app, "/webhook");

    cbot_command(bot, "start", hello);
    return cbot_run(bot);
}
```

## Building & Testing

```bash
# Build libraries (libcbot.a, libcbot_telegram.a), tests, and examples
make all

# Run full test suite
make test

# Run test suite under AddressSanitizer and UndefinedBehaviorSanitizer
make ASAN=1 test
```

## Running the Examples

```bash
# Console interactive bot:
./examples/echo_bot

# Telegram bot (long-polling):
TELEGRAM_BOT_TOKEN="123456:ABC-DEF..." ./examples/telegram_bot

# Telegram bot (cweb webhook):
TELEGRAM_BOT_TOKEN="123456:ABC-DEF..." ./examples/telegram_bot --webhook 8443 /telegram
```

## Project Structure

```
cbot/
├── include/
│   ├── cbot.h               # Public API header
│   └── cbot_telegram.h      # Telegram & cweb integration header
├── src/
│   ├── cbot_internal.h      # Private internals & definitions
│   ├── core.c               # Lifecycle, deduplication, event dispatch
│   ├── router.c             # Command, callback, & text routing
│   ├── middleware.c         # Middleware chain & built-in handlers
│   ├── state.c              # Thread-safe in-memory key/value store
│   └── threadpool.c         # POSIX worker thread pool
├── adapters/
│   ├── console.c            # Built-in stdin/stdout console adapter
│   ├── telegram.c           # Telegram webhook & long-polling adapter
│   └── irc.c                # IRC adapter (slice 3)
├── examples/
│   ├── echo_bot.c           # Interactive console bot demonstration
│   └── telegram_bot.c       # Telegram bot (polling or webhook via cweb)
├── tests/
│   ├── test_core.c          # Core lifecycle, dedupe & concurrency tests
│   ├── test_router.c        # Command, prefix, and fallback tests
│   ├── test_state.c         # Per-user state storage & isolation tests
│   ├── test_middleware.c     # Chain ordering & short-circuit tests
│   ├── test_console.c       # Console adapter pipe integration test
│   └── test_telegram.c      # Telegram parser, webhook, outbound & retry tests
├── cbot.pc                  # pkg-config definition
└── Makefile
```
