#include "s3.h"
#include <stdio.h>

#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_efuse.h"
#include "esp_clk_tree.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "lvgl.h"
#if CONFIG_IDF_TARGET_ESP32
#include "soc/efuse_defs.h"
#endif

// Same names as Arduino ESP.getChipModel()
static const char *chip_model(void)
{
#if CONFIG_IDF_TARGET_ESP32
    esp_chip_info_t info;
    esp_chip_info(&info);
    bool v3 = info.revision >= 300;

    switch (esp_efuse_get_pkg_ver()) {
        case EFUSE_RD_CHIP_VER_PKG_ESP32D0WDQ6:   return v3 ? "ESP32-D0WDQ6-V3" : "ESP32-D0WDQ6";
        case EFUSE_RD_CHIP_VER_PKG_ESP32D0WDQ5:   return v3 ? "ESP32-D0WD-V3" : "ESP32-D0WD";
        case EFUSE_RD_CHIP_VER_PKG_ESP32D2WDQ5:   return "ESP32-D2WD";
        case EFUSE_RD_CHIP_VER_PKG_ESP32U4WDH:    return "ESP32-U4WDH";
        case EFUSE_RD_CHIP_VER_PKG_ESP32PICOD4:   return v3 ? "ESP32-PICO-V3" : "ESP32-PICO-D4";
        case EFUSE_RD_CHIP_VER_PKG_ESP32PICOV302: return "ESP32-PICO-V3-02";
        case EFUSE_RD_CHIP_VER_PKG_ESP32D0WDR2V3: return "ESP32-D0WDR2-V3";
        default:                                  return "ESP32 (unknown package)";
    }
#else
    return CONFIG_IDF_TARGET;
#endif
}

void ESP32_detail(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    uint32_t cpu_hz = 0;
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &cpu_hz);

    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);

    printf("Chip Model:    %s\r\n", chip_model());
    printf("Chip Revision: v%d.%d\r\n", info.revision / 100, info.revision % 100);
    printf("Chip Cores:    %d\r\n", info.cores);
    printf("Chip Frequency:%lu MHz\r\n", (unsigned long)(cpu_hz / 1000000));
    printf("Fuse Mac:      %02X:%02X:%02X:%02X:%02X:%02X\r\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}


void ESP32_version(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    printf("Firmware:        %s %s (%s %s)\r\n", app->project_name, app->version, app->date, app->time);
    printf("ESP-IDF version: %s\r\n", esp_get_idf_version());
    printf("LVGL version:    %d.%d.%d\r\n", lv_version_major(), lv_version_minor(), lv_version_patch());
}

void ESP32_crash(void)
{
  printf("Reset reason: ");

  switch(esp_reset_reason()) {
    case ESP_RST_UNKNOWN:
      printf("Unknown\r\n");
      break;
    case ESP_RST_POWERON:
      printf("Power-on\r\n");
      break;
    case ESP_RST_EXT:
      printf("External reset\r\n");
      break;
    case ESP_RST_SW:
      printf("Software reset\r\n");
      break;
    case ESP_RST_PANIC:
      printf("Exception/panic\r\n");
      break;
    case ESP_RST_INT_WDT:
      printf("Interrupt watchdog\r\n");
      break;
    case ESP_RST_TASK_WDT:
      printf("Task watchdog\r\n");
      break;
    case ESP_RST_WDT:
      printf("Other watchdog\r\n");
      break;
    case ESP_RST_DEEPSLEEP:
      printf("Wake from deep sleep\r\n");
      break;
    case ESP_RST_BROWNOUT:
      printf("Brownout reset\r\n");
      break;
    case ESP_RST_SDIO:
      printf("SDIO reset\r\n");
      break;
    default:
      printf("Unknown reason\r\n");
      break;
  }
}
