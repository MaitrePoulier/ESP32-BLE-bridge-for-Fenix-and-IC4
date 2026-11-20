// GATT server and advertising of the "IC4 Bridge" (peripheral role)
//   S1 Cycling Power Service (0x1818): measurement 0x2A63 (notify 1 Hz), feature 0x2A65, sensor location 0x2A5D,
//      control point 0x2A66 (the "calibration" of a Garmin watch: offset compensation, always 0)
//   S3 Device Information (0x180A): manufacturer, model, firmware revision
// The crank data (cumulative revolutions, last event time) are synthesised from the FTMS cadence.
#include "BleServer.h"
#include "Ble.h"
#include "Bridge.h"
#include "Config.h"
#include "Crank.h"
#include "EventLog.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BleServer";

// Cycling Power Measurement flags: bit5 = Crank Revolution Data Present
#define CPM_FLAGS_CRANK_DATA     0x0020
// Cycling Power Feature: bit3 = Crank Revolution Data Supported, bit9 = Offset Compensation Supported (control
// point 0x2A66: the watch can "calibrate" the sensor; the IC4 computes the power itself, the offset is always 0)
#define CPS_FEATURE_CRANK_DATA   0x00000008u
#define CPS_FEATURE_OFFSET_COMP  0x00000200u
#define CPS_FEATURES             (CPS_FEATURE_CRANK_DATA | CPS_FEATURE_OFFSET_COMP)

// Cycling Power Control Point
#define CP_OP_START_OFFSET_COMPENSATION 0x0C
#define CP_OP_RESPONSE                  0x20
#define CP_RESULT_SUCCESS               0x01
#define CP_RESULT_NOT_SUPPORTED         0x02
#define ATT_ERR_CCCD_IMPROPERLY_CONFIGURED 0xFD   // Indications not enabled by the client
#define SENSOR_LOCATION_OTHER    0

static uint16_t s_cpm_handle;          // Value handle of 0x2A63
static uint16_t s_cp_handle;           // Value handle of 0x2A66

// ============================================================================
// FTMS data used by the 1 Hz notification (NimBLE task only)
// ============================================================================

static crank_t s_crank = {0};          // Crank data synthesised from the FTMS cadence (Crank.c)
static int16_t s_power_w = 0;
static int64_t s_last_ftms_us = 0;     // 0: never

void Cps_onFtmsData(int64_t now_us, bool power_present, int16_t power_w, bool cadence_present, float cadence_rpm)
{
    if (cadence_present) Crank_setCadence(&s_crank, now_us, cadence_rpm);   // Previous cadence integrated first
    else Crank_advance(&s_crank, now_us);
    s_power_w = power_present ? power_w : 0;
    s_last_ftms_us = now_us;
}

// ============================================================================
// GATT
// ============================================================================

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const uint16_t uuid = ble_uuid_u16(ctxt->chr->uuid);
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;

    int rc = 0;
    switch (uuid) {
        case 0x2A65: {   // Cycling Power Feature (uint32)
            const uint8_t v[4] = {CPS_FEATURES & 0xFF, (CPS_FEATURES >> 8) & 0xFF,
                                  (CPS_FEATURES >> 16) & 0xFF, (CPS_FEATURES >> 24) & 0xFF};
            rc = os_mbuf_append(ctxt->om, v, sizeof(v));
            break;
        }
        case 0x2A5D: {   // Sensor Location
            const uint8_t v = SENSOR_LOCATION_OTHER;
            rc = os_mbuf_append(ctxt->om, &v, 1);
            break;
        }
        case 0x2A29: rc = os_mbuf_append(ctxt->om, BRIDGE_MANUFACTURER, strlen(BRIDGE_MANUFACTURER)); break;
        case 0x2A24: rc = os_mbuf_append(ctxt->om, BRIDGE_MODEL, strlen(BRIDGE_MODEL)); break;
        case 0x2A26: {   // Firmware Revision: git version of the firmware
            const char *version = esp_app_get_description()->version;
            rc = os_mbuf_append(ctxt->om, version, strlen(version));
            break;
        }
        default: return BLE_ATT_ERR_UNLIKELY;
    }
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// Cycling Power Measurement: notify only (no read)
static int cpm_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static int client_index(uint16_t conn);

// Cycling Power Control Point (write + indicate). Start Offset Compensation: success with an offset of 0 (the
// "calibration" a Garmin watch proposes for a new power sensor). Any other op code: not supported.
static int cp_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    Bridge_lock();
    const int i = client_index(conn_handle);
    const bool indicate = (i >= 0) && g_bridge.clients[i].cp_indicate;
    Bridge_unlock();
    if (!indicate) return ATT_ERR_CCCD_IMPROPERLY_CONFIGURED;

    uint8_t op = 0;
    if (OS_MBUF_PKTLEN(ctxt->om) < 1 || os_mbuf_copydata(ctxt->om, 0, 1, &op) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    // Response: 0x20, request op code, result [, parameters]
    uint8_t rsp[5] = {CP_OP_RESPONSE, op, CP_RESULT_NOT_SUPPORTED, 0, 0};
    uint16_t rsp_len = 3;
    if (op == CP_OP_START_OFFSET_COMPENSATION) {
        rsp[2] = CP_RESULT_SUCCESS;
        rsp[3] = 0;          // Raw offset (sint16): 0
        rsp[4] = 0;
        rsp_len = 5;
        Bridge_lock();
        g_bridge.cps_calibrations++;
        Bridge_unlock();
        EventLog_add("Client handle %d: calibration (offset compensation) -> success, offset 0", conn_handle);
    } else {
        EventLog_add("Client handle %d: control point op 0x%02X not supported", conn_handle, op);
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(rsp, rsp_len);
    if (om == NULL) return BLE_ATT_ERR_INSUFFICIENT_RES;
    ble_gatts_indicate_custom(conn_handle, s_cp_handle, om);   // Sent after the write response
    return 0;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1818),          // Cycling Power
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A63), .access_cb = cpm_access, .flags = BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_cpm_handle },
            { .uuid = BLE_UUID16_DECLARE(0x2A65), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A5D), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A66), .access_cb = cp_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_INDICATE, .val_handle = &s_cp_handle },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),          // Device Information
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A24), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A26), .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    { 0 },
};

// ============================================================================
// Clients (centrals connected to the bridge)
// ============================================================================

static int client_index(uint16_t conn)
{
    for (int i = 0; i < g_bridge.client_count; i++) {
        if (g_bridge.clients[i].conn == conn) return i;
    }
    return -1;
}

static void client_add(uint16_t conn)
{
    struct ble_gap_conn_desc desc;
    char addr[18] = "?";
    if (ble_gap_conn_find(conn, &desc) == 0) Ble_addrToStr(&desc.peer_id_addr, addr);

    Bridge_lock();
    if (client_index(conn) < 0 && g_bridge.client_count < BRIDGE_MAX_CLIENTS) {
        int i = g_bridge.client_count++;
        g_bridge.clients[i].conn = conn;
        strcpy(g_bridge.clients[i].addr, addr);
        g_bridge.clients[i].cps_notify = false;
        g_bridge.clients[i].cp_indicate = false;
        g_bridge.clients[i].encrypted = false;
    }
    Bridge_unlock();
    ESP_LOGI(TAG, "Client %s connected (handle %d)", addr, conn);
    EventLog_add("Client %s connected", addr);
}

static void client_remove(uint16_t conn)
{
    Bridge_lock();
    int i = client_index(conn);
    if (i >= 0) {
        for (; i < g_bridge.client_count - 1; i++) g_bridge.clients[i] = g_bridge.clients[i + 1];
        g_bridge.client_count--;
    }
    Bridge_unlock();
}

// ============================================================================
// Advertising: "IC4 Bridge", appearance 0x0484, service 0x1818
// ============================================================================

static int gap_event(struct ble_gap_event *event, void *arg);

void BleServer_startAdvertising(void)
{
    if (ble_gap_adv_active()) return;
    Bridge_lock();
    const bool full = (g_bridge.client_count >= BRIDGE_MAX_CLIENTS);
    Bridge_unlock();
    if (full) return;   // Restarted at the next disconnection

    static const ble_uuid16_t uuids[] = { BLE_UUID16_INIT(0x1818) };
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)BRIDGE_NAME;
    fields.name_len = strlen(BRIDGE_NAME);
    fields.name_is_complete = 1;
    fields.appearance = BRIDGE_APPEARANCE;
    fields.appearance_is_present = 1;
    fields.uuids16 = uuids;
    fields.num_uuids16 = sizeof(uuids) / sizeof(uuids[0]);
    fields.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields: %d", rc);
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start: %d", rc);
        return;
    }
    Bridge_lock();
    g_bridge.advertising = true;
    Bridge_unlock();
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            Bridge_lock();
            g_bridge.advertising = false;   // A connection stops the advertising
            Bridge_unlock();
            if (event->connect.status == 0) client_add(event->connect.conn_handle);
            BleServer_startAdvertising();   // Keep advertising for the other clients
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Client disconnected (handle %d, reason 0x%02X)", event->disconnect.conn.conn_handle,
                     event->disconnect.reason);
            {
                char addr[18];
                Ble_addrToStr(&event->disconnect.conn.peer_id_addr, addr);
                EventLog_add("Client %s disconnected (reason 0x%03X)", addr, event->disconnect.reason);
            }
            client_remove(event->disconnect.conn.conn_handle);
            BleServer_startAdvertising();
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            Bridge_lock();
            g_bridge.advertising = false;
            Bridge_unlock();
            BleServer_startAdvertising();
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == s_cpm_handle) {
                Bridge_lock();
                const int i = client_index(event->subscribe.conn_handle);
                if (i >= 0) g_bridge.clients[i].cps_notify = event->subscribe.cur_notify;
                Bridge_unlock();
                ESP_LOGI(TAG, "Client handle %d: power notifications %s", event->subscribe.conn_handle,
                         event->subscribe.cur_notify ? "on" : "off");
                EventLog_add("Client handle %d: power notifications %s", event->subscribe.conn_handle,
                             event->subscribe.cur_notify ? "on" : "off");
            } else if (event->subscribe.attr_handle == s_cp_handle) {
                Bridge_lock();
                const int i = client_index(event->subscribe.conn_handle);
                if (i >= 0) g_bridge.clients[i].cp_indicate = event->subscribe.cur_indicate;
                Bridge_unlock();
            }
            return 0;

        case BLE_GAP_EVENT_ENC_CHANGE: {
            Bridge_lock();
            const int i = client_index(event->enc_change.conn_handle);
            if (i >= 0) g_bridge.clients[i].encrypted = (event->enc_change.status == 0);
            Bridge_unlock();
            return 0;
        }

        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // The client lost its bond and pairs again: delete the old bond and accept
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "Client handle %d: MTU %d", event->mtu.conn_handle, event->mtu.value);
            return 0;

        default:
            return 0;
    }
}

// ============================================================================
// 1 Hz Cycling Power Measurement
// ============================================================================

static struct ble_npl_callout s_notify_timer;

static void cps_notify_tick(struct ble_npl_event *ev)
{
    const int64_t now = esp_timer_get_time();
    const bool fresh = (s_last_ftms_us != 0) && (now - s_last_ftms_us <= (int64_t)FTMS_TIMEOUT_MS * 1000);

    if (fresh) {
        Crank_advance(&s_crank, now);
    } else {
        // No FTMS data any more: integrated up to the timeout, then the crank data stay frozen
        Crank_freeze(&s_crank, s_last_ftms_us + (int64_t)FTMS_TIMEOUT_MS * 1000, now);
    }
    const int16_t power = fresh ? s_power_w : 0;

    // flags (uint16), instantaneous power (sint16), cumulative crank revolutions (uint16),
    // last crank event time (uint16, 1/1024 s): little-endian
    const uint8_t pkt[8] = {
        CPM_FLAGS_CRANK_DATA & 0xFF, CPM_FLAGS_CRANK_DATA >> 8,
        (uint8_t)(power & 0xFF), (uint8_t)((uint16_t)power >> 8),
        (uint8_t)(s_crank.revs & 0xFF), (uint8_t)(s_crank.revs >> 8),
        (uint8_t)(s_crank.time & 0xFF), (uint8_t)(s_crank.time >> 8),
    };

    uint16_t subscribed[BRIDGE_MAX_CLIENTS];
    int n = 0;
    Bridge_lock();
    for (int i = 0; i < g_bridge.client_count; i++) {
        if (g_bridge.clients[i].cps_notify) subscribed[n++] = g_bridge.clients[i].conn;
    }
    Bridge_unlock();

    uint32_t sent = 0;
    for (int i = 0; i < n; i++) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(pkt, sizeof(pkt));
        if (om != NULL && ble_gatts_notify_custom(subscribed[i], s_cpm_handle, om) == 0) sent++;
    }

    Bridge_lock();
    g_bridge.cps_fresh = fresh;
    g_bridge.cps_power_w = power;
    g_bridge.cps_cadence_rpm = fresh ? s_crank.rpm : 0;
    g_bridge.cps_crank_revs = s_crank.revs;
    g_bridge.cps_crank_time = s_crank.time;
    g_bridge.cps_notifications += sent;
    Bridge_unlock();

    Ble_calloutStart(&s_notify_timer, CPS_NOTIFY_PERIOD_MS);
}

int BleServer_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(BRIDGE_NAME);
    ble_svc_gap_device_appearance_set(BRIDGE_APPEARANCE);

    int rc = ble_gatts_count_cfg(s_services);
    if (rc == 0) rc = ble_gatts_add_svcs(s_services);
    if (rc != 0) return rc;

    ble_npl_callout_init(&s_notify_timer, nimble_port_get_dflt_eventq(), cps_notify_tick, NULL);
    Ble_calloutStart(&s_notify_timer, CPS_NOTIFY_PERIOD_MS);
    return 0;
}
