#ifndef CBOT_INTERNAL_H
#define CBOT_INTERNAL_H

#include "cbot.h"

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *name;
    CbotHandler handler;
} CbotRouteCommand;

typedef struct {
    char *prefix;
    CbotHandler handler;
} CbotRouteCallback;

typedef struct CbotDedupe CbotDedupe;
typedef struct CbotStateStore CbotStateStore;
typedef struct CbotThreadPool CbotThreadPool;

struct Cbot {
    CbotConfig config;

    CbotAdapter **adapters;
    size_t num_adapters;
    size_t cap_adapters;

    CbotRouteCommand *commands;
    size_t num_commands;
    size_t cap_commands;

    CbotRouteCallback *callbacks;
    size_t num_callbacks;
    size_t cap_callbacks;

    CbotHandler on_text;

    CbotMiddleware *middlewares;
    size_t num_middlewares;
    size_t cap_middlewares;

    CbotDedupe *dedupe;
    CbotStateStore *state_store;
    CbotThreadPool *pool;

    volatile sig_atomic_t stop_flag;
    pthread_mutex_t lock;
};

struct CbotCtx {
    Cbot *bot;
    CbotAdapter *adapter;
    CbotEvent event;

    char **state_allocs;
    size_t num_state_allocs;
    size_t cap_state_allocs;
};

/* Event helpers */
void cbot_event_clone(const CbotEvent *src, CbotEvent *dst);
void cbot_event_free_fields(CbotEvent *ev);
void cbot_event_normalize(CbotEvent *ev);

/* Task execution */
void cbot_execute_task(Cbot *bot, CbotAdapter *adapter, const CbotEvent *ev);
void cbot_dispatch(Cbot *bot, CbotAdapter *adapter, const CbotEvent *raw_ev);

/* Router */
void cbot_router_dispatch(CbotCtx *ctx, const CbotEvent *ev);

/* State store */
CbotStateStore *cbot_state_store_create(void);
void cbot_state_store_destroy(CbotStateStore *store);
char *cbot_state_store_get(CbotStateStore *store, const char *user_id, const char *key);
CbotStatus cbot_state_store_set(CbotStateStore *store, const char *user_id, const char *key, const char *value);
void cbot_ctx_free_state_allocs(CbotCtx *ctx);

/* Dedupe */
CbotDedupe *cbot_dedupe_create(size_t capacity);
void cbot_dedupe_destroy(CbotDedupe *d);
int cbot_dedupe_check_and_add(CbotDedupe *d, uint64_t event_id);

/* Thread pool */
typedef void (*cbot_task_fn)(void *arg);
CbotThreadPool *cbot_threadpool_create(int num_threads, int queue_size);
int cbot_threadpool_submit(CbotThreadPool *pool, cbot_task_fn fn, void *arg);
void cbot_threadpool_drain(CbotThreadPool *pool);
void cbot_threadpool_destroy(CbotThreadPool *pool);

#endif /* CBOT_INTERNAL_H */
