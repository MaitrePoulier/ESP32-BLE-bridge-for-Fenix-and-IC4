#ifndef _BLE_H_
#define _BLE_H_

    #include <stdint.h>
    #include <stdbool.h>
    #include "host/ble_hs.h"

    #ifdef __cplusplus
        extern "C" {
    #endif

    // NimBLE: controller + host, GATT server (BleServer), IC4 client (Ic4Client). NVS must be initialised.
    // Everything BLE runs in the NimBLE host task (GAP/GATT callbacks and the ble_npl_callout timers).
    void Ble_init(void);

    extern uint8_t g_own_addr_type;      // Peripheral (advertising): public address, the one the Fenix bonded with
    extern uint8_t g_central_addr_type;  // Central (IC4, HRM): the same public address

    // One scan or connection establishment at a time (NimBLE): the IC4 and HRM clients take turns.
    // NimBLE task only. Taken before ble_gap_disc, released at the end of the scan or at the CONNECT event.
    typedef enum { BLE_CENTRAL_FREE = 0, BLE_CENTRAL_IC4, BLE_CENTRAL_HRM } ble_central_owner_t;
    bool Ble_centralAcquire(ble_central_owner_t owner);   // false: the other client is scanning or connecting
    void Ble_centralRelease(ble_central_owner_t owner);
    #define BLE_CENTRAL_BUSY_RETRY_MS 500

    // "AA:BB:CC:DD:EE:FF" (most significant byte first, as nRF Connect shows it)
    void Ble_addrToStr(const ble_addr_t *addr, char out[18]);

    // Timer callback run in the NimBLE host task
    struct ble_npl_callout;
    void Ble_calloutStart(struct ble_npl_callout *co, uint32_t ms);

    #ifdef __cplusplus
    }
    #endif

#endif
