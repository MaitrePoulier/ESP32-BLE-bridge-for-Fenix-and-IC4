#ifndef _CRANK_H_
#define _CRANK_H_

    #include <stdint.h>

    #ifdef __cplusplus
        extern "C" {
    #endif

    // Revolution data synthesised from a rotation speed: crank (Cycling Power Measurement bit5, 1/1024 s) from the
    // FTMS cadence, wheel (CPS bit4, CSC Measurement bit0, 1/2048 s) from the FTMS speed.
    // The speed is integrated: each complete revolution increments the counter and gives its event time.
    // Speed 0: both values stay frozen. The event time wraps on 16 bits as the specifications expect; the counter is
    // sent as uint16 (crank) or uint32 (wheel).
    typedef struct {
        double fraction;     // Position in the current revolution [0, 1)
        int64_t t_us;        // Time up to which the speed was integrated (0: not started)
        float rpm;           // Revolutions per minute since t_us
        uint32_t revs;       // Cumulative revolutions
        uint16_t time;       // Last revolution event time, 1/ticks_per_s s
        uint16_t ticks_per_s;// Unit of time: 0 = 1024 (crank), 2048 for the wheel
    } crank_t;

    // Integrates the current speed up to now_us
    void Crank_advance(crank_t *c, int64_t now_us);

    // New speed (rpm) from now_us (integrates the previous one up to now_us first)
    void Crank_setCadence(crank_t *c, int64_t now_us, float rpm);

    // No data any more since last_us: integrates up to last_us + timeout, then freezes (cadence 0) until now_us
    void Crank_freeze(crank_t *c, int64_t frozen_at_us, int64_t now_us);

    #ifdef __cplusplus
    }
    #endif

#endif
