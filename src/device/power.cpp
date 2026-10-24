#include "power.h"

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
#include <esp_sleep.h>

#include "board_config.h"
#include "display.h"

namespace {

XPowersAXP2101 s_pmic;
bool s_ready = false;
bool s_key_down = false;
// A tap seen whole in one poll: the level stays down for one more call so the
// caller gets an edge rather than nothing.
bool s_release_pending = false;

}  // namespace

void power_hold() {
  // On a board with a battery latch (the 2.41), pressing PWR only switches the
  // battery through for as long as the key is held: until this pin goes high,
  // letting go powers the board straight back off. So it is the first thing
  // setup() does, ahead of the panel bring-up and everything else that takes
  // time. Harmless on USB, where the latch has nothing to switch.
  if (PUCK_POWER_LATCH >= 0) {
    pinMode(PUCK_POWER_LATCH, OUTPUT);
    digitalWrite(PUCK_POWER_LATCH, HIGH);
  }
}

void power_begin() {
  if (PUCK_BUTTON_PWR >= 0) {
    // No PMIC on this board: PWR is a plain active-low key (with its own external
    // pull-up), read as a level in power_loop(). Nothing to probe for.
    pinMode(PUCK_BUTTON_PWR, INPUT_PULLUP);
    Serial.printf("[power] no PMIC: PWR key on GPIO%d (%s), battery latch on GPIO%d held\n",
                  PUCK_BUTTON_PWR, digitalRead(PUCK_BUTTON_PWR) == LOW ? "down" : "up",
                  PUCK_POWER_LATCH);
    return;
  }

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

  // Press and release edges rather than the PMIC's own short/long classification.
  //
  // The chip can only be told to call a hold "long" at 1 s, 1.5 s or 2 s, and the
  // gestures this device wants — a single press, a double press and a three
  // second hold — do not fit that. Given the two edges, buttons.cpp times the
  // press itself and both buttons behave identically.
  s_pmic.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  s_pmic.clearIrqStatus();
  s_pmic.enableIRQ(XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ | XPOWERS_AXP2101_PKEY_POSITIVE_IRQ);

  // The PMIC will cut every rail on its own after a long enough hold, whatever
  // firmware thinks. Pushed out to the longest it offers so it cannot fire before
  // our own three-second shutdown does — otherwise the device would die at 4 s
  // with none of the tidying that goes with a deliberate power-off.
  s_pmic.setPowerKeyPressOffTime(XPOWERS_POWEROFF_10S);

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
  if (PUCK_BUTTON_PWR >= 0) {
    // A plain level, polled like BOOT and debounced by buttons.cpp the same way.
    //
    // Not reported until the key has been seen up once since boot. On battery the
    // board is switched on *by* pressing PWR, so the press that powered it can
    // still be down when this first runs — and counted as a hold it would toggle
    // the auto-cycle at 2 s or power straight back off at 5 s. Waiting for a
    // release also means a key that read down for good would leave PWR inert
    // rather than shutting the device down five seconds into every boot.
    static bool armed = false;
    const bool down = digitalRead(PUCK_BUTTON_PWR) == LOW;
    if (!armed) {
      armed = !down;
      s_key_down = false;
      return;
    }
    s_key_down = down;
    return;
  }
  if (!s_ready) {
    return;
  }

  // A tap that began and ended between two polls arrives as both edges latched
  // together, and setting the level up and then down inside one call means the
  // caller never sees it down at all — the press vanishes. So it is held down
  // until the next poll, which gives the recogniser the transition it needs.
  //
  // This has to run before the early return below, or the release would wait for
  // whatever unrelated interrupt happened to come next.
  if (s_release_pending) {
    s_release_pending = false;
    s_key_down = false;
    return;
  }

  // Polled rather than interrupt-driven: the PMIC's IRQ line is not broken out to
  // a pin we own. Called from loop(), so usually every few milliseconds — but
  // lv_timer_handler() shares that loop and a full redraw can hold it for far
  // longer, which is exactly how both edges end up in one read.
  if (s_pmic.getIrqStatus() == 0) {
    return;
  }
  const bool pressed = s_pmic.isPekeyNegativeIrq();
  const bool released = s_pmic.isPekeyPositiveIrq();
  s_pmic.clearIrqStatus();

  if (pressed && released) {
    s_key_down = true;
    s_release_pending = true;
  } else if (pressed) {
    s_key_down = true;
  } else if (released) {
    s_key_down = false;
  }
}

bool power_key_down() {
  return s_key_down;
}

void power_shutdown() {
  if (s_ready) {
    s_pmic.shutdown();
    return;
  }
  if (PUCK_POWER_LATCH < 0) {
    return;
  }

  // Let go of the battery. While PWR is still held the key itself keeps the
  // switch closed, so on battery the board dies the moment it is released.
  digitalWrite(PUCK_POWER_LATCH, LOW);
  while (PUCK_BUTTON_PWR >= 0 && digitalRead(PUCK_BUTTON_PWR) == LOW) {
    delay(10);
  }
  delay(200);

  // Still running, so USB is powering the board and the latch had nothing to cut.
  // The nearest thing to off is a dark panel and a deep sleep that PWR wakes from
  // — a wake is a fresh boot, which takes the latch again. Armed only once the key
  // is up, or the wake source would fire the instant the chip went down.
  Serial.println("[power] still powered (USB): sleeping until PWR is pressed");
  display_set_sleep(true);
  if (PUCK_BUTTON_PWR >= 0) {
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(PUCK_BUTTON_PWR), 0);
  }
  esp_deep_sleep_start();
}
