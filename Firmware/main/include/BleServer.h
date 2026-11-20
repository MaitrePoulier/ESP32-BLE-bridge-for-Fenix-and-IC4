#ifndef _BLE_SERVER_H_
#define _BLE_SERVER_H_

    #include <stdint.h>
    #include <stdbool.h>

    #ifdef __cplusplus
        extern "C" {
    #endif

    // GATT server of the "IC4 Bridge": Cycling Power (0x1818), Cycling Speed and Cadence (0x1816, speed only),
    // Heart Rate (0x180D, relay of the HRM 600 for the IC4 console) and Device Information (0x180A)
    int  BleServer_init(void);             // Before the NimBLE host task starts
    void BleServer_startAdvertising(void); // NimBLE task (on_sync), restarted after each connection

    // New Indoor Bike Data from the IC4 (NimBLE task): power, cadence and speed used by the 1 Hz notifications
    void Cps_onFtmsData(int64_t now_us, bool power_present, int16_t power_w, bool cadence_present, float cadence_rpm,
                        bool speed_present, float speed_kmh);

    // New heart rate from the HRM 600 (NimBLE task): notified at once to the clients of the Heart Rate service
    void BleServer_relayHeartRate(uint8_t bpm);

    #ifdef __cplusplus
    }
    #endif

#endif
