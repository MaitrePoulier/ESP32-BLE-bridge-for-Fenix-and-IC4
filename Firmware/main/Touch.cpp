/*
 * XPT2046 touch controller.
 * Port of TFT_eSPI 2.5.43 Extensions/Touch.cpp to the ESP-IDF SPI master driver.
 */
#include "Touch.h"
#include "Display.h"
#include "Config.h"

#include <stdlib.h>
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define _RAWERR 20 // Deadband error allowed in successive position samples

static spi_device_handle_t s_touch = NULL;

static uint16_t cal_x0 = 300, cal_x1 = 3600, cal_y0 = 300, cal_y1 = 3600;
static bool cal_rotate = true, cal_invert_x = false, cal_invert_y = false;

static uint32_t s_press_time = 0;

static uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void touch_init(void)
{
    spi_device_interface_config_t dev = {};
    dev.mode = 0;
    dev.clock_speed_hz = SPI_TOUCH_FREQUENCY;
    dev.spics_io_num = TOUCH_CS;
    dev.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(DISPLAY_SPI_HOST, &dev, &s_touch));
}

// Send a control byte and read the 12-bit conversion result
static uint16_t xpt2046_read(uint8_t cmd)
{
    spi_transaction_t t = {};
    t.length = 24;
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.tx_data[0] = cmd;
    ESP_ERROR_CHECK(spi_device_transmit(s_touch, &t));
    return (uint16_t)(((t.rx_data[1] << 8) | t.rx_data[2]) >> 3);
}

void touch_get_raw(uint16_t *x, uint16_t *y)
{
    // Read 4 times and keep the last sample (as TFT_eSPI)
    for (int i = 0; i < 4; i++) *x = xpt2046_read(0xD0);   // YP sample -> x position
    for (int i = 0; i < 4; i++) *y = xpt2046_read(0x90);   // XP sample -> y position
}

uint16_t touch_get_raw_z(void)
{
    int16_t tz = 0xFFF;
    tz += xpt2046_read(0xB0);   // Z1
    tz -= xpt2046_read(0xC0);   // Z2
    if (tz == 4095) tz = 0;
    return (uint16_t)tz;
}

bool touch_valid(uint16_t *x, uint16_t *y, uint16_t threshold)
{
    uint16_t x_tmp, y_tmp, x_tmp2, y_tmp2;

    // Wait until pressure stops increasing to debounce pressure
    uint16_t z1 = 1;
    uint16_t z2 = 0;
    while (z1 > z2) {
        z2 = z1;
        z1 = touch_get_raw_z();
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (z1 <= threshold) return false;

    touch_get_raw(&x_tmp, &y_tmp);

    vTaskDelay(pdMS_TO_TICKS(1)); // Small delay to the next sample
    if (touch_get_raw_z() <= threshold) return false;

    vTaskDelay(pdMS_TO_TICKS(2)); // Small delay to the next sample
    touch_get_raw(&x_tmp2, &y_tmp2);

    if (abs(x_tmp - x_tmp2) > _RAWERR) return false;
    if (abs(y_tmp - y_tmp2) > _RAWERR) return false;

    *x = x_tmp;
    *y = y_tmp;
    return true;
}

void touch_raw_to_screen(uint16_t *x, uint16_t *y)
{
    int32_t x_tmp = *x, y_tmp = *y, xx, yy;

    if (!cal_rotate) {
        xx = (x_tmp - cal_x0) * SCREEN_WIDTH / cal_x1;
        yy = (y_tmp - cal_y0) * SCREEN_HEIGHT / cal_y1;
    } else {
        xx = (y_tmp - cal_x0) * SCREEN_WIDTH / cal_x1;
        yy = (x_tmp - cal_y0) * SCREEN_HEIGHT / cal_y1;
    }
    if (cal_invert_x) xx = SCREEN_WIDTH - xx;
    if (cal_invert_y) yy = SCREEN_HEIGHT - yy;

    // Same as TFT_eSPI: negative values wrap and are rejected by the caller
    *x = (uint16_t)xx;
    *y = (uint16_t)yy;
}

bool touch_get_point(uint16_t *x, uint16_t *y, uint16_t threshold)
{
    uint16_t x_tmp, y_tmp;

    if (threshold < 20) threshold = 20;
    if (s_press_time > millis()) threshold = 20;

    uint8_t n = 5;
    uint8_t valid = 0;
    while (n--) {
        if (touch_valid(&x_tmp, &y_tmp, threshold)) valid++;
    }

    if (valid < 1) {
        s_press_time = 0;
        return false;
    }

    s_press_time = millis() + 50;

    touch_raw_to_screen(&x_tmp, &y_tmp);

    if (x_tmp >= SCREEN_WIDTH || y_tmp >= SCREEN_HEIGHT) return false;

    *x = x_tmp;
    *y = y_tmp;
    return true;
}

void touch_set_calibration(const uint16_t cal[5])
{
    cal_x0 = cal[0];
    cal_x1 = cal[1];
    cal_y0 = cal[2];
    cal_y1 = cal[3];

    if (cal_x0 == 0) cal_x0 = 1;
    if (cal_x1 == 0) cal_x1 = 1;
    if (cal_y0 == 0) cal_y0 = 1;
    if (cal_y1 == 0) cal_y1 = 1;

    cal_rotate   = cal[4] & 0x01;
    cal_invert_x = cal[4] & 0x02;
    cal_invert_y = cal[4] & 0x04;
}

void touch_compute_calibration(const int32_t values[8], uint16_t cal[5])
{
    int32_t x0, x1, y0, y1, tmp;
    bool rotate = false, invert_x = false, invert_y = false;

    // from case 0 to case 1, the y value changed.
    // If the measured delta of the touch x axis is bigger than the delta of the y axis, the touch and TFT axes are switched.
    if (abs(values[0] - values[2]) > abs(values[1] - values[3])) {
        rotate = true;
        x0 = (values[1] + values[3]) / 2; // calc min x
        x1 = (values[5] + values[7]) / 2; // calc max x
        y0 = (values[0] + values[4]) / 2; // calc min y
        y1 = (values[2] + values[6]) / 2; // calc max y
    } else {
        x0 = (values[0] + values[2]) / 2; // calc min x
        x1 = (values[4] + values[6]) / 2; // calc max x
        y0 = (values[1] + values[5]) / 2; // calc min y
        y1 = (values[3] + values[7]) / 2; // calc max y
    }

    if (x0 > x1) { tmp = x0; x0 = x1; x1 = tmp; invert_x = true; }
    if (y0 > y1) { tmp = y0; y0 = y1; y1 = tmp; invert_y = true; }

    // pre calculate
    x1 -= x0;
    y1 -= y0;

    if (x0 == 0) x0 = 1;
    if (x1 == 0) x1 = 1;
    if (y0 == 0) y0 = 1;
    if (y1 == 0) y1 = 1;

    cal[0] = (uint16_t)x0;
    cal[1] = (uint16_t)x1;
    cal[2] = (uint16_t)y0;
    cal[3] = (uint16_t)y1;
    cal[4] = (uint16_t)(rotate | (invert_x << 1) | (invert_y << 2));
}
