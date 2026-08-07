#include "screen_battery.h"

#include <stdio.h>

#include "board_config.h"
#include "chart_band.h"
#include "format.h"
#include "theme.h"

namespace {

// ------------------------------------------------------------------ layout ---
//
// Three layers, back to front:
//
//   1. The state-of-charge arc at the bezel, which is where this screen says
//      what the battery is *now*.
//   2. The day's state of charge, ghosted right back and spread across the whole
//      lower half — how it got there. Big enough to read as the shape of a
//      cycle, faint enough to carry four figures on top of it.
//   3. The readings themselves: the headline block above the curve, and the four
//      occasional figures laid over it in two columns.
//
// The day curve used to be a small strip in a gap of its own. Making it the
// backdrop is what buys the room for capacity, health and the day's two totals
// without the screen turning into a table.

// Geometry from theme.h, shared with screen 1 — same ring, same place, so a
// swipe between the two does not move it.

// Drawn the full width of the panel and clipped to the bezel, rather than sized
// to a rectangle that fits inside it.
//
// A rectangle is the wrong shape here twice over. Wide enough to reach both
// edges, its bottom corners land outside the ring — at y = +170 the ring's inner
// edge only allows 286 px, so the old 316 px band was lying across it by 15 px a
// side. Narrow enough to clear the ring, it stops in open screen and the day
// ends at a hard vertical edge somewhere around 02:00 and 22:00. Clipped, the
// curve runs off under the ring on both sides and its foot follows the glass.
constexpr lv_coord_t BAND_WIDTH = PUCK_LCD_WIDTH;
constexpr lv_coord_t BAND_HEIGHT = 148;
constexpr lv_coord_t BAND_Y = 96;  // centre, so the band spans +22 to +170

// Just inside the ring's own inner edge, so the two never touch.
constexpr lv_coord_t BAND_CLIP_RADIUS = PUCK_RING_DIAMETER / 2 - PUCK_RING_WIDTH - 4;

// The strength of the fill just under the curve; the gradient takes it to nothing
// by the foot, and the cap is drawn about twice this. Tuned by eye in the
// simulator against 02_evening_discharge, which is the fixture whose curve runs
// highest and therefore crosses the most text.
constexpr lv_opa_t BAND_GHOST = 110;

// A day of state of charge is a slow curve already, so this is only knocking the
// corners off the reduction.
constexpr uint8_t BAND_SMOOTHING = 7;

// The two stat columns, either side of the middle.
constexpr lv_coord_t COLUMN_X = 88;
constexpr lv_coord_t ROW_ONE_LABEL_Y = 44;
constexpr lv_coord_t ROW_ONE_VALUE_Y = 70;
constexpr lv_coord_t ROW_TWO_LABEL_Y = 116;
constexpr lv_coord_t ROW_TWO_VALUE_Y = 142;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_band = nullptr;
lv_obj_t* s_arc = nullptr;
lv_obj_t* s_soc = nullptr;
lv_obj_t* s_stored = nullptr;
lv_obj_t* s_pill = nullptr;
lv_obj_t* s_rate = nullptr;
lv_obj_t* s_eta = nullptr;
lv_obj_t* s_charged = nullptr;
lv_obj_t* s_discharged = nullptr;
lv_obj_t* s_health = nullptr;
lv_obj_t* s_temp = nullptr;

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t colour, lv_coord_t x,
                     lv_coord_t y) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(colour), LV_PART_MAIN);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(label, LV_ALIGN_CENTER, x, y);
  return label;
}

// A column heading. Tracked out and muted so it reads as a caption for the
// figure under it rather than as another reading competing with the curve.
lv_obj_t* make_caption(lv_obj_t* parent, const char* text, lv_coord_t x, lv_coord_t y) {
  lv_obj_t* label = make_label(parent, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, x, y);
  lv_obj_set_style_text_letter_space(label, 2, LV_PART_MAIN);
  lv_label_set_text(label, text);
  return label;
}

// Formats one of the day's totals, or a dash when the day cannot be read.
void set_day_total(lv_obj_t* label, const Snapshot& snapshot, const MaybeFloat& value) {
  char text[24];
  if (!snapshot.valid || !snapshot.today.present) {
    lv_label_set_text(label, "--");
    return;
  }
  char scratch[16];
  puck_format_magnitude(value, 1, scratch, sizeof(scratch));
  snprintf(text, sizeof(text), "%s kWh", scratch);
  lv_label_set_text(label, text);
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
  lv_obj_set_size(s_arc, PUCK_RING_DIAMETER, PUCK_RING_DIAMETER);
  lv_obj_center(s_arc);
  lv_arc_set_rotation(s_arc, 270);  // zero at the top
  lv_arc_set_bg_angles(s_arc, 0, 360);
  lv_arc_set_range(s_arc, 0, 100);
  lv_arc_set_value(s_arc, 0);
  lv_obj_remove_style(s_arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(s_arc, PUCK_RING_WIDTH, LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_arc, PUCK_RING_WIDTH, LV_PART_INDICATOR);
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
    chart_band_set_intensity(s_band, BAND_GHOST);
    chart_band_set_bezel_clip(s_band, BAND_CLIP_RADIUS);
    chart_band_set_smoothing(s_band, BAND_SMOOTHING);
  }

  lv_obj_t* title = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 0, -166);
  lv_obj_set_style_text_letter_space(title, 3, LV_PART_MAIN);
  lv_label_set_text(title, "BATTERY");

  s_soc = make_label(s_root, PUCK_FONT_HERO, PUCK_COLOUR_TEXT, 0, -114);
  lv_label_set_text(s_soc, "--");

  s_stored = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 0, -70);
  lv_label_set_text(s_stored, "");

  // The live figure gets a pill of its own — the one thing on this screen that
  // changes minute to minute, so it should be findable without reading. Borrowed
  // from the server's own dashboard card, which does the same thing.
  s_pill = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_pill);
  lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(s_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(s_pill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(s_pill, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(s_pill, 7, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_pill, lv_color_hex(PUCK_COLOUR_BATTERY), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_pill, LV_OPA_20, LV_PART_MAIN);
  lv_obj_align(s_pill, LV_ALIGN_CENTER, 0, -26);

  s_rate = lv_label_create(s_pill);
  lv_obj_set_style_text_font(s_rate, PUCK_FONT_BODY, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_rate, lv_color_hex(PUCK_COLOUR_BATTERY), LV_PART_MAIN);
  lv_label_set_text(s_rate, "--");
  lv_obj_center(s_rate);

  s_eta = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 0, 8);
  lv_label_set_text(s_eta, "");

  // Four figures over the curve, in the order you would ask for them: what the
  // day did, then how the pack is.
  make_caption(s_root, "CHARGED", -COLUMN_X, ROW_ONE_LABEL_Y);
  s_charged = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, -COLUMN_X, ROW_ONE_VALUE_Y);
  lv_label_set_text(s_charged, "--");

  make_caption(s_root, "DISCHARGED", COLUMN_X, ROW_ONE_LABEL_Y);
  s_discharged = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, COLUMN_X, ROW_ONE_VALUE_Y);
  lv_label_set_text(s_discharged, "--");

  make_caption(s_root, "HEALTH", -COLUMN_X, ROW_TWO_LABEL_Y);
  s_health = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, -COLUMN_X, ROW_TWO_VALUE_Y);
  lv_label_set_text(s_health, "--");

  make_caption(s_root, "TEMP", COLUMN_X, ROW_TWO_LABEL_Y);
  s_temp = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, COLUMN_X, ROW_TWO_VALUE_Y);
  lv_label_set_text(s_temp, "--");

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

  // Energy stored, which is also where the pack's capacity is stated. This is
  // the one figure on any screen the Puck works out for itself: soc% of
  // capacity. It is a unit conversion rather than the business logic the server
  // owns, and it is the most useful single battery number — but if it should
  // come from the payload instead, this is the line to delete.
  if (battery != nullptr && battery->soc_pct.known && battery->capacity_kwh.known) {
    const float stored = battery->capacity_kwh.value * battery->soc_pct.value / 100.0f;
    snprintf(text, sizeof(text), "%.1f of %.1f kWh", stored, battery->capacity_kwh.value);
    lv_label_set_text(s_stored, text);
  } else if (battery != nullptr && battery->capacity_kwh.known) {
    snprintf(text, sizeof(text), "%.1f kWh capacity", battery->capacity_kwh.value);
    lv_label_set_text(s_stored, text);
  } else {
    lv_label_set_text(s_stored, "");
  }

  // Charge or discharge rate, in the pill.
  const MaybeFloat rate = snapshot.valid ? snapshot.power.batt : MaybeFloat{};
  const bool moving = rate.known && rate.value != 0.0f;
  const bool charging = rate.known && rate.value > 0.0f;
  if (!rate.known) {
    lv_label_set_text(s_rate, "-- kW");
  } else if (!moving) {
    // Same reading as a resting leg on screen 1, and worded the same way.
    lv_label_set_text(s_rate, "idle");
  } else {
    puck_format_magnitude(rate, PUCK_KW_DECIMALS, scratch, sizeof(scratch));
    snprintf(text, sizeof(text), "%s kW %s", scratch, charging ? "charging" : "discharging");
    lv_label_set_text(s_rate, text);
  }
  // Muted while nothing is moving, so the pill only draws the eye when there is
  // something happening in it.
  const uint32_t pill_colour = moving ? PUCK_COLOUR_BATTERY : PUCK_COLOUR_MUTED;
  lv_obj_set_style_text_color(s_rate, lv_color_hex(pill_colour), LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_pill, lv_color_hex(pill_colour), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_pill, moving ? LV_OPA_20 : LV_OPA_10, LV_PART_MAIN);

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

  set_day_total(s_charged, snapshot, snapshot.today.charge);
  set_day_total(s_discharged, snapshot, snapshot.today.discharge);

  if (battery != nullptr && battery->soh_pct.known) {
    snprintf(text, sizeof(text), "%.0f%%", battery->soh_pct.value);
    lv_label_set_text(s_health, text);
  } else {
    lv_label_set_text(s_health, "--");
  }

  // The degree sign is one of the glyphs our own Montserrat subsets carry and
  // LVGL's built-ins do not.
  if (battery != nullptr && battery->temp_c.known) {
    snprintf(text, sizeof(text), "%.1f°C", battery->temp_c.value);
    lv_label_set_text(s_temp, text);
  } else {
    lv_label_set_text(s_temp, "--");
  }
}
