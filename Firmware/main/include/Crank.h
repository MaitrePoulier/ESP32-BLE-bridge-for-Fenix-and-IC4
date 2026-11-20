#ifndef _CRANK_H_
#define _CRANK_H_

    #include <stdint.h>

    #ifdef __cplusplus
        extern "C" {
    #endif

    // Crank revolution data synthesised from a cadence (Cycling Power Measurement, bit5).
    // The cadence is integrated: each complete revolution increments the counter and gives its event time.
    // Cadence 0: both values stay frozen. uint16 values wrap as the specification expects.
    typedef struct {
        double fraction;     // Position in the current revolution [0, 1)
        int64_t t_us;        // Time up to which the cadence was integrated (0: not started)
        float rpm;           // Cadence since t_us
        uint16_t revs;       // Cumulative crank revolutions
        uint16_t time;       // Last crank event time, 1/1024 s
    } crank_t;

    // Integrates the current cadence up to now_us
    void Crank_advance(crank_t *c, int64_t now_us);

    // New cadence from now_us (integrates the previous one up to now_us first)
    void Crank_setCadence(crank_t *c, int64_t now_us, float rpm);

    // No data any more since last_us: integrates up to last_us + timeout, then freezes (cadence 0) until now_us
    void Crank_freeze(crank_t *c, int64_t frozen_at_us, int64_t now_us);

    #ifdef __cplusplus
    }
    #endif

#endif
