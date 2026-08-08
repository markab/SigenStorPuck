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
  // -1 when unknown. Also -1 when the gauge register reads zero against a healthy
  // voltage, because that is the gauge saying nothing rather than a flat battery.
  int percent = -1;
  uint16_t millivolts = 0;
};

// Wire.begin() must already have been called; this joins the shared bus.
void power_begin();

// Cheap: reads the PMIC over I2C, so call it at a human rate rather than per
// frame.
PowerStatus power_status();

// Services the PMIC's power-key interrupts. Call from loop().
void power_loop();

// Whether the power key is held down right now.
//
// The PWR button is not a GPIO — it reaches the AXP2101's PWRON pin — so this is
// reconstructed from the PMIC's press and release edge interrupts. Reported as a
// level rather than as events so buttons.cpp can time it exactly like the BOOT
// pin it can read directly, and one gesture recogniser serves both.
bool power_key_down();

// Cuts every rail. Only meaningful with a battery fitted: on USB alone the device
// either comes straight back or sits dark, which reads as a fault rather than as
// "off".
void power_shutdown();
