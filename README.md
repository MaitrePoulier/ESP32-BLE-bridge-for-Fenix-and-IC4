The goals of this BaseBoard:
* Support Bluetooth legacy for receiving audio from an external audio source
* Support 2x 18650 LiFePo4 cell that can be charged with the USB connector
* Low noise +/- 12.4V going to the mezzanine to powered Audio op-amp
* I2S port going to the mezz
* Support of the MHS 4.0 screen (ST7796S) with touch (XPT2046)
* can be placed inside an Pelican 1040 Micro Case

![ESP32 BaseBoard Picture](ESP32-BaseBoard.png)

## Firmware

ESP-IDF v6.1 project (ESP32-WROOM-32E-N8R2), LVGL 9 for the GUI.

```bash
cd Firmware
idf.py set-target esp32      # only the first time
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

* `main/` : application sources, `main/include/` : headers
* `main/include/Config.h` : pins of the board (screen, touch, I2S, battery)
* `sdkconfig.defaults` : project configuration (PSRAM, flash 8 MB, LVGL options)
* `main/idf_component.yml` : external components (LVGL), downloaded in `managed_components/`
