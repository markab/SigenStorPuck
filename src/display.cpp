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

// Rotation is done here, on the way to the panel, rather than by LVGL's sw_rotate.
// sw_rotate rotates each rendered fragment but leaves the area rectangle
// describing the unrotated one, so a partial buffer feeds the panel a block of
// h x w against a rect of w x h — every row walks sideways and the screen shears
// diagonally. Transforming in the flush keeps the 40-line buffer, needs no
// full_refresh, and touches nothing in the boot path.
uint8_t s_rotation = 0;
lv_color_t* s_rotated = nullptr;

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
  const int32_t w = area->x2 - area->x1 + 1;
  const int32_t h = area->y2 - area->y1 + 1;

  // Unrotated is the untouched fast path: straight out of the DMA-capable buffer.
  if (s_rotation == 0 || s_rotated == nullptr) {
    s_panel->draw16bitRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t*>(pixels), w, h);
    lv_disp_flush_ready(drv);
    return;
  }

  // A quarter turn swaps the block's dimensions; a half turn does not.
  const bool quarter = s_rotation == 1 || s_rotation == 3;
  const int32_t dst_w = quarter ? h : w;

  for (int32_t sy = 0; sy < h; ++sy) {
    const lv_color_t* src_row = pixels + sy * w;
    for (int32_t sx = 0; sx < w; ++sx) {
      int32_t dx = 0;
      int32_t dy = 0;
      switch (s_rotation) {
        case 1:  // 90 clockwise
          dx = h - 1 - sy;
          dy = sx;
          break;
        case 2:  // 180
          dx = w - 1 - sx;
          dy = h - 1 - sy;
          break;
        default:  // 3, 270 clockwise
          dx = sy;
          dy = w - 1 - sx;
          break;
      }
      s_rotated[dy * dst_w + dx] = src_row[sx];
    }
  }

  // Where that block lands on the physical panel. Even/odd alignment survives the
  // transform, so the 2-pixel rounding the CO5300 needs still holds.
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

  s_panel->draw16bitRGBBitmap(px, py, reinterpret_cast<uint16_t*>(s_rotated), dst_w,
                              quarter ? w : h);
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

  // A 40-line slice, always. Reverted from a full-frame + full_refresh buffer for
  // rotated displays: that combination stopped the device booting at all — it
  // panicked before USB-CDC could enumerate, so it could not even be diagnosed
  // without a download-mode recovery. LVGL 8's sw_rotate appears to want a second
  // buffer to rotate into when full_refresh is set, which this did not provide.
  //
  // So rotated output is mildly garbled again rather than dead. That is the better
  // failure of the two, and the real fix belongs with rotating in the flush
  // callback, which keeps partial buffers and needs no full_refresh at all.
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

  s_rotation = rotation & 0x03;
  if (s_rotation != 0) {
    // Somewhere to rotate each block into. Same size as the draw buffer, and only
    // allocated when it is actually needed.
    s_rotated = static_cast<lv_color_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL));
    if (s_rotated == nullptr) {
      s_rotated = static_cast<lv_color_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    }
    if (s_rotated == nullptr) {
      // Better to run unrotated than not at all.
      Serial.println("[display] no room for a rotation buffer, staying at 0");
      s_rotation = 0;
    }
  }

  // Single-buffered partial rendering.
  lv_disp_draw_buf_init(&s_draw_buf, s_pixels, nullptr, pixel_count);

  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = PUCK_LCD_WIDTH;
  s_disp_drv.ver_res = PUCK_LCD_HEIGHT;
  s_disp_drv.draw_buf = &s_draw_buf;
  s_disp_drv.flush_cb = flush_cb;
  s_disp_drv.rounder_cb = rounder_cb;
  // sw_rotate stays off: flush_cb does the rotation. `rotated` is still set,
  // because LVGL uses it to rotate touch coordinates (lv_indev.c) — exactly the
  // half we want from it.
  //
  // Note 90 and 270 are deliberately swapped here, and this is not a typo.
  // flush_cb maps an LVGL point to a panel point with some transform R. A physical
  // touch needs the inverse, R-inverse, to get back to LVGL space — but LVGL
  // applies R. At 0 and 180 R is its own inverse so it happens to be right; at 90
  // and 270 it is not, and swipes came out reversed. R-inverse(90) is R(270), so
  // handing LVGL the opposite quarter turn is what makes touch agree with what is
  // on the glass. Verified against lv_indev.c for every rotation.
  s_disp_drv.sw_rotate = 0;
  s_disp_drv.rotated = s_rotation == 1   ? LV_DISP_ROT_270
                       : s_rotation == 2 ? LV_DISP_ROT_180
                       : s_rotation == 3 ? LV_DISP_ROT_90
                                         : LV_DISP_ROT_NONE;
  lv_disp_drv_register(&s_disp_drv);

  Serial.printf("[display] rotation %u%s\n", static_cast<unsigned>(s_rotation),
                s_rotation != 0 ? " (rotated in flush)" : "");
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
