// Screen 3, landscape: the solar day for the 2.41" board.
//
// Phase 3a scaffold: headline generation, a live PV pill and the forecast
// figures. The generation-vs-forecast edge bar and the ghosted day chart are
// follow-up polish.

#include "screen_solar.h"

#include <stdio.h>

#include "board_config.h"
#include "chart_band.h"
#include "edge_bar.h"
#include "format.h"
#include "theme.h"

namespace {

// The day's generation curve, ghosted right back behind the figures. Range auto,
// so a dull day and a bright one both fill the band.
constexpr lv_coord_t BAND_WIDTH = 564;  // 00:00..24:00 touch the inner ring both sides
constexpr lv_coord_t BAND_HEIGHT = 130;
constexpr lv_coord_t BAND_Y = 108;   // lower strip, bottom clear of the ring corners
constexpr lv_opa_t BAND_GHOST = 110;
constexpr uint8_t BAND_SMOOTHING = 7;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_band = nullptr;
lv_obj_t* s_edge = nullptr;
lv_obj_t* s_headline = nullptr;
lv_obj_t* s_pill = nullptr;
lv_obj_t* s_forecast = nullptr;
lv_obj_t* s_remaining = nullptr;
bool s_live = true;

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t colour,
                     lv_coord_t dx, lv_coord_t dy) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(colour), LV_PART_MAIN);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(label, LV_ALIGN_CENTER, dx, dy);
  return label;
}

}  // namespace

lv_obj_t* screen_solar_create(lv_obj_t* parent, uint8_t /*figures*/) {
  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_center(s_root);

  // The ghosted day chart, behind everything else.
  s_band = chart_band_create(s_root, HistorySeries::Pv, PUCK_COLOUR_SOLAR);
  if (s_band != nullptr) {
    lv_obj_set_size(s_band, BAND_WIDTH, BAND_HEIGHT);
    lv_obj_align(s_band, LV_ALIGN_CENTER, 0, BAND_Y);
    chart_band_set_range(s_band, 0.0f, 0.0f);
    chart_band_set_intensity(s_band, BAND_GHOST);
    chart_band_set_smoothing(s_band, BAND_SMOOTHING);
  }

  // Generation against today's forecast; hidden outright when there is none.
  s_edge = edge_bar_create(s_root);

  lv_obj_t* caption = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_MUTED, -110, -132);
  lv_label_set_text(caption, "GENERATED");
  s_headline = make_label(s_root, PUCK_FONT_HERO, PUCK_COLOUR_TEXT, -110, -82);
  lv_label_set_text(s_headline, "--");
  lv_obj_t* unit = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_MUTED, -110, -40);
  lv_label_set_text(unit, "kWh so far");
  s_pill = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_SOLAR, -110, 0);
  lv_label_set_text(s_pill, "");

  s_forecast = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_TEXT, 150, -100);
  lv_label_set_text(s_forecast, "");
  s_remaining = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_TEXT, 150, -52);
  lv_label_set_text(s_remaining, "");
  return s_root;
}

void screen_solar_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }
  if (s_band != nullptr) {
    chart_band_refresh(s_band);
  }
  char text[24];
  if (snapshot.valid && snapshot.today.present && snapshot.today.solar.known) {
    snprintf(text, sizeof(text), "%.1f", snapshot.today.solar.value);
    lv_label_set_text(s_headline, text);
  } else {
    lv_label_set_text(s_headline, "--");
  }

  // The ring is generation so far against the whole day's forecast. No forecast,
  // no ring — an unfilled track reads as a real zero, and the Modbus source has
  // no forecast.
  const bool has_forecast = snapshot.valid && snapshot.solar.configured &&
                            snapshot.solar.forecast_kwh.known &&
                            snapshot.solar.forecast_kwh.value > 0.0f;
  if (has_forecast && snapshot.today.present && snapshot.today.solar.known) {
    edge_bar_set_hidden(s_edge, false);
    edge_bar_set(s_edge, snapshot.today.solar.value / snapshot.solar.forecast_kwh.value,
                 PUCK_COLOUR_SOLAR);
  } else {
    edge_bar_set_hidden(s_edge, true);
  }

  if (s_live && snapshot.valid && snapshot.power.pv.known) {
    puck_format_magnitude(snapshot.power.pv, PUCK_KW_DECIMALS, text, sizeof(text));
    char pill[32];
    snprintf(pill, sizeof(pill), "%s kW now", text);
    lv_label_set_text(s_pill, pill);
    lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
  }

  if (snapshot.valid && snapshot.solar.configured && snapshot.solar.forecast_kwh.known) {
    snprintf(text, sizeof(text), "%.1f kWh forecast", snapshot.solar.forecast_kwh.value);
    lv_label_set_text(s_forecast, text);
  } else {
    lv_label_set_text(s_forecast, "");
  }
  if (snapshot.valid && snapshot.solar.configured && snapshot.solar.remaining_kwh.known) {
    snprintf(text, sizeof(text), "%.1f kWh to come", snapshot.solar.remaining_kwh.value);
    lv_label_set_text(s_remaining, text);
  } else {
    lv_label_set_text(s_remaining, "");
  }
}

void screen_solar_set_live(bool live) {
  s_live = live;
}
