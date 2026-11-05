#ifndef CBOT_TELEGRAM_H
#define CBOT_TELEGRAM_H

#include "cbot.h"

#ifdef __cplusplus
extern "C" {
#endif

struct CwebApp;

/*
 * Attach a Telegram webhook route to a cweb HTTP server instance.
 * When cweb receives an incoming POST at `path`, it feeds the payload
 * to `adapter` via cbot_telegram_feed_update and responds with 200 OK.
 *
 * Returns 0 on success, or non-zero on failure.
 */
int cbot_telegram_attach_cweb(CbotAdapter *adapter, struct CwebApp *app, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CBOT_TELEGRAM_H */
