// NimBLE initialisation: peripheral (GATT server, advertising) and central (IC4 client) roles
#include "Ble.h"
#include "BleServer.h"
#include "Ic4Client.h"

#include <stdio.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "store/config/ble_store_config.h"

static const char *TAG = "Ble";

uint8_t g_own_addr_type = BLE_OWN_ADDR_PUBLIC;

void ble_store_config_init(void);   // NimBLE: bonds kept in NVS (CONFIG_BT_NIMBLE_NVS_PERSIST)

void Ble_addrToStr(const ble_addr_t *addr, char out[18])
{
    const uint8_t *v = addr->val;
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", v[5], v[4], v[3], v[2], v[1], v[0]);
}

void Ble_calloutStart(struct ble_npl_callout *co, uint32_t ms)
{
    ble_npl_callout_reset(co, ble_npl_time_ms_to_ticks32(ms));
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset, reason %d", reason);
}

// Host and controller synchronised: the roles can start
static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "No usable BLE address (%d)", rc);
        return;
    }
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(g_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE address %02X:%02X:%02X:%02X:%02X:%02X", addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    BleServer_startAdvertising();   // "IC4 Bridge" (CPS)
    Ic4_start();                    // Scan for the "IC Bike"
}

static void host_task(void *param)
{
    nimble_port_run();              // Returns only when nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

void Ble_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // No mandatory security; "Just Works" bonding accepted if a client asks for it
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    if (BleServer_init() != 0) {
        ESP_LOGE(TAG, "GATT server initialisation failed");
        return;
    }
    Ic4_init();

    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}
