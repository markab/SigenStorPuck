// Screen 2, landscape: battery detail for the 2.41" board.
//
// The day's state of charge ghosted across the whole width as a backdrop, with
// the headline SoC, a live charge/discharge pill and the supporting figures laid
// over it — the round screen's shape, re-laid for 600x450.

#include "screen_battery.h"

#include <math.h>
#include <stdio.h>

#include "board_config.h"
#include "chart_band.h"
#include "edge_bar.h"
#include "format.h"
#include "theme.h"

namespace {

// The day's SoC, ghosted right back behind the figures.
constexpr lv_coord_t BAND_WIDTH = 564;  // 00:00..24:00 touch the inner ring both sides
constexpr lv_coord_t BAND_HEIGHT = 130;
constexpr lv_coord_t BAND_Y = 108;   // lower strip, bottom clear of the ring corners
constexpr lv_opa_t BAND_GHOST = 110;
constexpr uint8_t BAND_SMOOTHING = 7;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_band = nullptr;
lv_obj_t* s_edge = nullptr;
lv_obj_t* s_headline = nullptr;
lv_obj_t* s_stored = nullptr;
lv_obj_t* s_pill = nullptr;
lv_obj_t* s_health = nullptr;
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

lv_obj_t* screen_battery_create(lv_obj_t* parent) {
  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_center(s_root);

  // The ghosted day chart, behind everything else.
  s_band = chart_band_create(s_root, HistorySeries::Soc, PUCK_COLOUR_BATTERY);
  if (s_band != nullptr) {
    lv_obj_set_size(s_band, BAND_WIDTH, BAND_HEIGHT);
    lv_obj_align(s_band, LV_ALIGN_CENTER, 0, BAND_Y);
    chart_band_set_range(s_band, 0.0f, 100.0f);
    chart_band_set_intensity(s_band, BAND_GHOST);
    chart_band_set_smoothing(s_band, BAND_SMOOTHING);
  }

  s_edge = edge_bar_create(s_root);

  lv_obj_t* caption = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_MUTED, -110, -132);
  lv_label_set_text(caption, "STATE OF CHARGE");
  s_headline = make_label(s_root, PUCK_FONT_HERO, PUCK_COLOUR_TEXT, -110, -82);
  lv_label_set_text(s_headline, "--%");
  s_stored = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_MUTED, -110, -40);
  lv_label_set_text(s_stored, "");
  s_pill = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_BATTERY, -110, 0);
  lv_label_set_text(s_pill, "");

  s_health = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_TEXT, 150, -100);
  lv_label_set_text(s_health, "");
  return s_root;
}

void screen_battery_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }
  if (s_band != nullptr) {
    chart_band_refresh(s_band);
  }
  char text[24];
  if (snapshot.valid && snapshot.battery.soc_pct.known) {
    snprintf(text, sizeof(text), "%.0f%%", snapshot.battery.soc_pct.value);
    lv_label_set_text(s_headline, text);
    edge_bar_set(s_edge, snapshot.battery.soc_pct.value / 100.0f, PUCK_COLOUR_BATTERY);
    if (snapshot.battery.capacity_kwh.known) {
      const float stored = snapshot.battery.capacity_kwh.value *
                           snapshot.battery.soc_pct.value / 100.0f;
      snprintf(text, sizeof(text), "%.1f kWh stored", stored);
      lv_label_set_text(s_stored, text);
    }
  } else {
    lv_label_set_text(s_headline, "--%");
    lv_label_set_text(s_stored, "");
    edge_bar_set(s_edge, 0.0f, PUCK_COLOUR_BATTERY);
  }

  if (s_live && snapshot.valid && snapshot.power.batt.known &&
      fabsf(snapshot.power.batt.value) > 0.0f) {
    const bool charging = snapshot.power.batt.value > 0.0f;
    char pill[32];
    puck_format_magnitude(snapshot.power.batt, PUCK_KW_DECIMALS, text, sizeof(text));
    snprintf(pill, sizeof(pill), "%s%s kW %s", charging ? "+" : "", text,
             charging ? "charging" : "discharging");
    lv_label_set_text(s_pill, pill);
    lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
  }

  if (snapshot.valid && snapshot.battery.soh_pct.known) {
    snprintf(text, sizeof(text), "%.0f%% health", snapshot.battery.soh_pct.value);
    lv_label_set_text(s_health, text);
  } else {
    lv_label_set_text(s_health, "");
  }
}

void screen_battery_set_live(bool live) {
  s_live = live;
}
