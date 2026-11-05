#include "cbot_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void cbot_use(Cbot *bot, CbotMiddleware mw) {
    if (!bot || !mw) return;

    pthread_mutex_lock(&bot->lock);
    if (bot->num_middlewares == bot->cap_middlewares) {
        size_t new_cap = bot->cap_middlewares == 0 ? 4 : bot->cap_middlewares * 2;
        CbotMiddleware *new_mws = realloc(bot->middlewares, new_cap * sizeof(CbotMiddleware));
        if (!new_mws) {
            pthread_mutex_unlock(&bot->lock);
            return;
        }
        bot->middlewares = new_mws;
        bot->cap_middlewares = new_cap;
    }
    bot->middlewares[bot->num_middlewares++] = mw;
    pthread_mutex_unlock(&bot->lock);
}

int cbot_mw_log(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    if (!ev) return 1;

    const char *type_str = "UNKNOWN";
    switch (ev->type) {
        case CBOT_EV_MESSAGE:  type_str = "MESSAGE"; break;
        case CBOT_EV_COMMAND:  type_str = "COMMAND"; break;
        case CBOT_EV_CALLBACK: type_str = "CALLBACK"; break;
        case CBOT_EV_JOIN:     type_str = "JOIN"; break;
        case CBOT_EV_LEAVE:    type_str = "LEAVE"; break;
    }

    fprintf(stderr, "[cbot log] [%s] type=%s chat=%s user=%s text=\"%s\"\n",
            ev->platform ? ev->platform : "unknown",
            type_str,
            ev->chat_id ? ev->chat_id : "-",
            ev->user_id ? ev->user_id : "-",
            ev->text ? ev->text : "");
    return 1;
}

#define RATELIMIT_WINDOW_SECS 5
#define RATELIMIT_MAX_EVENTS 20
#define RATELIMIT_BUCKETS 64

typedef struct RateLimitNode {
    char *user_id;
    time_t window_start;
    int count;
    struct RateLimitNode *next;
} RateLimitNode;

static RateLimitNode *g_rl_buckets[RATELIMIT_BUCKETS] = {0};
static pthread_mutex_t g_rl_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t rl_hash(const char *str) {
    uint32_t h = 5381;
    if (str) {
        while (*str) {
            h = ((h << 5) + h) + (unsigned char)(*str++);
        }
    }
    return h % RATELIMIT_BUCKETS;
}

int cbot_mw_ratelimit(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    if (!ev || !ev->user_id) return 1;

    time_t now = time(NULL);
    uint32_t bucket = rl_hash(ev->user_id);

    pthread_mutex_lock(&g_rl_lock);
    RateLimitNode *node = g_rl_buckets[bucket];
    while (node) {
        if (strcmp(node->user_id, ev->user_id) == 0) {
            if (now - node->window_start >= RATELIMIT_WINDOW_SECS) {
                node->window_start = now;
                node->count = 1;
                pthread_mutex_unlock(&g_rl_lock);
                return 1;
            }

            node->count++;
            if (node->count > RATELIMIT_MAX_EVENTS) {
                pthread_mutex_unlock(&g_rl_lock);
                fprintf(stderr, "[cbot ratelimit] Rate limit exceeded for user %s\n", ev->user_id);
                return 0;
            }

            pthread_mutex_unlock(&g_rl_lock);
            return 1;
        }
        node = node->next;
    }

    /* Add new user entry */
    RateLimitNode *new_node = calloc(1, sizeof(*new_node));
    if (new_node) {
        new_node->user_id = strdup(ev->user_id);
        new_node->window_start = now;
        new_node->count = 1;
        new_node->next = g_rl_buckets[bucket];
        g_rl_buckets[bucket] = new_node;
    }

    pthread_mutex_unlock(&g_rl_lock);
    return 1;
}
