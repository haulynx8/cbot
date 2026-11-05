#define _POSIX_C_SOURCE 200809L
#include "cbot.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_hello_received = 0;
static int g_text_received = 0;

static void on_cmd_hello(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ev;
    g_hello_received++;
    cbot_reply(ctx, "Hello back from test!");
}

static void on_any_text(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ctx;
    if (ev->text && strcmp(ev->text, "sample text") == 0) {
        g_text_received++;
    }
}

int main(void) {
    printf("Running test_console...\n");

    /* Create pipe to simulate console stdin */
    int fds[2];
    assert(pipe(fds) == 0);

    int saved_stdin = dup(STDIN_FILENO);
    assert(saved_stdin >= 0);

    /* Redirect stdin to reading end of pipe */
    assert(dup2(fds[0], STDIN_FILENO) >= 0);
    close(fds[0]);

    /* Write test commands into pipe */
    const char *inputs = "/hello\nsample text\n";
    ssize_t written = write(fds[1], inputs, strlen(inputs));
    assert(written == (ssize_t)strlen(inputs));
    close(fds[1]); /* Closes write end -> subsequent read will encounter EOF */

    CbotConfig cfg = {
        .worker_threads = 0,
        .queue_size = 16,
        .dedupe_window = 16,
        .poll_timeout_ms = 100,
        .user_data = NULL
    };

    Cbot *bot = cbot_new(&cfg);
    assert(bot != NULL);

    CbotAdapter *console_adapter = cbot_adapter_console();
    assert(console_adapter != NULL);
    assert(cbot_add_adapter(bot, console_adapter) == CBOT_OK);

    cbot_command(bot, "hello", on_cmd_hello);
    cbot_on_text(bot, on_any_text);

    int rc = cbot_run(bot);
    assert(rc == 0);

    assert(g_hello_received == 1);
    assert(g_text_received == 1);

    cbot_free(bot);

    /* Restore original stdin */
    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);

    printf("test_console passed!\n");
    return 0;
}
