#ifndef _BRIDGE_SCREEN_H_
#define _BRIDGE_SCREEN_H_

    // Diagnostic screen of the IC4 Bridge: state of each link (IC4, Fenix, HRM 600) and decoded values
    // (W, rpm, bpm), refreshed every UI_REFRESH_MS (30 Hz). LVGL loop only.
    void BridgeScreen_create(void);

#endif
