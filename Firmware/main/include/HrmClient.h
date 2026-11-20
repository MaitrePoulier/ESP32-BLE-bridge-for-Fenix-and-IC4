#ifndef _HRM_CLIENT_H_
#define _HRM_CLIENT_H_

    #ifdef __cplusplus
        extern "C" {
    #endif

    // C2: client of the Garmin HRM 600 heart rate strap (Heart Rate 0x180D), subscribed to Heart Rate Measurement
    // (0x2A37). Each value received is relayed by the Heart Rate service of the bridge (IC4 console).
    void Hrm_init(void);    // Before the NimBLE host task starts
    void Hrm_start(void);   // NimBLE task (on_sync): saved address read from NVS, first scan

    // Console "hrm forget": erases the saved strap (and its bond); the next strap named HRM_NAME_PREFIXES is used.
    // Any task: the work is done in the NimBLE task.
    void Hrm_forget(void);

    #ifdef __cplusplus
    }
    #endif

#endif
