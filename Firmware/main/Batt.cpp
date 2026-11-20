#include "Batt.h"
#include "Config.h"

#include <stdio.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static adc_channel_t s_channel;

void batt_init(void)
{
  adc_unit_t unit;
  ESP_ERROR_CHECK(adc_oneshot_io_to_channel(BattPin, &unit, &s_channel));

  adc_oneshot_unit_init_cfg_t unit_cfg = {};
  unit_cfg.unit_id = unit;
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

  // 12-bit resolution, 6 dB attenuation (optimal for ~1.25V - 1.85V input)
  adc_oneshot_chan_cfg_t chan_cfg = {};
  chan_cfg.atten = ADC_ATTEN_DB_6;
  chan_cfg.bitwidth = ADC_BITWIDTH_12;
  ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_channel, &chan_cfg));

  // eFuse calibration -> millivolts (same scheme as Arduino analogReadMilliVolts on ESP32)
  adc_cali_line_fitting_config_t cali_cfg = {};
  cali_cfg.unit_id = unit;
  cali_cfg.atten = ADC_ATTEN_DB_6;
  cali_cfg.bitwidth = ADC_BITWIDTH_12;
  cali_cfg.default_vref = 1100;
  if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali) != ESP_OK) {
    printf("ADC calibration not available, battery voltage will be approximate\r\n");
    s_cali = NULL;
  }
}

float battRead(void)
{
  int raw = 0, mv = 0;
  if (s_adc == NULL || adc_oneshot_read(s_adc, s_channel, &raw) != ESP_OK) return 0.0;

  if (s_cali) {
    adc_cali_raw_to_voltage(s_cali, raw, &mv);
  } else {
    mv = raw * 1750 / 4095;   // 6 dB: ~0 - 1.75 V full scale
  }
  // Voltage divider by 2 on the board
  return (float)(mv * 2.0 * CALIBRATION_FACTOR / 1000.0);
}

float getBatteryPercentage(float voltage) {
  // Handle upper boundary
  if (voltage >= socTable[0].voltage) return 100.0;
  // Handle lower boundary
  if (voltage <= socTable[TABLE_SIZE - 1].voltage) return 0.0;

  // Search for the voltage segment inside the table
  for (int i = 0; i < TABLE_SIZE - 1; i++) {
    if (voltage <= socTable[i].voltage && voltage >= socTable[i+1].voltage) {
      // Linear interpolation formula between point i and i+1
      float v_high = socTable[i].voltage;
      float v_low  = socTable[i+1].voltage;
      float p_high = socTable[i].percentage;
      float p_low  = socTable[i+1].percentage;

      return p_low + ((voltage - v_low) / (v_high - v_low)) * (p_high - p_low);
    }
  }
  return 0.0; 
}

