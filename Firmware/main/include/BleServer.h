#ifndef _BLE_SERVER_H_
#define _BLE_SERVER_H_

    #include <stdint.h>
    #include <stdbool.h>

    #ifdef __cplusplus
        extern "C" {
    #endif

    // GATT server of the "IC4 Bridge": Cycling Power (0x1818) and Device Information (0x180A)
    int  BleServer_init(void);             // Before the NimBLE host task starts
    void BleServer_startAdvertising(void); // NimBLE task (on_sync), restarted after each connection

    // New Indoor Bike Data from the IC4 (NimBLE task): power and cadence used by the 1 Hz CPS notification
    void Cps_onFtmsData(int64_t now_us, bool power_present, int16_t power_w, bool cadence_present, float cadence_rpm);

    #ifdef __cplusplus
    }
    #endif

#endif
