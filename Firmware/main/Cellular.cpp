#include "Cellular.h"
#include "Config.h"

#include <string.h>
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The game is drawn in an LVGL canvas (was drawn directly with TFT_eSPI)
#define CANVAS_W (GRIDX * CELLXY)   // 480
#define CANVAS_H (GRIDY * CELLXY)   // 318

#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF

static lv_obj_t *canvas = NULL;
static uint16_t *canvas_buf = NULL;   // RGB565, in PSRAM

//Current grid
uint8_t grid[GRIDX][GRIDY];
//The new grid for the next generation
uint8_t newgrid[GRIDX][GRIDY];

//The generation or iteration we now are
uint16_t gen = 0;

#define InitialColor 0x1111
uint16_t actualColor;


void drawGrid(void);
void initGridText(const char* );
void computeCA();
int getNumberOfNeighbors(int, int);

// Equivalent of tft.fillRect(CELLXY * x, CELLXY * y, CELLXY, CELLXY, color)
static void fillCell(int16_t x, int16_t y, uint16_t color)
{
  for (int dy = 0; dy < CELLXY; dy++) {
    uint16_t *p = &canvas_buf[(y * CELLXY + dy) * CANVAS_W + x * CELLXY];
    for (int dx = 0; dx < CELLXY; dx++) p[dx] = color;
  }
}

static bool canvasInit(void)
{
  if (canvas_buf == NULL) {
    canvas_buf = (uint16_t *)heap_caps_malloc(CANVAS_W * CANVAS_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (canvas_buf == NULL) return false;
  }
  if (canvas == NULL) {
    canvas = lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(canvas, canvas_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(canvas, 0, 0);
  }
  return true;
}


void game(const char* text){
    if (gen == 0)
    {
        if (!canvasInit()) return;
        initGridText(text);
        actualColor = InitialColor;
        memset(canvas_buf, 0, CANVAS_W * CANVAS_H * sizeof(uint16_t));   // fillScreen(TFT_BLACK)

        //Force-render Generation 0
        for (int16_t x = 1; x < GRIDX - 1; x++) {
            for (int16_t y = 1; y < GRIDY - 1; y++) {
                if (grid[x][y] == 1) {
                    fillCell(x, y, COLOR_WHITE);
                }
                // CRITICAL: Set newgrid equal to grid so drawGrid doesn't see a "change"
                // and try to instantly wipe it out on the next frame.
                newgrid[x][y] = grid[x][y];
            }
        }
        lv_obj_invalidate(canvas);
        gen++;
    }
    else if(gen < 100)
    {
        vTaskDelay(pdMS_TO_TICKS(1));
        gen++;
    }
    else
    {
        //Compute generations
        computeCA();
        drawGrid();
        for (int16_t x = 1; x < GRIDX-1; x++)
        {
            for (int16_t y = 1; y < GRIDY-1; y++)
            {
                grid[x][y] = newgrid[x][y];
            }
        }
        gen++;
        if (gen >= NUMGEN) gen = 0;
    }
}

//Draws the grid on the display
void drawGrid(void) {
  uint16_t color = COLOR_WHITE;

  for (int16_t x = 1; x < GRIDX - 1; x++) {
    for (int16_t y = 1; y < GRIDY - 1; y++) {
      // Only draw if the cell state changed from the last generation
      if ((grid[x][y]) != (newgrid[x][y])) {
        // If the current cell is alive, color it. If dead (0), paint it black.
        if (newgrid[x][y] == 1) color = actualColor; //random(0xFFFF);
        else color = COLOR_BLACK;
        fillCell(x, y, color);
      }
    }
  }
  lv_obj_invalidate(canvas);
  actualColor = actualColor + 100;
}

//Initialise Grid
void initGridText(const char* message) {
  // 1. Fully clear both tracking arrays
  for (int16_t x = 0; x < GRIDX; x++) {
    for (int16_t y = 0; y < GRIDY; y++) {
      grid[x][y] = 0;
      newgrid[x][y] = 0;
    }
  }

  // 2. Setup boundaries inside the padding perimeter
  int16_t spriteW = GRIDX - 2;
  int16_t spriteY = GRIDY - 2;

  // Render the text in an 8-bit grey canvas (was a 1-bit TFT_eSprite)
  uint8_t *spr_buf = (uint8_t *)heap_caps_calloc(spriteW * spriteY, 1, MALLOC_CAP_SPIRAM);
  if (spr_buf == NULL) return;

  lv_obj_t *spr = lv_canvas_create(lv_screen_active());
  lv_obj_set_hidden(spr, true);
  lv_canvas_set_buffer(spr, spr_buf, spriteW, spriteY, LV_COLOR_FORMAT_L8);
  lv_canvas_fill_bg(spr, lv_color_black(), LV_OPA_COVER);

  lv_layer_t layer;
  lv_canvas_init_layer(spr, &layer);

  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.color = lv_color_white();
  dsc.font = &lv_font_montserrat_14;
  dsc.align = LV_TEXT_ALIGN_CENTER;
  dsc.text = message;

  int32_t lh = lv_font_get_line_height(dsc.font);
  lv_area_t area = {0, (spriteY - lh) / 2, spriteW - 1, (spriteY + lh) / 2};
  lv_draw_label(&layer, &dsc, &area);
  lv_canvas_finish_layer(spr, &layer);

  // 3. Map memory layout securely into CA framework
  uint32_t stride = lv_canvas_get_draw_buf(spr)->header.stride;
  for (int16_t sx = 0; sx < spriteW; sx++) {
    for (int16_t sy = 0; sy < spriteY; sy++) {
      // Anti-aliased font: keep the pixels brighter than 50%
      if (spr_buf[sy * stride + sx] > 127) {
        grid[sx + 1][sy + 1] = 1;
      }
    }
  }

  lv_obj_delete(spr);
  heap_caps_free(spr_buf);
}

//Compute the CA. Basically everything related to CA starts here
void computeCA() {
  // Stop 1 cell before the border: getNumberOfNeighbors() reads x+1 and y+1
  for (int16_t x = 1; x < GRIDX - 1; x++) {
    for (int16_t y = 1; y < GRIDY - 1; y++) {
      int neighbors = getNumberOfNeighbors(x, y);
      if (grid[x][y] == 1 && (neighbors == 2 || neighbors == 3 ))
      {
        newgrid[x][y] = 1;
      }
      else if (grid[x][y] == 1)  newgrid[x][y] = 0;
      if (grid[x][y] == 0 && (neighbors == 3))
      {
        newgrid[x][y] = 1;
      }
      else if (grid[x][y] == 0) newgrid[x][y] = 0;
    }
  }
}

// Check the Moore neighbourhood
int getNumberOfNeighbors(int x, int y) {
  return grid[x - 1][y] + grid[x - 1][y - 1] + grid[x][y - 1] + grid[x + 1][y - 1] + grid[x + 1][y] + grid[x + 1][y + 1] + grid[x][y + 1] + grid[x - 1][y + 1];
}


/*
   The MIT License (MIT)

   Copyright (c) 2016 RuntimeProjects.com

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

