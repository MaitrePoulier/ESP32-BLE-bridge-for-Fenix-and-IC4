#ifndef _ESP32_H_
    #define _ESP32_H_

    // Shown by the console command detail
    const char *ESP32_model_str(void);          // "ESP32-D0WDR2-V3"... (same names as Arduino ESP.getChipModel())
    const char *ESP32_reset_reason_str(void);

    // °C. ESP32: undocumented, uncalibrated sensor (method of Arduino temperatureRead()): indicative only
    float ESP32_ReadTemp(void);

#endif
