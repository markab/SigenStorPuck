// RM690B0 AMOLED panel over QSPI, wired to LVGL — the 2.41" landscape board (V1).
//
// The puck241 counterpart of display.cpp (which drives the 1.75's CO5300). Both
// implement the same display.h contract and are swapped by build_src_filter per
// env, the way src/sim/ swaps the desktop backend.
//
// Pins are the real V1 values (board_2p41.h, from Waveshare's own V1 demo). The
// controller is an RM690B0, but it is command-compatible with the SH8601 and
// Waveshare's own examples wrap it with their SH8601 driver, so Arduino_GFX's
// Arduino_SH8601 drives it here too.
//
// Bring-up facts confirmed on real V1 hardware:
//   * OLED reset is GPIO21, a real GPIO. (On the V2 revision it is an expander
//     line and GPIO21 is the tearing-effect signal — the two are swapped between
//     revisions.) Without pulsing it the panel never leaves reset and stays dark
//     however correct the init and transport are.
//   * The stock Arduino_SH8601 init does not light this panel; it needs a software
//     reset, a leading MADCTL + COLMOD, and the panel's page-0x20 vendor block —
//     apply_sh8601_vendor_init() sends the demo's exact sequence instead.
//   * The panel is natively 450x600 portrait with a 16-pixel column offset in the
//     RM690B0's 482-wide RAM. It is constructed at that native size
//     (PUCK_LCD_NATIVE_*); the landscape 600x450 view comes from the flush_cb
//     software rotation the 1.75 uses for mounting turns. Hardware rotation
//     (MADCTL swap) is not usable — Arduino_GFX streams pixels row-major, which a
//     swap transposes.

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

// QSPI AMOLED controllers only accept even-aligned write windows; LVGL hands us
// odd areas otherwise. Confirmed needed on the RM690B0 here.
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

// Reproduce the V1 demo's init sequence (which wraps the RM690B0 as
// esp_lcd_sh8601). The stock Arduino_SH8601 init leaves out three things this
// panel needs — a software reset, a leading MADCTL + COLMOD, and the panel's
// page-0x20 block — and without them it comes up out of reset but dark. Called
// straight after the hardware reset, matching the demo's panel_reset() then
// panel_init() order. MADCTL is 0x00 (no hardware rotate): rotation is done in
// flush_cb and the draw window is set per flush, so the demo's 0x36 rotate and
// its CASET/PASET are deliberately left out.
void apply_sh8601_vendor_init() {
  s_bus->sendCommand(0x01);      // SWRESET (the demo's panel_reset with rst_gpio < 0)
  delay(80);

  s_bus->beginWrite();
  s_bus->writeC8D8(0x36, 0x00);  // MADCTL: RGB, no hardware rotate (flush_cb rotates)
  s_bus->writeC8D8(0x3A, 0x55);  // COLMOD: 16 bit/pixel (RGB565)
  s_bus->writeC8D8(0xFE, 0x20);  // select command page 0x20
  s_bus->writeC8D8(0x26, 0x0A);  // panel power-up config the stock init skips
  s_bus->writeC8D8(0x24, 0x80);
  s_bus->writeC8D8(0xFE, 0x00);  // back to command page 0
  s_bus->writeC8D8(0x3A, 0x55);
  s_bus->writeC8D8(0xC2, 0x00);
  s_bus->endWrite();
  delay(10);

  s_bus->beginWrite();
  s_bus->writeC8D8(0x35, 0x00);  // tearing-effect line on
  s_bus->writeC8D8(0x51, 0x00);  // brightness 0 for now; raised after DISPON
  s_bus->endWrite();
  delay(10);

  s_bus->sendCommand(0x11);  // sleep out
  delay(80);
  s_bus->sendCommand(0x29);  // display on
  delay(10);

  s_bus->beginWrite();
  s_bus->writeC8D8(0x51, 0xFF);  // brightness up now the panel is on
  s_bus->endWrite();
}

}  // namespace

bool display_begin(uint8_t rotation) {
  s_bus = new Arduino_ESP32QSPI(PUCK_LCD_CS, PUCK_LCD_SCLK, PUCK_LCD_D0, PUCK_LCD_D1,
                                PUCK_LCD_D2, PUCK_LCD_D3);
  // Constructed at the native portrait size; flush_cb rotates the logical
  // landscape frame onto it. col_offset lands the visible band at native column
  // 16 (Arduino_GFX adds it to x in writeAddrWindow).
  s_panel = new Arduino_SH8601(s_bus, PUCK_LCD_RST, 0, PUCK_LCD_NATIVE_WIDTH,
                               PUCK_LCD_NATIVE_HEIGHT, PUCK_LCD_COL_OFFSET, PUCK_LCD_ROW_OFFSET, 0, 0);

  if (!s_panel->begin(PUCK_LCD_QSPI_HZ)) {
    Serial.println("[display] SH8601 begin() failed");
    return false;
  }
  // Hardware reset on GPIO21 (a real GPIO on V1), active-low, with the demo's
  // timing — then the vendor init in the demo's order. begin() ran the stock init
  // already; this reset discards it so the vendor sequence starts from a clean
  // controller.
  pinMode(PUCK_LCD_RST, OUTPUT);
  digitalWrite(PUCK_LCD_RST, HIGH);
  delay(10);
  digitalWrite(PUCK_LCD_RST, LOW);
  delay(10);
  digitalWrite(PUCK_LCD_RST, HIGH);
  delay(150);
  apply_sh8601_vendor_init();
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

  // The panel is physically portrait, so its baseline is a 270-degree turn to
  // reach the product's landscape (which came out upright on the glass); any
  // runtime mounting turn composes on top.
  s_rotation = static_cast<uint8_t>((PUCK_LCD_ROTATION + rotation) & 0x03);
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
  s_disp_drv.sw_rotate = 0;
  // Deliberately NOT rotated through LVGL. On a non-square panel LV_DISP_ROT_90/270
  // makes lv_disp_get_hor_res() return ver_res, so LVGL would treat the screen as
  // 450x600 and lay the 600x450 UI out off-centre in a sub-window. flush_cb does
  // the pixel rotation, so LVGL stays at the true 600x450; touch is rotated in
  // touch_ft6336.cpp instead of by LVGL.
  s_disp_drv.rotated = LV_DISP_ROT_NONE;
  lv_disp_drv_register(&s_disp_drv);

  Serial.printf("[display] rotation %u%s\n", static_cast<unsigned>(s_rotation),
                s_rotation != 0 ? " (rotated in flush)" : "");
  Serial.printf("[display] RM690B0 %dx%d up, %u-byte buffer (%u lines) in %s\n", PUCK_LCD_WIDTH,
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
