// The AXP2101 PMIC: the Puck's own power state, not the house battery's.
//
// Worth keeping the distinction sharp — screen 2 is about a 16 kWh home battery,
// and this is about whether the display in your hand is plugged in.

#pragma once

#include <Arduino.h>

struct PowerStatus {
  bool pmic_ok = false;
  bool battery_present = false;
  bool usb_present = false;
  bool charging = false;
  // -1 when unknown, which is the normal answer with no battery fitted.
  int percent = -1;
  uint16_t millivolts = 0;
};

// Wire.begin() must already have been called; this joins the shared bus.
void power_begin();

// Cheap: reads the PMIC over I2C, so call it at a human rate rather than per
// frame.
PowerStatus power_status();
