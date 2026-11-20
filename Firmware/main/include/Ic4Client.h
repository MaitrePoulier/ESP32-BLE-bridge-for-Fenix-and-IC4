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

    // Only one link between the bridge and the IC4 (same chip and address for the bike and its console). When the
    // IC4 console connects to the bridge (heart rate), the FTMS data are read over that link. NimBLE task.
    void Ic4_onClientConnected(uint16_t conn, const ble_addr_t *peer);   // First event of a new client
    bool Ic4_isSharedLink(uint16_t conn);                                  // Link opened by the IC4 console
    void Ic4_sharedLinkEvent(struct ble_gap_event *event);                // NOTIFY_RX, DISCONNECT of that link
    void Ic4_onHeartRateClient(uint16_t conn);                            // A client subscribed to our heart rate

    #ifdef __cplusplus
    }
    #endif

#endif
