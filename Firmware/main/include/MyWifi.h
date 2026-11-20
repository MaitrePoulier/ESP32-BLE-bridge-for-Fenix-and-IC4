#ifndef _WIFI_H_
#define _WIFI_H_

    #include "esp_err.h"

    // The WiFi driver (and its internal SRAM) only exists for the duration of each operation below
    void MyWifiInit(void);          // Network, events and SNTP, then calls MyWifiSyncTime (NVS must be initialised)
    esp_err_t MyWifiSyncTime(void); // Radio on -> scan -> saved networks in range (strongest first) -> SNTP time -> radio off
    void MyWifiScan(void);          // Lists the visible networks (marks the saved ones)
    esp_err_t MyWifiConnect(const char *ssid, const char *password); // Adds/updates the network (NVS, max 10), then syncs trying it first
    esp_err_t MyWifiForget(const char *ssid_or_number);              // Deletes a network (SSID or number from wifi list)
    void MyWifiList(void);          // Prints the saved networks (without the passwords)

#endif
