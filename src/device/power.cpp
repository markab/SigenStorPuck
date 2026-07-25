#include "power.h"

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#include "board_config.h"

namespace {

XPowersAXP2101 s_pmic;
bool s_ready = false;

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
    status.percent = s_pmic.getBatteryPercent();
    status.millivolts = s_pmic.getBattVoltage();
  }
  return status;
}
