#include "cbot.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_u1_success = 0;
static int g_u2_success = 0;

static void handler_u1(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ev;
    cbot_state_set(ctx, "score", "100");
    const char *val = cbot_state_get(ctx, "score");
    assert(val != NULL);
    assert(strcmp(val, "100") == 0);

    /* Update */
    cbot_state_set(ctx, "score", "250");
    val = cbot_state_get(ctx, "score");
    assert(val != NULL);
    assert(strcmp(val, "250") == 0);

    g_u1_success = 1;
}

static void handler_u2(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ev;
    /* Verify user 2 doesn't see user 1's score */
    const char *val = cbot_state_get(ctx, "score");
    assert(val == NULL);

    cbot_state_set(ctx, "score", "999");
    val = cbot_state_get(ctx, "score");
    assert(val != NULL && strcmp(val, "999") == 0);

    /* Delete key */
    cbot_state_set(ctx, "score", NULL);
    val = cbot_state_get(ctx, "score");
    assert(val == NULL);

    g_u2_success = 1;
}

typedef struct {
    CbotEvent events[2];
    int count;
    int index;
} StateMockAdapter;

static CbotStatus state_mock_poll(CbotAdapter *a, CbotEvent *out, int timeout_ms) {
    (void)timeout_ms;
    StateMockAdapter *st = (StateMockAdapter *)a->impl;
    if (st->index >= st->count) {
        return CBOT_ERR_STOPPED;
    }
    *out = st->events[st->index++];
    return CBOT_OK;
}

static void state_mock_destroy(CbotAdapter *a) {
    if (a) {
        free(a->impl);
        free(a);
    }
}

static CbotAdapterVTable state_vt = {
    .name = "state_mock",
    .poll = state_mock_poll,
    .send_text = NULL,
    .release_event = NULL,
    .destroy = state_mock_destroy
};

static CbotAdapter *create_state_mock(StateMockAdapter *st) {
    CbotAdapter *a = calloc(1, sizeof(*a));
    a->vt = &state_vt;
    a->impl = st;
    return a;
}

int main(void) {
    printf("Running test_state...\n");

    CbotConfig cfg = {
        .worker_threads = 0,
        .queue_size = 16,
        .dedupe_window = 16,
        .poll_timeout_ms = 10,
        .user_data = NULL
    };

    Cbot *bot = cbot_new(&cfg);
    assert(bot != NULL);

    cbot_command(bot, "user1", handler_u1);
    cbot_command(bot, "user2", handler_u2);

    StateMockAdapter *st = calloc(1, sizeof(*st));
    st->count = 2;

    st->events[0].platform = "mock";
    st->events[0].chat_id = "c1";
    st->events[0].user_id = "user_1";
    st->events[0].text = "/user1";

    st->events[1].platform = "mock";
    st->events[1].chat_id = "c2";
    st->events[1].user_id = "user_2";
    st->events[1].text = "/user2";

    cbot_add_adapter(bot, create_state_mock(st));

    int rc = cbot_run(bot);
    assert(rc == 0);

    assert(g_u1_success == 1);
    assert(g_u2_success == 1);

    cbot_free(bot);

    printf("test_state passed!\n");
    return 0;
}
