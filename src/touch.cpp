#include "touch.h"

#include <Arduino.h>
#include <TouchDrv.hpp>
#include <Wire.h>

#include "board_config.h"

namespace {

TouchDrvCST92xx s_touch;
uint8_t s_address = 0;

lv_indev_drv_t s_indev_drv;
lv_point_t s_last_point = {PUCK_LCD_WIDTH / 2, PUCK_LCD_HEIGHT / 2};
bool s_pressed = false;

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
    s_last_point.x = static_cast<lv_coord_t>(point.x);
    s_last_point.y = static_cast<lv_coord_t>(point.y);
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
  s_touch.setMirrorXY(PUCK_TOUCH_MIRROR_X, PUCK_TOUCH_MIRROR_Y);

  lv_indev_drv_init(&s_indev_drv);
  s_indev_drv.type = LV_INDEV_TYPE_POINTER;
  s_indev_drv.read_cb = indev_read_cb;
  lv_indev_drv_register(&s_indev_drv);

  Serial.printf("[touch] %s at 0x%02X, %u points, mirror x=%d y=%d\n", s_touch.getModelName(),
                s_address, static_cast<unsigned>(s_touch.getSupportTouchPoint()),
                PUCK_TOUCH_MIRROR_X, PUCK_TOUCH_MIRROR_Y);
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
