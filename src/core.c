#include "cbot_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Errors ---------- */
const char *cbot_strerror(CbotStatus s) {
    switch (s) {
        case CBOT_OK:               return "Success";
        case CBOT_ERR_NOMEM:        return "Out of memory";
        case CBOT_ERR_NETWORK:      return "Network error";
        case CBOT_ERR_RATE_LIMITED: return "Rate limited";
        case CBOT_ERR_PROTOCOL:     return "Protocol error";
        case CBOT_ERR_INVALID_ARG:  return "Invalid argument";
        case CBOT_ERR_STOPPED:      return "Stopped";
        default:                    return "Unknown error";
    }
}

/* ---------- Deduplication ---------- */
struct CbotDedupe {
    uint64_t *entries;
    size_t capacity;
    size_t head;
    size_t count;
    pthread_mutex_t lock;
};

CbotDedupe *cbot_dedupe_create(size_t capacity) {
    if (capacity == 0) capacity = 1024;
    CbotDedupe *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->capacity = capacity;
    d->entries = malloc(capacity * sizeof(uint64_t));
    if (!d->entries) {
        free(d);
        return NULL;
    }
    pthread_mutex_init(&d->lock, NULL);
    return d;
}

void cbot_dedupe_destroy(CbotDedupe *d) {
    if (!d) return;
    pthread_mutex_destroy(&d->lock);
    free(d->entries);
    free(d);
}

int cbot_dedupe_check_and_add(CbotDedupe *d, uint64_t event_id) {
    if (!d || event_id == 0) return 0;

    pthread_mutex_lock(&d->lock);
    for (size_t i = 0; i < d->count; i++) {
        if (d->entries[i] == event_id) {
            pthread_mutex_unlock(&d->lock);
            return 1; /* duplicate */
        }
    }

    d->entries[d->head] = event_id;
    d->head = (d->head + 1) % d->capacity;
    if (d->count < d->capacity) {
        d->count++;
    }
    pthread_mutex_unlock(&d->lock);
    return 0; /* new */
}

/* ---------- Event Helpers ---------- */
void cbot_event_clone(const CbotEvent *src, CbotEvent *dst) {
    if (!src || !dst) return;
    dst->type = src->type;
    dst->event_id = src->event_id;
    dst->raw = src->raw;
    dst->platform = src->platform ? strdup(src->platform) : NULL;
    dst->user_id = src->user_id ? strdup(src->user_id) : NULL;
    dst->user_name = src->user_name ? strdup(src->user_name) : NULL;
    dst->chat_id = src->chat_id ? strdup(src->chat_id) : NULL;
    dst->text = src->text ? strdup(src->text) : NULL;
    dst->command = src->command ? strdup(src->command) : NULL;
    dst->args = src->args ? strdup(src->args) : NULL;
    dst->callback_data = src->callback_data ? strdup(src->callback_data) : NULL;
}

void cbot_event_free_fields(CbotEvent *ev) {
    if (!ev) return;
    free((void *)ev->platform);
    free((void *)ev->user_id);
    free((void *)ev->user_name);
    free((void *)ev->chat_id);
    free((void *)ev->text);
    free((void *)ev->command);
    free((void *)ev->args);
    free((void *)ev->callback_data);
    memset(ev, 0, sizeof(*ev));
}

void cbot_event_normalize(CbotEvent *ev) {
    if (!ev || !ev->text) return;

    const char *p = ev->text;
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '/') {
        p++; /* Skip leading slash */
        const char *cmd_start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '@') {
            p++;
        }
        size_t cmd_len = p - cmd_start;
        if (cmd_len > 0 && ev->command == NULL) {
            char *cmd = malloc(cmd_len + 1);
            if (cmd) {
                memcpy(cmd, cmd_start, cmd_len);
                cmd[cmd_len] = '\0';
                ev->command = cmd;
            }
        }

        /* If bot mention present (e.g. /cmd@mybot), skip past it */
        if (*p == '@') {
            while (*p && *p != ' ' && *p != '\t') p++;
        }

        while (*p == ' ' || *p == '\t') p++;
        if (*p != '\0' && ev->args == NULL) {
            ev->args = strdup(p);
        }
        ev->type = CBOT_EV_COMMAND;
    } else if (ev->type == 0) {
        ev->type = CBOT_EV_MESSAGE;
    }
}

/* ---------- Bot Lifecycle ---------- */
Cbot *cbot_new(const CbotConfig *cfg) {
    Cbot *bot = calloc(1, sizeof(*bot));
    if (!bot) return NULL;

    if (cfg) {
        bot->config = *cfg;
    } else {
        bot->config.worker_threads = 4;
        bot->config.queue_size = 128;
        bot->config.dedupe_window = 1024;
        bot->config.poll_timeout_ms = 30000;
        bot->config.user_data = NULL;
    }

    if (bot->config.worker_threads < 0) bot->config.worker_threads = 0;
    if (bot->config.queue_size <= 0) bot->config.queue_size = 128;
    if (bot->config.dedupe_window <= 0) bot->config.dedupe_window = 1024;
    if (bot->config.poll_timeout_ms <= 0) bot->config.poll_timeout_ms = 1000;

    pthread_mutex_init(&bot->lock, NULL);

    bot->state_store = cbot_state_store_create();
    if (!bot->state_store) {
        cbot_free(bot);
        return NULL;
    }

    bot->dedupe = cbot_dedupe_create((size_t)bot->config.dedupe_window);
    if (!bot->dedupe) {
        cbot_free(bot);
        return NULL;
    }

    if (bot->config.worker_threads > 0) {
        bot->pool = cbot_threadpool_create(bot->config.worker_threads, bot->config.queue_size);
        if (!bot->pool) {
            cbot_free(bot);
            return NULL;
        }
    }

    return bot;
}

void cbot_free(Cbot *bot) {
    if (!bot) return;

    if (bot->pool) {
        cbot_threadpool_destroy(bot->pool);
        bot->pool = NULL;
    }

    for (size_t i = 0; i < bot->num_adapters; i++) {
        if (bot->adapters[i]) {
            if (bot->adapters[i]->vt && bot->adapters[i]->vt->destroy) {
                bot->adapters[i]->vt->destroy(bot->adapters[i]);
            }
        }
    }
    free(bot->adapters);

    for (size_t i = 0; i < bot->num_commands; i++) {
        free(bot->commands[i].name);
    }
    free(bot->commands);

    for (size_t i = 0; i < bot->num_callbacks; i++) {
        free(bot->callbacks[i].prefix);
    }
    free(bot->callbacks);

    free(bot->middlewares);

    if (bot->state_store) {
        cbot_state_store_destroy(bot->state_store);
    }

    if (bot->dedupe) {
        cbot_dedupe_destroy(bot->dedupe);
    }

    pthread_mutex_destroy(&bot->lock);
    free(bot);
}

CbotStatus cbot_add_adapter(Cbot *bot, CbotAdapter *adapter) {
    if (!bot || !adapter) return CBOT_ERR_INVALID_ARG;

    pthread_mutex_lock(&bot->lock);
    if (bot->num_adapters == bot->cap_adapters) {
        size_t new_cap = bot->cap_adapters == 0 ? 4 : bot->cap_adapters * 2;
        CbotAdapter **new_arr = realloc(bot->adapters, new_cap * sizeof(CbotAdapter *));
        if (!new_arr) {
            pthread_mutex_unlock(&bot->lock);
            return CBOT_ERR_NOMEM;
        }
        bot->adapters = new_arr;
        bot->cap_adapters = new_cap;
    }
    bot->adapters[bot->num_adapters++] = adapter;
    pthread_mutex_unlock(&bot->lock);

    return CBOT_OK;
}

void cbot_stop(Cbot *bot) {
    if (bot) {
        bot->stop_flag = 1;
    }
}

/* ---------- Execution and Dispatch ---------- */
void cbot_execute_task(Cbot *bot, CbotAdapter *adapter, const CbotEvent *ev) {
    CbotCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.bot = bot;
    ctx.adapter = adapter;
    ctx.event = *ev; /* Transfer ownership of allocated fields to ctx.event */
    cbot_event_normalize(&ctx.event);

    pthread_mutex_lock(&bot->lock);
    size_t num_mws = bot->num_middlewares;
    CbotMiddleware *mws = NULL;
    if (num_mws > 0) {
        mws = malloc(num_mws * sizeof(CbotMiddleware));
        if (mws) {
            memcpy(mws, bot->middlewares, num_mws * sizeof(CbotMiddleware));
        }
    }
    pthread_mutex_unlock(&bot->lock);

    int allow = 1;
    if (mws) {
        for (size_t i = 0; i < num_mws; i++) {
            if (!mws[i](&ctx, &ctx.event)) {
                allow = 0;
                break;
            }
        }
        free(mws);
    }

    if (allow) {
        cbot_router_dispatch(&ctx, &ctx.event);
    }

    cbot_event_free_fields(&ctx.event);
    cbot_ctx_free_state_allocs(&ctx);
}

typedef struct {
    Cbot *bot;
    CbotAdapter *adapter;
    CbotEvent event;
} CbotTaskContext;

static void task_worker_cb(void *arg) {
    CbotTaskContext *task = (CbotTaskContext *)arg;
    cbot_execute_task(task->bot, task->adapter, &task->event);
    free(task);
}

void cbot_dispatch(Cbot *bot, CbotAdapter *adapter, const CbotEvent *raw_ev) {
    if (bot->pool) {
        CbotTaskContext *task = malloc(sizeof(*task));
        if (!task) return;
        task->bot = bot;
        task->adapter = adapter;
        memset(&task->event, 0, sizeof(task->event));
        cbot_event_clone(raw_ev, &task->event);

        if (cbot_threadpool_submit(bot->pool, task_worker_cb, task) != 0) {
            cbot_event_free_fields(&task->event);
            free(task);
        }
    } else {
        CbotEvent local_ev;
        memset(&local_ev, 0, sizeof(local_ev));
        cbot_event_clone(raw_ev, &local_ev);
        cbot_execute_task(bot, adapter, &local_ev);
    }
}

int cbot_run(Cbot *bot) {
    if (!bot) return -1;
    bot->stop_flag = 0;

    if (bot->num_adapters == 0) {
        return 0;
    }

    int timeout_ms = bot->config.poll_timeout_ms;
    if (timeout_ms <= 0) timeout_ms = 1000;
    if (bot->num_adapters > 1) {
        timeout_ms = 50;
    }

    while (!bot->stop_flag) {
        for (size_t i = 0; i < bot->num_adapters; i++) {
            if (bot->stop_flag) break;
            CbotAdapter *adapter = bot->adapters[i];
            if (!adapter || !adapter->vt || !adapter->vt->poll) continue;

            CbotEvent raw_ev;
            memset(&raw_ev, 0, sizeof(raw_ev));
            CbotStatus status = adapter->vt->poll(adapter, &raw_ev, timeout_ms);

            if (status == CBOT_ERR_STOPPED) {
                bot->stop_flag = 1;
                break;
            }

            if (status == CBOT_OK) {
                if (raw_ev.event_id != 0 && cbot_dedupe_check_and_add(bot->dedupe, raw_ev.event_id)) {
                    if (adapter->vt->release_event) {
                        adapter->vt->release_event(adapter, &raw_ev);
                    }
                    continue;
                }

                cbot_dispatch(bot, adapter, &raw_ev);

                if (adapter->vt->release_event) {
                    adapter->vt->release_event(adapter, &raw_ev);
                }
            }
        }
    }

    if (bot->pool) {
        cbot_threadpool_drain(bot->pool);
    }

    return 0;
}

/* ---------- Inside a Handler ---------- */
void *cbot_user_data(CbotCtx *ctx) {
    if (!ctx || !ctx->bot) return NULL;
    return ctx->bot->config.user_data;
}

CbotStatus cbot_send(CbotCtx *ctx, const char *chat_id, const char *text) {
    if (!ctx || !chat_id || !text) return CBOT_ERR_INVALID_ARG;
    if (ctx->adapter && ctx->adapter->vt && ctx->adapter->vt->send_text) {
        return ctx->adapter->vt->send_text(ctx->adapter, chat_id, text);
    }
    return CBOT_ERR_PROTOCOL;
}

CbotStatus cbot_reply(CbotCtx *ctx, const char *text) {
    if (!ctx || !text) return CBOT_ERR_INVALID_ARG;
    const char *chat_id = ctx->event.chat_id;
    if (!chat_id) return CBOT_ERR_INVALID_ARG;
    return cbot_send(ctx, chat_id, text);
}

CbotStatus cbot_replyf(CbotCtx *ctx, const char *fmt, ...) {
    if (!ctx || !fmt) return CBOT_ERR_INVALID_ARG;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);

    va_list ap_copy;
    va_copy(ap_copy, ap);
    int needed = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (needed < 0) {
        va_end(ap_copy);
        return CBOT_ERR_INVALID_ARG;
    }

    if ((size_t)needed < sizeof(buf)) {
        va_end(ap_copy);
        return cbot_reply(ctx, buf);
    }

    char *dyn = malloc((size_t)needed + 1);
    if (!dyn) {
        va_end(ap_copy);
        return CBOT_ERR_NOMEM;
    }

    vsnprintf(dyn, (size_t)needed + 1, fmt, ap_copy);
    va_end(ap_copy);

    CbotStatus res = cbot_reply(ctx, dyn);
    free(dyn);
    return res;
}
