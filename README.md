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
* `main/include/Config.h` : pins of the board (screen, touch, I2S, battery) and bridge constants
* `sdkconfig.defaults` : project configuration (PSRAM, flash 8 MB, LVGL, NimBLE, watchdog)
* `main/idf_component.yml` : external components (LVGL), downloaded in `managed_components/`

Serial console (115200 bauds, ANSI terminal such as Tera Term or `idf.py monitor`): type `help`.

### Host tests

The FTMS decoder and the crank data synthesis are plain C, tested on the PC:

```bash
cd Firmware
gcc -std=c11 -Wall -Wextra -Imain/include test/host/test_bridge.c main/Ftms.c main/Crank.c -lm -o /tmp/test_bridge
/tmp/test_bridge
```

## IC4 Bridge

BLE bridge between a Schwinn IC4 bike, a Garmin HRM 600 heart rate strap and a Garmin Fenix 8 watch.
The Fenix no longer connects to the IC4 (Garmin bug in FTMS "Trainer" mode): the bridge reads the FTMS data of the
IC4 and sends them again as a standard BLE cycling power sensor. Phase 2 will also relay the heart rate of the
HRM 600 to the IC4 console.

```
 Schwinn IC4 "IC Bike"            ESP32 "IC4 Bridge"                      Fenix 8
 FTMS 0x1826           notify     central: Ic4Client.c     peripheral:    Sensors > Add > Power
 Indoor Bike Data 0x2AD2 ------>  Ftms.c (decoder)         BleServer.c --> Cycling Power 0x1818
                                  Crank.c (crank data)     1 Hz           Device Information 0x180A
```

* Everything BLE runs in the NimBLE host task (GAP/GATT callbacks and `ble_npl_callout` timers): a reconnection to
  the IC4 never blocks the server.
* The bridge never writes the Fitness Machine Control Point (0x2AD9) and never reads the IC4 heart rate service
  (0x180D, it relays the heart rate: that would make a loop).
* Cycling Power Measurement (0x2A63, 1 Hz): flags 0x0020, power (sint16), cumulative crank revolutions (uint16),
  last crank event time (uint16, 1/1024 s). The crank data are synthesised by integrating the FTMS cadence;
  cadence 0 or no FTMS data for more than 3 s: power 0 and crank data frozen.
* Reconnection to the IC4 with a backoff of 1 s, 2 s, 4 s... 30 s max; a connection without data for 15 s is
  closed and restarted.
* Task watchdog: the board restarts if the LVGL loop or an idle task stops for 10 s.
* Screen (Garmin style): state of each link (IC4, Fenix, HRM 600) and decoded values, refreshed 30 times per
  second: power (W), cadence (green tile in the target 80-95 rpm), resistance level of the IC4, heart rate with its
  zone (Z1-Z5, Garmin colours; targets and zones in `Config.h`).
* Touching the screen shows the IC4 details page (last raw frame and its decoding, FTMS characteristics, devices
  seen while scanning, latest events): the diagnostic is possible next to the bike, without the console.
* Event log kept in flash across restarts (opening the serial monitor resets the board): connections,
  disconnections, devices seen, first FTMS frames of each connection.
* Console: `bridge` (status of the links, last FTMS frame decoded, power data sent), `bridge raw` (every frame),
  `bridge log [clear]` (event log).

### Phases

1. IC4 -> Cycling Power only. Check with nRF Connect (device "IC4 Bridge": service 0x1818, notifications of 0x2A63
   every second), then with the Fenix (Sensors -> Add -> Power).
2. Relay of the HRM 600 heart rate to the IC4 console (Heart Rate Service 0x180D on the bridge).

### Unknowns

* Does the Fenix accept this BLE power sensor? (Not checked yet.)
* Does the IC4 console reconnect by itself to the heart rate sensor, or must its button be held at each session?
* How many simultaneous BLE connections does the HRM 600 accept?
