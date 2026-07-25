#include "touch.h"

#include <Arduino.h>
#include <math.h>
#include <TouchDrv.hpp>
#include <Wire.h>

#include "board_config.h"

namespace {

TouchDrvCST92xx s_touch;
uint8_t s_address = 0;

lv_indev_drv_t s_indev_drv;
lv_point_t s_last_point = {PUCK_LCD_WIDTH / 2, PUCK_LCD_HEIGHT / 2};
bool s_pressed = false;
uint8_t s_rotation = 0;
// Cosine and sine of the *negative* fine angle, so the transform is the inverse of
// the one applied to the display. Precomputed: this runs on every touch report.
float s_fine_cos = 1.0f;
float s_fine_sin = 0.0f;
bool s_fine_active = false;

const char* i2c_device_name(uint8_t address) {
  switch (address) {
    case PUCK_TOUCH_ADDR_PRIMARY:
      return "CST9217 touch";
    case PUCK_TOUCH_ADDR_ALT:
      return "CST9217 touch (alternate address)";
    case PUCK_I2C_ADDR_ES8311:
      return "ES8311 audio codec";
    case PUCK_I2C_ADDR_TCA9554:
      return "TCA9554 I/O expander";
    case PUCK_I2C_ADDR_AXP2101:
      return "AXP2101 PMIC";
    case PUCK_I2C_ADDR_LC76G_GPS:
      return "LC76G GPS";
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
// before this reports no touch device at all. Idempotent: touch_begin() resets
// the part again through SensorLib.
void ensure_reset_released() {
  static bool done = false;
  if (done) {
    return;
  }
  pinMode(PUCK_TOUCH_RST, OUTPUT);
  digitalWrite(PUCK_TOUCH_RST, LOW);
  delay(30);
  digitalWrite(PUCK_TOUCH_RST, HIGH);
  delay(50);
  done = true;
}

// Note on a log line you will see here: on every finger-lift SensorLib prints
//   [E] getPoint(): Invalid touch point index: 0
// That is upstream, not us. TouchDrvCST92xx::getTouchPoints() calls
// getPoint(0) unconditionally after its parse loop, but a release report
// carries event 0x00 rather than 0x06 so nothing was added to the set. It
// logs, returns an empty set, and the behaviour is correct. Harmless.
void indev_read_cb(lv_indev_drv_t* /*drv*/, lv_indev_data_t* data) {
  const TouchPoints& points = s_touch.getTouchPoints();
  if (points.hasPoints()) {
    const TouchPoint& point = points.getPoint(0);

    // The touch layer is mounted 180 degrees round from the panel's scan order,
    // so start by undoing that; this reproduces the setMirrorXY(true, true) that
    // was verified on hardware at rotation 0.
    // Only the mounting correction belongs here. LVGL rotates pointer coordinates
    // itself from disp->driver->rotated (lv_indev.c), so applying the display
    // rotation here as well turned every swipe the wrong way.
    lv_coord_t x = PUCK_LCD_WIDTH - 1 - static_cast<lv_coord_t>(point.x);
    lv_coord_t y = PUCK_LCD_HEIGHT - 1 - static_cast<lv_coord_t>(point.y);

    // Undo the fine rotation about the centre of the panel. Rotations about the
    // same point commute, so it does not matter that LVGL applies its own quarter
    // turn after this.
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

  // LVGL wants the last known position on release too, not a jump to 0,0.
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

  const uint8_t candidates[] = {PUCK_TOUCH_ADDR_PRIMARY, PUCK_TOUCH_ADDR_ALT};
  s_touch.setPins(PUCK_TOUCH_RST, PUCK_TOUCH_INT);

  for (uint8_t address : candidates) {
    if (!i2c_responds(address)) {
      continue;
    }
    // No pins passed: main() already called Wire.begin(), and handing SensorLib
    // the pins again makes it call Wire.setPins() on a live bus, which logs an
    // error and changes nothing.
    if (!s_touch.begin(Wire, address)) {
      Serial.printf("[touch] 0x%02X answered the bus but would not initialise\n", address);
      continue;
    }
    s_address = address;
    break;
  }

  if (s_address == 0) {
    Serial.println("[touch] no CST9217 found at 0x5A or 0x15 — display only");
    return false;
  }

  s_touch.setMaxCoordinates(PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  // Mirroring is applied here rather than in the library, because it has to
  // compose with the display rotation and the library only knows about itself.
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
