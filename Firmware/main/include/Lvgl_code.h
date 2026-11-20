#ifndef __LVGL_CODE_H__
#define __LVGL_CODE_H__

    #include "lvgl.h"

    void my_flush_cb(lv_display_t *, const lv_area_t *, uint8_t *);
    void my_touchpad_read(lv_indev_t *, lv_indev_data_t *);


    uint32_t my_tick(void);
    void ScreenInit(void);
    void touch_calibrate(void);   // LVGL loop only (console: Gui_requestTouchCalibrate)

    // Requests from the console task, run by the LVGL loop (LVGL is not thread-safe).
    // Each function returns once the LVGL loop has handled it.
    void Gui_initRequests(void);          // Before the console starts
    void Gui_processRequests(void);       // Called by the LVGL loop, before lv_timer_handler()
    void Gui_requestTouchCalibrate(void); // Touch the 4 corners, prints the calibration
    void Gui_requestTouchTest(void);      // Prints the touched points for 10 s
    void Gui_requestBenchmark(void);      // Starts lv_demo_benchmark (results printed at the end)
    void Gui_requestUiPerf(bool on);      // LVGL measurements printed every second

#endif
