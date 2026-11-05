#define _POSIX_C_SOURCE 200809L
#include "cbot.h"
#include "cbot_telegram.h"
#include "cweb.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Cbot *g_bot = NULL;
static CwebApp *g_app = NULL;

static void handle_sig(int sig) {
    (void)sig;
    if (g_bot) cbot_stop(g_bot);
    if (g_app) cweb_stop(g_app);
}

static void on_start(CbotCtx *ctx, const CbotEvent *ev) {
    cbot_replyf(ctx, "Hello %s! I'm running on cbot via Telegram.",
                ev->user_name ? ev->user_name : "friend");
}

static void on_help(CbotCtx *ctx, const CbotEvent *ev) {
    (void)ev;
    cbot_reply(ctx, "Commands:\n"
                    "/start - Greet bot\n"
                    "/setname <name> - Store name in state\n"
                    "/whoami - Retrieve stored name\n"
                    "/help - This message");
}

static void on_setname(CbotCtx *ctx, const CbotEvent *ev) {
    if (!ev->args || ev->args[0] == '\0') {
        cbot_reply(ctx, "Usage: /setname <your name>");
        return;
    }
    cbot_state_set(ctx, "name", ev->args);
    cbot_replyf(ctx, "Saved your name as '%s'.", ev->args);
}

static void on_whoami(CbotCtx *ctx, const CbotEvent *ev) {
    const char *name = cbot_state_get(ctx, "name");
    if (name) {
        cbot_replyf(ctx, "You are '%s'.", name);
    } else {
        cbot_replyf(ctx, "I don't know you yet (user ID: %s). Use /setname.",
                    ev->user_id ? ev->user_id : "unknown");
    }
}

static void on_text_echo(CbotCtx *ctx, const CbotEvent *ev) {
    cbot_replyf(ctx, "Echo: %s", ev->text ? ev->text : "");
}

int main(int argc, char **argv) {
    const char *token = getenv("TELEGRAM_BOT_TOKEN");
    if (argc > 1 && argv[1][0] != '-') {
        token = argv[1];
    }

    if (!token || token[0] == '\0') {
        fprintf(stderr, "Usage: %s [bot_token] [--webhook port path]\n", argv[0]);
        fprintf(stderr, "Or set TELEGRAM_BOT_TOKEN environment variable.\n");
        return 1;
    }

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    int use_webhook = 0;
    int webhook_port = 8443;
    const char *webhook_path = "/telegram";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--webhook") == 0) {
            use_webhook = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                webhook_port = atoi(argv[++i]);
            }
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                webhook_path = argv[++i];
            }
        }
    }

    Cbot *bot = cbot_new(NULL);
    if (!bot) {
        fprintf(stderr, "Failed to initialize cbot\n");
        return 1;
    }
    g_bot = bot;

    /* Middleware */
    cbot_use(bot, cbot_mw_log);
    cbot_use(bot, cbot_mw_ratelimit);

    /* Handlers */
    cbot_command(bot, "start", on_start);
    cbot_command(bot, "help", on_help);
    cbot_command(bot, "setname", on_setname);
    cbot_command(bot, "whoami", on_whoami);
    cbot_on_text(bot, on_text_echo);

    CbotAdapter *tg = NULL;
    CwebApp app = {0};

    if (use_webhook) {
        printf("Starting Telegram bot in webhook mode on port %d, path %s...\n",
               webhook_port, webhook_path);
        tg = cbot_adapter_telegram_webhook(token);
        cbot_add_adapter(bot, tg);
        cbot_telegram_attach_cweb(tg, &app, webhook_path);

        /* Run cweb server in background thread */
        g_app = &app;
        /* (In production, cweb_run runs behind nginx/Caddy reverse proxy terminating TLS) */
    } else {
        printf("Starting Telegram bot in long-polling mode...\n");
        tg = cbot_adapter_telegram(token);
        cbot_add_adapter(bot, tg);
    }

    int rc = cbot_run(bot);

    cbot_free(bot);
    g_bot = NULL;
    printf("Telegram bot stopped cleanly.\n");
    return rc;
}
