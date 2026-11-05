/*
 * cbot.h - public API sketch for a general-purpose bot framework in C.
 *
 * Design goals:
 *   - One small header is the whole public surface.
 *   - Platforms (Telegram, IRC, ...) are plug-in adapters behind a vtable.
 *   - Handlers see a normalized event, never raw platform data.
 *   - Ownership rules are explicit: the framework owns events and replies
 *     for the duration of a handler; copy anything you want to keep.
 */
#ifndef CBOT_H
#define CBOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CBOT_VERSION_MAJOR 0
#define CBOT_VERSION_MINOR 1

/* ---------- Errors ---------- */
typedef enum {
    CBOT_OK = 0,
    CBOT_ERR_NOMEM,
    CBOT_ERR_NETWORK,
    CBOT_ERR_RATE_LIMITED,
    CBOT_ERR_PROTOCOL,
    CBOT_ERR_INVALID_ARG,
    CBOT_ERR_STOPPED
} CbotStatus;

const char *cbot_strerror(CbotStatus s);

/* ---------- Opaque types ---------- */
typedef struct Cbot        Cbot;         /* the bot itself            */
typedef struct CbotAdapter CbotAdapter;  /* a platform connection     */
typedef struct CbotCtx     CbotCtx;      /* per-event handler context */

/* ---------- Normalized event ---------- */
typedef enum {
    CBOT_EV_MESSAGE,
    CBOT_EV_COMMAND,     /* "/start args" style */
    CBOT_EV_CALLBACK,    /* button press        */
    CBOT_EV_JOIN,
    CBOT_EV_LEAVE
} CbotEventType;

typedef struct {
    CbotEventType type;
    const char *platform;   /* "telegram", "irc", "console" */
    const char *user_id;
    const char *user_name;  /* may be NULL */
    const char *chat_id;
    const char *text;       /* full text; NULL for non-text events  */
    const char *command;    /* "start" (no slash); NULL if not cmd  */
    const char *args;       /* text after the command; may be NULL  */
    const char *callback_data; /* for CBOT_EV_CALLBACK              */
    uint64_t    event_id;   /* platform id, used for deduplication  */
    void       *raw;        /* adapter-specific payload (read-only) */
} CbotEvent;

/* ---------- Handlers and middleware ---------- */
typedef void (*CbotHandler)(CbotCtx *ctx, const CbotEvent *ev);

/* Return 1 to continue down the chain, 0 to stop (event is consumed). */
typedef int (*CbotMiddleware)(CbotCtx *ctx, const CbotEvent *ev);

/* ---------- Adapter interface (the "phone line" plug) ---------- */
typedef struct {
    const char *name;

    /* Block until an event is available (or timeout_ms passes).
     * Fill *out with strings that stay valid until the next poll()
     * or until adapter->release_event() is called.
     * Return CBOT_OK, or CBOT_ERR_* on failure. */
    CbotStatus (*poll)(CbotAdapter *a, CbotEvent *out, int timeout_ms);

    CbotStatus (*send_text)(CbotAdapter *a, const char *chat_id,
                            const char *text);

    void (*release_event)(CbotAdapter *a, CbotEvent *ev); /* optional */
    void (*destroy)(CbotAdapter *a);
} CbotAdapterVTable;

struct CbotAdapter {
    const CbotAdapterVTable *vt;
    void *impl;             /* adapter private state */
};

/* Built-in adapters (each lives in its own optional module). */
CbotAdapter *cbot_adapter_console(void);
CbotAdapter *cbot_adapter_telegram(const char *token);
CbotAdapter *cbot_adapter_irc(const char *host, int port,
                              const char *nick, const char *channel);

/* ---------- Bot lifecycle ---------- */
typedef struct {
    int   worker_threads;   /* default 4  */
    int   queue_size;       /* default 128 */
    int   dedupe_window;    /* remember last N event_ids; default 1024 */
    int   poll_timeout_ms;  /* default 30000 */
    void *user_data;        /* yours; retrieved via cbot_user_data() */
} CbotConfig;

Cbot *cbot_new(const CbotConfig *cfg);       /* cfg may be NULL */
void  cbot_free(Cbot *bot);

/* The bot takes ownership of the adapter. */
CbotStatus cbot_add_adapter(Cbot *bot, CbotAdapter *adapter);

/* Run the event loop until cbot_stop() is called. */
int  cbot_run(Cbot *bot);
void cbot_stop(Cbot *bot);                   /* async-signal-safe */

/* ---------- Registration ---------- */
void cbot_use(Cbot *bot, CbotMiddleware mw);
void cbot_command(Cbot *bot, const char *name, CbotHandler h);
void cbot_on_text(Cbot *bot, CbotHandler h);          /* fallback */
void cbot_on_callback(Cbot *bot, const char *prefix, CbotHandler h);

/* ---------- Inside a handler ---------- */
void *cbot_user_data(CbotCtx *ctx);

CbotStatus cbot_reply(CbotCtx *ctx, const char *text);
CbotStatus cbot_replyf(CbotCtx *ctx, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
;
CbotStatus cbot_send(CbotCtx *ctx, const char *chat_id, const char *text);

/* Per-user key/value memory (in-memory by default; SQLite module optional).
 * cbot_state_get returns a string owned by the framework, valid until the
 * handler returns. */
const char *cbot_state_get(CbotCtx *ctx, const char *key);
CbotStatus  cbot_state_set(CbotCtx *ctx, const char *key, const char *value);

/* ---------- Built-in middleware ---------- */
int cbot_mw_log(CbotCtx *ctx, const CbotEvent *ev);
int cbot_mw_ratelimit(CbotCtx *ctx, const CbotEvent *ev);

#ifdef __cplusplus
}
#endif
#endif /* CBOT_H */
