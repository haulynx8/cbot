#include "cbot_internal.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

void cbot_command(Cbot *bot, const char *name, CbotHandler h) {
    if (!bot || !name || !h) return;

    if (name[0] == '/') {
        name++;
    }
    if (*name == '\0') return;

    pthread_mutex_lock(&bot->lock);
    if (bot->num_commands == bot->cap_commands) {
        size_t new_cap = bot->cap_commands == 0 ? 8 : bot->cap_commands * 2;
        CbotRouteCommand *new_cmds = realloc(bot->commands, new_cap * sizeof(CbotRouteCommand));
        if (!new_cmds) {
            pthread_mutex_unlock(&bot->lock);
            return;
        }
        bot->commands = new_cmds;
        bot->cap_commands = new_cap;
    }

    bot->commands[bot->num_commands].name = strdup(name);
    bot->commands[bot->num_commands].handler = h;
    if (bot->commands[bot->num_commands].name) {
        bot->num_commands++;
    }
    pthread_mutex_unlock(&bot->lock);
}

void cbot_on_text(Cbot *bot, CbotHandler h) {
    if (!bot) return;

    pthread_mutex_lock(&bot->lock);
    bot->on_text = h;
    pthread_mutex_unlock(&bot->lock);
}

void cbot_on_callback(Cbot *bot, const char *prefix, CbotHandler h) {
    if (!bot || !prefix || !h) return;

    pthread_mutex_lock(&bot->lock);
    if (bot->num_callbacks == bot->cap_callbacks) {
        size_t new_cap = bot->cap_callbacks == 0 ? 8 : bot->cap_callbacks * 2;
        CbotRouteCallback *new_cbs = realloc(bot->callbacks, new_cap * sizeof(CbotRouteCallback));
        if (!new_cbs) {
            pthread_mutex_unlock(&bot->lock);
            return;
        }
        bot->callbacks = new_cbs;
        bot->cap_callbacks = new_cap;
    }

    bot->callbacks[bot->num_callbacks].prefix = strdup(prefix);
    bot->callbacks[bot->num_callbacks].handler = h;
    if (bot->callbacks[bot->num_callbacks].prefix) {
        bot->num_callbacks++;
    }
    pthread_mutex_unlock(&bot->lock);
}

void cbot_router_dispatch(CbotCtx *ctx, const CbotEvent *ev) {
    if (!ctx || !ctx->bot || !ev) return;
    Cbot *bot = ctx->bot;

    /* Handle commands */
    if (ev->type == CBOT_EV_COMMAND || (ev->command && ev->command[0] != '\0')) {
        CbotHandler handler = NULL;

        pthread_mutex_lock(&bot->lock);
        for (size_t i = 0; i < bot->num_commands; i++) {
            if (ev->command && strcasecmp(ev->command, bot->commands[i].name) == 0) {
                handler = bot->commands[i].handler;
                break;
            }
        }
        CbotHandler fallback = bot->on_text;
        pthread_mutex_unlock(&bot->lock);

        if (handler) {
            handler(ctx, ev);
        } else if (fallback) {
            fallback(ctx, ev);
        }
        return;
    }

    /* Handle callbacks */
    if (ev->type == CBOT_EV_CALLBACK || (ev->callback_data && ev->callback_data[0] != '\0')) {
        CbotHandler handler = NULL;

        pthread_mutex_lock(&bot->lock);
        for (size_t i = 0; i < bot->num_callbacks; i++) {
            const char *prefix = bot->callbacks[i].prefix;
            size_t len = strlen(prefix);
            if (ev->callback_data && strncmp(ev->callback_data, prefix, len) == 0) {
                handler = bot->callbacks[i].handler;
                break;
            }
        }
        pthread_mutex_unlock(&bot->lock);

        if (handler) {
            handler(ctx, ev);
        }
        return;
    }

    /* Handle text messages */
    if (ev->type == CBOT_EV_MESSAGE || ev->text != NULL) {
        pthread_mutex_lock(&bot->lock);
        CbotHandler fallback = bot->on_text;
        pthread_mutex_unlock(&bot->lock);

        if (fallback) {
            fallback(ctx, ev);
        }
        return;
    }
}
