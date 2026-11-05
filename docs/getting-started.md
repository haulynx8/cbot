# Getting Started with cbot

`cbot` is designed to be lightweight, modular, and predictable.

## Core Concepts

### 1. The Bot Lifecycle
Create an instance using `cbot_new(cfg)`:
```c
Cbot *bot = cbot_new(NULL); /* NULL applies sane defaults */
```
The configuration options include:
- `worker_threads`: Number of worker threads (default `4`). Set to `0` for synchronous, single-threaded execution (ideal for unit testing).
- `queue_size`: Number of pending events allowed in the thread pool queue (default `128`).
- `dedupe_window`: Size of the sliding window for event ID deduplication (default `1024`).
- `poll_timeout_ms`: Poll timeout in milliseconds for adapters (default `30000`).

### 2. Adapters
Adapters implement the `CbotAdapterVTable` interface. To attach an adapter:
```c
cbot_add_adapter(bot, cbot_adapter_console());
```
The bot takes ownership of the adapter and frees it on `cbot_free(bot)`.

### 3. Routing
Register command handlers, fallback text handlers, and callback handlers:
```c
/* Matches "/help" */
cbot_command(bot, "help", on_help);

/* Matches buttons whose callback_data starts with "action:" */
cbot_on_callback(bot, "action:", on_action);

/* Fallback for regular text messages or unmatched commands */
cbot_on_text(bot, on_fallback);
```

### 4. Responding
Inside any handler, you can reply directly to the originating chat:
```c
static void on_help(CbotCtx *ctx, const CbotEvent *ev) {
    cbot_reply(ctx, "Here is help info...");
    cbot_replyf(ctx, "Hello %s, your chat ID is %s", ev->user_name, ev->chat_id);
}
```

### 5. Conversation State
Store per-user state with `cbot_state_set` and retrieve it with `cbot_state_get`:
```c
cbot_state_set(ctx, "step", "awaiting_email");

const char *step = cbot_state_get(ctx, "step");
/* Strings returned by cbot_state_get remain valid until the handler completes */
```
Setting a key to `NULL` deletes it from the store.
