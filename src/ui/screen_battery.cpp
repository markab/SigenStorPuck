#include "screen_battery.h"

#include <stdio.h>

#include "board_config.h"
#include "chart_band.h"
#include "format.h"
#include "theme.h"

namespace {

// The state-of-charge arc gets the bezel to itself here, so it can be much
// heavier than the thin version on screen 1 and read from across a room.
constexpr lv_coord_t ARC_DIAMETER = 448;
constexpr lv_coord_t ARC_WIDTH = 16;

// The day's state of charge (§D3), in the gap between the time-to-empty line and
// the health readout. The arc says where the battery is now; this says how it
// got there, which is what makes a charge/discharge cycle legible at a glance.
//
// Clear of the text rather than behind it, for the same reason as screen 3.
constexpr lv_coord_t BAND_WIDTH = 300;
constexpr lv_coord_t BAND_HEIGHT = 40;
constexpr lv_coord_t BAND_Y = 96;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_band = nullptr;
lv_obj_t* s_arc = nullptr;
lv_obj_t* s_soc = nullptr;
lv_obj_t* s_stored = nullptr;
lv_obj_t* s_rate = nullptr;
lv_obj_t* s_eta = nullptr;
lv_obj_t* s_health = nullptr;

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t colour, lv_coord_t y) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(colour), LV_PART_MAIN);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
  return label;
}

}  // namespace

lv_obj_t* screen_battery_create(lv_obj_t* parent) {
  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

  s_arc = lv_arc_create(s_root);
  lv_obj_set_size(s_arc, ARC_DIAMETER, ARC_DIAMETER);
  lv_obj_center(s_arc);
  lv_arc_set_rotation(s_arc, 270);  // zero at the top
  lv_arc_set_bg_angles(s_arc, 0, 360);
  lv_arc_set_range(s_arc, 0, 100);
  lv_arc_set_value(s_arc, 0);
  lv_obj_remove_style(s_arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(s_arc, ARC_WIDTH, LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_arc, ARC_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_arc, lv_color_hex(PUCK_COLOUR_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_color(s_arc, lv_color_hex(PUCK_COLOUR_BATTERY), LV_PART_INDICATOR);

  // Before the labels, so they sit on top of it.
  s_band = chart_band_create(s_root, HistorySeries::Soc, PUCK_COLOUR_BATTERY);
  if (s_band != nullptr) {
    lv_obj_set_size(s_band, BAND_WIDTH, BAND_HEIGHT);
    lv_obj_align(s_band, LV_ALIGN_CENTER, 0, BAND_Y);
    // Fixed 0-100 rather than autoscaled: the height of this curve should mean
    // the same thing every time you look at it, and a battery that stayed
    // between 60 % and 64 % all day must not be stretched to look like a full
    // cycle.
    chart_band_set_range(s_band, 0.0f, 100.0f);
  }

  lv_obj_t* title = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, -150);
  lv_label_set_text(title, "BATTERY");

  s_soc = make_label(s_root, PUCK_FONT_HERO, PUCK_COLOUR_TEXT, -66);
  lv_label_set_text(s_soc, "--");

  s_stored = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, -14);
  lv_label_set_text(s_stored, "");

  s_rate = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, 24);
  lv_label_set_text(s_rate, "--");

  s_eta = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 58);
  lv_label_set_text(s_eta, "");

  // Health and temperature: the two figures you check occasionally rather than
  // watch, so they sit lowest.
  s_health = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 132);
  lv_label_set_text(s_health, "");

  return s_root;
}

void screen_battery_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }

  char text[48];
  char scratch[24];

  if (s_band != nullptr) {
    chart_band_refresh(s_band);
  }

  const Snapshot::Battery* battery = nullptr;
  if (snapshot.valid) {
    battery = &snapshot.battery;
  }

  // State of charge, and the arc.
  if (battery != nullptr && battery->soc_pct.known) {
    snprintf(text, sizeof(text), "%.0f%%", battery->soc_pct.value);
    lv_label_set_text(s_soc, text);
    lv_arc_set_value(s_arc, static_cast<int16_t>(battery->soc_pct.value));
    const bool low = battery->soc_pct.value < PUCK_SOC_LOW_PCT;
    lv_obj_set_style_arc_color(s_arc,
                               lv_color_hex(low ? PUCK_COLOUR_WARN : PUCK_COLOUR_BATTERY),
                               LV_PART_INDICATOR);
  } else {
    lv_label_set_text(s_soc, "--");
    lv_arc_set_value(s_arc, 0);
  }

  // Energy stored. This is the one figure on any screen the Puck works out for
  // itself: soc% of capacity. It is a unit conversion rather than the business
  // logic the server owns, and it is the most useful single battery number —
  // but if it should come from the payload instead, this is the line to delete.
  if (battery != nullptr && battery->soc_pct.known && battery->capacity_kwh.known) {
    const float stored = battery->capacity_kwh.value * battery->soc_pct.value / 100.0f;
    snprintf(text, sizeof(text), "%.1f of %.1f kWh", stored, battery->capacity_kwh.value);
    lv_label_set_text(s_stored, text);
  } else {
    lv_label_set_text(s_stored, "");
  }

  // Charge or discharge rate, and what it means for the time remaining.
  const MaybeFloat rate = snapshot.valid ? snapshot.power.batt : MaybeFloat{};
  puck_format_magnitude(rate, PUCK_KW_DECIMALS, scratch, sizeof(scratch));
  const bool moving = rate.known && rate.value != 0.0f;
  const bool charging = rate.known && rate.value > 0.0f;
  if (!moving) {
    snprintf(text, sizeof(text), "%s kW idle", scratch);
  } else {
    snprintf(text, sizeof(text), "%s kW %s", scratch, charging ? "charging" : "discharging");
  }
  lv_label_set_text(s_rate, text);

  // eta_min is time-to-full while charging and time-to-empty while discharging,
  // so the label has to say which — the number alone is ambiguous. The server
  // withholds it at trickle rates, which is also when it would be nonsense.
  if (battery != nullptr && battery->eta_min.known && moving) {
    puck_format_duration(battery->eta_min, scratch, sizeof(scratch));
    snprintf(text, sizeof(text), "%s in %s", charging ? "full" : "empty", scratch);
    lv_label_set_text(s_eta, text);
  } else {
    lv_label_set_text(s_eta, "");
  }

  // Uses both glyphs the stock LVGL fonts lack: the middle dot and the degree.
  if (battery != nullptr && (battery->soh_pct.known || battery->temp_c.known)) {
    char health[16];
    char temperature[16];
    puck_format_magnitude(battery->soh_pct, 0, health, sizeof(health));
    puck_format_magnitude(battery->temp_c, 1, temperature, sizeof(temperature));
    snprintf(text, sizeof(text), "health %s%%  ·  %s°C", health, temperature);
    lv_label_set_text(s_health, text);
  } else {
    lv_label_set_text(s_health, "");
  }
}
