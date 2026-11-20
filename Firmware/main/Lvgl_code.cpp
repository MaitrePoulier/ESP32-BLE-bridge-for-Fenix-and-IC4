#include "Lvgl_code.h"
#include "Config.h"
#include "Display.h"
#include "Touch.h"

#include <stdio.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Display being flushed, released by the SPI "transfer done" interrupt
static lv_display_t * volatile flushing_disp = NULL;

static void flush_done(void *ctx)
{
    if (flushing_disp) {
        lv_display_flush_ready(flushing_disp);
    }
}

void ScreenInit(void){
  // Set up the display (SPI bus + ST7796S) and the touch controller
  display_init(flush_done, NULL);
  touch_init();
}

// The display uses LV_COLOR_FORMAT_RGB565_SWAPPED: LVGL already renders in the
// byte order of the screen, no swap needed here (was tft.setSwapBytes(true))
void my_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map) {
    flushing_disp = disp;
    // Asynchronous DMA transfer: lv_display_flush_ready() is called in flush_done()
    display_draw(area->x1, area->y1, area->x2, area->y2, px_map);
}

// Touchpad Read Callback
void my_touchpad_read(lv_indev_t * indev, lv_indev_data_t * data) {
    uint16_t touchX = 0, touchY = 0;

    // Check hardware reading
    bool touched = touch_get_point(&touchX, &touchY);

    if(!touched) {
        data->state = LV_INDEV_STATE_RELEASED;
    } else {
        // Debugging prints - Check your Serial Monitor!
        /*printf("Touch detected! X: %u Y: %u\r\n", touchX, touchY);*/

        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = touchX;
        data->point.y = touchY;
    }
}


// Simple test button event callback
static void btn_event_cb(lv_event_t * e) {
    // Added explicit typecast to fix the C++ conversion error
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0);
    lv_label_set_text(label, "Clicked!");
}

// Your UI Definition Function
void create_test_ui(void) {
    // Create an Interactive Button to test touch functionality
    lv_obj_t * btn = lv_button_create(lv_screen_active());
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * label = lv_label_create(btn);
    lv_label_set_text(label, "Touch Me");
    lv_obj_center(label);
}

uint32_t my_tick(void)
{
  return (uint32_t)(esp_timer_get_time() / 1000);
}

//*********************** Touch calibration ***********************
// Same procedure as TFT_eSPI calibrateTouch(), drawn with LVGL

#define CAL_MARK_SIZE 15

// Draw the arrow pointing to one corner (same shape as TFT_eSPI)
static void draw_corner_mark(lv_obj_t * scr, int corner, lv_point_precise_t pts[6])
{
    const int32_t s = CAL_MARK_SIZE;
    const int32_t w = SCREEN_WIDTH - 1;
    const int32_t h = SCREEN_HEIGHT - 1;
    int32_t cx = (corner >= 2) ? w : 0;                 // 0: up left, 1: bot left
    int32_t cy = (corner == 1 || corner == 3) ? h : 0;  // 2: up right, 3: bot right
    int32_t dx = (corner >= 2) ? -s : s;
    int32_t dy = (corner == 1 || corner == 3) ? -s : s;

    // Two sides of the corner + the diagonal
    pts[0] = {(lv_value_precise_t)(cx + dx), (lv_value_precise_t)cy};
    pts[1] = {(lv_value_precise_t)cx, (lv_value_precise_t)cy};
    pts[2] = {(lv_value_precise_t)cx, (lv_value_precise_t)(cy + dy)};
    pts[3] = {(lv_value_precise_t)cx, (lv_value_precise_t)cy};
    pts[4] = {(lv_value_precise_t)(cx + dx), (lv_value_precise_t)(cy + dy)};

    lv_obj_t * line = lv_line_create(scr);
    lv_line_set_points(line, pts, 5);
    lv_obj_set_style_line_color(line, lv_palette_main(LV_PALETTE_PINK), 0);   // ~TFT_MAGENTA
    lv_obj_set_style_line_width(line, 1, 0);
    lv_obj_set_pos(line, 0, 0);
}

static void wait_release(void)
{
    uint16_t x, y;
    while (touch_valid(&x, &y, TOUCH_Z_THRESHOLD / 2)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void touch_calibrate()
{
  uint16_t calData[5];
  int32_t values[8] = {0};
  static lv_point_precise_t pts[6];

  lv_obj_t * old_scr = lv_screen_active();
  lv_obj_t * scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t * label = lv_label_create(scr);
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_label_set_text(label, "Touch corners as indicated");
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);

  static const char * const corner_name[4] = {"top-left", "bottom-left", "top-right", "bottom-right"};

  printf("Touch calibration: touch the arrow in each corner of the screen\r\n");
  lv_screen_load(scr);

  for (int i = 0; i < 4; i++) {
    // Remove the previous arrow (child 0 is the label)
    while (lv_obj_get_child_count(scr) > 1) {
      lv_obj_delete(lv_obj_get_child(scr, 1));
    }
    draw_corner_mark(scr, i, pts);
    lv_refr_now(NULL);

    // user has to get the chance to release
    if (i > 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      wait_release();
    }

    printf("  %d/4: touch the %s corner... ", i + 1, corner_name[i]);
    fflush(stdout);
    for (int j = 0; j < 8; j++) {
      uint16_t x_tmp, y_tmp;
      // Use a lower detect threshold as corners tend to be less sensitive
      while (!touch_valid(&x_tmp, &y_tmp, TOUCH_Z_THRESHOLD / 2)) {
        vTaskDelay(pdMS_TO_TICKS(1));
      }
      values[i * 2]     += x_tmp;
      values[i * 2 + 1] += y_tmp;
    }
    values[i * 2]     /= 8;
    values[i * 2 + 1] /= 8;
    printf("OK (raw x=%ld y=%ld)\r\n", (long)values[i * 2], (long)values[i * 2 + 1]);
  }

  touch_compute_calibration(values, calData);
  touch_set_calibration(calData);

  printf("\r\n\r\n");
  printf("// Use this calibration code in setup():\r\n");
  printf("  uint16_t calData[5] = { %u, %u, %u, %u, %u };\r\n",
         calData[0], calData[1], calData[2], calData[3], calData[4]);
  printf("  touch_set_calibration(calData);\r\n\r\n");

  while (lv_obj_get_child_count(scr) > 1) {
    lv_obj_delete(lv_obj_get_child(scr, 1));
  }
  lv_obj_set_style_text_color(label, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_label_set_text(label, "Calibration complete!\nCalibration code sent to Serial port.");
  lv_refr_now(NULL);

  vTaskDelay(pdMS_TO_TICKS(4000));

  // Back to the previous screen
  lv_screen_load(old_scr);
  lv_obj_delete(scr);
}
