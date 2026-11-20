#ifndef _IC4_CLIENT_H_
#define _IC4_CLIENT_H_

    #ifdef __cplusplus
        extern "C" {
    #endif

    // C1: client of the Schwinn IC4 ("IC Bike", FTMS 0x1826), subscribed to Indoor Bike Data (0x2AD2).
    // Never writes the Fitness Machine Control Point (0x2AD9), never reads the IC4 heart rate service (0x180D).
    void Ic4_init(void);    // Before the NimBLE host task starts
    void Ic4_start(void);   // NimBLE task (on_sync): first scan

    #ifdef __cplusplus
    }
    #endif

#endif
