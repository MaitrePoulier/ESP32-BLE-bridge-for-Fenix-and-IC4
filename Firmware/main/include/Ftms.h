#ifndef _FTMS_H_
#define _FTMS_H_

    #include <stdint.h>
    #include <stdbool.h>
    #include <stddef.h>

    #ifdef __cplusplus
        extern "C" {
    #endif

    // FTMS Indoor Bike Data (0x2AD2), Fitness Machine Service v1.0, section 4.9.1.
    // Every field announced by the flags is read, so that the offsets stay right whatever the optional fields sent.
    typedef struct {
        uint16_t flags;

        bool speed_present;          float speed_kmh;          // bit0 = 0 (More Data): uint16, 0.01 km/h
        bool avg_speed_present;      float avg_speed_kmh;      // bit1: uint16, 0.01 km/h
        bool cadence_present;        float cadence_rpm;        // bit2: uint16, 0.5 rpm
        bool avg_cadence_present;    float avg_cadence_rpm;    // bit3: uint16, 0.5 rpm
        bool distance_present;       uint32_t distance_m;      // bit4: uint24, m
        bool resistance_present;     int16_t resistance;       // bit5: sint16
        bool power_present;          int16_t power_w;          // bit6: sint16, W
        bool avg_power_present;      int16_t avg_power_w;      // bit7: sint16, W
        bool energy_present;                                   // bit8: total (uint16 kcal), per hour (uint16 kcal),
        uint16_t energy_total_kcal;  uint16_t energy_per_hour_kcal; uint8_t energy_per_minute_kcal; // per min (uint8)
        bool heart_rate_present;     uint8_t heart_rate_bpm;   // bit9: uint8, bpm
        bool met_present;            float met;                // bit10: uint8, 0.1
        bool elapsed_present;        uint16_t elapsed_s;       // bit11: uint16, s
        bool remaining_present;      uint16_t remaining_s;     // bit12: uint16, s
    } ftms_bike_data_t;

    // Returns false if the frame is shorter than its flags announce (out is then incomplete)
    bool Ftms_decodeIndoorBikeData(const uint8_t *data, size_t len, ftms_bike_data_t *out);

    #ifdef __cplusplus
    }
    #endif

#endif
