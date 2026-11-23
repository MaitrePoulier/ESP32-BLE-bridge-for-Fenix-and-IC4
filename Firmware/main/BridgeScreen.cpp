// Main screen of the IC4 Bridge (480 x 320), in the style of the Garmin watches
//   top    : state of each link (IC4, Fenix, HRM 600) with a coloured dot, time of day (empty until known)
//   left   : heart rate gauge (HRM 600), bands of the 5 heart rate zones, zone below
//   centre : power (W) and distance (km)
//   right  : cadence gauge, bands of the 5 cadence zones (same colours as the heart rate, Z3 green: target)
// Each band is as wide as its span of values (Config.h); a marker in the colour of the zone shows the value.
// Touching the screen shows the IC4 details page (last frame, characteristics, devices seen, event log): the
// diagnostic is possible next to the bike, without the serial console. Touch again to come back.
#include "BridgeScreen.h"
#include "Bridge.h"
#include "Config.h"
#include "EventLog.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"
#include "lvgl.h"

// Zone colours
#define ZONE_GREY      0xD8D4DE
#define ZONE_BLUE      0x1E7FD6
#define ZONE_GREEN     0x3DBE5B
#define ZONE_ORANGE    0xF7A13A
#define ZONE_RED       0xF0504F

#define COLOR_BG       0x000000
#define COLOR_LINE     0x333333
#define COLOR_TEXT     0xFFFFFF
#define COLOR_DIM      0x9E9E9E
#define COLOR_OK       ZONE_GREEN
#define COLOR_BUSY     ZONE_ORANGE
#define COLOR_ERROR    ZONE_RED
#define COLOR_OFF      0x4A4A4A

// Gauges: 270 degree arc open at the bottom (LVGL angles: 0 = 3 o'clock, clockwise)
#define GAUGE_SIZE     164
#define GAUGE_Y        112
#define GAUGE_MARGIN   10
#define GAUGE_ARC_W    10
#define GAUGE_START    135
#define GAUGE_SWEEP    270
#define MARKER_SIZE    22
#define ZONE_COUNT     5

struct link_row_t { lv_obj_t *dot; lv_obj_t *state; uint32_t color; };

// Zones of a gauge: lower bound of each zone, upper bound of the last one, colour of each zone
struct gauge_scale_t { int min[ZONE_COUNT]; int max; uint32_t color[ZONE_COUNT]; };
static const gauge_scale_t HR_SCALE = {
    {HR_Z1_MIN, HR_Z2_MIN, HR_Z3_MIN, HR_Z4_MIN, HR_Z5_MIN}, HR_Z5_MAX,
    {ZONE_GREY, ZONE_BLUE, ZONE_GREEN, ZONE_ORANGE, ZONE_RED},
};
static const gauge_scale_t CADENCE_SCALE = {
    {CADENCE_Z1_MIN, CADENCE_Z2_MIN, CADENCE_Z3_MIN, CADENCE_Z4_MIN, CADENCE_Z5_MIN}, CADENCE_Z5_MAX,
    {ZONE_GREY, ZONE_BLUE, ZONE_GREEN, ZONE_ORANGE, ZONE_RED},   // Same colours as the heart rate
};

struct gauge_t {
    const gauge_scale_t *scale;
    int cx, cy;                 // Centre of the arc (screen coordinates)
    lv_obj_t *marker;
    lv_obj_t *value, *unit, *caption;
    int marker_angle;           // -1: hidden
    uint32_t marker_color;
};

struct value_box_t { lv_obj_t *value; };

static link_row_t s_ic4, s_fenix, s_hrm;
static gauge_t s_hr_gauge, s_cadence_gauge;
static value_box_t s_power, s_distance;
static lv_obj_t *s_clock;
static lv_obj_t *s_main_scr, *s_details_scr, *s_details_text;
static uint32_t s_tick = 0;

// Heart above the heart rate (A8: drawn in the image recolor colour)
static const uint8_t HEART_A8[] = {
    0x00, 0x00, 0x07, 0x80, 0xCD, 0xD4, 0xCD, 0x80, 0x0E, 0x0E, 0x80, 0xCD, 0xD4, 0xCD, 0x80, 0x07, 0x00, 0x00,
    0x00, 0x07, 0xCD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xDC, 0xDC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xCD, 0x07, 0x00,
    0x00, 0x78, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x78, 0x00,
    0x00, 0xC6, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC6, 0x00,
    0x00, 0xD4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xD4, 0x00,
    0x00, 0xCD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xCD, 0x00,
    0x00, 0x95, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x95, 0x00,
    0x00, 0x2A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x2A, 0x00,
    0x00, 0x00, 0x9C, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x9C, 0x00, 0x00,
    0x00, 0x00, 0x0E, 0xDC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xDC, 0x0E, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x2A, 0xEA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xEA, 0x2A, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1C, 0xCD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xCD, 0x1C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x8E, 0xFF, 0xFF, 0xFF, 0xFF, 0x8E, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x39, 0xCD, 0xCD, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
#define HEART_W 18
#define HEART_H 15
static lv_image_dsc_t s_heart_dsc;

static void set_text(lv_obj_t *label, const char *text)
{
    const char *current = lv_label_get_text(label);
    if (current == NULL || strcmp(current, text) != 0) lv_label_set_text(label, text);
}

static void set_dot(link_row_t &row, uint32_t color)
{
    if (row.color == color) return;
    row.color = color;
    lv_obj_set_style_bg_color(row.dot, lv_color_hex(color), 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static link_row_t make_row(lv_obj_t *parent, int y, const char *name, int state_width)
{
    link_row_t row = {};
    row.dot = lv_obj_create(parent);
    lv_obj_remove_style_all(row.dot);
    lv_obj_set_clickable(row.dot, false);
    lv_obj_set_size(row.dot, 12, 12);
    lv_obj_set_style_radius(row.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(row.dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row.dot, lv_color_hex(COLOR_OFF), 0);
    row.color = COLOR_OFF;
    lv_obj_set_pos(row.dot, 14, y + 6);

    lv_obj_t *label = make_label(parent, &lv_font_montserrat_20, COLOR_TEXT, name);
    lv_obj_set_pos(label, 36, y);

    row.state = make_label(parent, &lv_font_montserrat_16, COLOR_DIM, "");
    lv_obj_set_width(row.state, state_width);
    lv_label_set_long_mode(row.state, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(row.state, 150, y + 3);
    return row;
}

// Angle (degrees, LVGL) of a value on a gauge: the scale is linear from the first lower bound to the last upper bound
static int gauge_angle(const gauge_scale_t &scale, float v)
{
    const float lo = (float)scale.min[0], hi = (float)scale.max;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return GAUGE_START + (int)lroundf((v - lo) * GAUGE_SWEEP / (hi - lo));
}

// Zone of a value: 0..4, -1 below the first zone, ZONE_COUNT above the last one
static int gauge_zone(const gauge_scale_t &scale, float v)
{
    if (v < scale.min[0]) return -1;
    if (v > scale.max) return ZONE_COUNT;
    int zone = 0;
    for (int z = 0; z < ZONE_COUNT; z++) {
        if (v >= scale.min[z]) zone = z;
    }
    return zone;
}

static lv_obj_t *make_band(lv_obj_t *parent, int x, int y, int a0, int a1, uint32_t color, bool rounded)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);   // No knob, no indicator, no padding: only the background arc (the band)
    lv_obj_set_clickable(arc, false);
    lv_obj_set_size(arc, GAUGE_SIZE, GAUGE_SIZE);
    lv_obj_set_pos(arc, x, y);
    lv_arc_set_bg_angles(arc, a0 % 360, a1 % 360);
    lv_obj_set_style_arc_width(arc, GAUGE_ARC_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, rounded, LV_PART_MAIN);
    return arc;
}

static gauge_t make_gauge(lv_obj_t *parent, int x, const gauge_scale_t *scale, const char *unit, const char *caption,
                          bool heart)
{
    gauge_t g = {};
    g.scale = scale;
    g.cx = x + GAUGE_SIZE / 2;
    g.cy = GAUGE_Y + GAUGE_SIZE / 2;

    // Bands, as wide as the span of their values. Only the two ends of the gauge are rounded: the last band is drawn
    // first and each band covers the round cap of the previous one.
    int angle[ZONE_COUNT + 1];
    for (int z = 0; z < ZONE_COUNT; z++) angle[z] = gauge_angle(*scale, (float)scale->min[z]);
    angle[ZONE_COUNT] = GAUGE_START + GAUGE_SWEEP;
    make_band(parent, x, GAUGE_Y, angle[ZONE_COUNT - 1], angle[ZONE_COUNT], scale->color[ZONE_COUNT - 1], true);
    for (int z = 0; z < ZONE_COUNT - 1; z++) {
        make_band(parent, x, GAUGE_Y, angle[z], angle[z + 1], scale->color[z], z == 0);
    }

    // Marker: disc in the colour of the zone, black ring to detach it from the band
    g.marker = lv_obj_create(parent);
    lv_obj_remove_style_all(g.marker);
    lv_obj_set_clickable(g.marker, false);
    lv_obj_set_size(g.marker, MARKER_SIZE, MARKER_SIZE);
    lv_obj_set_style_radius(g.marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g.marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g.marker, 3, 0);
    lv_obj_set_style_border_color(g.marker, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_hidden(g.marker, true);
    g.marker_angle = -1;

    g.value = make_label(parent, &lv_font_montserrat_48, COLOR_TEXT, "--");
    lv_obj_align(g.value, LV_ALIGN_CENTER, g.cx - SCREEN_WIDTH / 2, g.cy + 4 - SCREEN_HEIGHT / 2);
    g.unit = make_label(parent, &lv_font_montserrat_16, COLOR_DIM, unit);
    lv_obj_align(g.unit, LV_ALIGN_CENTER, g.cx - SCREEN_WIDTH / 2, g.cy + 38 - SCREEN_HEIGHT / 2);
    g.caption = make_label(parent, &lv_font_montserrat_20, COLOR_TEXT, caption);
    lv_obj_align(g.caption, LV_ALIGN_TOP_MID, g.cx - SCREEN_WIDTH / 2, 284);

    if (heart) {
        lv_obj_t *img = lv_image_create(parent);
        lv_image_set_src(img, &s_heart_dsc);
        lv_obj_set_style_image_recolor(img, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
        lv_obj_align(img, LV_ALIGN_CENTER, g.cx - SCREEN_WIDTH / 2, g.cy - 34 - SCREEN_HEIGHT / 2);
    }
    return g;
}

// Value shown on a gauge (NAN: no value): text, marker on the arc in the colour of the zone
static void gauge_set(gauge_t &g, float v, const char *text)
{
    set_text(g.value, text);
    int angle = -1;
    uint32_t color = 0;
    if (!isnan(v)) {
        angle = gauge_angle(*g.scale, v);
        const int zone = gauge_zone(*g.scale, v);
        color = g.scale->color[zone < 0 ? 0 : zone >= ZONE_COUNT ? ZONE_COUNT - 1 : zone];
    }
    if (angle != g.marker_angle) {
        if (angle < 0) {
            lv_obj_set_hidden(g.marker, true);
        } else {
            const float r = (GAUGE_SIZE - GAUGE_ARC_W) / 2.0f;
            const float rad = angle * (float)M_PI / 180.0f;
            lv_obj_set_pos(g.marker, g.cx + (int)lroundf(r * cosf(rad)) - MARKER_SIZE / 2,
                           g.cy + (int)lroundf(r * sinf(rad)) - MARKER_SIZE / 2);
            if (g.marker_angle < 0) lv_obj_set_hidden(g.marker, false);
        }
        g.marker_angle = angle;
    }
    if (angle >= 0 && color != g.marker_color) {
        g.marker_color = color;
        lv_obj_set_style_bg_color(g.marker, lv_color_hex(color), 0);
    }
}

// Centre column: title, value, unit
static value_box_t make_value_box(lv_obj_t *parent, int y, const char *title, const char *unit)
{
    const int cx = SCREEN_WIDTH / 2;
    lv_obj_t *t = make_label(parent, &lv_font_montserrat_16, COLOR_DIM, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, cx - SCREEN_WIDTH / 2, y);
    value_box_t box = {};
    box.value = make_label(parent, &lv_font_montserrat_36, COLOR_TEXT, "--");
    lv_obj_align(box.value, LV_ALIGN_TOP_MID, cx - SCREEN_WIDTH / 2, y + 20);
    lv_obj_t *u = make_label(parent, &lv_font_montserrat_14, COLOR_DIM, unit);
    lv_obj_align(u, LV_ALIGN_TOP_MID, cx - SCREEN_WIDTH / 2, y + 60);
    return box;
}

static lv_obj_t *make_line(lv_obj_t *parent, int x, int y, int w)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_clickable(line, false);
    lv_obj_set_size(line, w, 1);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(COLOR_LINE), 0);
    return line;
}

static void refresh_cb(lv_timer_t *timer)
{
    bridge_status_t b;
    Bridge_getStatus(&b);
    const int64_t now = esp_timer_get_time();
    char text[96];

    // Time of day (set by SNTP at boot); empty as long as the time is not known
    const time_t t = time(NULL);
    text[0] = '\0';
    if (t > 1700000000) {
        struct tm local;
        localtime_r(&t, &local);
        strftime(text, sizeof(text), "%H:%M", &local);
    }
    set_text(s_clock, text);

    // IC4
    const bool ftms_fresh = b.ic4_state == LINK_READY && b.ic4_last_frame_us != 0 &&
                            now - b.ic4_last_frame_us <= (int64_t)FTMS_TIMEOUT_MS * 1000;
    switch (b.ic4_state) {
        case LINK_READY:
            if (ftms_fresh) {
                snprintf(text, sizeof(text), "Connected - data %.1f s ago", (now - b.ic4_last_frame_us) / 1e6);
                set_dot(s_ic4, COLOR_OK);
            } else {
                snprintf(text, sizeof(text), "Connected - no data");
                set_dot(s_ic4, COLOR_BUSY);
            }
            break;
        case LINK_BACKOFF: {
            const int64_t left = b.ic4_retry_at_us - now;
            const int s = left > 0 ? (int)((left + 999999) / 1000000) : 0;
            snprintf(text, sizeof(text), "Not found - retry in %d s", s);
            set_dot(s_ic4, COLOR_ERROR);
            break;
        }
        case LINK_OFF:
            snprintf(text, sizeof(text), "Off");
            set_dot(s_ic4, COLOR_OFF);
            break;
        default:
            snprintf(text, sizeof(text), "%s...", Bridge_linkStateStr(b.ic4_state));
            set_dot(s_ic4, COLOR_BUSY);
            break;
    }
    set_text(s_ic4.state, text);

    // Fenix (and IC4 console): clients of the bridge, subscribed to the power, speed and heart rate notifications
    int subscribed = 0, speed = 0, heart = 0;
    for (int i = 0; i < b.client_count; i++) {
        subscribed += b.clients[i].cps_notify;
        speed += b.clients[i].csc_notify;
        heart += b.clients[i].hrs_notify;
    }
    if (b.client_count == 0) {
        snprintf(text, sizeof(text), "%s", b.advertising ? "Advertising - no client" : "Not advertising");
        set_dot(s_fenix, b.advertising ? COLOR_BUSY : COLOR_ERROR);
    } else {
        int n = snprintf(text, sizeof(text), "%d client(s): %d power", b.client_count, subscribed);
        if (speed > 0) n += snprintf(text + n, sizeof(text) - n, ", %d speed", speed);
        if (heart > 0) snprintf(text + n, sizeof(text) - n, ", %d HR", heart);
        set_dot(s_fenix, subscribed > 0 ? COLOR_OK : COLOR_BUSY);
    }
    set_text(s_fenix.state, text);

    // HRM 600
    const bool hr_valid = b.hr_last_us != 0 && b.hr_bpm > 0 && now - b.hr_last_us <= (int64_t)HR_TIMEOUT_MS * 1000;
    switch (b.hrm_state) {
        case LINK_READY: {
            char battery[16] = "";
            if (b.hrm_battery >= 0) snprintf(battery, sizeof(battery), " - battery %d%%", b.hrm_battery);
            snprintf(text, sizeof(text), "%s%s", hr_valid ? "Connected" : "Connected - no heart rate", battery);
            set_dot(s_hrm, hr_valid ? COLOR_OK : COLOR_BUSY);
            break;
        }
        case LINK_BACKOFF: {
            const int64_t left = b.hrm_retry_at_us - now;
            if (b.hrm_secure_mode) {
                snprintf(text, sizeof(text), "Secure mode: press its button 2x");
            } else {
                snprintf(text, sizeof(text), "Not found - retry in %d s",
                         left > 0 ? (int)((left + 999999) / 1000000) : 0);
            }
            set_dot(s_hrm, COLOR_ERROR);
            break;
        }
        case LINK_DISCOVERING:
            snprintf(text, sizeof(text), "Subscribing...");
            set_dot(s_hrm, COLOR_BUSY);
            break;
        case LINK_OFF:
            snprintf(text, sizeof(text), "Off");
            set_dot(s_hrm, COLOR_OFF);
            break;
        default:
            snprintf(text, sizeof(text), "%s...", Bridge_linkStateStr(b.hrm_state));
            set_dot(s_hrm, COLOR_BUSY);
            break;
    }
    set_text(s_hrm.state, text);

    // Values decoded from the IC4
    if (ftms_fresh && b.ic4_data.power_present) snprintf(text, sizeof(text), "%d", b.ic4_data.power_w);
    else snprintf(text, sizeof(text), "--");
    set_text(s_power.value, text);
    if (ftms_fresh && b.ic4_data.cadence_present) {
        snprintf(text, sizeof(text), "%.0f", b.ic4_data.cadence_rpm);
        gauge_set(s_cadence_gauge, b.ic4_data.cadence_rpm, text);
    } else {
        gauge_set(s_cadence_gauge, NAN, "--");
    }

    // Distance of the session, from the IC4 speed (4 characters: fits the centre column)
    const float km = b.distance_km;
    snprintf(text, sizeof(text), km < 10 ? "%.2f" : km < 100 ? "%.1f" : "%.0f", km);
    set_text(s_distance.value, text);

    // Heart rate: HRM 600. The IC4 only receives it: its frames are never used for this gauge.
    if (hr_valid) {
        const int bpm = b.hr_bpm;
        snprintf(text, sizeof(text), "%d", bpm);
        gauge_set(s_hr_gauge, (float)bpm, text);
        const int zone = gauge_zone(HR_SCALE, (float)bpm);
        if (zone < 0) snprintf(text, sizeof(text), "Below zone 1");
        else if (zone >= ZONE_COUNT) snprintf(text, sizeof(text), "Above zone 5");
        else snprintf(text, sizeof(text), "Zone %d", zone + 1);
        set_text(s_hr_gauge.caption, text);
    } else {
        gauge_set(s_hr_gauge, NAN, "--");
        set_text(s_hr_gauge.caption, "Heart rate");
    }
}

// IC4 details page: everything needed to diagnose the link next to the bike
static void details_refresh(void)
{
    bridge_status_t b;
    Bridge_getStatus(&b);
    const int64_t now = esp_timer_get_time();
    static char text[2600];
    size_t n = 0;
#define ADD(...) do { if (n < sizeof(text)) n += snprintf(text + n, sizeof(text) - n, __VA_ARGS__); } while (0)

    ADD("IC4 DETAILS  (touch to go back)      boot %u\n", EventLog_bootNumber());
    ADD("State: %s   %s   MTU %u\n", Bridge_linkStateStr(b.ic4_state), b.ic4_addr[0] ? b.ic4_addr : "--", b.ic4_mtu);
    if (b.ic4_last_frame_us) ADD("Frames: %lu (%lu bad), last %.1f s ago\n", (unsigned long)b.ic4_frames,
                                 (unsigned long)b.ic4_bad_frames, (now - b.ic4_last_frame_us) / 1e6);
    else ADD("Frames: none\n");
    if (b.ic4_raw_len) {
        ADD("Raw:");
        for (int i = 0; i < b.ic4_raw_len; i++) ADD(" %02X", b.ic4_raw[i]);
        ADD("\n");
        const ftms_bike_data_t &d = b.ic4_data;
        ADD("Flags 0x%04X:", d.flags);
        if (d.speed_present) ADD(" %.2f km/h", d.speed_kmh);
        if (d.cadence_present) ADD(" | %.1f rpm", d.cadence_rpm);
        if (d.power_present) ADD(" | %d W", d.power_w);
        if (d.resistance_present) ADD(" | res %d", d.resistance);
        ADD("\n");
        if (d.distance_present || d.energy_present || d.heart_rate_present || d.elapsed_present ||
            d.avg_speed_present || d.avg_cadence_present || d.avg_power_present || d.met_present ||
            d.remaining_present) {
            if (d.distance_present) ADD("%lu m  ", (unsigned long)d.distance_m);
            if (d.energy_present) ADD("%u kcal  ", d.energy_total_kcal);
            if (d.heart_rate_present) ADD("HR %u  ", d.heart_rate_bpm);
            if (d.elapsed_present) ADD("%u s  ", d.elapsed_s);
            if (d.avg_speed_present) ADD("avg %.1f km/h  ", d.avg_speed_kmh);
            if (d.avg_cadence_present) ADD("avg %.0f rpm  ", d.avg_cadence_rpm);
            if (d.avg_power_present) ADD("avg %d W  ", d.avg_power_w);
            if (d.met_present) ADD("MET %.1f  ", d.met);
            if (d.remaining_present) ADD("left %u s", d.remaining_s);
            ADD("\n");
        }
    }
    if (b.ic4_char_count) {
        ADD("FTMS chars:");
        for (int i = 0; i < b.ic4_char_count; i++) ADD(" %04X", b.ic4_chars[i]);
        ADD("\n");
    }
    if (b.ic4_features_read) {
        ADD("Features 0x%08lX target 0x%08lX: resistance %s\n", (unsigned long)b.ic4_features,
            (unsigned long)b.ic4_target_features, (b.ic4_features & (1u << 7)) ? "supported" : "NOT supported");
    }
    if (b.ic4_status_len) {
        ADD("Machine status (%lu):", (unsigned long)b.ic4_status_count);
        for (int i = 0; i < b.ic4_status_len; i++) ADD(" %02X", b.ic4_status[i]);
        ADD("\n");
    }
    if (b.ic4_training_len) {
        ADD("Training status (%lu):", (unsigned long)b.ic4_training_count);
        for (int i = 0; i < b.ic4_training_len; i++) ADD(" %02X", b.ic4_training[i]);
        ADD("\n");
    }
    ADD("Seen while scanning:%s\n", b.seen_count ? "" : " nothing (FTMS or \"" IC4_NAME "\")");
    for (int i = 0; i < b.seen_count; i++) {
        ADD("  \"%s\" %s %d dBm%s%s\n", b.seen[i].name, b.seen[i].addr, b.seen[i].rssi,
            b.seen[i].ftms ? " FTMS" : "", b.seen[i].name_ok ? " name ok" : "");
    }

    // Latest events, as many as the screen can show
    static event_t events[8];
    const int count = EventLog_copy(events, 8);
    ADD("Events:\n");
    for (int i = 0; i < count; i++) {
        const event_t &e = events[i];
        char when[16];
        if (e.boot != EventLog_bootNumber()) snprintf(when, sizeof(when), "boot %u", e.boot);
        else snprintf(when, sizeof(when), "%lu:%02lu:%02lu", (unsigned long)(e.uptime_s / 3600),
                      (unsigned long)(e.uptime_s / 60 % 60), (unsigned long)(e.uptime_s % 60));
        if (e.repeat) ADD("  %s %s (x%u)\n", when, e.text, e.repeat + 1);
        else ADD("  %s %s\n", when, e.text);
    }
#undef ADD
    set_text(s_details_text, text);
}

static void timer_cb(lv_timer_t *timer)
{
    if (lv_screen_active() == s_details_scr) {
        if (s_tick++ % 6 == 0) details_refresh();   // 5 Hz: a long text
    } else {
        refresh_cb(timer);
    }
}

static void toggle_cb(lv_event_t *e)
{
    if (lv_screen_active() == s_main_scr) {
        details_refresh();
        lv_screen_load(s_details_scr);
    } else {
        lv_screen_load(s_main_scr);
    }
}

void BridgeScreen_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    s_heart_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_heart_dsc.header.cf = LV_COLOR_FORMAT_A8;
    s_heart_dsc.header.w = HEART_W;
    s_heart_dsc.header.h = HEART_H;
    s_heart_dsc.header.stride = HEART_W;
    s_heart_dsc.data_size = sizeof(HEART_A8);
    s_heart_dsc.data = HEART_A8;

    s_clock = make_label(scr, &lv_font_montserrat_26, COLOR_TEXT, "");
    lv_obj_align(s_clock, LV_ALIGN_TOP_RIGHT, -12, 8);

    // The IC4 state is shorter: the clock is on its right
    s_ic4 = make_row(scr, 8, "IC4", 240);
    s_fenix = make_row(scr, 38, "Fenix", SCREEN_WIDTH - 150 - 8);
    s_hrm = make_row(scr, 68, "HRM 600", SCREEN_WIDTH - 150 - 8);
    make_line(scr, 0, 102, SCREEN_WIDTH);

    s_hr_gauge = make_gauge(scr, GAUGE_MARGIN, &HR_SCALE, "bpm", "Heart rate", true);
    s_cadence_gauge = make_gauge(scr, SCREEN_WIDTH - GAUGE_MARGIN - GAUGE_SIZE, &CADENCE_SCALE, "rpm", "Cadence",
                                 false);
    s_power = make_value_box(scr, 126, "Power", "W");
    make_line(scr, SCREEN_WIDTH / 2 - 40, 208, 80);
    s_distance = make_value_box(scr, 220, "Distance", "km");

    // Details page, shown by touching the screen
    s_main_scr = scr;
    lv_obj_add_event_cb(scr, toggle_cb, LV_EVENT_CLICKED, NULL);
    s_details_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_details_scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_details_scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_details_scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_details_scr, toggle_cb, LV_EVENT_CLICKED, NULL);
    s_details_text = lv_label_create(s_details_scr);
    lv_obj_set_style_text_font(s_details_text, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_details_text, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_width(s_details_text, SCREEN_WIDTH - 12);
    lv_label_set_long_mode(s_details_text, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(s_details_text, 6, 4);
    lv_label_set_text(s_details_text, "");

    refresh_cb(NULL);
    lv_timer_create(timer_cb, UI_REFRESH_MS, NULL);
}
