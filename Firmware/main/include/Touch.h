#ifndef __TOUCH_H__
#define __TOUCH_H__

    #include <stdint.h>
    #include <stdbool.h>

    // XPT2046 resistive touch controller, on the display SPI bus.
    // Port of the TFT_eSPI "Touch" extension: same algorithm, same thresholds
    // and same 5-value calibration format as tft.setTouch()/calibrateTouch().

    #define TOUCH_Z_THRESHOLD 350   // Touch pressure threshold for validating touches

    void touch_init(void);   // call after display_init() (the SPI bus must exist)

    void     touch_get_raw(uint16_t *x, uint16_t *y);
    uint16_t touch_get_raw_z(void);
    bool     touch_valid(uint16_t *x, uint16_t *y, uint16_t threshold);

    // Calibrated screen coordinates. Returns false if not touched.
    bool touch_get_point(uint16_t *x, uint16_t *y, uint16_t threshold = 600);

    // cal = { x0, x1, y0, y1, flags(rotate | invert_x<<1 | invert_y<<2) }
    void touch_set_calibration(const uint16_t cal[5]);

    // values[8] = raw x,y of the 4 corners: up-left, bottom-left, up-right, bottom-right
    void touch_compute_calibration(const int32_t values[8], uint16_t cal[5]);

#endif
