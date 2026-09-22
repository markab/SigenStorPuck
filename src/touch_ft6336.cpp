// FT6336 (FocalTech) capacitive touch over the shared I2C bus, wired to LVGL —
// the 2.41" landscape board (V1). The puck241 counterpart of touch.cpp (CST9217),
// implementing the same touch.h contract and swapped in by build_src_filter.
//
// On V1: TP_RESET = GPIO3 (a real GPIO), TP_INT = EXIO2 (unused; we poll). The
// controller holds its I2C lines idle until reset is released, so ensure_reset_
// released() pulses GPIO3 before the boot-time bus scan or nothing answers.
//
// The panel is shown landscape via a software rotation in the display backend,
// which keeps disp_drv.rotated at NONE (LVGL would otherwise swap the resolution,
// see display_sh8601.cpp). So LVGL does not rotate pointer input — indev_read_cb
// does, mapping the FT6336's native-portrait reading into the logical landscape
// frame. If taps land mirrored, the fix is PUCK_TOUCH_MIRROR_X/Y in board_2p41.h.

#include "touch.h"

#include <Arduino.h>
#include <math.h>
#include <TouchDrvFT6X36.hpp>
#include <Wire.h>

#include "board_config.h"

namespace {

TouchDrvFT6X36 s_touch;
uint8_t s_address = 0;

lv_indev_drv_t s_indev_drv;
lv_point_t s_last_point = {PUCK_LCD_WIDTH / 2, PUCK_LCD_HEIGHT / 2};
bool s_pressed = false;
uint8_t s_rotation = 0;
float s_fine_cos = 1.0f;
float s_fine_sin = 0.0f;
bool s_fine_active = false;

const char* i2c_device_name(uint8_t address) {
  switch (address) {
    case PUCK_TOUCH_ADDR_PRIMARY:
      return "FT6336 touch";
    case PUCK_I2C_ADDR_TCA9554:
      return "TCA9554 I/O expander";
    case PUCK_I2C_ADDR_PCF85063:
      return "PCF85063 RTC";
    case PUCK_I2C_ADDR_QMI8658_L:
    case PUCK_I2C_ADDR_QMI8658_H:
      return "QMI8658 IMU";
    default:
      return "unrecognised";
  }
}

bool i2c_responds(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

// The controller holds its I2C lines idle until reset is released, so a scan run
// before this reports no touch device. On V1 reset is GPIO3, a real GPIO.
void ensure_reset_released() {
  static bool done = false;
  if (done) {
    return;
  }
  if (PUCK_TOUCH_RST >= 0) {
    pinMode(PUCK_TOUCH_RST, OUTPUT);
    digitalWrite(PUCK_TOUCH_RST, LOW);
    delay(30);
    digitalWrite(PUCK_TOUCH_RST, HIGH);
    delay(50);
  }
  done = true;
}

void indev_read_cb(lv_indev_drv_t* /*drv*/, lv_indev_data_t* data) {
  const TouchPoints& points = s_touch.getTouchPoints();
  if (points.hasPoints()) {
    const TouchPoint& point = points.getPoint(0);

    // The FT6336 reports in native portrait (0..449 x 0..599). The display driver
    // leaves disp_drv.rotated at NONE on this board (LVGL would otherwise swap the
    // resolution), so LVGL does not rotate the pointer — we do it here to match the
    // display's software rotation of 270 degrees: native (nx, ny) -> logical
    // (NH-1-ny, nx). The mount mirror flips (board profile) compose on top, for
    // squaring touch to the glass once seen on hardware.
    lv_coord_t x = static_cast<lv_coord_t>(PUCK_LCD_NATIVE_HEIGHT - 1 - point.y);
    lv_coord_t y = static_cast<lv_coord_t>(point.x);
    if (PUCK_TOUCH_MIRROR_X) {
      x = static_cast<lv_coord_t>(PUCK_LCD_WIDTH - 1 - x);
    }
    if (PUCK_TOUCH_MIRROR_Y) {
      y = static_cast<lv_coord_t>(PUCK_LCD_HEIGHT - 1 - y);
    }

    // Undo the fine rotation about the centre of the panel.
    if (s_fine_active) {
      const float cx = PUCK_LCD_WIDTH / 2.0f;
      const float cy = PUCK_LCD_HEIGHT / 2.0f;
      const float ox = x - cx;
      const float oy = y - cy;
      x = static_cast<lv_coord_t>(lroundf(cx + ox * s_fine_cos - oy * s_fine_sin));
      y = static_cast<lv_coord_t>(lroundf(cy + ox * s_fine_sin + oy * s_fine_cos));
    }

    s_last_point.x = x;
    s_last_point.y = y;
    s_pressed = true;
  } else {
    s_pressed = false;
  }

  data->point = s_last_point;
  data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace

uint8_t touch_scan_i2c() {
  ensure_reset_released();

  uint8_t found = 0;
  Serial.printf("[i2c] scanning SDA=%d SCL=%d\n", PUCK_I2C_SDA, PUCK_I2C_SCL);
  for (uint8_t address = 0x08; address < 0x78; ++address) {
    if (i2c_responds(address)) {
      Serial.printf("[i2c]   0x%02X  %s\n", address, i2c_device_name(address));
      ++found;
    }
  }
  if (found == 0) {
    Serial.println("[i2c]   nothing responded — check the bus pins");
  }
  return found;
}

bool touch_begin() {
  ensure_reset_released();

  s_touch.setPins(PUCK_TOUCH_RST, PUCK_TOUCH_INT);

  if (!i2c_responds(PUCK_TOUCH_ADDR_PRIMARY)) {
    Serial.printf("[touch] no FT6336 at 0x%02X — display only\n", PUCK_TOUCH_ADDR_PRIMARY);
    return false;
  }
  // No pins passed to begin(): main() already called Wire.begin(), and handing
  // SensorLib the pins again makes it re-init a live bus.
  if (!s_touch.begin(Wire, PUCK_TOUCH_ADDR_PRIMARY)) {
    Serial.printf("[touch] 0x%02X answered the bus but would not initialise\n",
                  PUCK_TOUCH_ADDR_PRIMARY);
    return false;
  }
  s_address = PUCK_TOUCH_ADDR_PRIMARY;

  // Native portrait, matching what the controller reports; indev_read_cb rotates
  // the point into the logical landscape frame (LVGL does not, see the header).
  s_touch.setMaxCoordinates(PUCK_LCD_NATIVE_WIDTH, PUCK_LCD_NATIVE_HEIGHT);
  // Mirroring is applied in indev_read_cb from the board profile, not in the
  // library, because it has to compose with the software rotation.
  s_touch.setMirrorXY(false, false);

  lv_indev_drv_init(&s_indev_drv);
  s_indev_drv.type = LV_INDEV_TYPE_POINTER;
  s_indev_drv.read_cb = indev_read_cb;
  lv_indev_drv_register(&s_indev_drv);

  Serial.printf("[touch] %s at 0x%02X, %u points, rotation %u\n", s_touch.getModelName(),
                s_address, static_cast<unsigned>(s_touch.getSupportTouchPoint()),
                static_cast<unsigned>(s_rotation));
  return true;
}

uint8_t touch_address() {
  return s_address;
}

bool touch_pressed(lv_point_t* point) {
  if (point != nullptr) {
    *point = s_last_point;
  }
  return s_pressed;
}

void touch_set_orientation(uint8_t rotation) {
  s_rotation = rotation & 0x03;
}

void touch_set_fine_rotation(int16_t tenths_of_a_degree) {
  s_fine_active = tenths_of_a_degree != 0;
  const float radians = -static_cast<float>(tenths_of_a_degree) / 10.0f * 3.14159265f / 180.0f;
  s_fine_cos = cosf(radians);
  s_fine_sin = sinf(radians);
}
