// Host tests of main/Ftms.c and main/Crank.c, run on the PC (see README.md, "Host tests")
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "Ftms.h"
#include "Crank.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-3)

static void test_ftms_typical(void)
{
    // flags 0x0044: speed (bit0 = 0), cadence (bit2), power (bit6)
    const uint8_t f[] = {0x44, 0x00, 0x28, 0x0A, 0xB4, 0x00, 0x96, 0x00};
    ftms_bike_data_t d;
    CHECK(Ftms_decodeIndoorBikeData(f, sizeof(f), &d), "typical: decode");
    CHECK(d.speed_present && NEAR(d.speed_kmh, 26.00), "speed %f", d.speed_kmh);
    CHECK(d.cadence_present && NEAR(d.cadence_rpm, 90.0), "cadence %f", d.cadence_rpm);
    CHECK(d.power_present && d.power_w == 150, "power %d", d.power_w);
    CHECK(!d.heart_rate_present && !d.distance_present, "no extra field");
}

static void test_ftms_optional_fields(void)
{
    // flags 0x0A74: speed, cadence (2), distance (4), resistance (5), power (6), heart rate (9), elapsed (11)
    const uint8_t f[] = {0x74, 0x0A,
                         0x10, 0x27,             // speed 100.00 km/h
                         0x50, 0x00,             // cadence 40 rpm
                         0x39, 0x30, 0x00,       // distance 12345 m
                         0x0C, 0x00,             // resistance 12
                         0xF6, 0xFF,             // power -10 W
                         0x7B,                   // heart rate 123
                         0x10, 0x0E};            // elapsed 3600 s
    ftms_bike_data_t d;
    CHECK(Ftms_decodeIndoorBikeData(f, sizeof(f), &d), "optional: decode");
    CHECK(NEAR(d.speed_kmh, 100.0), "speed %f", d.speed_kmh);
    CHECK(NEAR(d.cadence_rpm, 40.0), "cadence %f", d.cadence_rpm);
    CHECK(d.distance_present && d.distance_m == 12345, "distance %u", d.distance_m);
    CHECK(d.resistance_present && d.resistance == 12, "resistance %d", d.resistance);
    CHECK(d.power_present && d.power_w == -10, "power %d", d.power_w);
    CHECK(d.heart_rate_present && d.heart_rate_bpm == 123, "hr %u", d.heart_rate_bpm);
    CHECK(d.elapsed_present && d.elapsed_s == 3600, "elapsed %u", d.elapsed_s);
}

static void test_ftms_all_fields(void)
{
    // flags 0x1FFF: More Data (no speed), every other field
    const uint8_t f[] = {0xFF, 0x1F,
                         0xE8, 0x03,             // average speed 10.00
                         0x3C, 0x00,             // cadence 30
                         0x50, 0x00,             // average cadence 40
                         0x01, 0x00, 0x01,       // distance 65537
                         0xFE, 0xFF,             // resistance -2
                         0xC8, 0x00,             // power 200
                         0x64, 0x00,             // average power 100
                         0x2C, 0x01, 0x58, 0x02, 0x0A,   // energy 300 kcal, 600 kcal/h, 10 kcal/min
                         0x50,                   // heart rate 80
                         0x23,                   // MET 3.5
                         0x3C, 0x00,             // elapsed 60
                         0x78, 0x00};            // remaining 120
    ftms_bike_data_t d;
    CHECK(Ftms_decodeIndoorBikeData(f, sizeof(f), &d), "all: decode");
    CHECK(!d.speed_present, "no speed when More Data = 1");
    CHECK(NEAR(d.avg_speed_kmh, 10.0), "avg speed %f", d.avg_speed_kmh);
    CHECK(NEAR(d.cadence_rpm, 30.0) && NEAR(d.avg_cadence_rpm, 40.0), "cadences");
    CHECK(d.distance_m == 65537, "distance %u", d.distance_m);
    CHECK(d.resistance == -2 && d.power_w == 200 && d.avg_power_w == 100, "resistance/power");
    CHECK(d.energy_total_kcal == 300 && d.energy_per_hour_kcal == 600 && d.energy_per_minute_kcal == 10, "energy");
    CHECK(d.heart_rate_bpm == 80 && NEAR(d.met, 3.5), "hr/met");
    CHECK(d.elapsed_s == 60 && d.remaining_s == 120, "times %u %u", d.elapsed_s, d.remaining_s);
}

static void test_ftms_truncated(void)
{
    const uint8_t f[] = {0x44, 0x00, 0x28, 0x0A, 0xB4};   // cadence cut, power missing
    ftms_bike_data_t d;
    CHECK(!Ftms_decodeIndoorBikeData(f, sizeof(f), &d), "truncated frame must fail");
    CHECK(d.speed_present && !d.cadence_present && !d.power_present, "only complete fields present");
    CHECK(!Ftms_decodeIndoorBikeData(f, 1, &d), "1 byte must fail");
}

static uint16_t ticks1024(double t_s) { return (uint16_t)((uint64_t)llround(t_s * 1024.0) & 0xFFFF); }

static void test_crank_constant(void)
{
    // 90 rpm = 1.5 rev/s from t = 1 s; samples every second
    crank_t c = {0};
    Crank_setCadence(&c, 1000000, 90.0f);
    for (int s = 2; s <= 11; s++) Crank_advance(&c, (int64_t)s * 1000000);
    // 10 s * 1.5 = 15 revolutions, the 15th exactly at t = 1 + 15 / 1.5 = 11 s
    CHECK(c.revs == 15, "revs %u", c.revs);
    CHECK(c.time == ticks1024(11.0), "time %u expected %u", c.time, ticks1024(11.0));
}

static void test_crank_change_and_zero(void)
{
    crank_t c = {0};
    Crank_setCadence(&c, 1000000, 60.0f);          // 1 rev/s
    Crank_setCadence(&c, 3500000, 120.0f);         // 2.5 revs at 60 rpm, then 2 rev/s
    // fraction 0.5 at 3.5 s; next revolution at 3.5 + 0.5/2 = 3.75 s, then every 0.5 s
    Crank_advance(&c, 4600000);                    // revolutions at 3.75, 4.25 -> 2 + 2 = 4
    CHECK(c.revs == 4, "revs %u", c.revs);
    CHECK(c.time == ticks1024(4.25), "time %u expected %u", c.time, ticks1024(4.25));

    const uint16_t revs = c.revs, time = c.time;
    Crank_setCadence(&c, 4700000, 0.0f);           // fraction 0.7 at 4.6 s, +0.2 -> 0.9, then stopped
    Crank_advance(&c, 20000000);
    CHECK(c.revs == revs && c.time == time, "cadence 0: frozen (revs %u time %u)", c.revs, c.time);
    Crank_setCadence(&c, 21000000, 60.0f);         // Restart: fraction 0.9 kept, next revolution at +0.1 s
    Crank_advance(&c, 21700000);
    CHECK(c.revs == revs + 1u && c.time == ticks1024(21.1), "restart: revs %u time %u expected %u", c.revs, c.time,
          ticks1024(21.1));
}

static void test_crank_freeze(void)
{
    crank_t c = {0};
    Crank_setCadence(&c, 1000000, 60.0f);
    // Last data at 2 s, timeout 3 s: integrated up to 5 s (4 revolutions), then frozen
    Crank_freeze(&c, 5000000, 9000000);
    CHECK(c.revs == 4 && c.time == ticks1024(5.0), "freeze: revs %u time %u", c.revs, c.time);
    Crank_advance(&c, 30000000);
    CHECK(c.revs == 4, "frozen: revs %u", c.revs);
}

static void test_crank_wrap(void)
{
    crank_t c = {0};
    c.revs = 65534;
    Crank_setCadence(&c, 1000000, 120.0f);         // 2 rev/s
    Crank_advance(&c, 3000000);                    // 4 revolutions: 65534 + 4 = 2 (uint16, as sent)
    CHECK((uint16_t)c.revs == 2, "revs wrap %u", (unsigned)(uint16_t)c.revs);
    // Event time after 64 s wraps (1/1024 s on 16 bits)
    Crank_advance(&c, 70000000);
    CHECK(c.time == ticks1024(70.0) && c.time < 65536, "time wrap %u expected %u", c.time, ticks1024(70.0));
}

// Wheel: 30 km/h with a 2096 mm wheel = 3.976 rev/s; time in 1/2048 s
static void test_wheel(void)
{
    crank_t w = {0};
    w.ticks_per_s = 2048;
    const float rpm = 30.0f / 3.6f / 2.096f * 60.0f;
    Crank_setCadence(&w, 1000000, rpm);
    Crank_advance(&w, 11000000);                   // 10 s: 39.76 revolutions -> 39, last one at 1 + 39 / 3.976 s
    const double t = 1.0 + 39.0 / (rpm / 60.0);
    CHECK(w.revs == 39, "wheel revs %u", (unsigned)w.revs);
    CHECK(w.time == (uint16_t)llround(t * 2048.0), "wheel time %u expected %u", w.time,
          (unsigned)(uint16_t)llround(t * 2048.0));
}

int main(void)
{
    test_ftms_typical();
    test_ftms_optional_fields();
    test_ftms_all_fields();
    test_ftms_truncated();
    test_crank_constant();
    test_crank_change_and_zero();
    test_crank_freeze();
    test_crank_wrap();
    test_wheel();
    printf(failures ? "%d FAILURE(S)\n" : "All tests passed\n", failures);
    return failures != 0;
}
