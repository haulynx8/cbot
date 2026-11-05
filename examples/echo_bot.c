#include "cbot.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static Cbot *g_bot = NULL;

static void on_sigint(int sig) {
    (void)sig;
    if (g_bot) {
        cbot_stop(g_bot);
    }
}

static void cmd_start(CbotCtx *ctx, const CbotEvent *ev) {
    cbot_replyf(ctx, "Hello %s! Welcome to cbot.", ev->user_name ? ev->user_name : "there");
}

static void cmd_help(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ev;
    cbot_reply(ctx, "Available commands:\n"
                    "  /start          - Greet the bot\n"
                    "  /setname <name> - Store your custom name\n"
                    "  /whoami         - Query stored name\n"
                    "  /help           - Show this message");
}

static void cmd_setname(CbotCtx *ctx, const CbotEvent *ev) {
    if (!ev->args || ev->args[0] == '\0') {
        cbot_reply(ctx, "Usage: /setname <your name>");
        return;
    }
    cbot_state_set(ctx, "name", ev->args);
    cbot_replyf(ctx, "Got it, I will remember you as '%s'.", ev->args);
}

static void cmd_whoami(CbotCtx *ctx, const CbotEvent *ev) {
    const char *name = cbot_state_get(ctx, "name");
    if (name) {
        cbot_replyf(ctx, "You are '%s'.", name);
    } else {
        cbot_replyf(ctx, "I don't know your name yet. You are %s (chat: %s).",
                    ev->user_name ? ev->user_name : "unknown",
                    ev->chat_id ? ev->chat_id : "unknown");
    }
}

static void on_fallback_text(CbotCtx *ctx, const CbotEvent *ev) {
    cbot_replyf(ctx, "Echo: %s", ev->text ? ev->text : "");
}

int main(void) {
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    printf("Starting cbot echo example on console adapter...\n");
    printf("Type '/help' or any message (Ctrl+D to exit):\n");

    Cbot *bot = cbot_new(NULL);
    if (!bot) {
        fprintf(stderr, "Failed to create bot\n");
        return 1;
    }
    g_bot = bot;

    cbot_add_adapter(bot, cbot_adapter_console());

    /* Middlewares */
    cbot_use(bot, cbot_mw_log);
    cbot_use(bot, cbot_mw_ratelimit);

    /* Commands */
    cbot_command(bot, "start", cmd_start);
    cbot_command(bot, "help", cmd_help);
    cbot_command(bot, "setname", cmd_setname);
    cbot_command(bot, "whoami", cmd_whoami);

    /* Text fallback */
    cbot_on_text(bot, on_fallback_text);

    int rc = cbot_run(bot);

    cbot_free(bot);
    g_bot = NULL;

    printf("\nBot stopped cleanly.\n");
    return rc;
}
