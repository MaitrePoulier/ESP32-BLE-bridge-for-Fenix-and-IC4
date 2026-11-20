// Shared state of the IC4 Bridge
#include "Bridge.h"
#include <string.h>
#include "freertos/FreeRTOS.h"

bridge_status_t g_bridge = {0};
volatile bool g_bridge_raw_print = false;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void Bridge_lock(void)   { taskENTER_CRITICAL(&s_mux); }
void Bridge_unlock(void) { taskEXIT_CRITICAL(&s_mux); }

void Bridge_getStatus(bridge_status_t *out)
{
    Bridge_lock();
    memcpy(out, &g_bridge, sizeof(*out));
    Bridge_unlock();
}

const char *Bridge_linkStateStr(link_state_t state)
{
    switch (state) {
        case LINK_OFF:         return "Off";
        case LINK_BACKOFF:     return "Waiting";
        case LINK_SCANNING:    return "Scanning";
        case LINK_CONNECTING:  return "Connecting";
        case LINK_DISCOVERING: return "Discovering";
        case LINK_READY:       return "Connected";
        default:               return "?";
    }
}
