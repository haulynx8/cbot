#define _POSIX_C_SOURCE 200809L
#include "cbot.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *line_buf;
    size_t line_cap;
    uint64_t next_event_id;
} ConsoleImpl;

static CbotStatus console_poll(CbotAdapter *a, CbotEvent *out, int timeout_ms) {
    if (!a || !a->impl || !out) return CBOT_ERR_INVALID_ARG;
    ConsoleImpl *impl = (ConsoleImpl *)a->impl;

    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int r = poll(&pfd, 1, timeout_ms);
    if (r < 0) {
        if (errno == EINTR) return CBOT_OK;
        return CBOT_ERR_NETWORK;
    }
    if (r == 0) {
        return CBOT_OK; /* Timeout */
    }

    ssize_t nread = getline(&impl->line_buf, &impl->line_cap, stdin);
    if (nread < 0) {
        if (feof(stdin)) {
            return CBOT_ERR_STOPPED;
        }
        return CBOT_ERR_PROTOCOL;
    }

    /* Strip trailing newline and carriage return */
    while (nread > 0 && (impl->line_buf[nread - 1] == '\n' || impl->line_buf[nread - 1] == '\r')) {
        impl->line_buf[--nread] = '\0';
    }

    if (nread == 0) {
        return CBOT_OK;
    }

    memset(out, 0, sizeof(*out));
    out->event_id = ++impl->next_event_id;
    out->platform = "console";
    out->user_id = "console_user";
    out->user_name = "Console User";
    out->chat_id = "console";
    out->text = impl->line_buf;

    return CBOT_OK;
}

static CbotStatus console_send_text(CbotAdapter *a, const char *chat_id, const char *text) {
    (void)a;
    (void)chat_id;
    printf("[bot] %s\n", text ? text : "");
    fflush(stdout);
    return CBOT_OK;
}

static void console_release_event(CbotAdapter *a, CbotEvent *ev) {
    (void)a;
    (void)ev;
}

static void console_destroy(CbotAdapter *a) {
    if (!a) return;
    if (a->impl) {
        ConsoleImpl *impl = (ConsoleImpl *)a->impl;
        free(impl->line_buf);
        free(impl);
    }
    free(a);
}

static const CbotAdapterVTable console_vtable = {
    .name = "console",
    .poll = console_poll,
    .send_text = console_send_text,
    .release_event = console_release_event,
    .destroy = console_destroy
};

CbotAdapter *cbot_adapter_console(void) {
    CbotAdapter *a = calloc(1, sizeof(*a));
    if (!a) return NULL;

    ConsoleImpl *impl = calloc(1, sizeof(*impl));
    if (!impl) {
        free(a);
        return NULL;
    }

    a->vt = &console_vtable;
    a->impl = impl;
    return a;
}
