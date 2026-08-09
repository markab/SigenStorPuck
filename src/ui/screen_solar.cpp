#include "screen_solar.h"

#include <stdio.h>

#include "board_config.h"
#include "chart_band.h"
#include "format.h"
#include "theme.h"

namespace {

// ------------------------------------------------------------------ layout ---
//
// Built to the same three layers as screen 2, because they are the same kind of
// screen: a quantity that fills up over a day, its live rate, and the figures
// you check rather than watch.
//
//   1. The ring at the bezel — today's generation as a fraction of today's
//      forecast. The direct analogue of state of charge: how full is today.
//   2. The day's PV curve, ghosted right back across the lower half.
//   3. The readings on top.
//
// The ring is the one part that has nothing to show without a forecast, so it is
// hidden rather than drawn empty on the Modbus source — an unfilled track reads
// as a confident zero.

// Full width and clipped to the bezel, for the reasons set out on screen 2: a
// rectangle either lies across the ring at its corners or ends in open screen.
constexpr lv_coord_t BAND_WIDTH = PUCK_LCD_WIDTH;
constexpr lv_coord_t BAND_HEIGHT = 156;
constexpr lv_coord_t BAND_Y = 90;  // centre, so the band spans +12 to +168

constexpr lv_coord_t BAND_CLIP_RADIUS = PUCK_RING_DIAMETER / 2 - PUCK_RING_WIDTH - 4;

constexpr lv_opa_t BAND_GHOST = 110;

// Wider than screen 2's: PV under broken cloud reduces to a picket fence, and
// behind four figures that is noise rather than information. The day's shape is
// what this is here to carry.
constexpr uint8_t BAND_SMOOTHING = 13;

constexpr lv_coord_t COLUMN_X = 88;
constexpr lv_coord_t ROW_ONE_LABEL_Y = 44;
constexpr lv_coord_t ROW_ONE_VALUE_Y = 70;
constexpr lv_coord_t ROW_TWO_LABEL_Y = 116;
constexpr lv_coord_t ROW_TWO_VALUE_Y = 142;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_band = nullptr;
lv_obj_t* s_arc = nullptr;
lv_obj_t* s_generated = nullptr;
lv_obj_t* s_caption = nullptr;
lv_obj_t* s_pill = nullptr;
lv_obj_t* s_rate = nullptr;
lv_obj_t* s_forecast = nullptr;
lv_obj_t* s_remaining = nullptr;
lv_obj_t* s_versus = nullptr;
lv_obj_t* s_peak = nullptr;
bool s_live = true;

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t colour, lv_coord_t x,
                     lv_coord_t y) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(colour), LV_PART_MAIN);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(label, LV_ALIGN_CENTER, x, y);
  return label;
}

lv_obj_t* make_caption(lv_obj_t* parent, const char* text, lv_coord_t x, lv_coord_t y) {
  lv_obj_t* label = make_label(parent, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, x, y);
  lv_obj_set_style_text_letter_space(label, 2, LV_PART_MAIN);
  lv_label_set_text(label, text);
  return label;
}

// One of the forecast figures. All four share the same failure: the server has
// no location or no array configured, or there is no server at all.
void set_forecast_figure(lv_obj_t* label, const Snapshot& snapshot, const MaybeFloat& value,
                         int decimals, const char* unit) {
  if (!snapshot.valid || !snapshot.solar.configured) {
    lv_label_set_text(label, "--");
    return;
  }
  char scratch[16];
  char text[24];
  puck_format_magnitude(value, decimals, scratch, sizeof(scratch));
  snprintf(text, sizeof(text), "%s%s", scratch, unit);
  lv_label_set_text(label, text);
}

}  // namespace

lv_obj_t* screen_solar_create(lv_obj_t* parent) {
  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

  // Same ring geometry as screens 1 and 2, from theme.h.
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
  lv_obj_set_style_arc_color(s_arc, lv_color_hex(PUCK_COLOUR_SOLAR), LV_PART_INDICATOR);
  lv_obj_add_flag(s_arc, LV_OBJ_FLAG_HIDDEN);

  s_band = chart_band_create(s_root, HistorySeries::Pv, PUCK_COLOUR_SOLAR);
  if (s_band != nullptr) {
    lv_obj_set_size(s_band, BAND_WIDTH, BAND_HEIGHT);
    lv_obj_align(s_band, LV_ALIGN_CENTER, 0, BAND_Y);
    // Autoscaled, unlike screen 2's. There is no ceiling a PV curve is a
    // fraction of — an array's output is bounded by the weather, not by a rated
    // capacity — so the band scales to the day it is drawing.
    chart_band_set_range(s_band, 0.0f, 0.0f);
    chart_band_set_intensity(s_band, BAND_GHOST);
    chart_band_set_bezel_clip(s_band, BAND_CLIP_RADIUS);
    chart_band_set_smoothing(s_band, BAND_SMOOTHING);
  }

  lv_obj_t* title = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 0, -166);
  lv_obj_set_style_text_letter_space(title, 3, LV_PART_MAIN);
  lv_label_set_text(title, "SOLAR");

  s_generated = make_label(s_root, PUCK_FONT_HERO, PUCK_COLOUR_TEXT, 0, -114);
  lv_label_set_text(s_generated, "--");

  s_caption = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 0, -70);
  lv_label_set_text(s_caption, "generated");

  // The live rate, in the same pill screen 2 uses for the same job.
  s_pill = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_pill);
  lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(s_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(s_pill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(s_pill, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(s_pill, 7, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_pill, lv_color_hex(PUCK_COLOUR_SOLAR), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_pill, LV_OPA_20, LV_PART_MAIN);
  lv_obj_align(s_pill, LV_ALIGN_CENTER, 0, -26);

  s_rate = lv_label_create(s_pill);
  lv_obj_set_style_text_font(s_rate, PUCK_FONT_BODY, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_rate, lv_color_hex(PUCK_COLOUR_SOLAR), LV_PART_MAIN);
  lv_label_set_text(s_rate, "--");
  lv_obj_center(s_rate);

  make_caption(s_root, "FORECAST", -COLUMN_X, ROW_ONE_LABEL_Y);
  s_forecast = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, -COLUMN_X, ROW_ONE_VALUE_Y);
  lv_label_set_text(s_forecast, "--");

  make_caption(s_root, "REMAINING", COLUMN_X, ROW_ONE_LABEL_Y);
  s_remaining = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, COLUMN_X, ROW_ONE_VALUE_Y);
  lv_label_set_text(s_remaining, "--");

  make_caption(s_root, "VS FORECAST", -COLUMN_X, ROW_TWO_LABEL_Y);
  s_versus = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, -COLUMN_X, ROW_TWO_VALUE_Y);
  lv_label_set_text(s_versus, "--");

  make_caption(s_root, "PEAK", COLUMN_X, ROW_TWO_LABEL_Y);
  s_peak = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, COLUMN_X, ROW_TWO_VALUE_Y);
  lv_label_set_text(s_peak, "--");

  return s_root;
}

void screen_solar_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }

  char text[48];
  char scratch[24];

  if (s_band != nullptr) {
    chart_band_refresh(s_band);
  }

  // Today's generation. today.solar rather than a field of its own in the solar
  // block, so this figure is the same one on both data sources — the Modbus path
  // fills it from the pv_daily registers.
  const bool have_today = snapshot.valid && snapshot.today.present;
  const MaybeFloat generated = have_today ? snapshot.today.solar : MaybeFloat{};
  if (generated.known) {
    snprintf(text, sizeof(text), "%.1f kWh", generated.value);
    lv_label_set_text(s_generated, text);
  } else {
    lv_label_set_text(s_generated, "--");
  }

  // The ring: how much of today's forecast has been generated. Hidden outright
  // without a forecast, because an empty track is indistinguishable from a real
  // zero and this screen exists on a source that will never have one.
  const MaybeFloat forecast =
      snapshot.valid && snapshot.solar.configured ? snapshot.solar.forecast_kwh : MaybeFloat{};
  if (generated.known && forecast.known && forecast.value > 0.0f) {
    float pct = generated.value / forecast.value * 100.0f;
    if (pct < 0.0f) {
      pct = 0.0f;
    }
    // A day that beats its forecast fills the ring and stops. The overshoot is
    // still readable as "vs forecast" over 100 %.
    if (pct > 100.0f) {
      pct = 100.0f;
    }
    lv_arc_set_value(s_arc, static_cast<int16_t>(pct));
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_arc, LV_OBJ_FLAG_HIDDEN);
  }

  // The pill is live-only, so a past day simply does not have one.
  if (s_live) {
    lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
  }

  // Live PV, in the pill.
  const MaybeFloat pv = snapshot.valid ? snapshot.power.pv : MaybeFloat{};
  const bool generating = pv.known && pv.value > 0.0f;
  if (!pv.known) {
    lv_label_set_text(s_rate, "-- kW");
  } else if (!generating) {
    // The same word a resting leg gets on screen 1.
    lv_label_set_text(s_rate, "idle");
  } else {
    puck_format_magnitude(pv, PUCK_KW_DECIMALS, scratch, sizeof(scratch));
    snprintf(text, sizeof(text), "%s kW generating", scratch);
    lv_label_set_text(s_rate, text);
  }
  const uint32_t pill_colour = generating ? PUCK_COLOUR_SOLAR : PUCK_COLOUR_MUTED;
  lv_obj_set_style_text_color(s_rate, lv_color_hex(pill_colour), LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_pill, lv_color_hex(pill_colour), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_pill, generating ? LV_OPA_20 : LV_OPA_10, LV_PART_MAIN);

  set_forecast_figure(s_forecast, snapshot, snapshot.solar.forecast_kwh, 1, " kWh");
  set_forecast_figure(s_remaining, snapshot, snapshot.solar.remaining_kwh, 1, " kWh");
  set_forecast_figure(s_versus, snapshot, snapshot.solar.vs_forecast_pct, 0, "%");
  set_forecast_figure(s_peak, snapshot, snapshot.solar.peak_kw, 1, " kW");
}

void screen_solar_set_live(bool live) {
  s_live = live;
}
