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

    //********************* IC4 Bridge (BLE) *********************
    // Peripheral (server): what the Fenix 8 and the IC4 console see
    #define BRIDGE_NAME             "IC4 Bridge"
    #define BRIDGE_APPEARANCE       0x0484          // Cycling: power sensor
    #define BRIDGE_MANUFACTURER     "ESP32-Base-IC4" // Device Information (0x180A)
    #define BRIDGE_MODEL            "IC4 Bridge"
    #define BRIDGE_MAX_CLIENTS      3               // Simultaneous centrals (Fenix + IC4 console + nRF Connect)
    #define CPS_NOTIFY_PERIOD_MS    1000            // Cycling Power Measurement: 1 Hz

    // Central (client): Schwinn IC4, FTMS
    #define IC4_NAME                "IC Bike"       // Advertised name (with the FTMS service 0x1826)
    #define IC4_SCAN_TIMEOUT_MS     10000           // One scan; then retry with the backoff
    #define IC4_CONNECT_TIMEOUT_MS  10000
    #define FTMS_TIMEOUT_MS         3000            // No Indoor Bike Data: power 0, crank data frozen
    #define IC4_DATA_LOST_MS        15000           // Connected without data that long: disconnect and reconnect
    #define HR_TIMEOUT_MS           5000            // No heart rate that long: no value (phase 2: notifications stop)

    // Reconnection backoff (IC4, HRM 600): 1 s, 2 s, 4 s... 30 s max
    #define BLE_BACKOFF_MIN_MS      1000
    #define BLE_BACKOFF_MAX_MS      30000

    // Diagnostic screen refresh (30 Hz)
    #define UI_REFRESH_MS           33

    // Training targets shown on the screen
    #define CADENCE_TARGET_MIN      80              // rpm: cadence tile highlighted in [MIN, MAX]
    #define CADENCE_TARGET_MAX      95
    // Heart rate zones (bpm, lower bound of each zone): Z1 92-117, Z2 118-132, Z3 133-155, Z4 156-165, Z5 166-183
    #define HR_Z1_MIN               92              // Recovery
    #define HR_Z2_MIN               118             // Base endurance
    #define HR_Z3_MIN               133             // Tempo
    #define HR_Z4_MIN               156             // Threshold
    #define HR_Z5_MIN               166             // Anaerobic
    #define HR_Z5_MAX               183

    //Define the random number source
    //static inline int32_t console_random(void){return rng_read();}
    static inline int32_t console_random(void){return esp_random();}  //if not on STM32F753

#endif
