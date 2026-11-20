#ifndef _BLE_H_
#define _BLE_H_

    #include <stdint.h>
    #include "host/ble_hs.h"

    #ifdef __cplusplus
        extern "C" {
    #endif

    // NimBLE: controller + host, GATT server (BleServer), IC4 client (Ic4Client). NVS must be initialised.
    // Everything BLE runs in the NimBLE host task (GAP/GATT callbacks and the ble_npl_callout timers).
    void Ble_init(void);

    extern uint8_t g_own_addr_type;      // Set when the host is synchronised

    // "AA:BB:CC:DD:EE:FF" (most significant byte first, as nRF Connect shows it)
    void Ble_addrToStr(const ble_addr_t *addr, char out[18]);

    // Timer callback run in the NimBLE host task
    struct ble_npl_callout;
    void Ble_calloutStart(struct ble_npl_callout *co, uint32_t ms);

    #ifdef __cplusplus
    }
    #endif

#endif
