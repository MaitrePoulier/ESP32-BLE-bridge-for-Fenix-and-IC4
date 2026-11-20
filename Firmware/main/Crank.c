// Crank revolution data synthesised from the FTMS cadence (see Crank.h)
#include "Crank.h"
#include <math.h>

void Crank_advance(crank_t *c, int64_t now_us)
{
    if (c->t_us == 0 || now_us <= c->t_us) { c->t_us = now_us; return; }
    const double t0 = c->t_us / 1e6;
    const double dt = (now_us - c->t_us) / 1e6;
    c->t_us = now_us;
    if (c->rpm <= 0) return;

    const double rps = c->rpm / 60.0;
    const double position = c->fraction + rps * dt;
    const double complete = floor(position);
    if (complete >= 1) {
        // Time of the last complete revolution in this interval
        const double t_event = t0 + (complete - c->fraction) / rps;
        c->revs = (uint16_t)(c->revs + (uint32_t)complete);
        c->time = (uint16_t)((uint64_t)llround(t_event * 1024.0) & 0xFFFF);
    }
    c->fraction = position - complete;
}

void Crank_setCadence(crank_t *c, int64_t now_us, float rpm)
{
    Crank_advance(c, now_us);
    c->rpm = rpm;
}

void Crank_freeze(crank_t *c, int64_t frozen_at_us, int64_t now_us)
{
    if (c->rpm > 0) {
        Crank_advance(c, frozen_at_us);
        c->rpm = 0;
    }
    c->t_us = now_us;
}
