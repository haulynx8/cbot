#include "cbot.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_start_called = 0;
static char g_last_arg[128] = {0};
static int g_cb_called = 0;
static int g_fallback_called = 0;
static char g_fallback_text[128] = {0};

static void on_start(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    (void)ev;
    g_start_called++;
}

static void on_echo(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    if (ev->args) {
        strncpy(g_last_arg, ev->args, sizeof(g_last_arg) - 1);
    }
}

static void on_btn_click(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    (void)ev;
    g_cb_called++;
}

static void on_text(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    g_fallback_called++;
    if (ev->text) {
        strncpy(g_fallback_text, ev->text, sizeof(g_fallback_text) - 1);
    }
}

typedef struct {
    CbotEvent events[10];
    int count;
    int index;
} MockAdapterState;

static CbotStatus mock_poll(CbotAdapter *a, CbotEvent *out, int timeout_ms) {
    (void)timeout_ms;
    MockAdapterState *st = (MockAdapterState *)a->impl;
    if (st->index >= st->count) {
        return CBOT_ERR_STOPPED;
    }
    *out = st->events[st->index++];
    return CBOT_OK;
}

static void mock_destroy(CbotAdapter *a) {
    if (a) {
        free(a->impl);
        free(a);
    }
}

static CbotAdapterVTable mock_vt = {
    .name = "mock",
    .poll = mock_poll,
    .send_text = NULL,
    .release_event = NULL,
    .destroy = mock_destroy
};

static CbotAdapter *create_mock_adapter(MockAdapterState *st) {
    CbotAdapter *a = calloc(1, sizeof(*a));
    a->vt = &mock_vt;
    a->impl = st;
    return a;
}

int main(void) {
    printf("Running test_router...\n");

    CbotConfig cfg = {
        .worker_threads = 0, /* Synchronous for deterministic test */
        .queue_size = 16,
        .dedupe_window = 16,
        .poll_timeout_ms = 10,
        .user_data = NULL
    };

    Cbot *bot = cbot_new(&cfg);
    assert(bot != NULL);

    cbot_command(bot, "start", on_start);
    cbot_command(bot, "echo", on_echo);
    cbot_on_callback(bot, "btn:", on_btn_click);
    cbot_on_text(bot, on_text);

    MockAdapterState *st = calloc(1, sizeof(*st));
    st->count = 5;

    /* 1. Command /start */
    st->events[0].platform = "mock";
    st->events[0].chat_id = "1";
    st->events[0].user_id = "u1";
    st->events[0].text = "/start";

    /* 2. Command /echo hello world */
    st->events[1].platform = "mock";
    st->events[1].chat_id = "1";
    st->events[1].user_id = "u1";
    st->events[1].text = "/echo hello world";

    /* 3. Case insensitive command /START */
    st->events[2].platform = "mock";
    st->events[2].chat_id = "1";
    st->events[2].user_id = "u1";
    st->events[2].text = "/START";

    /* 4. Callback */
    st->events[3].type = CBOT_EV_CALLBACK;
    st->events[3].platform = "mock";
    st->events[3].chat_id = "1";
    st->events[3].user_id = "u1";
    st->events[3].callback_data = "btn:confirm";

    /* 5. Plain text fallback */
    st->events[4].platform = "mock";
    st->events[4].chat_id = "1";
    st->events[4].user_id = "u1";
    st->events[4].text = "just a text message";

    cbot_add_adapter(bot, create_mock_adapter(st));

    int rc = cbot_run(bot);
    assert(rc == 0);

    assert(g_start_called == 2);
    assert(strcmp(g_last_arg, "hello world") == 0);
    assert(g_cb_called == 1);
    assert(g_fallback_called == 1);
    assert(strcmp(g_fallback_text, "just a text message") == 0);

    cbot_free(bot);

    printf("test_router passed!\n");
    return 0;
}
