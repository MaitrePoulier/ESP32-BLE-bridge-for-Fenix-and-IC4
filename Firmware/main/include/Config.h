#ifndef _GENERAL_H_
#define _GENERAL_H_
    #include <stdint.h>
    #include "esp_random.h"

    //********************* Audio (I2S to the mezzanine) *********************
    #define I2S_WS      26   // LRCK (WSEL)
    #define I2S_SD      25   // DATA OUT
    #define I2S_SCK     27   // BCLK

    //********************* Battery *********************
    #define BattPin     39   // GPIO39 = ADC1 channel 3

    // ADC calibration characteristics
    // Mutimeter say 3.629V, reading of the ESP32 say 3.680 so: 0.9861413
    #define CALIBRATION_FACTOR 0.9861413f

    //********************* Screen MHS 4.0" (ST7796S + XPT2046) *********************
    // (was lib/Config/User_Setup.h of TFT_eSPI)
    // Landscape, same as tft.setRotation(3)
    #define SCREEN_WIDTH   480
    #define SCREEN_HEIGHT  320

    // VSPI native pins (IO_MUX) -> allows 80 MHz
    #define TFT_MISO  19
    #define TFT_MOSI  23
    #define TFT_SCLK  18
    #define TFT_CS     5   // Chip select control pin
    #define TFT_DC     4   // Data Command control pin
    #define TFT_RST   33   // Reset pin
    #define TOUCH_CS   2   // Chip select pin (T_CS) of touch screen

    #define SPI_FREQUENCY        80000000
    #define SPI_TOUCH_FREQUENCY   2500000

    //********************* Time of day (WiFi + SNTP) *********************
    #define SNTP_SERVER  "pool.ntp.org"
    #define TIMEZONE     "EST5EDT,M3.2.0,M11.1.0"

    //Define the random number source
    //static inline int32_t console_random(void){return rng_read();}
    inline int32_t console_random(void){return esp_random();}  //if not on STM32F753

#endif
