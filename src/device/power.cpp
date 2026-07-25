#include "power.h"

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#include "board_config.h"

namespace {

XPowersAXP2101 s_pmic;
bool s_ready = false;
bool s_short_press = false;

}  // namespace

void power_begin() {
  // -1 for the pins: main() already called Wire.begin(), and handing the library
  // the pins again makes it re-init a live bus, which logs an error and changes
  // nothing (the same trap as the touch driver).
  if (!s_pmic.begin(Wire, AXP2101_SLAVE_ADDRESS, -1, -1)) {
    Serial.println("[power] AXP2101 not responding");
    return;
  }

  // Without these the battery readings come back as zero rather than as an
  // obvious failure, which is worse than not reading them at all.
  s_pmic.enableBattDetection();
  s_pmic.enableBattVoltageMeasure();
  s_pmic.enableVbusVoltageMeasure();
  s_pmic.enableSystemVoltageMeasure();

  // Power-key events. A long press shuts down; a short press is ours to use.
  s_pmic.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  s_pmic.clearIrqStatus();
  s_pmic.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);

  s_ready = true;

  const PowerStatus status = power_status();
  Serial.printf("[power] AXP2101 up: usb=%s battery=%s", status.usb_present ? "yes" : "no",
                status.battery_present ? "yes" : "no");
  if (status.battery_present) {
    Serial.printf(" %d%% %umV%s", status.percent, status.millivolts,
                  status.charging ? " charging" : "");
  }
  Serial.println();
}

PowerStatus power_status() {
  PowerStatus status;
  if (!s_ready) {
    return status;
  }
  status.pmic_ok = true;
  status.battery_present = s_pmic.isBatteryConnect();
  status.usb_present = s_pmic.isVbusIn();
  status.charging = s_pmic.isCharging();
  if (status.battery_present) {
    const int percent = s_pmic.getBatteryPercent();
    status.millivolts = s_pmic.getBattVoltage();
    // The AXP2101's percentage is a plain register read with no calibration behind
    // it, and on this board it reports 0 while the cell sits at over 4 V. A zero
    // against a healthy voltage is the gauge declining to answer, not a flat
    // battery, so it is reported as unknown and the voltage is shown instead.
    // Anything above 3.3 V is nowhere near empty.
    const bool gauge_credible = percent > 0 || status.millivolts < 3300;
    status.percent = gauge_credible ? percent : -1;
  }
  return status;
}

void power_loop() {
  if (!s_ready) {
    return;
  }
  // Polled rather than interrupt-driven: the PMIC's IRQ line is not broken out to
  // a pin we own, and at human button-press speeds polling is plenty.
  if (s_pmic.getIrqStatus() == 0) {
    return;
  }
  if (s_pmic.isPekeyShortPressIrq()) {
    s_short_press = true;
    Serial.println("[power] PWR short press");
  }
  if (s_pmic.isPekeyLongPressIrq()) {
    Serial.println("[power] PWR long press, shutting down");
    s_pmic.clearIrqStatus();
    delay(50);
    power_shutdown();
    return;
  }
  s_pmic.clearIrqStatus();
}

bool power_take_short_press() {
  const bool pressed = s_short_press;
  s_short_press = false;
  return pressed;
}

void power_shutdown() {
  if (s_ready) {
    s_pmic.shutdown();
  }
}
