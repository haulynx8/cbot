#include "cbot.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_dedupe_runs = 0;
static _Atomic int g_threaded_runs = 0;

static void handler_dedupe(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    (void)ev;
    g_dedupe_runs++;
}

static void handler_threaded(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    (void)ev;
    g_threaded_runs++;
}

typedef struct {
    CbotEvent events[100];
    int count;
    int index;
} CoreMockAdapter;

static CbotStatus core_mock_poll(CbotAdapter *a, CbotEvent *out, int timeout_ms) {
    (void)timeout_ms;
    CoreMockAdapter *st = (CoreMockAdapter *)a->impl;
    if (st->index >= st->count) {
        return CBOT_ERR_STOPPED;
    }
    *out = st->events[st->index++];
    return CBOT_OK;
}

static void core_mock_destroy(CbotAdapter *a) {
    if (a) {
        free(a->impl);
        free(a);
    }
}

static CbotAdapterVTable core_vt = {
    .name = "core_mock",
    .poll = core_mock_poll,
    .send_text = NULL,
    .release_event = NULL,
    .destroy = core_mock_destroy
};

static CbotAdapter *create_core_mock(CoreMockAdapter *st) {
    CbotAdapter *a = calloc(1, sizeof(*a));
    a->vt = &core_vt;
    a->impl = st;
    return a;
}

int main(void) {
    printf("Running test_core...\n");

    /* 1. Test cbot_strerror */
    assert(strcmp(cbot_strerror(CBOT_OK), "Success") == 0);
    assert(strcmp(cbot_strerror(CBOT_ERR_NOMEM), "Out of memory") == 0);
    assert(strcmp(cbot_strerror(CBOT_ERR_STOPPED), "Stopped") == 0);

    /* 2. Test user_data */
    int my_data = 12345;
    CbotConfig cfg = {
        .worker_threads = 0,
        .queue_size = 16,
        .dedupe_window = 16,
        .poll_timeout_ms = 10,
        .user_data = &my_data
    };
    Cbot *bot = cbot_new(&cfg);
    assert(bot != NULL);

    /* 3. Test deduplication */
    cbot_on_text(bot, handler_dedupe);

    CoreMockAdapter *st = calloc(1, sizeof(*st));
    st->count = 3;

    st->events[0].platform = "core_mock";
    st->events[0].chat_id = "c1";
    st->events[0].user_id = "u1";
    st->events[0].text = "first";
    st->events[0].event_id = 42;

    st->events[1].platform = "core_mock";
    st->events[1].chat_id = "c1";
    st->events[1].user_id = "u1";
    st->events[1].text = "duplicate";
    st->events[1].event_id = 42; /* Same event_id -> must be dropped */

    st->events[2].platform = "core_mock";
    st->events[2].chat_id = "c1";
    st->events[2].user_id = "u1";
    st->events[2].text = "third";
    st->events[2].event_id = 43; /* Different event_id -> must be processed */

    cbot_add_adapter(bot, create_core_mock(st));

    int rc = cbot_run(bot);
    assert(rc == 0);
    assert(g_dedupe_runs == 2);

    cbot_free(bot);

    /* 4. Test multithreaded execution */
    CbotConfig threaded_cfg = {
        .worker_threads = 4,
        .queue_size = 64,
        .dedupe_window = 128,
        .poll_timeout_ms = 10,
        .user_data = NULL
    };
    Cbot *threaded_bot = cbot_new(&threaded_cfg);
    assert(threaded_bot != NULL);

    cbot_on_text(threaded_bot, handler_threaded);

    CoreMockAdapter *st_threaded = calloc(1, sizeof(*st_threaded));
    st_threaded->count = 50;
    for (int i = 0; i < 50; i++) {
        st_threaded->events[i].platform = "core_mock";
        st_threaded->events[i].chat_id = "c1";
        st_threaded->events[i].user_id = "u1";
        st_threaded->events[i].text = "msg";
        st_threaded->events[i].event_id = (uint64_t)(100 + i);
    }

    cbot_add_adapter(threaded_bot, create_core_mock(st_threaded));

    rc = cbot_run(threaded_bot);
    assert(rc == 0);
    assert(g_threaded_runs == 50);

    cbot_free(threaded_bot);

    printf("test_core passed!\n");
    return 0;
}
