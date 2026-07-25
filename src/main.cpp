// SigenStorPuck — device entry point.
//
// Still scaffolding (PLAN.md §C4 step 2): it renders the real screens against
// canned payloads compiled in from test/fixtures, so a design can be judged on
// the actual 1.75" panel before the network client of step 4 exists. Tap the
// screen to step through the fixtures.
//
// At step 4 the fixture cycling and src/dev_fixtures.h come out, replaced by a
// poll of /api/summary feeding the same screen_power_update() call.

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "board_config.h"
#include "dev_fixtures.h"
#include "display.h"
#include "snapshot.h"
#include "touch.h"
#include "ui/theme.h"
#include "ui/ui.h"

namespace {

size_t s_fixture = 0;
lv_obj_t* s_fixture_label = nullptr;

void show_fixture(size_t index) {
  const DevFixture& fixture = DEV_FIXTURES[index];

  Snapshot snapshot;
  if (!snapshot_parse(fixture.json, strlen(fixture.json), &snapshot)) {
    Serial.printf("[dev] %s did not parse\n", fixture.name);
    ui_update(Snapshot{});
    return;
  }

  ui_update(snapshot);
  lv_label_set_text_fmt(s_fixture_label, "%u/%u %s", static_cast<unsigned>(index + 1),
                        static_cast<unsigned>(DEV_FIXTURE_COUNT), fixture.name);
  Serial.printf("[dev] %s\n", fixture.name);
}

// A tap advances the fixture; a drag is left alone so it reaches the tileview and
// swipes between screens. Without the distance test every swipe would also jump
// to the next fixture.
constexpr lv_coord_t TAP_SLOP_PX = 20;

void poll_touch_advance(lv_timer_t* /*timer*/) {
  static bool was_pressed = false;
  static lv_point_t pressed_at = {};

  lv_point_t point = {};
  const bool pressed = touch_pressed(&point);

  if (pressed && !was_pressed) {
    pressed_at = point;
  } else if (!pressed && was_pressed) {
    const int32_t dx = point.x - pressed_at.x;
    const int32_t dy = point.y - pressed_at.y;
    if (dx * dx + dy * dy <= TAP_SLOP_PX * TAP_SLOP_PX) {
      s_fixture = (s_fixture + 1) % DEV_FIXTURE_COUNT;
      show_fixture(s_fixture);
    }
  }
  was_pressed = pressed;
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

  // main() owns the shared I2C bus: touch now, and the AXP2101 PMIC and
  // PCF85063 RTC join it in later steps.
  Wire.begin(PUCK_I2C_SDA, PUCK_I2C_SCL, PUCK_I2C_HZ);
  touch_scan_i2c();

  lv_init();
  if (!display_begin()) {
    Serial.println("[boot] display bring-up failed — halting");
    while (true) {
      delay(1000);
    }
  }

  // Touch is not fatal: a working screen is a more useful diagnostic than a
  // dead board. Without it the fixtures simply cannot be cycled.
  const bool touch_ok = touch_begin();

  lv_obj_t* screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  ui_create(screen);

  // Which fixture is on screen. Tucked just under the plant node in the dimmest
  // colour available: the star fills every bearing, so the middle is the only
  // free space left. Scaffolding, so it lives here and not in screen_power.cpp.
  s_fixture_label = lv_label_create(screen);
  lv_obj_set_style_text_font(s_fixture_label, PUCK_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_fixture_label, lv_color_hex(PUCK_COLOUR_TRACK), LV_PART_MAIN);
  lv_obj_align(s_fixture_label, LV_ALIGN_CENTER, 0, 40);

  show_fixture(s_fixture);

  if (touch_ok) {
    lv_timer_create(poll_touch_advance, 40, nullptr);
    Serial.printf("[dev] tap to cycle %u fixtures, swipe to change screen\n",
                  static_cast<unsigned>(DEV_FIXTURE_COUNT));
  }

  Serial.println("[boot] ready");
}

void loop() {
  lv_timer_handler();
  delay(5);
}
