// FTMS Indoor Bike Data decoder (Fitness Machine Service v1.0, section 4.9.1). Little-endian fields.
#include "Ftms.h"
#include <string.h>

typedef struct {
    const uint8_t *p;
    size_t left;
    bool ok;
} reader_t;

static bool take(reader_t *r, size_t n)
{
    if (!r->ok || r->left < n) { r->ok = false; return false; }
    return true;
}

static uint8_t rd_u8(reader_t *r)
{
    if (!take(r, 1)) return 0;
    const uint8_t v = r->p[0];
    r->p += 1; r->left -= 1;
    return v;
}

static uint16_t rd_u16(reader_t *r)
{
    if (!take(r, 2)) return 0;
    const uint16_t v = (uint16_t)(r->p[0] | (r->p[1] << 8));
    r->p += 2; r->left -= 2;
    return v;
}

static uint32_t rd_u24(reader_t *r)
{
    if (!take(r, 3)) return 0;
    const uint32_t v = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) | ((uint32_t)r->p[2] << 16);
    r->p += 3; r->left -= 3;
    return v;
}

bool Ftms_decodeIndoorBikeData(const uint8_t *data, size_t len, ftms_bike_data_t *out)
{
    memset(out, 0, sizeof(*out));
    reader_t r = { data, len, true };

    const uint16_t f = rd_u16(&r);
    out->flags = f;

    // Fields in the order of the specification
    if (!(f & (1 << 0))) { out->speed_kmh = rd_u16(&r) * 0.01f;            out->speed_present = r.ok; }
    if (f & (1 << 1))    { out->avg_speed_kmh = rd_u16(&r) * 0.01f;        out->avg_speed_present = r.ok; }
    if (f & (1 << 2))    { out->cadence_rpm = rd_u16(&r) * 0.5f;           out->cadence_present = r.ok; }
    if (f & (1 << 3))    { out->avg_cadence_rpm = rd_u16(&r) * 0.5f;       out->avg_cadence_present = r.ok; }
    if (f & (1 << 4))    { out->distance_m = rd_u24(&r);                   out->distance_present = r.ok; }
    if (f & (1 << 5))    { out->resistance = (int16_t)rd_u16(&r);          out->resistance_present = r.ok; }
    if (f & (1 << 6))    { out->power_w = (int16_t)rd_u16(&r);             out->power_present = r.ok; }
    if (f & (1 << 7))    { out->avg_power_w = (int16_t)rd_u16(&r);         out->avg_power_present = r.ok; }
    if (f & (1 << 8)) {
        out->energy_total_kcal = rd_u16(&r);
        out->energy_per_hour_kcal = rd_u16(&r);
        out->energy_per_minute_kcal = rd_u8(&r);
        out->energy_present = r.ok;
    }
    if (f & (1 << 9))    { out->heart_rate_bpm = rd_u8(&r);                out->heart_rate_present = r.ok; }
    if (f & (1 << 10))   { out->met = rd_u8(&r) * 0.1f;                    out->met_present = r.ok; }
    if (f & (1 << 11))   { out->elapsed_s = rd_u16(&r);                    out->elapsed_present = r.ok; }
    if (f & (1 << 12))   { out->remaining_s = rd_u16(&r);                  out->remaining_present = r.ok; }
    // bits 13-15: reserved

    return r.ok;
}
