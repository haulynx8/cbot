#include "cbot.h"

#include <stddef.h>

CbotAdapter *cbot_adapter_telegram(const char *token) {
    (void)token;
    /* Telegram adapter (cweb webhook + libcurl long polling) scheduled for slice 2 */
    return NULL;
}
