#include "cbot_internal.h"

#include <stdlib.h>
#include <string.h>

#define STATE_BUCKET_COUNT 256

typedef struct CbotStateNode {
    char *user_id;
    char *key;
    char *value;
    struct CbotStateNode *next;
} CbotStateNode;

struct CbotStateStore {
    CbotStateNode *buckets[STATE_BUCKET_COUNT];
    pthread_mutex_t lock;
};

static uint32_t state_hash(const char *user_id, const char *key) {
    uint32_t h = 5381;
    if (user_id) {
        while (*user_id) {
            h = ((h << 5) + h) + (unsigned char)(*user_id++);
        }
    }
    h = ((h << 5) + h) + ':';
    if (key) {
        while (*key) {
            h = ((h << 5) + h) + (unsigned char)(*key++);
        }
    }
    return h % STATE_BUCKET_COUNT;
}

CbotStateStore *cbot_state_store_create(void) {
    CbotStateStore *store = calloc(1, sizeof(*store));
    if (!store) return NULL;
    pthread_mutex_init(&store->lock, NULL);
    return store;
}

void cbot_state_store_destroy(CbotStateStore *store) {
    if (!store) return;

    pthread_mutex_lock(&store->lock);
    for (size_t i = 0; i < STATE_BUCKET_COUNT; i++) {
        CbotStateNode *node = store->buckets[i];
        while (node) {
            CbotStateNode *next = node->next;
            free(node->user_id);
            free(node->key);
            free(node->value);
            free(node);
            node = next;
        }
        store->buckets[i] = NULL;
    }
    pthread_mutex_unlock(&store->lock);
    pthread_mutex_destroy(&store->lock);
    free(store);
}

char *cbot_state_store_get(CbotStateStore *store, const char *user_id, const char *key) {
    if (!store || !user_id || !key) return NULL;

    uint32_t bucket = state_hash(user_id, key);
    char *result = NULL;

    pthread_mutex_lock(&store->lock);
    CbotStateNode *node = store->buckets[bucket];
    while (node) {
        if (strcmp(node->user_id, user_id) == 0 && strcmp(node->key, key) == 0) {
            if (node->value) {
                result = strdup(node->value);
            }
            break;
        }
        node = node->next;
    }
    pthread_mutex_unlock(&store->lock);

    return result;
}

CbotStatus cbot_state_store_set(CbotStateStore *store, const char *user_id, const char *key, const char *value) {
    if (!store || !user_id || !key) return CBOT_ERR_INVALID_ARG;

    uint32_t bucket = state_hash(user_id, key);

    pthread_mutex_lock(&store->lock);
    CbotStateNode *node = store->buckets[bucket];
    CbotStateNode *prev = NULL;

    while (node) {
        if (strcmp(node->user_id, user_id) == 0 && strcmp(node->key, key) == 0) {
            if (!value) {
                /* Delete key */
                if (prev) {
                    prev->next = node->next;
                } else {
                    store->buckets[bucket] = node->next;
                }
                free(node->user_id);
                free(node->key);
                free(node->value);
                free(node);
            } else {
                /* Update value */
                char *new_val = strdup(value);
                if (!new_val) {
                    pthread_mutex_unlock(&store->lock);
                    return CBOT_ERR_NOMEM;
                }
                free(node->value);
                node->value = new_val;
            }
            pthread_mutex_unlock(&store->lock);
            return CBOT_OK;
        }
        prev = node;
        node = node->next;
    }

    if (!value) {
        /* Key not found and deleting, nothing to do */
        pthread_mutex_unlock(&store->lock);
        return CBOT_OK;
    }

    /* Insert new node */
    CbotStateNode *new_node = calloc(1, sizeof(*new_node));
    if (!new_node) {
        pthread_mutex_unlock(&store->lock);
        return CBOT_ERR_NOMEM;
    }

    new_node->user_id = strdup(user_id);
    new_node->key = strdup(key);
    new_node->value = strdup(value);
    if (!new_node->user_id || !new_node->key || !new_node->value) {
        free(new_node->user_id);
        free(new_node->key);
        free(new_node->value);
        free(new_node);
        pthread_mutex_unlock(&store->lock);
        return CBOT_ERR_NOMEM;
    }

    new_node->next = store->buckets[bucket];
    store->buckets[bucket] = new_node;

    pthread_mutex_unlock(&store->lock);
    return CBOT_OK;
}

const char *cbot_state_get(CbotCtx *ctx, const char *key) {
    if (!ctx || !ctx->bot || !key) return NULL;

    const char *user_id = ctx->event.user_id ? ctx->event.user_id : "default";
    char *val = cbot_state_store_get(ctx->bot->state_store, user_id, key);
    if (!val) return NULL;

    if (ctx->num_state_allocs == ctx->cap_state_allocs) {
        size_t new_cap = ctx->cap_state_allocs == 0 ? 4 : ctx->cap_state_allocs * 2;
        char **new_allocs = realloc(ctx->state_allocs, new_cap * sizeof(char *));
        if (!new_allocs) {
            free(val);
            return NULL;
        }
        ctx->state_allocs = new_allocs;
        ctx->cap_state_allocs = new_cap;
    }

    ctx->state_allocs[ctx->num_state_allocs++] = val;
    return val;
}

CbotStatus cbot_state_set(CbotCtx *ctx, const char *key, const char *value) {
    if (!ctx || !ctx->bot || !key) return CBOT_ERR_INVALID_ARG;

    const char *user_id = ctx->event.user_id ? ctx->event.user_id : "default";
    return cbot_state_store_set(ctx->bot->state_store, user_id, key, value);
}

void cbot_ctx_free_state_allocs(CbotCtx *ctx) {
    if (!ctx) return;
    for (size_t i = 0; i < ctx->num_state_allocs; i++) {
        free(ctx->state_allocs[i]);
    }
    free(ctx->state_allocs);
    ctx->state_allocs = NULL;
    ctx->num_state_allocs = 0;
    ctx->cap_state_allocs = 0;
}
