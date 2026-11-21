#ifndef _IC4_CLIENT_H_
#define _IC4_CLIENT_H_

    #include <stdbool.h>
    #include "host/ble_hs.h"

    #ifdef __cplusplus
        extern "C" {
    #endif

    // C1: client of the Schwinn IC4 ("IC Bike", FTMS 0x1826), subscribed to Indoor Bike Data (0x2AD2).
    // Never writes the Fitness Machine Control Point (0x2AD9), never reads the IC4 heart rate service (0x180D).
    void Ic4_init(void);    // Before the NimBLE host task starts
    void Ic4_start(void);   // NimBLE task (on_sync): first scan

    // First event of a new client (NimBLE task). The IC4 console (same chip and address as the bike) is disconnected
    // at once: the IC4 sends no FTMS data over a link opened by its console, and only one link per peer is possible.
    void Ic4_onClientConnected(uint16_t conn, const ble_addr_t *peer);

    #ifdef __cplusplus
    }
    #endif

#endif
