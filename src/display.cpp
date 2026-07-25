#include "display.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

#include "board_config.h"

namespace {

Arduino_DataBus* s_bus = nullptr;

// Typed as the concrete panel, not the Arduino_GFX base: setBrightness() only
// exists on the OLED subclass, and this board has no other way to dim.
Arduino_CO5300* s_panel = nullptr;

lv_disp_draw_buf_t s_draw_buf;
lv_disp_drv_t s_disp_drv;
lv_color_t* s_pixels = nullptr;
bool s_buffer_is_internal = false;

// The CO5300 only accepts even-aligned write windows. LVGL is happy to hand us
// odd areas, and the panel then renders them offset and torn, so widen every
// invalidated area to an even start and an odd end.
//
// This is safe with a partial buffer: LVGL's get_max_row() re-applies this
// callback when it splits an area into buffer-sized chunks, and shrinks the
// chunk height until the rounded result still fits.
void rounder_cb(lv_disp_drv_t* /*drv*/, lv_area_t* area) {
  area->x1 &= ~1;
  area->y1 &= ~1;
  area->x2 |= 1;
  area->y2 |= 1;
}

void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* pixels) {
  s_panel->draw16bitRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t*>(pixels),
                              area->x2 - area->x1 + 1, area->y2 - area->y1 + 1);
  lv_disp_flush_ready(drv);
}

}  // namespace

bool display_begin(uint8_t rotation) {
  s_bus = new Arduino_ESP32QSPI(PUCK_LCD_CS, PUCK_LCD_SCLK, PUCK_LCD_D0, PUCK_LCD_D1,
                                PUCK_LCD_D2, PUCK_LCD_D3);
  // Always 0 here. The CO5300's own rotation parameter drives MADCTL bits that
  // this panel interprets as an axis flip, so asking it for 90 degrees mirrors the
  // image instead of turning it. Waveshare's own LVGL example does not use it
  // either — it rotates in software, which is what happens below.
  s_panel = new Arduino_CO5300(s_bus, PUCK_LCD_RST, 0, PUCK_LCD_WIDTH,
                               PUCK_LCD_HEIGHT, PUCK_LCD_COL_OFFSET, PUCK_LCD_ROW_OFFSET, 0, 0);

  if (!s_panel->begin(PUCK_LCD_QSPI_HZ)) {
    Serial.println("[display] CO5300 begin() failed");
    return false;
  }
  s_panel->fillScreen(RGB565_BLACK);
  s_panel->setBrightness(PUCK_LCD_BRIGHTNESS);

  const size_t pixel_count = static_cast<size_t>(PUCK_LCD_WIDTH) * PUCK_LVGL_BUFFER_LINES;
  const size_t bytes = pixel_count * sizeof(lv_color_t);

  // DMA-capable internal RAM matters for QSPI throughput; PSRAM works but the
  // frame rate drops noticeably, so log which one we got.
  s_pixels = static_cast<lv_color_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  s_buffer_is_internal = s_pixels != nullptr;
  if (s_pixels == nullptr) {
    s_pixels = static_cast<lv_color_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
  }
  if (s_pixels == nullptr) {
    Serial.printf("[display] could not allocate a %u-byte draw buffer\n",
                  static_cast<unsigned>(bytes));
    return false;
  }

  // Single-buffered partial rendering.
  lv_disp_draw_buf_init(&s_draw_buf, s_pixels, nullptr, pixel_count);

  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = PUCK_LCD_WIDTH;
  s_disp_drv.ver_res = PUCK_LCD_HEIGHT;
  s_disp_drv.draw_buf = &s_draw_buf;
  s_disp_drv.flush_cb = flush_cb;
  s_disp_drv.rounder_cb = rounder_cb;
  // Software rotation, per Waveshare's 06_LVGL_Widgets sketch. Costs some CPU in
  // the flush path but actually rotates rather than mirroring.
  s_disp_drv.sw_rotate = 1;
  lv_disp_t* disp = lv_disp_drv_register(&s_disp_drv);

  switch (rotation & 0x03) {
    case 1: lv_disp_set_rotation(disp, LV_DISP_ROT_90); break;
    case 2: lv_disp_set_rotation(disp, LV_DISP_ROT_180); break;
    case 3: lv_disp_set_rotation(disp, LV_DISP_ROT_270); break;
    default: lv_disp_set_rotation(disp, LV_DISP_ROT_NONE); break;
  }

  Serial.printf("[display] CO5300 %dx%d up, %u-byte buffer (%u lines) in %s\n", PUCK_LCD_WIDTH,
                PUCK_LCD_HEIGHT, static_cast<unsigned>(bytes),
                static_cast<unsigned>(PUCK_LVGL_BUFFER_LINES),
                s_buffer_is_internal ? "internal DMA RAM" : "PSRAM");
  return true;
}

void display_set_brightness(uint8_t level) {
  if (s_panel != nullptr) {
    s_panel->setBrightness(level);
  }
}

bool display_buffer_is_internal() {
  return s_buffer_is_internal;
}
