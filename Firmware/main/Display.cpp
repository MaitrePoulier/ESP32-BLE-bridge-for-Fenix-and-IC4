/*
 * ST7796S driver for the MHS 4.0" Raspberry Pi display (480x320)
 *
 * This display is a "RPi display type" (TFT_eSPI: RPI_DISPLAY_TYPE): the SPI
 * stream is converted to a 16-bit parallel bus, so EVERY command and EVERY
 * parameter has to be sent as a 16-bit word: [0x00, value].
 * Pixels are sent as normal big-endian RGB565 words.
 *
 * -> esp_lcd panel IO with lcd_cmd_bits = 16 and lcd_param_bits = 16.
 * The init sequence is the one of TFT_eSPI 2.5.43 (TFT_Drivers/ST7796_Init.h).
 */
#include "Display.h"
#include "Config.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ST7796_MADCTL_MY  0x80
#define ST7796_MADCTL_MX  0x40
#define ST7796_MADCTL_MV  0x20
#define ST7796_MADCTL_BGR 0x08

// Biggest flush = 1/10 of the screen (see main.cpp)
#define DISPLAY_MAX_TRANSFER  (SCREEN_WIDTH * (SCREEN_HEIGHT / 10) * 2)

static esp_lcd_panel_io_handle_t s_io = NULL;
static display_done_cb_t s_done_cb = NULL;
static void *s_done_ctx = NULL;

typedef struct {
    uint8_t cmd;
    uint8_t data[14];
    uint8_t len;
    uint16_t delay_ms;
} lcd_init_cmd_t;

static const lcd_init_cmd_t st7796_init[] = {
    {0x01, {0}, 0, 120},                     // Software reset
    {0x11, {0}, 0, 120},                     // Sleep exit
    {0xF0, {0xC3}, 1, 0},                    // Command Set control: enable extension command 2 part I
    {0xF0, {0x96}, 1, 0},                    // Command Set control: enable extension command 2 part II
    {0x36, {0x48}, 1, 0},                    // Memory Data Access Control: MX, BGR
    {0x3A, {0x55}, 1, 0},                    // Interface Pixel Format: 16 bits/pixel
    {0xB4, {0x01}, 1, 0},                    // Column inversion: 1-dot inversion
    {0xB6, {0x80, 0x02, 0x3B}, 3, 0},        // Display Function Control
    {0xE8, {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 8, 0}, // Display Output Ctrl Adjust
    {0xC1, {0x06}, 1, 0},                    // Power control 2
    {0xC2, {0xA7}, 1, 0},                    // Power control 3
    {0xC5, {0x18}, 1, 120},                  // VCOM Control: VCOM=0.9
    {0xE0, {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B}, 14, 0},   // Gamma +
    {0xE1, {0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B}, 14, 120}, // Gamma -
    {0xF0, {0x3C}, 1, 0},                    // Command Set control: disable extension command 2 part I
    {0xF0, {0x69}, 1, 120},                  // Command Set control: disable extension command 2 part II
    {0x29, {0}, 0, 0},                       // Display on
};

// Send a command and its 8-bit parameters, each one as a 16-bit word
static void lcd_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    // esp_lcd swaps the bytes of this buffer in place: always use a fresh copy
    uint16_t params[16];
    for (size_t i = 0; i < len; i++) {
        params[i] = data[i];
    }
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, cmd, len ? params : NULL, len * sizeof(uint16_t)));
}

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    if (s_done_cb) {
        s_done_cb(s_done_ctx);
    }
    return false;
}

void display_init(display_done_cb_t done_cb, void *ctx)
{
    s_done_cb = done_cb;
    s_done_ctx = ctx;

    // SPI bus (also used by the touch controller)
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = TFT_MOSI;
    buscfg.miso_io_num = TFT_MISO;
    buscfg.sclk_io_num = TFT_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = DISPLAY_MAX_TRANSFER;
    ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = (gpio_num_t)TFT_CS;
    io_config.dc_gpio_num = (gpio_num_t)TFT_DC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = SPI_FREQUENCY;
    io_config.trans_queue_depth = 10;
    io_config.on_color_trans_done = on_color_trans_done;
    io_config.lcd_cmd_bits = 16;     // RPi display: 16-bit commands
    io_config.lcd_param_bits = 16;   // RPi display: 16-bit parameters
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST, &io_config, &s_io));

    // Hardware reset (same timing as TFT_eSPI::init())
    gpio_config_t rst_cfg = {};
    rst_cfg.pin_bit_mask = 1ULL << TFT_RST;
    rst_cfg.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&rst_cfg));
    gpio_set_level((gpio_num_t)TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level((gpio_num_t)TFT_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150 + 120));

    for (size_t i = 0; i < sizeof(st7796_init) / sizeof(st7796_init[0]); i++) {
        lcd_cmd(st7796_init[i].cmd, st7796_init[i].data, st7796_init[i].len);
        if (st7796_init[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(st7796_init[i].delay_ms));
        }
    }

    // Rotation 3 (inverted landscape), as tft.setRotation(3)
    const uint8_t madctl = ST7796_MADCTL_MX | ST7796_MADCTL_MY | ST7796_MADCTL_MV | ST7796_MADCTL_BGR;
    lcd_cmd(0x36, &madctl, 1);
}

void display_draw(int x1, int y1, int x2, int y2, const void *pixels)
{
    const uint8_t caset[4] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
    const uint8_t raset[4] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2};
    lcd_cmd(0x2A, caset, 4);   // Column address set
    lcd_cmd(0x2B, raset, 4);   // Row address set

    size_t len = (size_t)(x2 - x1 + 1) * (y2 - y1 + 1) * sizeof(uint16_t);
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(s_io, 0x2C, pixels, len));   // Memory write
}
