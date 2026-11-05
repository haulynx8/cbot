#include "cbot.h"

#include <stddef.h>

CbotAdapter *cbot_adapter_irc(const char *host, int port,
                              const char *nick, const char *channel) {
    (void)host;
    (void)port;
    (void)nick;
    (void)channel;
    /* IRC adapter (raw TCP socket) scheduled for slice 3 */
    return NULL;
}
