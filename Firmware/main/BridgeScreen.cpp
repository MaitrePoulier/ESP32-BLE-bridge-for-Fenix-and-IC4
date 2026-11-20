// Diagnostic screen of the IC4 Bridge (480 x 320)
//   header : name, time of day (SNTP at boot; uptime until the time is known)
//   links  : IC4 (FTMS), Fenix (clients of the bridge), HRM 600, with a coloured dot
//   values : power (W), cadence (rpm), distance (km), heart rate (bpm)
// Touching the screen shows the IC4 details page (last frame, characteristics, devices seen, event log): the
// diagnostic is possible next to the bike, without the serial console. Touch again to come back.
#include "BridgeScreen.h"
#include "Bridge.h"
#include "Config.h"
#include "EventLog.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"
#include "lvgl.h"

// Theme in the style of the Garmin watches: black background, white figures, grey labels, and the colours of the
// Garmin heart rate zones (approximations) for every state
#define GARMIN_GREY    0x8C8C8C   // Zone 1
#define GARMIN_BLUE    0x1D8FE1   // Zone 2
#define GARMIN_GREEN   0x4FB848   // Zone 3
#define GARMIN_ORANGE  0xF28C1E   // Zone 4
#define GARMIN_RED     0xE2231A   // Zone 5

#define COLOR_BG       0x000000
#define COLOR_PANEL    0x1A1A1A
#define COLOR_TEXT     0xFFFFFF
#define COLOR_DIM      0x9E9E9E
#define COLOR_OK       GARMIN_GREEN
#define COLOR_BUSY     GARMIN_ORANGE
#define COLOR_ERROR    GARMIN_RED
#define COLOR_OFF      0x4A4A4A

#define TILE_W   114      // 4 tiles of 114 px with 6 px gaps
#define TILE_GAP 6

struct link_row_t { lv_obj_t *dot; lv_obj_t *state; uint32_t color; };
struct value_tile_t { lv_obj_t *tile; lv_obj_t *title; lv_obj_t *value; lv_obj_t *unit; uint32_t bg; };

static link_row_t s_ic4, s_fenix, s_hrm;
static value_tile_t s_power, s_cadence, s_distance, s_heart;
static lv_obj_t *s_clock;
static lv_obj_t *s_main_scr, *s_details_scr, *s_details_text;
static uint32_t s_tick = 0;

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

static link_row_t make_row(lv_obj_t *parent, int y, const char *name)
{
    link_row_t row = {};
    row.dot = lv_obj_create(parent);
    lv_obj_remove_style_all(row.dot);
    lv_obj_set_clickable(row.dot, false);
    lv_obj_set_size(row.dot, 14, 14);
    lv_obj_set_style_radius(row.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(row.dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row.dot, lv_color_hex(COLOR_OFF), 0);
    row.color = COLOR_OFF;
    lv_obj_set_pos(row.dot, 14, y + 4);

    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT), 0);
    lv_label_set_text(label, name);
    lv_obj_set_pos(label, 38, y);

    row.state = lv_label_create(parent);
    lv_obj_set_style_text_font(row.state, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(row.state, lv_color_hex(COLOR_DIM), 0);
    lv_obj_set_width(row.state, 300);
    lv_label_set_long_mode(row.state, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(row.state, "");
    lv_obj_set_pos(row.state, 160, y + 3);
    return row;
}

static value_tile_t make_tile(lv_obj_t *parent, int x, const char *title, const char *unit)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, TILE_W, 132);
    lv_obj_set_pos(tile, x, 180);
    lv_obj_set_style_radius(tile, 10, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_clickable(tile, false);   // The touch goes to the screen (details page)

    value_tile_t tv = {};
    tv.title = lv_label_create(tile);
    lv_obj_set_style_text_color(tv.title, lv_color_hex(COLOR_DIM), 0);
    lv_label_set_text(tv.title, title);
    lv_obj_align(tv.title, LV_ALIGN_TOP_MID, 0, 8);

    tv.tile = tile;
    tv.bg = COLOR_PANEL;
    tv.value = lv_label_create(tile);
    lv_obj_set_style_text_font(tv.value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(tv.value, lv_color_hex(COLOR_TEXT), 0);
    lv_label_set_text(tv.value, "--");
    lv_obj_align(tv.value, LV_ALIGN_CENTER, 0, 4);

    tv.unit = lv_label_create(tile);
    lv_obj_set_style_text_color(tv.unit, lv_color_hex(COLOR_DIM), 0);
    lv_label_set_text(tv.unit, unit);
    lv_obj_align(tv.unit, LV_ALIGN_BOTTOM_MID, 0, -8);
    return tv;
}

// Background of a tile (highlight): only changed when needed. On a colour, the labels turn white.
static void set_tile_bg(value_tile_t &tile, uint32_t color)
{
    if (tile.bg == color) return;
    tile.bg = color;
    lv_obj_set_style_bg_color(tile.tile, lv_color_hex(color), 0);
    const uint32_t labels = (color == COLOR_PANEL) ? COLOR_DIM : COLOR_TEXT;
    lv_obj_set_style_text_color(tile.title, lv_color_hex(labels), 0);
    lv_obj_set_style_text_color(tile.unit, lv_color_hex(labels), 0);
}

// Heart rate zones (Config.h): Z1 Recovery ... Z5 Anaerobic, colours of the Garmin zones
struct hr_zone_t { int min; const char *name; uint32_t color; };
static const hr_zone_t HR_ZONES[] = {
    {HR_Z1_MIN, "Z1 Recovery",  GARMIN_GREY},
    {HR_Z2_MIN, "Z2 Endurance", GARMIN_BLUE},
    {HR_Z3_MIN, "Z3 Tempo",     GARMIN_GREEN},
    {HR_Z4_MIN, "Z4 Threshold", GARMIN_ORANGE},
    {HR_Z5_MIN, "Z5 Anaerobic", GARMIN_RED},
};

static const hr_zone_t *hr_zone(int bpm)
{
    const hr_zone_t *zone = nullptr;
    for (const hr_zone_t &z : HR_ZONES) {
        if (bpm >= z.min) zone = &z;
    }
    return zone;   // nullptr: below Z1
}

static void refresh_cb(lv_timer_t *timer)
{
    bridge_status_t b;
    Bridge_getStatus(&b);
    const int64_t now = esp_timer_get_time();
    char text[96];

    // Time of day (set by SNTP at boot); uptime as long as the time is not known
    const time_t t = time(NULL);
    if (t > 1700000000) {
        struct tm local;
        localtime_r(&t, &local);
        strftime(text, sizeof(text), "%H:%M", &local);
    } else {
        const uint32_t up = (uint32_t)(now / 1000000);
        snprintf(text, sizeof(text), "up %lu:%02lu:%02lu", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60),
                 (unsigned long)(up % 60));
    }
    set_text(s_clock, text);

    // IC4
    const bool ftms_fresh = b.ic4_state == LINK_READY && b.ic4_last_frame_us != 0 &&
                            now - b.ic4_last_frame_us <= (int64_t)FTMS_TIMEOUT_MS * 1000;
    switch (b.ic4_state) {
        case LINK_READY:
            if (ftms_fresh) {
                snprintf(text, sizeof(text), "Connected%s - data %.1f s ago", b.ic4_shared ? " (console)" : "",
                         (now - b.ic4_last_frame_us) / 1e6);
                set_dot(s_ic4, COLOR_OK);
            } else {
                snprintf(text, sizeof(text), "Connected - no data");
                set_dot(s_ic4, COLOR_BUSY);
            }
            break;
        case LINK_BACKOFF: {
            const int64_t left = b.ic4_retry_at_us - now;
            const int s = left > 0 ? (int)((left + 999999) / 1000000) : 0;
            if (b.ic4_grace) {
                snprintf(text, sizeof(text), "Seen - waiting for its console (%d s)", s);
                set_dot(s_ic4, COLOR_BUSY);
            } else {
                snprintf(text, sizeof(text), "Not found - retry in %d s", s);
                set_dot(s_ic4, COLOR_ERROR);
            }
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
    if (ftms_fresh && b.ic4_data.cadence_present) snprintf(text, sizeof(text), "%.0f", b.ic4_data.cadence_rpm);
    else snprintf(text, sizeof(text), "--");
    set_text(s_cadence.value, text);
    // Target cadence: tile highlighted in green
    const bool cadence_target = ftms_fresh && b.ic4_data.cadence_present &&
                                b.ic4_data.cadence_rpm >= CADENCE_TARGET_MIN && b.ic4_data.cadence_rpm <= CADENCE_TARGET_MAX;
    set_tile_bg(s_cadence, cadence_target ? GARMIN_GREEN : COLOR_PANEL);
    set_text(s_cadence.unit, cadence_target ? "rpm - target" : "rpm");

    // Distance of the session, from the IC4 speed (4 characters: fits the tile)
    const float km = b.distance_km;
    snprintf(text, sizeof(text), km < 10 ? "%.2f" : km < 100 ? "%.1f" : "%.0f", km);
    set_text(s_distance.value, text);

    // Heart rate: HRM 600. The IC4 only receives it: its frames are never used for this tile.
    if (hr_valid) {
        const int bpm = b.hr_bpm;
        snprintf(text, sizeof(text), "%d", bpm);
        set_text(s_heart.value, text);
        const hr_zone_t *zone = hr_zone(bpm);
        if (zone != nullptr && bpm <= HR_Z5_MAX) {
            set_text(s_heart.unit, zone->name);
            set_tile_bg(s_heart, zone->color);
        } else {
            set_text(s_heart.unit, zone != nullptr ? "Above Z5" : "Below Z1");
            set_tile_bg(s_heart, zone != nullptr ? GARMIN_RED : COLOR_PANEL);
        }
    } else {
        set_text(s_heart.value, "--");
        set_text(s_heart.unit, "bpm");
        set_tile_bg(s_heart, COLOR_PANEL);
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

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, BRIDGE_NAME);
    lv_obj_set_pos(title, 12, 8);

    s_clock = lv_label_create(scr);
    lv_obj_set_style_text_font(s_clock, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_clock, lv_color_hex(COLOR_TEXT), 0);
    lv_label_set_text(s_clock, "");
    lv_obj_align(s_clock, LV_ALIGN_TOP_RIGHT, -12, 8);

    s_ic4 = make_row(scr, 52, "IC4");
    s_fenix = make_row(scr, 92, "Fenix");
    s_hrm = make_row(scr, 132, "HRM 600");

    s_power = make_tile(scr, TILE_GAP, "Power", "W");
    s_cadence = make_tile(scr, TILE_GAP + (TILE_W + TILE_GAP), "Cadence", "rpm");
    s_distance = make_tile(scr, TILE_GAP + 2 * (TILE_W + TILE_GAP), "Distance", "km");
    s_heart = make_tile(scr, TILE_GAP + 3 * (TILE_W + TILE_GAP), "Heart rate", "bpm");

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
