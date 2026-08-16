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
#include "device/updater.h"
#include "display.h"
#include "screen_window.h"
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
bool s_overlay_qr = false;

void set_overlay(const char* title, const char* detail, bool with_qr = false) {
  const bool same = strncmp(s_overlay_title, title == nullptr ? "" : title,
                            sizeof(s_overlay_title)) == 0 &&
                    strncmp(s_overlay_detail, detail == nullptr ? "" : detail,
                            sizeof(s_overlay_detail)) == 0 &&
                    with_qr == s_overlay_qr;
  if (same) {
    return;
  }
  snprintf(s_overlay_title, sizeof(s_overlay_title), "%s", title == nullptr ? "" : title);
  snprintf(s_overlay_detail, sizeof(s_overlay_detail), "%s", detail == nullptr ? "" : detail);
  s_overlay_qr = with_qr;
  ui_set_overlay(title, detail, with_qr);

  // Logged because it is the one part of the UI you cannot ask the device about
  // over the network, and it is exactly what you want to know when someone says
  // the screen is covered. Only on a change, so it stays quiet — the dedupe
  // above is what makes that true.
  if (title == nullptr) {
    Serial.println("[ui] overlay cleared");
  } else {
    Serial.printf("[ui] overlay: %s | %s\n", s_overlay_title, s_overlay_detail);
  }
}

// Decides what, if anything, should cover the screens. Ordered by what the viewer
// can actually do about it: no network, then nothing configured, then a revoked
// token, then simply waiting.
void refresh_overlay(bool have_snapshot, const PollStatus& status) {
  static String detail;

  // Both addresses, not whichever one happens to be available.
  //
  // The .local name is the nicer one to type, but mDNS resolution is not
  // universal — Android has never supported it properly, and plenty of routers
  // drop multicast between bands or VLANs. Showing only the name strands anyone
  // whose phone cannot resolve it, on the one screen whose entire job is to tell
  // them where to go. The IP always works, so it is always shown.
  const String host = net_hostname();
  const String ip = net_ip();

  // Fed to the UI before the switch below: the last screen shows this whether or
  // not an overlay is up, and both are empty strings until WiFi is joined.
  ui_set_address(host.c_str(), ip.c_str());

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

  const bool modbus = settings_get().source == DataSource::Modbus;

  String where = "http://";
  if (!host.isEmpty()) {
    where += host;
    where += "/\nor http://";
  }
  where += ip;
  where += "/";

  if (!settings_is_provisioned()) {
    // The code carries the same address as the text under it, so a phone can
    // skip the typing and everyone else still has something to read.
    detail = "Scan, or open ";
    detail += where;
    // Different instruction per source: there is no enrolment URL to paste when
    // the Puck talks to the plant directly, and telling someone to find one
    // would send them looking for something that does not exist.
    detail += modbus ? "\nand set the plant's IP address" : "\nand paste the enrolment URL";
    set_overlay("Not configured", detail.c_str(), true);
    return;
  }

  // A revoked token is not a network fault and will never fix itself, so it says
  // so rather than sitting on "waiting for data" forever.
  if (status.last_result == FetchResult::Unauthorised) {
    detail = "The kiosk token was revoked.\nRe-enrol at ";
    detail += where;
    set_overlay("Re-enrol needed", detail.c_str());
    return;
  }

  if (!have_snapshot) {
    if (status.last_result == FetchResult::ClockUnset) {
      set_overlay("Waiting for clock", "HTTPS needs the time set.\nWaiting for NTP.");
    } else if (status.consecutive_failures > 0) {
      // Naming what is unreachable, because on the Modbus path "the server" is
      // not a thing that exists and the first place to look is the Sigen app's
      // Modbus whitelist.
      detail = modbus ? String("Cannot reach the plant\n") : String("Cannot reach the server\n");
      detail += fetch_result_name(status.last_result);
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

// Blanks the panel outright between two local times, and brings it back for a
// button (buttons.cpp swallows the gesture that does the waking) or for the end
// of the window.
//
// Local, not UTC. The device's own clock is UTC — net.cpp asks NTP for no
// timezone — so the offset has to come from the reading, which is the same one
// the day charts are anchored to. Until one arrives the window is read against
// UTC, which in Britain is right for half the year and an hour out for the other
// half; the settings page prints the device's current local time next to the
// fields so that is visible rather than mysterious.
void apply_screen_schedule() {
  const Settings& settings = settings_get();
  const time_t now = time(nullptr);

  // Without a real clock every time of day is a guess, and the guess is 1970.
  // Blanking on it would leave a device that has lost NTP dark with no way to
  // tell why. The same 2024 floor sigen_api.cpp uses to judge certificates.
  const bool can_tell_the_time = now > 1704067200;

  bool wanted = false;
  if (settings.screen_off_set && can_tell_the_time) {
    const uint16_t minute =
        screen_local_minute(static_cast<uint32_t>(now), poller_tz_offset_min());
    wanted = screen_window_contains(settings.screen_off_start_min, settings.screen_off_end_min,
                                    minute);
  }

  if (!wanted) {
    display_set_sleep(false);
    return;
  }
  if (display_asleep()) {
    return;
  }
  // Inside the window but recently touched or pressed: leave it up. LVGL's
  // inactivity timer is the same one the idle dim reads, so a swipe through the
  // screens holds it awake as naturally as it holds off the dim.
  if (lv_disp_get_inactive_time(nullptr) < PUCK_SCREEN_WAKE_S * 1000) {
    return;
  }
  display_set_sleep(true);
}

void refresh_ui() {
  Snapshot snapshot;
  const bool have = poller_snapshot(&snapshot);
  const PollStatus status = poller_status();

  // The viewed day before the live reading, so one pass never draws the new day's
  // chart against the old day's figures.
  Snapshot day;
  if (poller_day_snapshot(&day)) {
    ui_update_day(day);
  } else if (ui_day_offset() != 0) {
    // Asked for but not here yet. Dashes and LOADING, not the live reading:
    // falling back to live flashed today's figures under a past date on every
    // step between days.
    ui_set_day_loading();
  } else {
    ui_clear_day();
  }
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

  // Which screens exist is settled here and not revisited: the tileview's tiles
  // are built once, so the masks and the data source both apply on the next boot
  // — same as `orientation`.
  UiConfig ui_config;
  ui_config.with_server_screens = settings_get().source != DataSource::Modbus;
  ui_config.visible = settings_get().screens_visible;
  ui_config.rotate = settings_get().screens_rotate;
  ui_create(lv_scr_act(), ui_config);
  // No dated API behind the Modbus source — it has daily counters and no history
  // to step into — so the buttons refuse rather than moving an indicator over
  // figures that will never change.
  ui_set_day_stepping(ui_config.with_server_screens);
  display_set_brightness(settings_get().brightness);
  // A named boot screen rather than a bare word: on a device that takes a couple
  // of seconds to find WiFi, this is the only proof it is alive.
  set_overlay("SigenStorPuck", PUCK_FW_VERSION);
  ui_set_overlay("SigenStorPuck", PUCK_FW_VERSION);
  // Draw the first frame before WiFi so the screen is alive while the radio comes
  // up, rather than black for a couple of seconds.
  lv_timer_handler();

  // Neither applied while the drawing side is blocked: skewing touch to match a
  // rotation that is not on screen would put every tap 2.5 degrees out for no
  // visible reason.
  ui_set_rotate_interval(settings_get().rotate_s);
  ui_set_rotate_enabled(settings_get().rotate_enabled);
  ui_set_sweep_interval(settings_get().sweep_min);

  net_begin();
  poller_begin();
  updater_begin();

  Serial.println("[boot] ready");
}

void loop() {
  // The PMIC's key edges before the gesture recogniser reads them, so a press
  // is seen on the pass it arrives rather than the next one.
  power_loop();
  buttons_loop();

  // Any button press wakes a dimmed screen. buttons_loop() has already told LVGL
  // the device is active; this is the brightness half of the same thought.
  if (lv_disp_get_inactive_time(nullptr) < 100) {
    display_set_brightness(settings_get().brightness);
  }

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
  apply_screen_schedule();
  lv_timer_handler();
  delay(5);
}
