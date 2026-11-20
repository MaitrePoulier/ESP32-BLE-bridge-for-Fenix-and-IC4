// C1: client of the Schwinn IC4 (central role)
// Scan "IC Bike" + FTMS (0x1826) -> connect -> MTU -> FTMS service -> Indoor Bike Data (0x2AD2) -> its CCCD ->
// notifications. Any failure or disconnection: new attempt after a backoff of 1 s, 2 s, 4 s... 30 s max.
// Everything runs in the NimBLE host task: the GATT server is never blocked.
#include "Ic4Client.h"
#include "Ble.h"
#include "BleServer.h"
#include "Bridge.h"
#include "Config.h"
#include "Ftms.h"
#include "EventLog.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"

static const char *TAG = "Ic4";

#define UUID_FTMS                0x1826
#define UUID_INDOOR_BIKE_DATA    0x2AD2
#define UUID_CONTROL_POINT       0x2AD9   // NEVER written
#define UUID_FEATURE             0x2ACC   // Read once (diagnostic)
#define UUID_MACHINE_STATUS      0x2ADA   // Notifications, diagnostic only
#define UUID_TRAINING_STATUS     0x2AD3   // Notifications, diagnostic only
#define UUID_CCCD                0x2902

// Subscriptions, in this order: Indoor Bike Data (required), then the optional ones (a failure is only logged)
static const uint16_t SUBSCRIPTIONS[] = { UUID_INDOOR_BIKE_DATA, UUID_MACHINE_STATUS, UUID_TRAINING_STATUS };
#define SUBSCRIPTION_COUNT (sizeof(SUBSCRIPTIONS) / sizeof(SUBSCRIPTIONS[0]))

#define STRINGIFY2(x) #x
#define STRINGIFY(x) STRINGIFY2(x)

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start, s_svc_end;
static uint16_t s_ibd_handle;              // Indoor Bike Data value handle
static uint16_t s_status_handle, s_training_handle;
static uint16_t s_cp_handle;               // Control Point value handle: only to refuse writing it
static uint16_t s_cccd_handle;
static struct { uint16_t uuid, def, val; } s_chrs[BRIDGE_CHARS_MAX];   // Characteristics of the FTMS service
static int s_chr_count;
static unsigned s_sub_step;                // Index in SUBSCRIPTIONS
static uint32_t s_backoff_ms = BLE_BACKOFF_MIN_MS;
static bool s_had_data = false;            // The last connection received data: backoff restarts at the minimum
static struct ble_npl_callout s_retry_timer;
static struct ble_npl_callout s_watch_timer;   // Connected but no data any more: reconnection
static int64_t s_last_data_us;                 // Subscription or last frame
static uint32_t s_conn_frames;                 // Frames of the current connection (the first ones are logged)
static uint16_t s_last_flags;                  // Flags of the last frame (a change is logged)

// Scan candidates: the name and the FTMS UUID may come in the advertising data or in the scan response
#define CANDIDATES 8
static struct { ble_addr_t addr; bool name_ok, ftms_ok; char name[20]; } s_cand[CANDIDATES];
static int s_cand_next = 0;

static int gap_event(struct ble_gap_event *event, void *arg);
static void scan(void);

static void set_state(link_state_t state)
{
    Bridge_lock();
    g_bridge.ic4_state = state;
    Bridge_unlock();
}

// New attempt after the backoff (doubled at each failure, up to BLE_BACKOFF_MAX_MS)
static void schedule_retry(void)
{
    if (s_had_data) s_backoff_ms = BLE_BACKOFF_MIN_MS;
    s_had_data = false;
    const uint32_t delay = s_backoff_ms;
    s_backoff_ms = (s_backoff_ms * 2 > BLE_BACKOFF_MAX_MS) ? BLE_BACKOFF_MAX_MS : s_backoff_ms * 2;

    Bridge_lock();
    g_bridge.ic4_state = LINK_BACKOFF;
    g_bridge.ic4_backoff_ms = delay;
    g_bridge.ic4_retry_at_us = esp_timer_get_time() + (int64_t)delay * 1000;
    Bridge_unlock();
    ESP_LOGI(TAG, "Next attempt in %lu s", (unsigned long)(delay / 1000));
    Ble_calloutStart(&s_retry_timer, delay);
}

static void retry_cb(struct ble_npl_event *ev)
{
    scan();
}

// Failure while connected: disconnect, the DISCONNECT event schedules the retry
static void fail(const char *step, int status)
{
    ESP_LOGW(TAG, "%s failed (%d): disconnecting", step, status);
    EventLog_add("IC4: %s failed (%d)", step, status);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

// ---------------------------------------------------------------------------------------------------------------
// Scan and connection
// ---------------------------------------------------------------------------------------------------------------

static void scan(void)
{
    memset(s_cand, 0, sizeof(s_cand));
    s_cand_next = 0;

    struct ble_gap_disc_params params = {0};
    params.passive = 0;               // Active: the scan response may hold the name
    params.filter_duplicates = 0;
    int rc = ble_gap_disc(g_own_addr_type, IC4_SCAN_TIMEOUT_MS, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc: %d", rc);
        schedule_retry();
        return;
    }
    set_state(LINK_SCANNING);
}

// Devices that look like the IC4 (FTMS service or name): kept for the diagnostic, logged at the end of the scan
// (the name often comes in the scan response, after the first advertising report)
static void seen_record(const struct ble_gap_disc_desc *disc, const char *name, bool ftms, bool name_ok)
{
    char addr[18];
    Ble_addrToStr(&disc->addr, addr);
    Bridge_lock();
    int i;
    for (i = 0; i < g_bridge.seen_count; i++) {
        if (strcmp(g_bridge.seen[i].addr, addr) == 0) break;
    }
    if (i == g_bridge.seen_count && g_bridge.seen_count < BRIDGE_SEEN_MAX) {
        g_bridge.seen_count++;
        memset(&g_bridge.seen[i], 0, sizeof(g_bridge.seen[i]));
        strcpy(g_bridge.seen[i].addr, addr);
    }
    if (i < g_bridge.seen_count) {
        if (name[0]) strlcpy(g_bridge.seen[i].name, name, sizeof(g_bridge.seen[i].name));
        g_bridge.seen[i].rssi = disc->rssi;
        g_bridge.seen[i].ftms |= ftms;
        g_bridge.seen[i].name_ok |= name_ok;
    }
    Bridge_unlock();
}

// End of a scan: devices seen for the first time go to the event log
static void seen_log_new(void)
{
    for (int i = 0; i < BRIDGE_SEEN_MAX; i++) {
        char name[20], addr[18];
        int rssi;
        bool ftms, log_it = false;
        Bridge_lock();
        if (i < g_bridge.seen_count && !g_bridge.seen[i].logged) {
            g_bridge.seen[i].logged = true;
            log_it = true;
            strcpy(name, g_bridge.seen[i].name);
            strcpy(addr, g_bridge.seen[i].addr);
            rssi = g_bridge.seen[i].rssi;
            ftms = g_bridge.seen[i].ftms;
        }
        Bridge_unlock();
        if (log_it) EventLog_add("Seen \"%s\" %s %d dBm%s", name, addr, rssi, ftms ? " FTMS" : "");
    }
}

static bool adv_matches(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) return false;

    int i;
    for (i = 0; i < CANDIDATES; i++) {
        if (ble_addr_cmp(&s_cand[i].addr, &disc->addr) == 0) break;
    }
    if (i == CANDIDATES) {
        i = s_cand_next;
        s_cand_next = (s_cand_next + 1) % CANDIDATES;
        memset(&s_cand[i], 0, sizeof(s_cand[i]));
        s_cand[i].addr = disc->addr;
    }

    const size_t name_len = strlen(IC4_NAME);
    if (fields.name != NULL && fields.name_len > 0) {
        const size_t n = fields.name_len < sizeof(s_cand[i].name) - 1 ? fields.name_len : sizeof(s_cand[i].name) - 1;
        for (size_t k = 0; k < n; k++) {   // Name received over the air: printable characters only
            const char c = (char)fields.name[k];
            s_cand[i].name[k] = (c >= 0x20 && c < 0x7F) ? c : '?';
        }
        s_cand[i].name[n] = '\0';
        if (fields.name_len >= name_len && memcmp(fields.name, IC4_NAME, name_len) == 0) s_cand[i].name_ok = true;
    }
    for (int u = 0; u < fields.num_uuids16; u++) {
        if (ble_uuid_u16(&fields.uuids16[u].u) == UUID_FTMS) s_cand[i].ftms_ok = true;
    }
    if (s_cand[i].ftms_ok || s_cand[i].name_ok) {
        seen_record(disc, s_cand[i].name, s_cand[i].ftms_ok, s_cand[i].name_ok);
    }
    return s_cand[i].name_ok && s_cand[i].ftms_ok;
}

static void ic4_connect(const struct ble_gap_disc_desc *disc)
{
    ble_gap_disc_cancel();
    seen_log_new();

    char addr[18];
    Ble_addrToStr(&disc->addr, addr);
    Bridge_lock();
    strcpy(g_bridge.ic4_addr, addr);
    g_bridge.ic4_rssi = disc->rssi;
    Bridge_unlock();
    ESP_LOGI(TAG, "IC4 found: %s (%d dBm), connecting", addr, disc->rssi);
    EventLog_add("IC4 found %s %d dBm, connecting", addr, disc->rssi);

    int rc = ble_gap_connect(g_own_addr_type, &disc->addr, IC4_CONNECT_TIMEOUT_MS, NULL, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_connect: %d", rc);
        schedule_retry();
        return;
    }
    set_state(LINK_CONNECTING);
}

// ---------------------------------------------------------------------------------------------------------------
// Discovery: MTU -> FTMS service -> characteristics -> CCCD of Indoor Bike Data -> notifications on
// ---------------------------------------------------------------------------------------------------------------

static int chr_find(uint16_t uuid)
{
    for (int i = 0; i < s_chr_count; i++) {
        if (s_chrs[i].uuid == uuid) return i;
    }
    return -1;
}

// Last handle of a characteristic (its descriptors end before the next characteristic)
static uint16_t chr_end(int i)
{
    return (i + 1 < s_chr_count) ? s_chrs[i + 1].def - 1 : s_svc_end;
}

static void subscribe_next(uint16_t conn);

// The only writes of this client: CCCDs (notifications on). Never the Control Point (0x2AD9) nor its CCCD.
static int write_guarded(uint16_t conn, uint16_t handle, const void *data, uint16_t len, ble_gatt_attr_fn *cb)
{
    const int cp = chr_find(UUID_CONTROL_POINT);
    if (handle == 0 || handle == s_cp_handle || (cp >= 0 && handle >= s_chrs[cp].def && handle <= chr_end(cp))) {
        ESP_LOGE(TAG, "Refused: write to handle %d (Fitness Machine Control Point)", handle);
        return BLE_HS_EINVAL;
    }
    return ble_gattc_write_flat(conn, handle, data, len, cb, NULL);
}

// Fitness Machine Feature: machine features (uint32) + target setting features (uint32)
static int on_feature(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0 || attr == NULL) {
        EventLog_add("FTMS feature read failed (%d)", error->status);
        return 0;
    }
    uint8_t v[8] = {0};
    const uint16_t len = OS_MBUF_PKTLEN(attr->om) < sizeof(v) ? OS_MBUF_PKTLEN(attr->om) : sizeof(v);
    os_mbuf_copydata(attr->om, 0, len, v);
    const uint32_t machine = v[0] | (v[1] << 8) | (v[2] << 16) | ((uint32_t)v[3] << 24);
    const uint32_t target = v[4] | (v[5] << 8) | (v[6] << 16) | ((uint32_t)v[7] << 24);
    Bridge_lock();
    g_bridge.ic4_features_read = true;
    g_bridge.ic4_features = machine;
    g_bridge.ic4_target_features = target;
    Bridge_unlock();
    // Machine bit 7: Resistance Level Supported; target bit 2: Resistance Target Setting Supported
    EventLog_add("FTMS features 0x%08lX target 0x%08lX, resistance %s", (unsigned long)machine,
                 (unsigned long)target, (machine & (1u << 7)) ? "supported" : "NOT supported");
    return 0;
}

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    const uint16_t uuid = SUBSCRIPTIONS[s_sub_step];
    if (error->status != 0) {
        if (s_sub_step == 0) { fail("Subscription", error->status); return 0; }
        EventLog_add("Subscription to %04X failed (%d)", uuid, error->status);
    } else if (s_sub_step == 0) {
        Bridge_lock();
        g_bridge.ic4_state = LINK_READY;
        g_bridge.ic4_connections++;
        Bridge_unlock();
        s_last_data_us = esp_timer_get_time();
        s_conn_frames = 0;
        ESP_LOGI(TAG, "Subscribed to Indoor Bike Data");
        EventLog_add("IC4 subscribed to Indoor Bike Data (MTU %u)", ble_att_mtu(conn));
    } else {
        EventLog_add("Subscribed to %04X (diagnostic)", uuid);
    }
    s_sub_step++;
    subscribe_next(conn);
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    if (error->status == 0) {
        if (ble_uuid_u16(&dsc->uuid.u) == UUID_CCCD) s_cccd_handle = dsc->handle;
        return 0;
    }
    const uint16_t uuid = SUBSCRIPTIONS[s_sub_step];
    int rc = (error->status == BLE_HS_EDONE && s_cccd_handle == 0) ? BLE_HS_ENOENT : error->status;
    if (rc == BLE_HS_EDONE) {
        static const uint8_t notify_on[2] = {0x01, 0x00};
        rc = write_guarded(conn, s_cccd_handle, notify_on, sizeof(notify_on), on_cccd_written);
        if (rc == 0) return 0;
    }
    if (s_sub_step == 0) { fail("CCCD of Indoor Bike Data", rc); return 0; }
    EventLog_add("No subscription to %04X (%d)", uuid, rc);
    s_sub_step++;
    subscribe_next(conn);
    return 0;
}

// Next subscription of SUBSCRIPTIONS (one GATT procedure at a time), then the feature read
static void subscribe_next(uint16_t conn)
{
    while (s_sub_step < SUBSCRIPTION_COUNT) {
        const int i = chr_find(SUBSCRIPTIONS[s_sub_step]);
        if (i >= 0) {
            s_cccd_handle = 0;
            const int rc = ble_gattc_disc_all_dscs(conn, s_chrs[i].val, chr_end(i), on_dsc, NULL);
            if (rc == 0) return;
            if (s_sub_step == 0) { fail("ble_gattc_disc_all_dscs", rc); return; }
        } else if (s_sub_step == 0) {
            fail("Indoor Bike Data (0x2AD2)", BLE_HS_ENOENT);
            return;
        }
        s_sub_step++;
    }
    const int f = chr_find(UUID_FEATURE);
    if (f >= 0) ble_gattc_read(conn, s_chrs[f].val, on_feature, NULL);
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0) {
        const uint16_t uuid = ble_uuid_u16(&chr->uuid.u);
        if (s_chr_count < BRIDGE_CHARS_MAX) {
            s_chrs[s_chr_count].uuid = uuid;
            s_chrs[s_chr_count].def = chr->def_handle;
            s_chrs[s_chr_count].val = chr->val_handle;
            s_chr_count++;
        }
        Bridge_lock();
        if (g_bridge.ic4_char_count < BRIDGE_CHARS_MAX) g_bridge.ic4_chars[g_bridge.ic4_char_count++] = uuid;
        Bridge_unlock();
        if (uuid == UUID_INDOOR_BIKE_DATA) s_ibd_handle = chr->val_handle;
        if (uuid == UUID_MACHINE_STATUS) s_status_handle = chr->val_handle;
        if (uuid == UUID_TRAINING_STATUS) s_training_handle = chr->val_handle;
        if (uuid == UUID_CONTROL_POINT) s_cp_handle = chr->val_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE) { fail("Characteristic discovery", error->status); return 0; }
    {
        // Characteristics of the FTMS service, in the log (what the IC4 really offers)
        char list[EVENT_TEXT_LEN] = "";
        size_t len = 0;
        for (int i = 0; i < s_chr_count && len + 6 < sizeof(list); i++) {
            len += snprintf(list + len, sizeof(list) - len, " %04X", s_chrs[i].uuid);
        }
        EventLog_add("FTMS chars:%s", list);
    }
    s_sub_step = 0;
    subscribe_next(conn);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == 0) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE) { fail("Service discovery", error->status); return 0; }
    if (s_svc_start == 0) { fail("FTMS service (0x1826)", BLE_HS_ENOENT); return 0; }

    const int rc = ble_gattc_disc_all_chrs(conn, s_svc_start, s_svc_end, on_chr, NULL);
    if (rc != 0) fail("ble_gattc_disc_all_chrs", rc);
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    // MTU in use (23 if the exchange failed or was refused)
    Bridge_lock();
    g_bridge.ic4_mtu = ble_att_mtu(conn);
    Bridge_unlock();
    // Only the FTMS service is discovered: the IC4 heart rate service (0x180D) is never read
    const int rc = ble_gattc_disc_svc_by_uuid(conn, BLE_UUID16_DECLARE(UUID_FTMS), on_svc, NULL);
    if (rc != 0) fail("ble_gattc_disc_svc_by_uuid", rc);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Indoor Bike Data
// ---------------------------------------------------------------------------------------------------------------

static void print_frame(const uint8_t *data, uint16_t len, const ftms_bike_data_t *d, bool ok)
{
    char hex[3 * BRIDGE_RAW_MAX + 1] = "";
    for (int i = 0; i < len && i < BRIDGE_RAW_MAX; i++) snprintf(hex + 3 * i, 4, "%02X ", data[i]);
    printf("IBD [%s] flags 0x%04X%s", hex, d->flags, ok ? "" : " TRUNCATED");
    if (d->speed_present) printf(" | %.2f km/h", d->speed_kmh);
    if (d->cadence_present) printf(" | %.1f rpm", d->cadence_rpm);
    if (d->power_present) printf(" | %d W", d->power_w);
    if (d->resistance_present) printf(" | resistance %d", d->resistance);
    if (d->distance_present) printf(" | %lu m", (unsigned long)d->distance_m);
    if (d->energy_present) printf(" | %u kcal", d->energy_total_kcal);
    if (d->heart_rate_present) printf(" | HR %u (not used)", d->heart_rate_bpm);
    if (d->elapsed_present) printf(" | %u s", d->elapsed_s);
    printf("\n");
}

static void on_indoor_bike_data(struct os_mbuf *om)
{
    uint8_t data[BRIDGE_RAW_MAX * 2];
    const uint16_t len = OS_MBUF_PKTLEN(om) < sizeof(data) ? OS_MBUF_PKTLEN(om) : sizeof(data);
    if (os_mbuf_copydata(om, 0, len, data) != 0) return;

    ftms_bike_data_t d;
    const bool ok = Ftms_decodeIndoorBikeData(data, len, &d);
    const int64_t now = esp_timer_get_time();

    Bridge_lock();
    if (ok) {
        g_bridge.ic4_frames++;
        g_bridge.ic4_last_frame_us = now;
        g_bridge.ic4_data = d;
    } else {
        g_bridge.ic4_bad_frames++;
    }
    g_bridge.ic4_raw_len = len < BRIDGE_RAW_MAX ? len : BRIDGE_RAW_MAX;
    memcpy(g_bridge.ic4_raw, data, g_bridge.ic4_raw_len);
    Bridge_unlock();

    // First frames of each connection and every change of flags: logged with their bytes
    if (s_conn_frames++ < 3 || d.flags != s_last_flags) {
        char hex[EVENT_TEXT_LEN] = "";
        for (int i = 0; i < len && 3 * i + 4 < (int)sizeof(hex) - 4; i++) snprintf(hex + 3 * i, 4, "%02X ", data[i]);
        EventLog_add("IBD %s%s", hex, ok ? "" : "TRUNC");
    }
    s_last_flags = d.flags;

    if (ok) {
        s_had_data = true;
        s_last_data_us = now;
        Cps_onFtmsData(now, d.power_present, d.power_w, d.cadence_present, d.cadence_rpm);
    }
    if (g_bridge_raw_print) print_frame(data, len, &d, ok);
}

// Fitness Machine Status (0x2ADA) or Training Status (0x2AD3): kept and logged (identical ones merged)
static void on_diagnostic(bool status, struct os_mbuf *om)
{
    uint8_t data[BRIDGE_RAW_MAX];
    const uint16_t len = OS_MBUF_PKTLEN(om) < sizeof(data) ? OS_MBUF_PKTLEN(om) : sizeof(data);
    if (os_mbuf_copydata(om, 0, len, data) != 0) return;
    Bridge_lock();
    if (status) {
        memcpy(g_bridge.ic4_status, data, len);
        g_bridge.ic4_status_len = len;
        g_bridge.ic4_status_count++;
    } else {
        memcpy(g_bridge.ic4_training, data, len);
        g_bridge.ic4_training_len = len;
        g_bridge.ic4_training_count++;
    }
    Bridge_unlock();
    char hex[3 * 16 + 1] = "";
    for (int i = 0; i < len && i < 16; i++) snprintf(hex + 3 * i, 4, "%02X ", data[i]);
    EventLog_add("%s %s", status ? "Machine status" : "Training status", hex);
    if (g_bridge_raw_print) printf("%s [%s]\n", status ? "Machine status 2ADA" : "Training status 2AD3", hex);
}

// ---------------------------------------------------------------------------------------------------------------
// GAP events of the scan and of the IC4 connection
// ---------------------------------------------------------------------------------------------------------------

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            if (adv_matches(&event->disc)) ic4_connect(&event->disc);
            return 0;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            // Scan timeout without finding the IC4 (a cancel for the connection does not send this event)
            if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
                Bridge_lock();
                const bool scanning = (g_bridge.ic4_state == LINK_SCANNING);
                Bridge_unlock();
                if (scanning) {
                    seen_log_new();
                    ESP_LOGI(TAG, "IC4 not found");
                    EventLog_add("IC4 not found");
                    schedule_retry();
                }
            }
            return 0;

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                ESP_LOGW(TAG, "Connection failed (%d)", event->connect.status);
                EventLog_add("IC4 connection failed (%d)", event->connect.status);
                schedule_retry();
                return 0;
            }
            s_conn = event->connect.conn_handle;
            s_svc_start = s_svc_end = s_ibd_handle = s_cp_handle = s_cccd_handle = 0;
            s_status_handle = s_training_handle = 0;
            s_chr_count = 0;
            Bridge_lock();
            g_bridge.ic4_char_count = 0;
            Bridge_unlock();
            set_state(LINK_DISCOVERING);
            ESP_LOGI(TAG, "Connected to the IC4 (handle %d)", s_conn);
            if (ble_gattc_exchange_mtu(s_conn, on_mtu, NULL) != 0) {
                on_mtu(s_conn, &(struct ble_gatt_error){ .status = BLE_HS_EUNKNOWN }, 23, NULL);
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "IC4 disconnected (reason 0x%03X)", event->disconnect.reason);
            EventLog_add("IC4 disconnected (reason 0x%03X, %lu frames)", event->disconnect.reason,
                         (unsigned long)s_conn_frames);
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            schedule_retry();
            return 0;

        case BLE_GAP_EVENT_NOTIFY_RX:
            if (event->notify_rx.conn_handle != s_conn) return 0;
            if (event->notify_rx.attr_handle == s_ibd_handle) {
                on_indoor_bike_data(event->notify_rx.om);
            } else if (event->notify_rx.attr_handle == s_status_handle ||
                       event->notify_rx.attr_handle == s_training_handle) {
                on_diagnostic(event->notify_rx.attr_handle == s_status_handle, event->notify_rx.om);
            }
            return 0;   // NimBLE frees the mbuf

        default:
            return 0;
    }
}

// Every second: a connection that does not send data any more (IC4 asleep, link kept open) is closed
static void watch_cb(struct ble_npl_event *ev)
{
    Bridge_lock();
    const bool ready = (g_bridge.ic4_state == LINK_READY);
    Bridge_unlock();
    if (ready && s_conn != BLE_HS_CONN_HANDLE_NONE &&
        esp_timer_get_time() - s_last_data_us > (int64_t)IC4_DATA_LOST_MS * 1000) {
        fail("Waiting for Indoor Bike Data (" STRINGIFY(IC4_DATA_LOST_MS) " ms)", BLE_HS_ETIMEOUT);
        s_last_data_us = esp_timer_get_time();   // One terminate request per timeout
    }
    Ble_calloutStart(&s_watch_timer, 1000);
}

void Ic4_init(void)
{
    ble_npl_callout_init(&s_retry_timer, nimble_port_get_dflt_eventq(), retry_cb, NULL);
    ble_npl_callout_init(&s_watch_timer, nimble_port_get_dflt_eventq(), watch_cb, NULL);
    Ble_calloutStart(&s_watch_timer, 1000);
}

void Ic4_start(void)
{
    scan();
}
