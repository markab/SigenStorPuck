// SigenStorPuck — device entry point.
//
// Brings up the hardware, joins WiFi, serves the settings page, and polls
// /api/summary on a task pinned to core 0 while this loop runs LVGL on core 1.
// Screens never learn where a reading came from: everything arrives through one
// ui_update() call (PLAN.md §B3).

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "board_config.h"
#include "device/buttons.h"
#include "device/net.h"
#include "device/poller.h"
#include "device/power.h"
#include "device/settings.h"
#include "device/settings_server.h"
#include "display.h"
#include "snapshot.h"
#include "touch.h"
#include "ui/theme.h"
#include "ui/ui.h"

namespace {

// The UI is refreshed on a timer rather than whenever a poll lands, so the render
// rate is decoupled from the network entirely.
constexpr uint32_t UI_REFRESH_MS = 250;

// The PMIC is read over I2C, so at a human rate rather than per frame.
constexpr uint32_t POWER_POLL_MS = 5000;

// Two lines of storage for the overlay, so a message that has not changed is not
// rewritten into LVGL 4 times a second.
char s_overlay_title[48] = {};
char s_overlay_detail[96] = {};

void set_overlay(const char* title, const char* detail) {
  const bool same = strncmp(s_overlay_title, title == nullptr ? "" : title,
                            sizeof(s_overlay_title)) == 0 &&
                    strncmp(s_overlay_detail, detail == nullptr ? "" : detail,
                            sizeof(s_overlay_detail)) == 0;
  if (same) {
    return;
  }
  snprintf(s_overlay_title, sizeof(s_overlay_title), "%s", title == nullptr ? "" : title);
  snprintf(s_overlay_detail, sizeof(s_overlay_detail), "%s", detail == nullptr ? "" : detail);
  ui_set_overlay(title, detail);
}

// Decides what, if anything, should cover the screens. Ordered by what the viewer
// can actually do about it: no network, then nothing configured, then a revoked
// token, then simply waiting.
void refresh_overlay(bool have_snapshot, const PollStatus& status) {
  static String detail;

  switch (net_state()) {
    case NetState::Portal:
      detail = String("Join the WiFi network\n") + net_setup_ap_name();
      set_overlay("WiFi setup", detail.c_str());
      return;
    case NetState::Starting:
    case NetState::Connecting:
      set_overlay("Connecting to WiFi", nullptr);
      return;
    case NetState::Connected:
      break;
  }

  if (!settings_is_provisioned()) {
    detail = "Open http://";
    detail += net_hostname().isEmpty() ? net_ip() : net_hostname();
    detail += "/\nand paste the enrolment URL";
    set_overlay("Not configured", detail.c_str());
    return;
  }

  // A revoked token is not a network fault and will never fix itself, so it says
  // so rather than sitting on "waiting for data" forever.
  if (status.last_result == FetchResult::Unauthorised) {
    detail = "The kiosk token was revoked.\nRe-enrol at http://";
    detail += net_hostname().isEmpty() ? net_ip() : net_hostname();
    detail += "/";
    set_overlay("Re-enrol needed", detail.c_str());
    return;
  }

  if (!have_snapshot) {
    if (status.last_result == FetchResult::ClockUnset) {
      set_overlay("Waiting for clock", "HTTPS needs the time set.\nWaiting for NTP.");
    } else if (status.consecutive_failures > 0) {
      detail = String("Cannot reach the server\n") + fetch_result_name(status.last_result);
      set_overlay("No data", detail.c_str());
    } else {
      set_overlay("Waiting for data", nullptr);
    }
    return;
  }

  // Data is flowing: get out of the way. A stale reading is reported by the plant
  // node on screen 1, not by covering everything up.
  set_overlay(nullptr, nullptr);
}

// Dims after a period with no touch. LVGL's own inactivity timer is used rather
// than tracking touch here, because it already accounts for every input the
// indev driver reports — so a tap wakes the screen for free.
//
// Dimmed, never blanked: a status display you have to wake up before you can read
// it has stopped being a status display. It also matters on AMOLED, where a static
// image at full brightness for months is what causes burn-in.
void apply_idle_dim() {
  const Settings& settings = settings_get();
  if (settings.dim_after_s == 0) {
    return;
  }
  const bool should_dim = lv_disp_get_inactive_time(nullptr) > settings.dim_after_s * 1000;

  static bool dimmed = false;
  if (should_dim != dimmed) {
    dimmed = should_dim;
    display_set_brightness(dimmed ? settings.dim_brightness : settings.brightness);
  }
}

void refresh_ui() {
  Snapshot snapshot;
  const bool have = poller_snapshot(&snapshot);
  const PollStatus status = poller_status();

  if (have) {
    ui_update(snapshot);
  }
  refresh_overlay(have, status);
}

// The Puck's own battery, shown only when it is actually running on one. Normally
// this is a mains-powered display and the indicator would be permanent clutter
// telling you nothing.
void refresh_device_battery() {
  const PowerStatus power = power_status();
  const bool on_battery = power.pmic_ok && power.battery_present && !power.usb_present;
  ui_set_device_battery(on_battery, power.percent, power.charging);
}

void log_boot_banner() {
  Serial.printf("\nSigenStorPuck %s\n", PUCK_FW_VERSION);
  Serial.printf("[chip] %s rev %d, %d MHz, flash %u MB, PSRAM %u MB, free heap %u B\n",
                ESP.getChipModel(), ESP.getChipRevision(), getCpuFrequencyMhz(),
                static_cast<unsigned>(ESP.getFlashChipSize() / (1024 * 1024)),
                static_cast<unsigned>(ESP.getPsramSize() / (1024 * 1024)),
                static_cast<unsigned>(ESP.getFreeHeap()));
}

}  // namespace

void setup() {
  Serial.begin(PUCK_SERIAL_BAUD);
  // USB-CDC takes a moment to enumerate; without this the banner is lost.
  delay(1500);
  log_boot_banner();

  // main() owns the shared I2C bus: touch now, the AXP2101 PMIC and PCF85063 RTC
  // later.
  Wire.begin(PUCK_I2C_SDA, PUCK_I2C_SCL, PUCK_I2C_HZ);
  touch_scan_i2c();

  // Before the panel: the rotation is applied as it is brought up.
  settings_begin();

  lv_init();
  if (!display_begin(settings_get().orientation)) {
    Serial.println("[boot] display bring-up failed — halting");
    while (true) {
      delay(1000);
    }
  }
  touch_begin();
  touch_set_orientation(settings_get().orientation);

  power_begin();
  buttons_begin();

  ui_create(lv_scr_act());
  display_set_brightness(settings_get().brightness);
  // A named boot screen rather than a bare word: on a device that takes a couple
  // of seconds to find WiFi, this is the only proof it is alive.
  set_overlay("SigenStorPuck", PUCK_FW_VERSION);
  ui_set_overlay("SigenStorPuck", PUCK_FW_VERSION);
  // Draw the first frame before WiFi so the screen is alive while the radio comes
  // up, rather than black for a couple of seconds.
  lv_timer_handler();

  ui_set_rotate_interval(settings_get().rotate_s);
  ui_set_sweep_interval(settings_get().sweep_min);

  net_begin();
  poller_begin();

  Serial.println("[boot] ready");
}

void loop() {
  buttons_loop();

  // A short press of PWR wakes the screen, and only that. Advancing a screen as
  // well meant you could not wake the device without also moving off whatever you
  // had left on it.
  if (power_take_short_press()) {
    lv_disp_trig_activity(nullptr);
    display_set_brightness(settings_get().brightness);
  }
  power_loop();
  net_loop();

  // Started only once connected: the captive portal owns port 80 until then.
  if (net_state() == NetState::Connected && !settings_server_running()) {
    settings_server_begin();
  }
  settings_server_loop();

  static uint32_t last_refresh = 0;
  if (millis() - last_refresh >= UI_REFRESH_MS) {
    last_refresh = millis();
    refresh_ui();
  }

  static uint32_t last_power = 0;
  if (millis() - last_power >= POWER_POLL_MS) {
    last_power = millis();
    refresh_device_battery();
  }

  apply_idle_dim();
  lv_timer_handler();
  delay(5);
}
