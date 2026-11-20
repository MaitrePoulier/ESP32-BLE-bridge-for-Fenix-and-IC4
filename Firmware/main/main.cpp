#include <stdio.h>
#include <string.h>
#include "Config.h"
#include "Console.h"
#include "MyWifi.h"
#include "s3.h"
#include "Cellular.h"
#include "Batt.h"

#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// LVGL specific
#include "lvgl.h"
#include "Lvgl_code.h"
#include "demos/lv_demos.h"

// Screen (ST7796S) and touch (XPT2046) drivers
#include "Touch.h"

static lv_display_t * disp;
static lv_indev_t * indev; // LVGL input device handle
static uint8_t *buf1 = nullptr;
static uint8_t *buf2 = nullptr;



/********************* Console **************************************/
// Handlers simples
extern console_t console[13];
void handler_help(char *)      { console_displayHelp(console); }
void handler_history(char *)   { console_displayHistory(); }
void handler_colors(char *)    { console_test();}
void handler_restart(char *)   { esp_restart();}
void handler_detail(char *)    { ESP32_detail();}
void handler_temp(char *)      { printf("Internal temperature: not available on the ESP32 (no temperature sensor driver in ESP-IDF)\r\n");}
//void handler_batt(char *)      { printf("Battery Voltage: %.1fV\r\n",battRead());}
void handler_wifi(char *)      { MyWifiScan();}
void handler_crash(char *)     { ESP32_crash();}
void handler_heap(char *)      { printf("Available heap: %u Bytes\r\n",(unsigned)esp_get_free_heap_size());}
void handler_version(char *)   { ESP32_version();}
void handler_lvgVersion(char *)  { printf("LVGL version %d.%d.%d\r\n",lv_version_major(), lv_version_minor(), lv_version_patch());}
void handler_touch(char *)     { touch_calibrate();}


console_t console[] = {
  {"help",       "Display this menu",                              0, handler_help},
  {"h",          "Display command history",                        0, handler_history},
  {"colors",     "Test the color in the AINSI console",            0, handler_colors},
  {"restart",    "Restart the ESP32-WROOM-32E-N8R2",               0, handler_restart},
  {"detail",     "give detail about the ESP32 uCtrl used",         0, handler_detail},
  {"temp",       "give the internal temperature of the ESP32",     0, handler_temp},
  //{"batt",      "give the voltage of the battery",               0, handler_batt},
  {"wifi",       "scan all wifi SSID and give the strengh of the signal",  0, handler_wifi},
  {"crash",      "List the cause of the last reset",               0, handler_crash},
  {"heap",       "Return the remaining heap",                      0, handler_heap},
  {"version",    "Return the version of the firmware and ESP-IDF", 0, handler_version},
  {"lvglVersion","Return the version of the LVGL library",         0, handler_lvgVersion},
  {"calTouch",   "Calibrate the touch screen",                     0, handler_touch},
  {nullptr, nullptr, 0, nullptr} // Sentinelle
};

//********************************************* */

command_t console_cmd = CMD_noCommand;


static void setup() {
  console_init();   // UART0 at 115200 (was Serial.begin(115200))
  console_displayIntro();

  printf("\r\n> ");

  //*************************
  // Set up the display
  ScreenInit();

  //Use the touch calibration routine to get the correct values for your screen
  uint16_t calData[5] = { 297, 3619, 254, 3622, 5 };
  touch_set_calibration(calData);

  //*********************************
  // Initialize LVGL
  // 1. Calculate buffer size in pixels and bytes (RGB565 = 2 bytes per pixel)
  uint32_t buf_pixel_count = SCREEN_WIDTH * (SCREEN_HEIGHT / 10);
  uint32_t buf_byte_size = buf_pixel_count * sizeof(uint16_t);

  // 2. Allocate dynamically from fast internal DMA memory
  //No real speed difference between malloc and aligned_alloc, but aligned_alloc is more explicit about the alignment requirement for DMA.
  buf1 = (uint8_t *)heap_caps_aligned_alloc(32,buf_byte_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  buf2 = (uint8_t *)heap_caps_aligned_alloc(32,buf_byte_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);


  // 3. Fallback check to ensure allocation succeeded
  if (buf1 == nullptr || buf2 == nullptr) {
      printf("Critical Error: DMA Buffer allocation failed!\r\n");
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

  // Setup Touch Input
  indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);

  //create_test_ui();
  lv_demo_benchmark();

  //**************************

  // Battery ADC: 12-bit resolution, 6 dB attenuation (optimal for ~1.25V - 1.85V input)
  batt_init();

}

static void loop()
{
  /*char buffer[30];
  float VBatt, ChargeBatt;
  VBatt = battRead();
  ChargeBatt = getBatteryPercentage(VBatt);
  sprintf(buffer, "Batt: %.2fV - %.0f%%",VBatt,ChargeBatt);
  game(buffer);*/

  // Let the GUI do its work
  lv_timer_handler();

  int Input_char;
  char argument[10];
  int idx;

  // On récupère le caractère de l'UART
  Input_char = console_read(0);
  if (Input_char != -1) {
      console_cmd = console_getInput((uint8_t)Input_char, argument, console, &idx);

      // Si le parser a trouvé un index valide
      if (idx >= 0) {
          if (console[idx].func != NULL) {
              console[idx].func(argument);
          }
          printf("\r\n> ");
      }
      else if (console_cmd == Error) {
          printf("Unknown command!\r\n> ");
      }
  }

  // picolibc keeps text without '\n' (prompt, echo) in its buffer: send it now
  fflush(stdout);

  // Give 1 ms to the other tasks of this core (idle task frees deleted tasks, e.g. the WiFi scan)
  vTaskDelay(1);
}

// ESP-IDF entry point. Runs on core 1 (CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1), as Arduino loop()
extern "C" void app_main(void)
{
  setup();
  while (true) {
    loop();
  }
}
