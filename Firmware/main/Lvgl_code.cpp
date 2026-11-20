#include "Lvgl_code.h"
#include "Config.h"
#include "Display.h"
#include "Touch.h"

#include <stdio.h>
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "demos/lv_demos.h"
#include "display/lv_display_private.h"            // disp->perf_sysmon_backend (console "ui perf"); LVGL 9.6: subject is a pointer
#include "debugging/sysmon/lv_sysmon_private.h"   // lv_sysmon_perf_info_t

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
        esp_task_wdt_reset();
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
        esp_task_wdt_reset();   // The user may take a while: the LVGL loop is blocked meanwhile
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

  for (int i = 0; i < 40; i++) {   // 4 s
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  // Back to the previous screen
  lv_screen_load(old_scr);
  lv_obj_delete(scr);
}


//*********************** Touch test (console "touch test") ***********************

#define TEST_DOT_SIZE  5
#define TEST_MAX_DOTS  300   // Oldest dots deleted after that

static void touch_test_10s(void)
{
  printf("Touch test for 10 s: touch the screen...\n");

  lv_obj_t * old_scr = lv_screen_active();
  lv_obj_t * scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
  lv_obj_t * label = lv_label_create(scr);
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_label_set_text(label, "Touch test (10 s)");
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);
  lv_screen_load(scr);
  lv_refr_now(NULL);

  const int64_t start = esp_timer_get_time();
  int32_t last_x = -1, last_y = -1;
  while (esp_timer_get_time() - start < 10LL * 1000 * 1000) {
    uint16_t raw_x, raw_y;
    if (touch_valid(&raw_x, &raw_y, TOUCH_Z_THRESHOLD)) {
      uint16_t x = raw_x, y = raw_y;
      touch_raw_to_screen(&x, &y);
      if (x < SCREEN_WIDTH && y < SCREEN_HEIGHT && (x != last_x || y != last_y)) {
        printf("Touch X: %u, Y: %u  (raw X: %u, Y: %u)\n", x, y, raw_x, raw_y);
        last_x = x;
        last_y = y;

        // Red dot where the screen was touched (child 0 is the label)
        if (lv_obj_get_child_count(scr) > TEST_MAX_DOTS) lv_obj_delete(lv_obj_get_child(scr, 1));
        lv_obj_t * dot = lv_obj_create(scr);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, TEST_DOT_SIZE, TEST_DOT_SIZE);
        lv_obj_set_pos(dot, x - TEST_DOT_SIZE / 2, y - TEST_DOT_SIZE / 2);
        lv_obj_set_style_bg_color(dot, lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_refr_now(NULL);
      }
    } else {
      last_x = -1;
      last_y = -1;
    }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(15));
  }

  printf("Time is up\n");
  lv_screen_load(old_scr);
  lv_obj_delete(scr);
}

//*********************** LVGL benchmark (console "ui benchmark") ***********************

static bool s_benchmark_running = false;

static void benchmark_end_cb(const lv_demo_benchmark_summary_t *summary)
{
  printf("\n===== LVGL benchmark %d.%d.%d - %dx%d =====\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
         SCREEN_WIDTH, SCREEN_HEIGHT);
  printf("%-34s %5s %5s %8s %8s\n", "Scene", "FPS", "CPU%", "Render", "Flush");
  for (const lv_demo_benchmark_scene_dsc_t *scene = summary->scenes; scene->create_cb != NULL; scene++) {
    if (scene->measurement_cnt == 0) {
      printf("%-34s %5s\n", scene->name, "N/A");
      continue;
    }
    printf("%-34s %5lu %5lu %6lums %6lums\n", scene->name,
           (unsigned long)(scene->fps_avg / scene->measurement_cnt),
           (unsigned long)(scene->cpu_avg_usage / scene->measurement_cnt),
           (unsigned long)(scene->render_avg_time / scene->measurement_cnt),
           (unsigned long)(scene->flush_avg_time / scene->measurement_cnt));
  }
  // The total_avg_* are sums of the per-scene averages: divided by valid_scene_cnt
  const int32_t n = summary->valid_scene_cnt > 0 ? summary->valid_scene_cnt : 1;
  printf("%-34s %5ld %5ld %6ldms %6ldms   (%ld scenes)\n", "AVERAGE",
         (long)(summary->total_avg_fps / n), (long)(summary->total_avg_cpu / n),
         (long)(summary->total_avg_render_time / n), (long)(summary->total_avg_flush_time / n),
         (long)summary->valid_scene_cnt);
  printf("Benchmark done. Type 'restart' to go back to the user interface.\n");

  lv_demo_benchmark_summary_display(summary);   // Summary on the screen too
  s_benchmark_running = false;
}

static void benchmark_start(void)
{
  if (s_benchmark_running) {
    printf("The benchmark is already running\n");
    return;
  }
  s_benchmark_running = true;
  printf("LVGL benchmark running (a few minutes)...\n");
  lv_demo_benchmark_set_end_cb(benchmark_end_cb);
  lv_demo_benchmark();
}

//*********************** UI performance (console "ui perf") ***********************

static lv_observer_t *s_uiperf_observer = nullptr;
static uint32_t s_uiperf_inv_px = 0;   // Pixels invalidated since the last line (before LVGL merges the areas)
static uint32_t s_uiperf_frames = 0, s_uiperf_areas = 0, s_uiperf_full = 0;

static void uiperf_invalidate_cb(lv_event_t *e)
{
  const lv_area_t *area = static_cast<const lv_area_t *>(lv_event_get_param(e));
  if (area != NULL) s_uiperf_inv_px += lv_area_get_size(area);
}

// Start of each refresh: number of areas to redraw, and whole screen?
static void uiperf_refr_start_cb(lv_event_t *e)
{
  const lv_display_t *disp = static_cast<lv_display_t *>(lv_event_get_target(e));
  if (disp->inv_p == 0) return;
  s_uiperf_frames++;
  s_uiperf_areas += disp->inv_p;
  for (uint32_t i = 0; i < disp->inv_p; i++) {
    if (lv_area_get_width(&disp->inv_areas[i]) >= disp->hor_res &&
        lv_area_get_height(&disp->inv_areas[i]) >= disp->ver_res) {
      s_uiperf_full++;
      break;
    }
  }
}

static void uiperf_observer_cb(lv_observer_t *observer, lv_subject_t *subject)
{
  const lv_sysmon_perf_info_t *info = static_cast<const lv_sysmon_perf_info_t *>(lv_subject_get_pointer(subject));
  printf("ui perf: %2lu FPS, %3lu %% CPU, render %3lu ms, flush %2lu ms | free internal SRAM %u B (block %u)"
         " | invalidated %lu kpx/s | %lu frames, %lu areas/frame, %lu full screen\n",
         (unsigned long)info->calculated.fps, (unsigned long)info->calculated.cpu,
         (unsigned long)info->calculated.render_avg_time, (unsigned long)info->calculated.flush_avg_time,
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
         (unsigned long)(s_uiperf_inv_px / 1000),
         (unsigned long)s_uiperf_frames, (unsigned long)(s_uiperf_frames ? s_uiperf_areas / s_uiperf_frames : 0),
         (unsigned long)s_uiperf_full);
  s_uiperf_inv_px = 0;
  s_uiperf_frames = s_uiperf_areas = s_uiperf_full = 0;
}

static void uiperf_set(bool on)
{
#if LV_USE_PERF_MONITOR
  lv_display_t *disp = lv_display_get_default();
  if (on && s_uiperf_observer == nullptr) {
    s_uiperf_observer = lv_subject_add_observer(disp->perf_sysmon_backend.subject, uiperf_observer_cb, NULL);
    s_uiperf_inv_px = 0;
    lv_display_add_event_cb(disp, uiperf_invalidate_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(disp, uiperf_refr_start_cb, LV_EVENT_REFR_START, NULL);
  } else if (!on && s_uiperf_observer != nullptr) {
    lv_observer_remove(s_uiperf_observer);
    s_uiperf_observer = nullptr;
    lv_display_remove_event_cb_with_user_data(disp, uiperf_invalidate_cb, NULL);
    lv_display_remove_event_cb_with_user_data(disp, uiperf_refr_start_cb, NULL);
  }
#else
  printf("ui perf: CONFIG_LV_USE_PERF_MONITOR disabled\n");
#endif
}

//*********************** Requests from the console ***********************
// LVGL is not thread-safe: the console task posts a request, the LVGL loop (app_main) runs it and gives s_done back

enum gui_request_t { GUI_REQ_TOUCH_CAL, GUI_REQ_TOUCH_TEST, GUI_REQ_BENCHMARK, GUI_REQ_PERF_ON, GUI_REQ_PERF_OFF };

static QueueHandle_t s_requests = NULL;
static SemaphoreHandle_t s_done = NULL;

void Gui_initRequests(void)
{
  s_requests = xQueueCreate(4, sizeof(int));
  s_done = xSemaphoreCreateBinary();
}

// Waits until the LVGL loop has run the request
static void gui_request(gui_request_t request)
{
  if (s_requests == NULL) return;
  const int req = request;
  xQueueSend(s_requests, &req, portMAX_DELAY);
  xSemaphoreTake(s_done, portMAX_DELAY);
}

void Gui_requestTouchCalibrate(void) { gui_request(GUI_REQ_TOUCH_CAL); }
void Gui_requestTouchTest(void)      { gui_request(GUI_REQ_TOUCH_TEST); }
void Gui_requestBenchmark(void)      { gui_request(GUI_REQ_BENCHMARK); }
void Gui_requestUiPerf(bool on)      { gui_request(on ? GUI_REQ_PERF_ON : GUI_REQ_PERF_OFF); }

void Gui_processRequests(void)
{
  int req;
  while (s_requests != NULL && xQueueReceive(s_requests, &req, 0) == pdTRUE) {
    switch (req) {
      case GUI_REQ_TOUCH_CAL:  touch_calibrate();   break;
      case GUI_REQ_TOUCH_TEST: touch_test_10s();    break;
      case GUI_REQ_BENCHMARK:  benchmark_start();   break;
      case GUI_REQ_PERF_ON:    uiperf_set(true);    break;
      case GUI_REQ_PERF_OFF:   uiperf_set(false);   break;
      default: break;
    }
    xSemaphoreGive(s_done);
  }
}
