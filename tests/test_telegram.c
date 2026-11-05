#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "cbot.h"
#include "cbot_telegram.h"
#include "cbot_internal.h"
#include "cweb.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <assert.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Track handler executions */
static int g_cmd_start_count = 0;
static char g_last_user_name[64] = {0};
static char g_last_chat_id[64] = {0};
static int g_cb_count = 0;
static char g_last_cb_data[64] = {0};
static int g_text_count = 0;
static char g_last_text[128] = {0};

static void on_start(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    g_cmd_start_count++;
    if (ev->user_name) strncpy(g_last_user_name, ev->user_name, sizeof(g_last_user_name) - 1);
    if (ev->chat_id) strncpy(g_last_chat_id, ev->chat_id, sizeof(g_last_chat_id) - 1);
}

static void on_btn(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    g_cb_count++;
    if (ev->callback_data) strncpy(g_last_cb_data, ev->callback_data, sizeof(g_last_cb_data) - 1);
}

static void on_text(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    g_text_count++;
    if (ev->text) strncpy(g_last_text, ev->text, sizeof(g_last_text) - 1);
}

/* 1. Test Telegram JSON parser & feed updates */
static void test_telegram_parser_and_feed(void) {
    printf("Testing Telegram update parser and feed...\n");

    CbotConfig cfg = {
        .worker_threads = 0,
        .queue_size = 16,
        .dedupe_window = 16,
        .poll_timeout_ms = 10,
        .user_data = NULL
    };
    Cbot *bot = cbot_new(&cfg);
    assert(bot != NULL);

    CbotAdapter *tg = cbot_adapter_telegram_webhook("dummy_token");
    assert(tg != NULL);
    assert(cbot_add_adapter(bot, tg) == CBOT_OK);

    cbot_command(bot, "start", on_start);
    cbot_on_callback(bot, "btn:", on_btn);
    cbot_on_text(bot, on_text);

    /* Feed 1: Command /start with 64-bit negative group ID */
    const char *payload_cmd =
        "{\"update_id\":1001,\"message\":{\"message_id\":5,\"from\":{\"id\":444,\"first_name\":\"Alice\",\"username\":\"alice_bot\"},\"chat\":{\"id\":-1001928374650},\"text\":\"/start now\"}}";
    assert(cbot_telegram_feed_update(tg, payload_cmd) == CBOT_OK);

    /* Feed 2: Button callback */
    const char *payload_cb =
        "{\"update_id\":1002,\"callback_query\":{\"id\":\"cb1\",\"from\":{\"id\":444,\"username\":\"alice_bot\"},\"message\":{\"message_id\":5,\"chat\":{\"id\":-1001928374650}},\"data\":\"btn:confirm\"}}";
    assert(cbot_telegram_feed_update(tg, payload_cb) == CBOT_OK);

    /* Feed 3: Text with unicode emoji surrogate pair */
    const char *payload_text =
        "{\"update_id\":1003,\"message\":{\"message_id\":6,\"from\":{\"id\":444},\"chat\":{\"id\":-1001928374650},\"text\":\"Hello \\ud83d\\ude00\"}}";
    assert(cbot_telegram_feed_update(tg, payload_text) == CBOT_OK);

    /* Run 3 events */
    for (int i = 0; i < 3; i++) {
        CbotEvent ev;
        CbotStatus s = tg->vt->poll(tg, &ev, 10);
        assert(s == CBOT_OK);
        cbot_dispatch(bot, tg, &ev);
        if (tg->vt->release_event) tg->vt->release_event(tg, &ev);
    }

    assert(g_cmd_start_count == 1);
    assert(strcmp(g_last_user_name, "alice_bot") == 0);
    assert(strcmp(g_last_chat_id, "-1001928374650") == 0);
    assert(g_cb_count == 1);
    assert(strcmp(g_last_cb_data, "btn:confirm") == 0);
    assert(g_text_count == 1);
    /* Verify unicode emoji was decoded to UTF-8 */
    assert(strstr(g_last_text, "\xf0\x9f\x98\x80") != NULL);

    cbot_free(bot);
    printf("Telegram parser and feed passed!\n");
}

/* 2. Test cweb Webhook Integration */
typedef struct {
    CwebApp *app;
    int port;
} ServerThreadArg;

static void *cweb_server_thread(void *arg) {
    ServerThreadArg *sa = (ServerThreadArg *)arg;
    cweb_run(sa->app, sa->port);
    return NULL;
}

static void stop_cweb_server(CwebApp *app, int port) {
    cweb_stop(app);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock >= 0) {
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons((unsigned short)port);
        sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connect(sock, (struct sockaddr *)&sin, sizeof(sin));
        close(sock);
    }
}

static void test_cweb_webhook_integration(void) {
    printf("Testing cweb Telegram webhook integration...\n");

    int port = 9123;
    CwebApp app = {0};
    app.thread_count = 2;

    CbotConfig cfg = {
        .worker_threads = 0,
        .queue_size = 16,
        .dedupe_window = 16,
        .poll_timeout_ms = 10,
        .user_data = NULL
    };
    Cbot *bot = cbot_new(&cfg);
    assert(bot != NULL);

    CbotAdapter *tg = cbot_adapter_telegram_webhook("webhook_test_token");
    assert(tg != NULL);
    assert(cbot_add_adapter(bot, tg) == CBOT_OK);

    /* Attach to cweb route */
    int attach_rc = cbot_telegram_attach_cweb(tg, &app, "/telegram/webhook");
    assert(attach_rc == 0);

    /* Start cweb server thread */
    ServerThreadArg sa = { .app = &app, .port = port };
    pthread_t th;
    assert(pthread_create(&th, NULL, cweb_server_thread, &sa) == 0);
    usleep(100000); /* 100ms for server to bind */

    /* Reset counters */
    g_cmd_start_count = 0;
    cbot_command(bot, "start", on_start);

    /* Send HTTP POST using curl to cweb webhook route */
    CURL *curl = curl_easy_init();
    assert(curl != NULL);

    const char *webhook_url = "http://127.0.0.1:9123/telegram/webhook";
    const char *update_body =
        "{\"update_id\":2001,\"message\":{\"message_id\":10,\"from\":{\"id\":555,\"username\":\"cweb_user\"},\"chat\":{\"id\":999},\"text\":\"/start\"}}";

    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, webhook_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, update_body);

    CURLcode res = curl_easy_perform(curl);
    assert(res == CURLE_OK);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    assert(http_code == 200);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    /* Poll adapter to process event */
    CbotEvent ev;
    CbotStatus s = tg->vt->poll(tg, &ev, 500);
    assert(s == CBOT_OK);
    cbot_dispatch(bot, tg, &ev);
    if (tg->vt->release_event) tg->vt->release_event(tg, &ev);

    assert(g_cmd_start_count == 1);
    assert(strcmp(g_last_user_name, "cweb_user") == 0);

    /* Stop server */
    stop_cweb_server(&app, port);
    pthread_join(th, NULL);

    cbot_free(bot);
    printf("cweb Telegram webhook integration passed!\n");
}

/* 3. Test Outbound Client (sendMessage) with Rate Limiting (429) & Long Polling */
static int g_mock_send_calls = 0;
static int mock_tg_send_message_handler(CwebRequest *req, CwebResponse *res) {
    (void)req;
    g_mock_send_calls++;
    if (g_mock_send_calls == 1) {
        /* Simulate Telegram 429 Too Many Requests on first attempt */
        cweb_json(res, 429, "{\"ok\":false,\"error_code\":429,\"parameters\":{\"retry_after\":1}}");
        return 429;
    }
    /* Succeed on retry */
    cweb_json(res, 200, "{\"ok\":true,\"result\":{\"message_id\":888}}");
    return 200;
}

static int mock_tg_get_updates_handler(CwebRequest *req, CwebResponse *res) {
    (void)req;
    cweb_json(res, 200, "{\"ok\":true,\"result\":[{\"update_id\":7777,\"message\":{\"from\":{\"id\":10,\"username\":\"lp_user\"},\"chat\":{\"id\":10},\"text\":\"Hello LongPoll\"}}]}");
    return 200;
}

static void test_telegram_outbound_and_longpoll(void) {
    printf("Testing Telegram outbound client (with 429 backoff) and long polling...\n");

    int port = 9124;
    CwebApp mock_tg_app = {0};
    mock_tg_app.thread_count = 2;

    cweb_add_route(&mock_tg_app, "POST", "/botmock_token/sendMessage", mock_tg_send_message_handler);
    cweb_add_route(&mock_tg_app, "GET", "/botmock_token/getUpdates", mock_tg_get_updates_handler);

    ServerThreadArg sa = { .app = &mock_tg_app, .port = port };
    pthread_t th;
    assert(pthread_create(&th, NULL, cweb_server_thread, &sa) == 0);
    usleep(100000);

    CbotConfig cfg = {
        .worker_threads = 0,
        .queue_size = 16,
        .dedupe_window = 16,
        .poll_timeout_ms = 10,
        .user_data = NULL
    };
    Cbot *bot = cbot_new(&cfg);
    assert(bot != NULL);

    CbotAdapter *tg = cbot_adapter_telegram("mock_token");
    assert(tg != NULL);
    cbot_telegram_set_base_url(tg, "http://127.0.0.1:9124");
    assert(cbot_add_adapter(bot, tg) == CBOT_OK);

    /* Test 1: Outbound send_text triggers 429 -> auto-retry -> success */
    g_mock_send_calls = 0;
    CbotStatus send_status = tg->vt->send_text(tg, "12345", "Test Message With Quotes \"Hello\"");
    assert(send_status == CBOT_OK);
    assert(g_mock_send_calls == 2); /* 1st failed with 429, 2nd succeeded */

    /* Test 2: Inbound long polling fetches update */
    g_text_count = 0;
    cbot_on_text(bot, on_text);

    CbotEvent ev;
    CbotStatus poll_status = tg->vt->poll(tg, &ev, 100);
    assert(poll_status == CBOT_OK);
    assert(ev.event_id == 7777);

    cbot_dispatch(bot, tg, &ev);
    if (tg->vt->release_event) tg->vt->release_event(tg, &ev);

    assert(g_text_count == 1);
    assert(strcmp(g_last_text, "Hello LongPoll") == 0);

    /* Stop mock server */
    stop_cweb_server(&mock_tg_app, port);
    pthread_join(th, NULL);

    cbot_free(bot);
    printf("Telegram outbound and long-poll passed!\n");
}

int main(void) {
    printf("=== Running Telegram Adapter Test Suite ===\n");
    test_telegram_parser_and_feed();
    test_cweb_webhook_integration();
    test_telegram_outbound_and_longpoll();
    printf("=== All Telegram tests passed! ===\n");
    return 0;
}
