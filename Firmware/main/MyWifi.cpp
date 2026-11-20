#include "MyWifi.h"
#include <stdio.h>
#include <stdlib.h>

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool s_wifi_ready = false;
static volatile bool s_scan_running = false;

// Start the WiFi stack once, in station mode (was WiFi.mode(WIFI_STA))
static esp_err_t MyWifiInit(void)
{
  if (s_wifi_ready) return ESP_OK;

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) return err;

  if ((err = esp_netif_init()) != ESP_OK) return err;
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;   // already created is OK
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  if ((err = esp_wifi_init(&cfg)) != ESP_OK) return err;
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
  if ((err = esp_wifi_start()) != ESP_OK) return err;

  s_wifi_ready = true;
  return ESP_OK;
}

//Create the wifi task
void MyWifiScan(void)
{
  if (s_scan_running) {
    printf("A WiFi scan is already running.\r\n");
    return;
  }
  s_scan_running = true;

  xTaskCreatePinnedToCore(MyWifiScanTask,
                          "Wifi task",
                          6144,     //WiFi init + scan need space
                          NULL,
                          10,
                          NULL,
                          1         // Core ID (1 = App Core, 0 = Protocol Core
                        );
}


void MyWifiScanTask(void * pvParameters)
{

  printf("Scanning WiFi networks...\r\n");

  esp_err_t err = MyWifiInit();
  if (err != ESP_OK) {
    printf("WiFi init failed: %s\r\n> ", esp_err_to_name(err));
    fflush(stdout);
    s_scan_running = false;
    vTaskDelete(NULL);
  }

  esp_wifi_disconnect();     // Make sure we're not connected
  vTaskDelay(pdMS_TO_TICKS(100));

  uint16_t n = 0;
  err = esp_wifi_scan_start(NULL, true);   // blocking scan, all channels
  if (err == ESP_OK) {
    esp_wifi_scan_get_ap_num(&n);
  } else {
    printf("WiFi scan failed: %s\r\n", esp_err_to_name(err));
  }

  if (n == 0) {
    printf("No networks found.\r\n");
  }
  else {
    wifi_ap_record_t *ap = (wifi_ap_record_t *)calloc(n, sizeof(wifi_ap_record_t));
    if (ap == NULL) {
      printf("Not enough memory for %u networks.\r\n", n);
      esp_wifi_clear_ap_list();
    } else {
      esp_wifi_scan_get_ap_records(&n, ap);
      printf("%d networks found:\r\n", n);

      for (int i = 0; i < n; ++i) {
        printf("%d: %s | RSSI: %d dBm | %s\r\n",
          i + 1,
          (const char *)ap[i].ssid,
          ap[i].rssi,
          (ap[i].authmode == WIFI_AUTH_OPEN) ? "Open" : "Secured");

          vTaskDelay(pdMS_TO_TICKS(10));
      }
      free(ap);
    }
  }

  printf("\r\nScan complete.\r\n> ");
  fflush(stdout);
  s_scan_running = false;
  vTaskDelete(NULL);
}
