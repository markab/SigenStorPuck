// SH8601 AMOLED panel over QSPI, wired to LVGL — the 2.41" (V2) landscape board.
//
// The puck241 counterpart of display.cpp (which drives the 1.75's CO5300). Both
// implement the same display.h contract and are swapped by build_src_filter per
// env, the way src/sim/ swaps the desktop backend. The structure mirrors
// display.cpp so the two stay easy to diff; only the panel class and the
// controller-specific notes differ.
//
// Pins are the real V2 values (board_2p41.h, from Waveshare's 09_LVGL_Test).
//
// !!! TWO HARDWARE BRING-UP ITEMS — confirm on real glass !!!
//   * RESET IS ON THE TCA9554 EXPANDER, NOT A GPIO. PUCK_LCD_RST is -1, so
//     Arduino_SH8601 will not pulse reset. The panel needs a reset pulse driven
//     through the TCA9554 (0x20) before init — see the TODO in display_begin().
//     Without it the panel may not come up.
//   * The SH8601 is natively 450x600 portrait; the 2.41 shows 600x450 landscape.
//     The construction below uses the landscape (logical) size directly, with
//     turning left to flush_cb like the 1.75. Whether the panel instead needs a
//     native-portrait construction plus the controller's own rotation is a
//     bring-up question — Waveshare's example rotates 90 degrees.

#include "display.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

#include "board_config.h"
#include "display_rotation.h"
#include "ui/ui_perf.h"

namespace {

Arduino_DataBus* s_bus = nullptr;

// Typed as the concrete panel, not the Arduino_GFX base: setBrightness() only
// exists on the OLED subclass, and this board has no PWM backlight either.
Arduino_SH8601* s_panel = nullptr;

lv_disp_draw_buf_t s_draw_buf;
lv_disp_drv_t s_disp_drv;
lv_color_t* s_pixels = nullptr;
bool s_buffer_is_internal = false;
bool s_asleep = false;

// Rotation is done here, on the way to the panel, rather than by LVGL's sw_rotate
// — same reasoning as display.cpp (sw_rotate shears a partial buffer).
uint8_t s_rotation = 0;
lv_color_t* s_rotated = nullptr;

// Most QSPI AMOLED controllers only accept even-aligned write windows; LVGL hands
// us odd areas otherwise. VERIFY whether the SH8601 needs this on the 2.41.
void rounder_cb(lv_disp_drv_t* /*drv*/, lv_area_t* area) {
  area->x1 &= ~1;
  area->y1 &= ~1;
  area->x2 |= 1;
  area->y2 |= 1;
}

void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* pixels) {
  const int32_t w = area->x2 - area->x1 + 1;
  const int32_t h = area->y2 - area->y1 + 1;
  const uint32_t pixel_count = static_cast<uint32_t>(w * h);
  const bool frame_end = lv_disp_flush_is_last(drv);

  if (s_rotation == 0 || s_rotated == nullptr) {
    const uint32_t panel_started = ui_perf_now_us();
    s_panel->draw16bitRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t*>(pixels), w, h);
    const uint32_t panel_us = ui_perf_now_us() - panel_started;
    ui_perf_flush(s_rotation, pixel_count, 0, panel_us, frame_end);
    lv_disp_flush_ready(drv);
    return;
  }

  const bool quarter = s_rotation == 1 || s_rotation == 3;
  const int32_t dst_w = quarter ? h : w;
  const uint32_t rotation_started = ui_perf_now_us();
  display_rotate_rgb565(s_rotation, reinterpret_cast<uint16_t*>(pixels), w, h,
                        reinterpret_cast<uint16_t*>(s_rotated));
  const uint32_t rotation_us = ui_perf_now_us() - rotation_started;

  int32_t px = 0;
  int32_t py = 0;
  switch (s_rotation) {
    case 1:
      px = PUCK_LCD_HEIGHT - 1 - area->y2;
      py = area->x1;
      break;
    case 2:
      px = PUCK_LCD_WIDTH - 1 - area->x2;
      py = PUCK_LCD_HEIGHT - 1 - area->y2;
      break;
    default:
      px = area->y1;
      py = PUCK_LCD_WIDTH - 1 - area->x2;
      break;
  }

  const uint32_t panel_started = ui_perf_now_us();
  s_panel->draw16bitRGBBitmap(px, py, reinterpret_cast<uint16_t*>(s_rotated), dst_w,
                             quarter ? w : h);
  const uint32_t panel_us = ui_perf_now_us() - panel_started;
  ui_perf_flush(s_rotation, pixel_count, rotation_us, panel_us, frame_end);
  lv_disp_flush_ready(drv);
}

}  // namespace

bool display_begin(uint8_t rotation) {
  // TODO(hardware bring-up): pulse the panel reset line via the TCA9554 expander
  // (0x20) here before constructing the panel — PUCK_LCD_RST is -1 because reset
  // is not on a GPIO. The exact EXIO bit is a schematic detail to confirm on V2.

  s_bus = new Arduino_ESP32QSPI(PUCK_LCD_CS, PUCK_LCD_SCLK, PUCK_LCD_D0, PUCK_LCD_D1,
                                PUCK_LCD_D2, PUCK_LCD_D3);
  s_panel = new Arduino_SH8601(s_bus, PUCK_LCD_RST, 0, PUCK_LCD_WIDTH,
                               PUCK_LCD_HEIGHT, PUCK_LCD_COL_OFFSET, PUCK_LCD_ROW_OFFSET, 0, 0);

  if (!s_panel->begin(PUCK_LCD_QSPI_HZ)) {
    Serial.println("[display] SH8601 begin() failed");
    return false;
  }
  s_panel->fillScreen(RGB565_BLACK);
  s_panel->setBrightness(PUCK_LCD_BRIGHTNESS);

  const size_t pixel_count = static_cast<size_t>(PUCK_LCD_WIDTH) * PUCK_LVGL_BUFFER_LINES;
  const size_t bytes = pixel_count * sizeof(lv_color_t);

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

  s_rotation = rotation & 0x03;
  if (s_rotation != 0) {
    s_rotated = static_cast<lv_color_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL));
    if (s_rotated == nullptr) {
      s_rotated = static_cast<lv_color_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    }
    if (s_rotated == nullptr) {
      Serial.println("[display] no room for a rotation buffer, staying at 0");
      s_rotation = 0;
    }
  }

  lv_disp_draw_buf_init(&s_draw_buf, s_pixels, nullptr, pixel_count);

  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = PUCK_LCD_WIDTH;
  s_disp_drv.ver_res = PUCK_LCD_HEIGHT;
  s_disp_drv.draw_buf = &s_draw_buf;
  s_disp_drv.flush_cb = flush_cb;
  s_disp_drv.rounder_cb = rounder_cb;
  // sw_rotate off; flush_cb rotates. `rotated` is set so LVGL rotates touch
  // coordinates (lv_indev.c), with 90/270 swapped — see display.cpp for the full
  // inverse-transform argument, which applies to any panel driven this way.
  s_disp_drv.sw_rotate = 0;
  s_disp_drv.rotated = s_rotation == 1   ? LV_DISP_ROT_270
                       : s_rotation == 2 ? LV_DISP_ROT_180
                       : s_rotation == 3 ? LV_DISP_ROT_90
                                         : LV_DISP_ROT_NONE;
  lv_disp_drv_register(&s_disp_drv);

  Serial.printf("[display] rotation %u%s\n", static_cast<unsigned>(s_rotation),
                s_rotation != 0 ? " (rotated in flush)" : "");
  Serial.printf("[display] SH8601 %dx%d up, %u-byte buffer (%u lines) in %s\n", PUCK_LCD_WIDTH,
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

void display_set_sleep(bool asleep) {
  if (s_panel == nullptr || asleep == s_asleep) {
    return;
  }
  s_asleep = asleep;
  if (asleep) {
    s_panel->displayOff();
  } else {
    s_panel->displayOn();
    lv_obj_invalidate(lv_scr_act());
  }
  Serial.printf("[display] panel %s\n", asleep ? "asleep" : "awake");
}

bool display_asleep() {
  return s_asleep;
}

bool display_buffer_is_internal() {
  return s_buffer_is_internal;
}
