#ifndef _BRIDGE_H_
#define _BRIDGE_H_

    #include <stdint.h>
    #include <stdbool.h>
    #include "Config.h"
    #include "Ftms.h"

    #ifdef __cplusplus
        extern "C" {
    #endif

    // State of a central link (IC4, HRM 600)
    typedef enum {
        LINK_OFF = 0,        // Not started (BLE not synchronised yet)
        LINK_BACKOFF,        // Waiting before the next attempt (retry_at_us)
        LINK_SCANNING,
        LINK_CONNECTING,
        LINK_DISCOVERING,    // Connected: MTU, services, characteristics, CCCD
        LINK_READY,          // Subscribed to the notifications
    } link_state_t;

    #define BRIDGE_RAW_MAX   32   // Last frame kept for the console
    #define BRIDGE_CHARS_MAX 12   // Characteristics of the IC4 FTMS service kept for the diagnostic
    #define BRIDGE_SEEN_MAX  6    // Devices seen while scanning

    // Whole bridge state: written by the NimBLE task (Bridge_lock), read with Bridge_getStatus (UI, console)
    typedef struct {
        // IC4 (central, FTMS)
        link_state_t ic4_state;
        int64_t ic4_retry_at_us;          // LINK_BACKOFF: time of the next attempt (esp_timer)
        uint32_t ic4_backoff_ms;
        char ic4_addr[18];
        int8_t ic4_rssi;
        uint16_t ic4_mtu;
        uint32_t ic4_connections;         // Successful subscriptions since boot
        bool ic4_shared;                  // Link opened by the IC4 console (to our heart rate), FTMS read over it
        bool ic4_hr_client;               // The IC4 console uses our heart rate (NVS): grace period at its wake-up
        bool ic4_grace;                   // LINK_BACKOFF: IC4 seen, waiting for its console to connect first
        uint32_t ic4_frames;
        uint32_t ic4_bad_frames;          // Shorter than their flags announce
        int64_t ic4_last_frame_us;        // 0: no frame yet
        ftms_bike_data_t ic4_data;        // Last frame decoded
        uint8_t ic4_raw[BRIDGE_RAW_MAX];
        uint8_t ic4_raw_len;
        uint16_t ic4_chars[BRIDGE_CHARS_MAX];   // Characteristics of the FTMS service (last discovery)
        uint8_t ic4_char_count;

        // Optional FTMS characteristics, read-only (diagnostic: is the resistance level available?)
        bool ic4_features_read;
        uint32_t ic4_features;            // Fitness Machine Feature (0x2ACC): machine features
        uint32_t ic4_target_features;     //   and target setting features
        uint8_t ic4_status[BRIDGE_RAW_MAX];   // Last Fitness Machine Status notification (0x2ADA)
        uint8_t ic4_status_len;
        uint32_t ic4_status_count;
        uint8_t ic4_training[BRIDGE_RAW_MAX]; // Last Training Status notification (0x2AD3)
        uint8_t ic4_training_len;
        uint32_t ic4_training_count;

        // Devices seen while scanning for the IC4: advertising FTMS, or with a name starting like IC4_NAME
        uint8_t seen_count;
        struct {
            char addr[18];
            char name[20];
            int8_t rssi;
            bool ftms;                    // Advertises the FTMS service 0x1826
            bool name_ok;                 // Name starts with IC4_NAME
            bool logged;                  // Already in the event log
        } seen[BRIDGE_SEEN_MAX];

        // HRM 600 (central, Heart Rate 0x180D)
        link_state_t hrm_state;
        int64_t hrm_retry_at_us;          // LINK_BACKOFF: time of the next attempt
        uint32_t hrm_backoff_ms;
        char hrm_addr[18];                // Strap connected (or being connected)
        char hrm_name[20];
        int8_t hrm_rssi;
        bool hrm_saved;                   // Address saved in NVS: only this strap is used
        char hrm_saved_addr[18];
        bool hrm_encrypted;
        bool hrm_secure_mode;             // The strap wants an authenticated link and refuses the pairing: it is in
                                          // its "secure" connection type (press its button twice: "open" type)
        int8_t hrm_battery;               // %, -1: unknown
        uint32_t hrm_connections;         // Successful subscriptions since boot
        uint32_t hrm_frames;

        // Heart rate (HRM 600). The IC4 only receives the heart rate: never taken from it.
        uint8_t hr_bpm;
        int64_t hr_last_us;               // 0: never

        // Clients (peripheral: Fenix 8, IC4 console, nRF Connect...)
        bool advertising;
        uint8_t client_count;
        struct {
            uint16_t conn;
            char addr[18];
            bool cps_notify;              // Subscribed to Cycling Power Measurement
            bool cp_indicate;             // Indications of the Cycling Power Control Point enabled
            bool csc_notify;              // Subscribed to CSC Measurement (speed sensor)
            bool sc_indicate;             // Indications of the SC Control Point enabled
            bool hrs_notify;              // Subscribed to Heart Rate Measurement (IC4 console)
            bool encrypted;
        } clients[BRIDGE_MAX_CLIENTS];

        // Cycling Power Measurement and CSC Measurement sent (1 Hz)
        bool cps_fresh;                   // FTMS data younger than FTMS_TIMEOUT_MS
        int16_t cps_power_w;
        float cps_cadence_rpm;            // Cadence used for the crank synthesis
        uint16_t cps_crank_revs;          // Cumulative crank revolutions (uint16, wraps)
        uint16_t cps_crank_time;          // Last crank event time (1/1024 s, wraps)
        float cps_speed_kmh;              // Speed used for the wheel synthesis
        uint32_t cps_wheel_revs;          // Cumulative wheel revolutions (uint32)
        uint16_t cps_wheel_time;          // Last wheel event time (1/2048 s, wraps)
        uint32_t cps_notifications;
        uint32_t csc_notifications;
        uint32_t hrs_notifications;       // Heart rate values relayed (one per value received from the HRM)
        float distance_km;                // Integrated from the IC4 speed (DISTANCE_RESET_IDLE_MS)
        uint32_t cps_calibrations;        // Offset compensations (watch calibration) answered
    } bridge_status_t;

    extern bridge_status_t g_bridge;     // Only written between Bridge_lock / Bridge_unlock
    void Bridge_lock(void);
    void Bridge_unlock(void);
    void Bridge_getStatus(bridge_status_t *out);   // Consistent copy, from any task

    const char *Bridge_linkStateStr(link_state_t state);

    // Console "bridge raw": every Indoor Bike Data frame printed (hex + decoded)
    extern volatile bool g_bridge_raw_print;

    #ifdef __cplusplus
    }
    #endif

#endif
