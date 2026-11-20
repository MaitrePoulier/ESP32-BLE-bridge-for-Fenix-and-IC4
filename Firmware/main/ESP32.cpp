#include "ESP32.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_efuse.h"
#if CONFIG_IDF_TARGET_ESP32
#include "soc/efuse_defs.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/sens_reg.h"
#endif

const char *ESP32_model_str(void)
{
#if CONFIG_IDF_TARGET_ESP32
    esp_chip_info_t info;
    esp_chip_info(&info);
    const bool v3 = info.revision >= 300;

    switch (esp_efuse_get_pkg_ver()) {
        case EFUSE_RD_CHIP_VER_PKG_ESP32D0WDQ6:   return v3 ? "ESP32-D0WDQ6-V3" : "ESP32-D0WDQ6";
        case EFUSE_RD_CHIP_VER_PKG_ESP32D0WDQ5:   return v3 ? "ESP32-D0WD-V3" : "ESP32-D0WD";
        case EFUSE_RD_CHIP_VER_PKG_ESP32D2WDQ5:   return "ESP32-D2WD";
        case EFUSE_RD_CHIP_VER_PKG_ESP32U4WDH:    return "ESP32-U4WDH";
        case EFUSE_RD_CHIP_VER_PKG_ESP32PICOD4:   return v3 ? "ESP32-PICO-V3" : "ESP32-PICO-D4";
        case EFUSE_RD_CHIP_VER_PKG_ESP32PICOV302: return "ESP32-PICO-V3-02";
        case EFUSE_RD_CHIP_VER_PKG_ESP32D0WDR2V3: return "ESP32-D0WDR2-V3";
        default:                                  return "ESP32";
    }
#else
    return CONFIG_IDF_TARGET;
#endif
}

const char *ESP32_reset_reason_str(void)
{
    switch (esp_reset_reason())
    {
        case ESP_RST_UNKNOWN:   return "Unknown";
        case ESP_RST_POWERON:   return "Power-on";
        case ESP_RST_EXT:       return "External reset";
        case ESP_RST_SW:        return "Software reset";
        case ESP_RST_PANIC:     return "Exception/panic";
        case ESP_RST_INT_WDT:   return "Interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "Task watchdog";
        case ESP_RST_WDT:       return "Other watchdog";
        case ESP_RST_DEEPSLEEP: return "Wake from deep sleep";
        case ESP_RST_BROWNOUT:  return "Brownout reset";
        case ESP_RST_SDIO:      return "SDIO reset";
        default:                return "Unknown reason";
    }
}

float ESP32_ReadTemp(void)
{
#if CONFIG_IDF_TARGET_ESP32
    // Internal sensor of the SAR ADC, read through its registers (no ESP-IDF driver on the ESP32). Same sequence as
    // the old ESP-IDF temprature_sens_read() used by Arduino. Raw value in "°F", not calibrated.
    SET_PERI_REG_BITS(SENS_SAR_TSENS_CTRL_REG, SENS_TSENS_CLK_DIV, 10, SENS_TSENS_CLK_DIV_S);
    SET_PERI_REG_BITS(SENS_SAR_MEAS_WAIT2_REG, SENS_FORCE_XPD_SAR, 3, SENS_FORCE_XPD_SAR_S);
    CLEAR_PERI_REG_MASK(SENS_SAR_TSENS_CTRL_REG, SENS_TSENS_POWER_UP);
    CLEAR_PERI_REG_MASK(SENS_SAR_TSENS_CTRL_REG, SENS_TSENS_DUMP_OUT);
    SET_PERI_REG_MASK(SENS_SAR_TSENS_CTRL_REG, SENS_TSENS_POWER_UP_FORCE);
    SET_PERI_REG_MASK(SENS_SAR_TSENS_CTRL_REG, SENS_TSENS_POWER_UP);
    vTaskDelay(pdMS_TO_TICKS(100));
    SET_PERI_REG_MASK(SENS_SAR_TSENS_CTRL_REG, SENS_TSENS_DUMP_OUT);
    vTaskDelay(pdMS_TO_TICKS(5));

    const uint32_t raw_val = GET_PERI_REG_BITS2(SENS_SAR_SLAVE_ADDR3_REG, SENS_TSENS_OUT, SENS_TSENS_OUT_S);
    return ((float)raw_val - 32.0f) / 1.8f;
#else
    return 0.0f / 0.0f;   // NAN: no sensor code for this target
#endif
}
