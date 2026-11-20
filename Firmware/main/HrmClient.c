// C2: client of the Garmin HRM 600 heart rate strap (central role)
// Scan 0x180D -> filter (saved address, else name; IC4, bridges and watches excluded) -> connect -> Heart Rate
// service -> Heart Rate Measurement (0x2A37) -> its CCCD -> notifications, relayed by the Heart Rate service of the
// bridge. No pairing: in its "open" connection type (recommended by Garmin for third-party equipment) the HRM 600
// sends the heart rate without it. Only if the strap refuses the CCCD write for lack of encryption, "Just Works"
// pairing then the write again. In its "secure" type (encrypted AND authenticated: impossible without a screen or
// keyboard) it refuses the pairing: shown on the screen, the strap must be switched to "open" (button pressed twice).
#define SM_ERR_PEER_PAIRING_NOT_SUPPORTED (BLE_HS_ERR_SM_PEER_BASE + BLE_SM_ERR_PAIR_NOT_SUPP)
// The address of the first strap is saved in NVS; "hrm forget" erases it. Any failure or disconnection: new attempt
// after a backoff of 1 s, 2 s, 4 s... 30 s max. Scans and connection establishments take turns with the IC4 client.
// Everything runs in the NimBLE host task.
#include "HrmClient.h"
#include "Ble.h"
#include "BleServer.h"
#include "Bridge.h"
#include "Config.h"
#include "EventLog.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"

static const char *TAG = "Hrm";

#define UUID_HEART_RATE          0x180D
#define UUID_HR_MEASUREMENT      0x2A37
#define UUID_BATTERY_LEVEL       0x2A19
#define UUID_FTMS                0x1826   // Advertised by the IC4: excluded
#define UUID_CYCLING_POWER       0x1818   // Advertised by a bridge: excluded
#define UUID_CCCD                0x2902
#define APPEARANCE_CATEGORY_WATCH 0x03    // Appearance bits 15..6 (0x00C0-0x00FF): watches, excluded

#define NVS_NAMESPACE            "hrm"
#define NVS_KEY_ADDR             "addr"
#define DISCOVERY_TIMEOUT_MS     30000    // Pairing + discovery + subscription

static const char *const NAME_PREFIXES[] = HRM_NAME_PREFIXES;
#define NAME_PREFIX_COUNT (sizeof(NAME_PREFIXES) / sizeof(NAME_PREFIXES[0]))

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start, s_svc_end, s_hrm_handle, s_cccd_handle;
static uint32_t s_backoff_ms = BLE_BACKOFF_MIN_MS;
static bool s_had_data = false;
static int64_t s_last_data_us;           // Subscription or last value
static int64_t s_connected_us;           // Start of the pairing / discovery
static uint32_t s_conn_values;           // Values of the current connection
static bool s_security_tried;            // This connection: encryption asked after a refused CCCD write
static struct ble_npl_callout s_retry_timer;
static struct ble_npl_callout s_watch_timer;
static struct ble_npl_event s_forget_event;

// Strap saved in NVS (NimBLE task only)
static bool s_saved = false;
static ble_addr_t s_saved_addr;

// Scan candidates: the name and the services may come in the advertising data or in the scan response
#define CANDIDATES 8
static struct {
    ble_addr_t addr;
    char name[20];
    int8_t rssi;
    bool hrs, name_ok, excluded, logged;
} s_cand[CANDIDATES];
static int s_cand_next = 0;

// Heart rate devices already in the event log (once per boot)
#define LOGGED_MAX 8
static ble_addr_t s_logged[LOGGED_MAX];
static int s_logged_count = 0;

static int gap_event(struct ble_gap_event *event, void *arg);
static void scan(void);

static void set_state(link_state_t state)
{
    Bridge_lock();
    g_bridge.hrm_state = state;
    Bridge_unlock();
}

static void schedule_retry(void)
{
    if (s_had_data) s_backoff_ms = BLE_BACKOFF_MIN_MS;
    s_had_data = false;
    const uint32_t delay = s_backoff_ms;
    s_backoff_ms = (s_backoff_ms * 2 > BLE_BACKOFF_MAX_MS) ? BLE_BACKOFF_MAX_MS : s_backoff_ms * 2;

    Bridge_lock();
    g_bridge.hrm_state = LINK_BACKOFF;
    g_bridge.hrm_backoff_ms = delay;
    g_bridge.hrm_retry_at_us = esp_timer_get_time() + (int64_t)delay * 1000;
    Bridge_unlock();
    Ble_calloutStart(&s_retry_timer, delay);
}

static void retry_cb(struct ble_npl_event *ev)
{
    scan();
}

static void fail(const char *step, int status)
{
    ESP_LOGW(TAG, "%s failed (%d): disconnecting", step, status);
    EventLog_add("HRM: %s failed (%d)", step, status);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

// ---------------------------------------------------------------------------------------------------------------
// Saved strap (NVS)
// ---------------------------------------------------------------------------------------------------------------

static void publish_saved(void)
{
    char addr[18] = "";
    if (s_saved) Ble_addrToStr(&s_saved_addr, addr);
    Bridge_lock();
    g_bridge.hrm_saved = s_saved;
    strcpy(g_bridge.hrm_saved_addr, addr);
    Bridge_unlock();
}

static void load_saved(void)
{
    nvs_handle_t h;
    s_saved = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_saved_addr);
        s_saved = (nvs_get_blob(h, NVS_KEY_ADDR, &s_saved_addr, &len) == ESP_OK && len == sizeof(s_saved_addr));
        nvs_close(h);
    }
    publish_saved();
}

static void save(const ble_addr_t *addr)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    const bool ok = nvs_set_blob(h, NVS_KEY_ADDR, addr, sizeof(*addr)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!ok) return;
    s_saved = true;
    s_saved_addr = *addr;
    publish_saved();
    char text[18];
    Ble_addrToStr(addr, text);
    EventLog_add("HRM %s saved: only this strap from now on", text);
}

static void forget_cb(struct ble_npl_event *ev)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_ADDR);
        nvs_commit(h);
        nvs_close(h);
    }
    if (s_saved) ble_store_util_delete_peer(&s_saved_addr);   // Its bond too: the next strap pairs from scratch
    s_saved = false;
    publish_saved();
    EventLog_add("HRM forgotten: next strap named \"%s\"", NAME_PREFIXES[0]);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

void Hrm_forget(void)
{
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_forget_event);
}

// ---------------------------------------------------------------------------------------------------------------
// Scan and connection
// ---------------------------------------------------------------------------------------------------------------

static void scan(void)
{
    // The IC4 client is scanning or connecting: try again shortly (not a failure, no backoff)
    if (!Ble_centralAcquire(BLE_CENTRAL_HRM)) {
        Ble_calloutStart(&s_retry_timer, BLE_CENTRAL_BUSY_RETRY_MS);
        return;
    }
    memset(s_cand, 0, sizeof(s_cand));
    s_cand_next = 0;

    struct ble_gap_disc_params params = {0};
    params.passive = 0;               // Active: the name of the HRM 600 is in its scan response
    params.filter_duplicates = 0;
    const int rc = ble_gap_disc(g_central_addr_type, HRM_SCAN_TIMEOUT_MS, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc: %d", rc);
        Ble_centralRelease(BLE_CENTRAL_HRM);
        schedule_retry();
        return;
    }
    set_state(LINK_SCANNING);
}

static bool name_starts_with(const char *name, const char *prefix)
{
    return strncmp(name, prefix, strlen(prefix)) == 0;
}

// End of a scan: heart rate devices seen for the first time go to the event log (with the reason of the choice)
static void seen_log_new(void)
{
    for (int i = 0; i < CANDIDATES; i++) {
        if (!s_cand[i].hrs) continue;
        bool known = false;
        for (int k = 0; k < s_logged_count; k++) known |= (ble_addr_cmp(&s_logged[k], &s_cand[i].addr) == 0);
        if (known) continue;
        if (s_logged_count < LOGGED_MAX) s_logged[s_logged_count++] = s_cand[i].addr;
        char addr[18];
        Ble_addrToStr(&s_cand[i].addr, addr);
        const char *why = s_cand[i].excluded ? "excluded"
                        : s_saved ? (ble_addr_cmp(&s_saved_addr, &s_cand[i].addr) == 0 ? "saved" : "not the saved one")
                        : s_cand[i].name_ok ? "name ok" : "other name";
        EventLog_add("HR seen \"%s\" %s %d dBm, %s", s_cand[i].name, addr, s_cand[i].rssi, why);
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
    s_cand[i].rssi = disc->rssi;

    if (fields.name != NULL && fields.name_len > 0) {
        const size_t n = fields.name_len < sizeof(s_cand[i].name) - 1 ? fields.name_len : sizeof(s_cand[i].name) - 1;
        for (size_t k = 0; k < n; k++) {   // Name received over the air: printable characters only
            const char c = (char)fields.name[k];
            s_cand[i].name[k] = (c >= 0x20 && c < 0x7F) ? c : '?';
        }
        s_cand[i].name[n] = '\0';
        s_cand[i].name_ok = false;
        for (size_t p = 0; p < NAME_PREFIX_COUNT; p++) s_cand[i].name_ok |= name_starts_with(s_cand[i].name, NAME_PREFIXES[p]);
        // The IC4 and the bridges (this one or another) relay a heart rate: never a source
        if (name_starts_with(s_cand[i].name, IC4_NAME) || name_starts_with(s_cand[i].name, BRIDGE_NAME)) {
            s_cand[i].excluded = true;
        }
    }
    for (int u = 0; u < fields.num_uuids16; u++) {
        const uint16_t uuid = ble_uuid_u16(&fields.uuids16[u].u);
        if (uuid == UUID_HEART_RATE) s_cand[i].hrs = true;
        if (uuid == UUID_FTMS || uuid == UUID_CYCLING_POWER) s_cand[i].excluded = true;
    }
    // A watch broadcasting the heart rate
    if (fields.appearance_is_present && (fields.appearance >> 6) == APPEARANCE_CATEGORY_WATCH) s_cand[i].excluded = true;

    if (s_cand[i].excluded) return false;
    if (s_saved) return ble_addr_cmp(&s_saved_addr, &disc->addr) == 0;
    return s_cand[i].hrs && s_cand[i].name_ok;
}

static void hrm_connect(const struct ble_gap_disc_desc *disc, const char *name)
{
    ble_gap_disc_cancel();
    seen_log_new();

    char addr[18];
    Ble_addrToStr(&disc->addr, addr);
    Bridge_lock();
    strcpy(g_bridge.hrm_addr, addr);
    strlcpy(g_bridge.hrm_name, name, sizeof(g_bridge.hrm_name));
    g_bridge.hrm_rssi = disc->rssi;
    Bridge_unlock();
    ESP_LOGI(TAG, "HRM found: %s \"%s\" (%d dBm), connecting", addr, name, disc->rssi);
    EventLog_add("HRM found \"%s\" %s %d dBm, connecting", name, addr, disc->rssi);

    const int rc = ble_gap_connect(g_central_addr_type, &disc->addr, HRM_CONNECT_TIMEOUT_MS, NULL, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_connect: %d", rc);
        Ble_centralRelease(BLE_CENTRAL_HRM);
        schedule_retry();
        return;
    }
    set_state(LINK_CONNECTING);
}

// ---------------------------------------------------------------------------------------------------------------
// Discovery: encryption -> Heart Rate service -> Heart Rate Measurement -> its CCCD -> notifications on
// ---------------------------------------------------------------------------------------------------------------

static int on_battery(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0 && attr != NULL && OS_MBUF_PKTLEN(attr->om) >= 1) {
        uint8_t level = 0;
        os_mbuf_copydata(attr->om, 0, 1, &level);
        Bridge_lock();
        g_bridge.hrm_battery = (int8_t)(level <= 100 ? level : -1);
        Bridge_unlock();
        EventLog_add("HRM battery %u %%", level);
    }
    return 0;   // BLE_HS_EDONE or an error: nothing more (the battery is only a diagnostic)
}

static const uint8_t NOTIFY_ON[2] = {0x01, 0x00};

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
        error->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC)) {
        // The strap wants an encrypted link: pairing (or the stored bond), then the write again (ENC_CHANGE)
        if (!s_security_tried) {
            s_security_tried = true;
            EventLog_add("HRM wants encryption (%d): pairing", error->status);
            const int rc = ble_gap_security_initiate(conn);
            if (rc != 0 && rc != BLE_HS_EALREADY) fail("ble_gap_security_initiate", rc);
            return 0;
        }
    }
    if (error->status != 0) { fail("Subscription", error->status); return 0; }

    struct ble_gap_conn_desc desc;
    Bridge_lock();
    g_bridge.hrm_state = LINK_READY;
    g_bridge.hrm_connections++;
    g_bridge.hrm_secure_mode = false;
    Bridge_unlock();
    s_last_data_us = esp_timer_get_time();
    s_conn_values = 0;
    ESP_LOGI(TAG, "Subscribed to Heart Rate Measurement");
    EventLog_add("HRM subscribed to the heart rate");

    // First strap: saved, only this one from now on
    if (!s_saved && ble_gap_conn_find(conn, &desc) == 0) save(&desc.peer_id_addr);

    static const ble_uuid16_t battery = BLE_UUID16_INIT(UUID_BATTERY_LEVEL);
    ble_gattc_read_by_uuid(conn, 1, 0xFFFF, &battery.u, on_battery, NULL);
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    if (error->status == 0) {
        // First CCCD after the value: the one of Heart Rate Measurement
        if (s_cccd_handle == 0 && ble_uuid_u16(&dsc->uuid.u) == UUID_CCCD) s_cccd_handle = dsc->handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE) { fail("Descriptor discovery", error->status); return 0; }
    if (s_cccd_handle == 0) { fail("CCCD of the heart rate", BLE_HS_ENOENT); return 0; }
    const int rc = ble_gattc_write_flat(conn, s_cccd_handle, NOTIFY_ON, sizeof(NOTIFY_ON), on_cccd_written, NULL);
    if (rc != 0) fail("CCCD write", rc);
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0) {
        if (ble_uuid_u16(&chr->uuid.u) == UUID_HR_MEASUREMENT) s_hrm_handle = chr->val_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE) { fail("Characteristic discovery", error->status); return 0; }
    if (s_hrm_handle == 0) { fail("Heart Rate Measurement (0x2A37)", BLE_HS_ENOENT); return 0; }
    s_cccd_handle = 0;
    const int rc = ble_gattc_disc_all_dscs(conn, s_hrm_handle, s_svc_end, on_dsc, NULL);
    if (rc != 0) fail("ble_gattc_disc_all_dscs", rc);
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
    if (s_svc_start == 0) { fail("Heart Rate service (0x180D)", BLE_HS_ENOENT); return 0; }
    const int rc = ble_gattc_disc_all_chrs(conn, s_svc_start, s_svc_end, on_chr, NULL);
    if (rc != 0) fail("ble_gattc_disc_all_chrs", rc);
    return 0;
}

static void start_discovery(uint16_t conn)
{
    s_svc_start = s_svc_end = s_hrm_handle = s_cccd_handle = 0;
    const int rc = ble_gattc_disc_svc_by_uuid(conn, BLE_UUID16_DECLARE(UUID_HEART_RATE), on_svc, NULL);
    if (rc != 0) fail("ble_gattc_disc_svc_by_uuid", rc);
}

// ---------------------------------------------------------------------------------------------------------------
// Heart Rate Measurement: flags (uint8), heart rate uint8 (bit0 = 0) or uint16 (bit0 = 1), then optional fields
// ---------------------------------------------------------------------------------------------------------------

static void on_heart_rate(struct os_mbuf *om)
{
    uint8_t d[4] = {0};
    const uint16_t len = OS_MBUF_PKTLEN(om) < sizeof(d) ? OS_MBUF_PKTLEN(om) : sizeof(d);
    if (len < 2 || os_mbuf_copydata(om, 0, len, d) != 0) return;
    const bool uint16_format = d[0] & 0x01;
    if (uint16_format && len < 3) return;
    const unsigned bpm = uint16_format ? (unsigned)(d[1] | (d[2] << 8)) : d[1];
    // Bits 1-2 = 0b10: sensor contact supported but not detected (strap not worn)
    const bool no_contact = ((d[0] >> 1) & 0x03) == 0x02;

    const int64_t now = esp_timer_get_time();
    s_last_data_us = now;   // The strap is sending: the link is alive
    s_conn_values++;
    if (bpm == 0 || bpm > 255 || no_contact) return;

    s_had_data = true;
    Bridge_lock();
    g_bridge.hr_bpm = (uint8_t)bpm;
    g_bridge.hr_last_us = now;
    g_bridge.hrm_frames++;
    Bridge_unlock();
    BleServer_relayHeartRate((uint8_t)bpm);
}

// ---------------------------------------------------------------------------------------------------------------
// GAP events of the scan and of the HRM connection
// ---------------------------------------------------------------------------------------------------------------

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            if (adv_matches(&event->disc)) {
                int i;
                for (i = 0; i < CANDIDATES && ble_addr_cmp(&s_cand[i].addr, &event->disc.addr) != 0; i++) {}
                hrm_connect(&event->disc, i < CANDIDATES ? s_cand[i].name : "");
            }
            return 0;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            Ble_centralRelease(BLE_CENTRAL_HRM);
            if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
                Bridge_lock();
                const bool scanning = (g_bridge.hrm_state == LINK_SCANNING);
                Bridge_unlock();
                if (scanning) {
                    seen_log_new();
                    schedule_retry();
                }
            }
            return 0;

        case BLE_GAP_EVENT_CONNECT:
            Ble_centralRelease(BLE_CENTRAL_HRM);
            if (event->connect.status != 0) {
                ESP_LOGW(TAG, "Connection failed (%d)", event->connect.status);
                EventLog_add("HRM connection failed (%d)", event->connect.status);
                schedule_retry();
                return 0;
            }
            s_conn = event->connect.conn_handle;
            s_connected_us = esp_timer_get_time();
            s_security_tried = false;
            set_state(LINK_DISCOVERING);
            ESP_LOGI(TAG, "Connected to the HRM (handle %d)", s_conn);
            start_discovery(s_conn);
            return 0;

        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.conn_handle != s_conn) return 0;
            if (event->enc_change.status == 0) {
                Bridge_lock();
                g_bridge.hrm_encrypted = true;
                Bridge_unlock();
                EventLog_add("HRM encryption on");
                // Asked after a refused CCCD write: write it again (encryption started by the strap: nothing to do)
                if (s_security_tried && s_cccd_handle != 0) {
                    const int rc = ble_gattc_write_flat(s_conn, s_cccd_handle, NOTIFY_ON, sizeof(NOTIFY_ON),
                                                        on_cccd_written, NULL);
                    if (rc != 0) fail("CCCD write", rc);
                }
            } else if (event->enc_change.status == SM_ERR_PEER_PAIRING_NOT_SUPPORTED) {
                Bridge_lock();
                g_bridge.hrm_secure_mode = true;
                Bridge_unlock();
                fail("Pairing refused: strap in secure mode, press its button twice (open)", event->enc_change.status);
            } else {
                // Typically: the strap lost its bond (reset, paired elsewhere). Ours is deleted: the next connection
                // pairs again from scratch.
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(s_conn, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
                fail("Encryption (bond deleted, pairing again at the next connection)", event->enc_change.status);
            }
            return 0;

        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // The strap pairs again while a bond exists: the old one is deleted and the pairing goes on
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            EventLog_add("HRM pairs again, old bond deleted");
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "HRM disconnected (reason 0x%03X)", event->disconnect.reason);
            EventLog_add("HRM disconnected (reason 0x%03X, %lu values)", event->disconnect.reason,
                         (unsigned long)s_conn_values);
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            Bridge_lock();
            g_bridge.hrm_encrypted = false;
            Bridge_unlock();
            schedule_retry();
            return 0;

        case BLE_GAP_EVENT_NOTIFY_RX:
            if (event->notify_rx.conn_handle == s_conn && event->notify_rx.attr_handle == s_hrm_handle) {
                on_heart_rate(event->notify_rx.om);
            }
            return 0;   // NimBLE frees the mbuf

        default:
            return 0;
    }
}

// Every second: a strap that does not send any more, or a pairing / discovery that does not end, is disconnected
static void watch_cb(struct ble_npl_event *ev)
{
    Bridge_lock();
    const link_state_t state = g_bridge.hrm_state;
    Bridge_unlock();
    const int64_t now = esp_timer_get_time();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        if (state == LINK_READY && now - s_last_data_us > (int64_t)HRM_DATA_LOST_MS * 1000) {
            fail("Waiting for the heart rate", BLE_HS_ETIMEOUT);
            s_last_data_us = now;    // One terminate request per timeout
        } else if (state == LINK_DISCOVERING && now - s_connected_us > (int64_t)DISCOVERY_TIMEOUT_MS * 1000) {
            fail("Pairing / discovery", BLE_HS_ETIMEOUT);
            s_connected_us = now;
        }
    }
    Ble_calloutStart(&s_watch_timer, 1000);
}

void Hrm_init(void)
{
    Bridge_lock();
    g_bridge.hrm_battery = -1;
    Bridge_unlock();
    ble_npl_callout_init(&s_retry_timer, nimble_port_get_dflt_eventq(), retry_cb, NULL);
    ble_npl_callout_init(&s_watch_timer, nimble_port_get_dflt_eventq(), watch_cb, NULL);
    ble_npl_event_init(&s_forget_event, forget_cb, NULL);
    Ble_calloutStart(&s_watch_timer, 1000);
}

void Hrm_start(void)
{
    load_saved();
    scan();
}
