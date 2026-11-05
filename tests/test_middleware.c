#include "cbot.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_order1_calls = 0;
static int g_order2_calls = 0;
static int g_last_order = 0;
static int g_handler_ran = 0;

static int mw_order1(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    (void)ev;
    g_order1_calls++;
    g_last_order = 1;
    return 1;
}

static int mw_order2(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    (void)ev;
    g_order2_calls++;
    assert(g_last_order == 1);
    g_last_order = 2;
    return 1;
}

static int mw_blocker(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    if (ev->text && strcmp(ev->text, "block_me") == 0) {
        return 0; /* Stop chain */
    }
    return 1;
}

static void on_message(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    (void)ev;
    g_handler_ran++;
}

typedef struct {
    CbotEvent events[3];
    int count;
    int index;
} MwMockAdapter;

static CbotStatus mw_mock_poll(CbotAdapter *a, CbotEvent *out, int timeout_ms) {
    (void)timeout_ms;
    MwMockAdapter *st = (MwMockAdapter *)a->impl;
    if (st->index >= st->count) {
        return CBOT_ERR_STOPPED;
    }
    *out = st->events[st->index++];
    return CBOT_OK;
}

static void mw_mock_destroy(CbotAdapter *a) {
    if (a) {
        free(a->impl);
        free(a);
    }
}

static CbotAdapterVTable mw_vt = {
    .name = "mw_mock",
    .poll = mw_mock_poll,
    .send_text = NULL,
    .release_event = NULL,
    .destroy = mw_mock_destroy
};

static CbotAdapter *create_mw_mock(MwMockAdapter *st) {
    CbotAdapter *a = calloc(1, sizeof(*a));
    a->vt = &mw_vt;
    a->impl = st;
    return a;
}

int main(void) {
    printf("Running test_middleware...\n");

    CbotConfig cfg = {
        .worker_threads = 0,
        .queue_size = 16,
        .dedupe_window = 16,
        .poll_timeout_ms = 10,
        .user_data = NULL
    };

    Cbot *bot = cbot_new(&cfg);
    assert(bot != NULL);

    /* Register middlewares */
    cbot_use(bot, cbot_mw_log);
    cbot_use(bot, mw_order1);
    cbot_use(bot, mw_order2);
    cbot_use(bot, mw_blocker);

    cbot_on_text(bot, on_message);

    MwMockAdapter *st = calloc(1, sizeof(*st));
    st->count = 2;

    st->events[0].platform = "mock";
    st->events[0].chat_id = "c1";
    st->events[0].user_id = "u1";
    st->events[0].text = "allowed";

    st->events[1].platform = "mock";
    st->events[1].chat_id = "c1";
    st->events[1].user_id = "u1";
    st->events[1].text = "block_me";

    cbot_add_adapter(bot, create_mw_mock(st));

    int rc = cbot_run(bot);
    assert(rc == 0);

    /* Both events passed order1 and order2 in that sequence */
    assert(g_order1_calls == 2);
    assert(g_order2_calls == 2);
    /* Only event 0 reached on_message because event 1 was blocked by mw_blocker */
    assert(g_handler_ran == 1);

    cbot_free(bot);

    printf("test_middleware passed!\n");
    return 0;
}
