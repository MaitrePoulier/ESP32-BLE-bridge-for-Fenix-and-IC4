// WiFi only for the time sync (SNTP) and the console (wifi list|scan|connect|forget)
// Port of esp32-mezz-jauges/Firmware/main/MyWifi.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "Config.h"
#include "MyWifi.h"
#include "Console.h"   // wifi list and wifi scan display

static const char *TAG = "MyWifi";

// The WiFi driver only exists for the duration of one operation (time sync at boot, wifi connect, wifi scan):
// wifi_driver_up() (esp_wifi_init + network interface) ... wifi_driver_down() (esp_wifi_stop + esp_wifi_deinit).
// esp_wifi_init takes about 45 KB of internal SRAM, given back to LVGL afterwards.
static constexpr int WIFI_MAX_RETRY = 5;   // Reconnection attempts during a sync
static constexpr uint32_t WIFI_SYNC_TIMEOUT_MS = 30000;
static int s_retry = 0;
static bool s_no_reconnect = true;         // No automatic connection (scan, radio stopped)
static SemaphoreHandle_t s_wifi_lock = nullptr; // One WiFi operation at a time (boot / console)
static esp_netif_t *s_sta_netif = nullptr;       // Station interface, only between wifi_driver_up and _down

// List of the saved networks, kept in NVS (namespace "wifi_list").
// The WiFi driver keeps only one network: it is switched to RAM storage, this list is the reference.
// NOTE: NVS is not encrypted (CONFIG_NVS_ENCRYPTION disabled): passwords are stored in clear in flash.
static constexpr int WIFI_MAX_NETWORKS = 10;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;   // Per network tried
static constexpr uint16_t WIFI_SCAN_MAX_RECORDS = 32;
static constexpr const char *WIFI_NVS_NAMESPACE = "wifi_list";

struct WifiNetwork {
    char ssid[33];       // 32 characters max + '\0'
    char password[65];   // 64 characters max (hexadecimal key) + '\0'
};
static WifiNetwork s_networks[WIFI_MAX_NETWORKS] = {};
static int s_network_count = 0;

static EventGroupHandle_t s_wifi_events = nullptr;
static constexpr EventBits_t WIFI_GOT_IP_BIT = BIT0;
static constexpr EventBits_t WIFI_FAIL_BIT   = BIT1;


static void networks_load(void)
{
    s_network_count = 0;
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;   // Nothing saved
    uint8_t count = 0;
    size_t size = sizeof(s_networks);
    if (nvs_get_u8(nvs, "count", &count) == ESP_OK && count <= WIFI_MAX_NETWORKS &&
        nvs_get_blob(nvs, "list", s_networks, &size) == ESP_OK && size == count * sizeof(WifiNetwork)) {
        s_network_count = count;
    }
    nvs_close(nvs);
}

static esp_err_t networks_save(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, "count", (uint8_t)s_network_count);
    if (err == ESP_OK) err = nvs_set_blob(nvs, "list", s_networks, s_network_count * sizeof(WifiNetwork));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) ESP_LOGE(TAG, "Saving the WiFi networks in NVS: %s", esp_err_to_name(err));
    return err;
}

static int networks_find(const char *ssid)
{
    for (int i = 0; i < s_network_count; i++) {
        if (strcmp(s_networks[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Nothing: the connection is started explicitly by wifi_sync() after the scan
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_no_reconnect) return;
        const wifi_event_sta_disconnected_t *event = static_cast<const wifi_event_sta_disconnected_t *>(data);
        if (++s_retry <= WIFI_MAX_RETRY) {
            ESP_LOGW(TAG, "WiFi disconnected (reason %d), attempt %d/%d", event->reason, s_retry, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "WiFi connection failed (reason %d)", event->reason);
            s_no_reconnect = true;
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = static_cast<const ip_event_got_ip_t *>(data);
        s_retry = 0;
        ESP_LOGI(TAG, "WiFi connected, IP " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_GOT_IP_BIT);
        esp_netif_sntp_start();   // SNTP request as soon as the network is available
    }
}

static void time_sync_cb(struct timeval *tv)
{
    const time_t now = tv->tv_sec;
    struct tm local;
    localtime_r(&now, &local);
    char text[32];
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    ESP_LOGI(TAG, "Time synchronised (SNTP): %s", text);
}


void MyWifiInit(void)
{
    ESP_LOGI(TAG, "Initializing WiFi...");

    // NVS: initialised at the start of app_main

    // Time of day: local time zone, even without WiFi
    setenv("TZ", TIMEZONE, 1);
    tzset();

    // A failure leaves the WiFi disabled (s_wifi_lock stays null: every operation is refused) instead of restarting
    // the board. WiFi/IP events: event loop, independent of the driver initialisation. SNTP is started when the IP is
    // obtained (see wifi_event_handler).
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);
    sntp_config.start = false;
    sntp_config.sync_cb = time_sync_cb;
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) err = esp_event_loop_create_default();
    if (err == ESP_OK) err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr);
    if (err == ESP_OK) err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr);
    if (err == ESP_OK) err = esp_netif_sntp_init(&sntp_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi initialisation failed (%s): WiFi disabled", esp_err_to_name(err));
        return;
    }

    networks_load();
    ESP_LOGI(TAG, "%d saved WiFi network(s)", s_network_count);

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == nullptr) return;
    s_wifi_lock = xSemaphoreCreateMutex();
    if (s_wifi_lock == nullptr) return;
    ESP_LOGI(TAG, "WiFi initialized");

    // Time of day at boot, then the WiFi driver is released
    MyWifiSyncTime();
}


// WiFi driver + station interface. Called with s_wifi_lock taken.
static esp_err_t wifi_driver_up(void)
{
    s_sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s (free internal SRAM %u bytes, largest block %u)",
                 esp_err_to_name(err), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = nullptr;
        return err;
    }

    // The driver does not keep its configuration in NVS: the s_networks list is the reference
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = nullptr;
    }
    return err;
}

// Radio stopped, driver released (gives its internal SRAM back). Called with s_wifi_lock taken.
static void wifi_driver_down(void)
{
    if (s_sta_netif == nullptr) return;
    s_no_reconnect = true;
    // Clean disconnection first: deinit while still associated times out ("timeout when WiFi un-init") with the
    // BLE coexistence running
    if (esp_wifi_disconnect() == ESP_OK) vTaskDelay(pdMS_TO_TICKS(200));
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_netif_destroy_default_wifi(s_sta_netif);
    s_sta_netif = nullptr;
    ESP_LOGI(TAG, "WiFi driver released (free internal SRAM %u bytes, largest block %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}


// Scan (radio already started). Returns the number of records copied into records.
static uint16_t wifi_scan(wifi_ap_record_t *records, uint16_t max_records)
{
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = true;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return 0;
    }
    uint16_t n = max_records;
    if (esp_wifi_scan_get_ap_records(&n, records) != ESP_OK) n = 0;   // Also frees the driver's list
    return n;
}

// Radio on -> scan -> saved networks in range, strongest first (preferred first) -> SNTP -> radio off.
// Called with s_wifi_lock taken.
static esp_err_t wifi_sync(int preferred)
{
    if (s_network_count == 0) {
        ESP_LOGW(TAG, "No saved WiFi network: use the command wifi connect <ssid> <password>");
        return ESP_ERR_NOT_FOUND;
    }

    s_no_reconnect = true;
    esp_err_t err = wifi_driver_up();
    if (err != ESP_OK) return err;

    // Best RSSI of each saved network (-128 = not seen)
    int rssi[WIFI_MAX_NETWORKS];
    for (int i = 0; i < WIFI_MAX_NETWORKS; i++) rssi[i] = -128;
    wifi_ap_record_t *records = static_cast<wifi_ap_record_t *>(calloc(WIFI_SCAN_MAX_RECORDS, sizeof(wifi_ap_record_t)));
    if (records != nullptr) {
        const uint16_t n = wifi_scan(records, WIFI_SCAN_MAX_RECORDS);
        for (uint16_t r = 0; r < n; r++) {
            const int k = networks_find(reinterpret_cast<const char *>(records[r].ssid));
            if (k >= 0 && records[r].rssi > rssi[k]) rssi[k] = records[r].rssi;
        }
        free(records);
    }

    // Candidates: networks seen, by decreasing RSSI; the "preferred" network comes first if it is seen.
    // NOTE: a hidden network (SSID not broadcast) is never "seen", so it is never tried.
    int order[WIFI_MAX_NETWORKS];
    int count = 0;
    for (int i = 0; i < s_network_count; i++) {
        if (rssi[i] > -128) order[count++] = i;
    }
    for (int a = 0; a < count; a++) {
        for (int b = a + 1; b < count; b++) {
            const bool b_first = (order[b] == preferred) ||
                                 (order[a] != preferred && rssi[order[b]] > rssi[order[a]]);
            if (b_first) { const int t = order[a]; order[a] = order[b]; order[b] = t; }
        }
    }

    err = ESP_ERR_NOT_FOUND;
    if (count == 0) {
        ESP_LOGW(TAG, "No saved network in range (%d saved, see wifi list)", s_network_count);
    }
    for (int c = 0; c < count; c++) {
        const WifiNetwork &net = s_networks[order[c]];
        wifi_config_t conf = {};
        strlcpy(reinterpret_cast<char *>(conf.sta.ssid), net.ssid, sizeof(conf.sta.ssid));
        // 64 bytes without '\0' are allowed (hexadecimal key): exact-length copy
        memcpy(conf.sta.password, net.password, strnlen(net.password, sizeof(conf.sta.password)));
        // Network saved with a password: WPA2 at least, so that a fake open access point with the same name is
        // refused (WPA3 networks are accepted)
        if (net.password[0] != '\0') conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        esp_wifi_set_config(WIFI_IF_STA, &conf);

        char shown[33];
        ESP_LOGI(TAG, "Connecting to \"%s\" (%d dBm)...", console_safe_text(shown, sizeof(shown), net.ssid),
                 rssi[order[c]]);
        esp_netif_sntp_sync_wait(0);   // Clears a sync signal left by a previous session
        xEventGroupClearBits(s_wifi_events, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT);
        s_retry = 0;
        s_no_reconnect = false;
        esp_wifi_connect();
        const EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT,
                                                     pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
        if (bits & WIFI_GOT_IP_BIT) {
            err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(WIFI_SYNC_TIMEOUT_MS));
            break;
        }
        ESP_LOGW(TAG, "Failed with \"%s\", next network", shown);
        s_no_reconnect = true;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));   // Lets the DISCONNECTED event pass before the next network
        err = ESP_FAIL;
    }

    wifi_driver_down();
    if (err != ESP_OK) ESP_LOGW(TAG, "Time not synchronised (%s)", esp_err_to_name(err));
    return err;
}


esp_err_t MyWifiSyncTime(void)
{
    if (s_wifi_lock == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    const esp_err_t err = wifi_sync(-1);
    xSemaphoreGive(s_wifi_lock);
    return err;
}


esp_err_t MyWifiConnect(const char *ssid, const char *password)
{
    if (s_wifi_lock == nullptr) return ESP_ERR_INVALID_STATE;
    if (ssid == nullptr || ssid[0] == '\0' || strlen(ssid) > 32 || strlen(password) > 64) return ESP_ERR_INVALID_ARG;

    // Adds the network (or updates its password), saves it, then tries it first
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    int index = networks_find(ssid);
    if (index < 0) {
        if (s_network_count >= WIFI_MAX_NETWORKS) {
            xSemaphoreGive(s_wifi_lock);
            ESP_LOGE(TAG, "List full (%d networks): delete one with wifi forget", WIFI_MAX_NETWORKS);
            return ESP_ERR_NO_MEM;
        }
        index = s_network_count++;
        strlcpy(s_networks[index].ssid, ssid, sizeof(s_networks[index].ssid));
    }
    strlcpy(s_networks[index].password, password, sizeof(s_networks[index].password));
    esp_err_t err = networks_save();
    if (err == ESP_OK) err = wifi_sync(index);
    xSemaphoreGive(s_wifi_lock);
    return err;
}


esp_err_t MyWifiForget(const char *ssid_or_number)
{
    if (s_wifi_lock == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    int index = networks_find(ssid_or_number);
    if (index < 0) {
        char *end = nullptr;
        const long number = strtol(ssid_or_number, &end, 10);   // Number shown by wifi list (1..n)
        if (end != ssid_or_number && *end == '\0' && number >= 1 && number <= s_network_count) index = (int)number - 1;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (index >= 0) {
        char ssid[33];
        printf("Network \"%s\" deleted\n", console_safe_text(ssid, sizeof(ssid), s_networks[index].ssid));
        for (int i = index; i < s_network_count - 1; i++) s_networks[i] = s_networks[i + 1];
        s_network_count--;
        memset(&s_networks[s_network_count], 0, sizeof(WifiNetwork));
        err = networks_save();
    }
    xSemaphoreGive(s_wifi_lock);
    return err;
}


void MyWifiList(void)
{
    if (s_wifi_lock == nullptr) {
        printf("WiFi not initialised\n");
        return;
    }
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    char title[48];
    snprintf(title, sizeof(title), "Saved WiFi networks (%d/%d)", s_network_count, WIFI_MAX_NETWORKS);
    console_section(title);
    if (s_network_count == 0) console_row("--", "none ('wifi connect <ssid> [password]' to add one)");
    for (int i = 0; i < s_network_count; i++) {
        char number[8];
        snprintf(number, sizeof(number), "%d", i + 1);
        char ssid[33];
        console_safe_text(ssid, sizeof(ssid), s_networks[i].ssid);
        console_row(number, "%s (%s)", ssid, s_networks[i].password[0] ? "password" : "open");
    }
    printf("\n");
    xSemaphoreGive(s_wifi_lock);
}


static void MyWifiScanList(void)
{
    // Every network on every channel, hidden ones included
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = true;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;

    // Starts the scan and waits for it to complete
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t number = 0;
    err = esp_wifi_scan_get_ap_num(&number);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP count: %s", esp_err_to_name(err));
        return;
    }
    if (number == 0) {
        printf("No networks found.\n");
        return;
    }

    wifi_ap_record_t *records = static_cast<wifi_ap_record_t *>(calloc(number, sizeof(wifi_ap_record_t)));
    if (records == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for AP records");
        esp_wifi_clear_ap_list();
        return;
    }

    uint16_t max_records = number;
    err = esp_wifi_scan_get_ap_records(&max_records, records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP records: %s", esp_err_to_name(err));
        free(records);
        return;
    }

    // Results, strongest first (order given by the driver)
    char title[40];
    snprintf(title, sizeof(title), "WiFi networks in range (%u)", (unsigned)max_records);
    console_section(title);
    for (uint16_t i = 0; i < max_records; i++) {
        // SSID received over the air: control characters (terminal escape sequences) replaced before printing
        const char *ssid = reinterpret_cast<const char *>(records[i].ssid);
        char shown[33];
        console_safe_text(shown, sizeof(shown), ssid);
        console_row(ssid[0] ? shown : "(hidden)", "%4d dBm  %s%s", records[i].rssi,
                    records[i].authmode == WIFI_AUTH_OPEN ? "open   " : "secured",
                    networks_find(ssid) >= 0 ? "  saved" : "");
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("\n");

    free(records);
}


void MyWifiScan(void)
{
    if (s_wifi_lock == nullptr) {
        printf("WiFi not initialised\n");
        return;
    }

    printf("Scanning WiFi networks...\n");

    // Radio on only during the scan, without connecting (s_no_reconnect stays true)
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    if (wifi_driver_up() == ESP_OK) {
        MyWifiScanList();
        wifi_driver_down();
    }
    xSemaphoreGive(s_wifi_lock);
}
