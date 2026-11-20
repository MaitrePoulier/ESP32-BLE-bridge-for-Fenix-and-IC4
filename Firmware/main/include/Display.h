#ifndef __DISPLAY_H__
#define __DISPLAY_H__

    #include <stdint.h>
    #include "driver/spi_master.h"

    // SPI bus shared by the screen (ST7796S) and the touch controller (XPT2046)
    #define DISPLAY_SPI_HOST SPI3_HOST   // = VSPI

    // Called (from the SPI ISR) when a display_draw() transfer is finished
    typedef void (*display_done_cb_t)(void *ctx);

    // Init the SPI bus, reset and configure the ST7796S (landscape, rotation 3)
    void display_init(display_done_cb_t done_cb, void *ctx);

    // Send a block of RGB565 pixels, big-endian (LV_COLOR_FORMAT_RGB565_SWAPPED)
    // Asynchronous (DMA): done_cb is called when the transfer is finished
    void display_draw(int x1, int y1, int x2, int y2, const void *pixels);

#endif
