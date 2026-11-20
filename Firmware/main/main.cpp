#include <stdio.h>
#include <string.h>
#include "Config.h"
#include "Console.h"
#include "MyWifi.h"
#include "Cellular.h"
#include "Batt.h"

#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// LVGL specific
#include "lvgl.h"
#include "Lvgl_code.h"

// Screen (ST7796S) and touch (XPT2046) drivers
#include "Touch.h"

// IC4 Bridge
#include "Ble.h"
#include "BridgeScreen.h"
#include "EventLog.h"

static lv_display_t * disp;
static lv_indev_t * indev; // LVGL input device handle
static uint8_t *buf1 = nullptr;
static uint8_t *buf2 = nullptr;


static void setup() {
  // NVS: saved WiFi networks, BLE bonds, PHY calibration
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Event log of the bridge, kept in NVS (console: bridge log, screen: details page)
  EventLog_init();

  //*************************
  // Set up the display
  ScreenInit();

  //Use the touch calibration routine (console: touch cal) to get the correct values for your screen
  uint16_t calData[5] = { 297, 3619, 254, 3622, 5 };
  touch_set_calibration(calData);

  //*********************************
  // Initialize LVGL
  // 1. Calculate buffer size in pixels and bytes (RGB565 = 2 bytes per pixel)
  uint32_t buf_pixel_count = SCREEN_WIDTH * (SCREEN_HEIGHT / 10);
  uint32_t buf_byte_size = buf_pixel_count * sizeof(uint16_t);

  // 2. Allocate dynamically from fast internal DMA memory (before the WiFi, which needs internal SRAM too)
  //No real speed difference between malloc and aligned_alloc, but aligned_alloc is more explicit about the alignment requirement for DMA.
  buf1 = (uint8_t *)heap_caps_aligned_alloc(32,buf_byte_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  buf2 = (uint8_t *)heap_caps_aligned_alloc(32,buf_byte_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);


  // 3. Fallback check to ensure allocation succeeded
  if (buf1 == nullptr || buf2 == nullptr) {
      printf("Critical Error: DMA Buffer allocation failed!\n");
      while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
  }

  lv_init();

  lv_tick_set_cb(my_tick);

  // Create display
  disp = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
  // Render directly in the byte order of the screen (big-endian RGB565)
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
  lv_display_set_flush_cb(disp, my_flush_cb);

  lv_display_set_buffers(disp, buf1, buf2, buf_byte_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

  // FPS / CPU label of the performance monitor hidden: the measurements keep running for the console (ui perf)
  lv_sysmon_hide_performance(disp);

  // Setup Touch Input
  indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);

  // Diagnostic screen of the IC4 Bridge; the LVGL benchmark is started by the console (ui benchmark)
  BridgeScreen_create();

  //**************************

  // Battery ADC: 12-bit resolution, 6 dB attenuation (optimal for ~1.25V - 1.85V input)
  batt_init();

  // IC4 Bridge: NimBLE host task (core 0), GATT server and IC4 client start by themselves
  Ble_init();

  // Console prepared (commands registered), opened by console_start at the end of the initialisation
  Gui_initRequests();
  console_init();

  // End of the initialisation, on core 0 (the LVGL loop keeps running on core 1 meanwhile):
  // WiFi + SNTP time (radio on only for the sync, driver released afterwards), then console banner and prompt
  xTaskCreatePinnedToCore([](void *) {
      MyWifiInit();
      console_start();
      vTaskDelete(NULL);
  }, "init_end", 6144, NULL, 5, NULL, 0);
}

static void loop()
{
  /*char buffer[30];
  float VBatt, ChargeBatt;
  VBatt = battRead();
  ChargeBatt = getBatteryPercentage(VBatt);
  sprintf(buffer, "Batt: %.2fV - %.0f%%",VBatt,ChargeBatt);
  game(buffer);*/

  // Console requests (touch cal, touch test, ui benchmark, ui perf): LVGL is only used from this loop
  Gui_processRequests();

  // Let the GUI do its work
  lv_timer_handler();

  // picolibc keeps text without '\n' in its buffer: send it now
  fflush(stdout);

  // Task watchdog (CONFIG_ESP_TASK_WDT_TIMEOUT_S): restarts the board if this loop stops
  esp_task_wdt_reset();

  // Give 1 ms to the other tasks of this core
  vTaskDelay(1);
}

// ESP-IDF entry point. Runs on core 1 (CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1), as Arduino loop()
extern "C" void app_main(void)
{
  setup();
  esp_task_wdt_add(NULL);   // The LVGL loop is watched by the task watchdog
  while (true) {
    loop();
  }
}
