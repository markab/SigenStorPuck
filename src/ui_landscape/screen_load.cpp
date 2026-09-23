// Screen (swiped 4th), landscape: the day's consumption for the 2.41" board.
//
// Headline consumption over a ghosted day curve, with a live draw pill. The band
// is ghosted lower than the others because the near-white home colour reads far
// brighter at the same opacity.
//
// The bezel ring here is self-sufficiency, not consumption: consumption has no
// ceiling to be a fraction of, but the share of it met without the grid is a
// clean 0..100%, so it earns a ring. Drawn only when the source carries the day's
// flow split; hidden otherwise, like the figure beside it.

#include "screen_load.h"

#include <math.h>
#include <stdio.h>

#include "board_config.h"
#include "chart_band.h"
#include "edge_bar.h"
#include "format.h"
#include "theme.h"

namespace {

constexpr lv_coord_t BAND_WIDTH = 564;  // 00:00..24:00 touch the inner ring both sides
constexpr lv_coord_t BAND_HEIGHT = 130;
constexpr lv_coord_t BAND_Y = 108;   // lower strip, bottom clear of the ring corners
constexpr lv_opa_t BAND_GHOST = 70;  // lower than the others: home is near-white
constexpr uint8_t BAND_SMOOTHING = 7;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_band = nullptr;
lv_obj_t* s_edge = nullptr;
lv_obj_t* s_headline = nullptr;
lv_obj_t* s_pill = nullptr;
lv_obj_t* s_selfsuff_caption = nullptr;
lv_obj_t* s_selfsuff = nullptr;
bool s_live = true;
bool s_breakdown = false;

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

lv_obj_t* screen_load_create(lv_obj_t* parent, bool with_breakdown) {
  s_breakdown = with_breakdown;
  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_center(s_root);

  // The ghosted day curve, behind everything else.
  s_band = chart_band_create(s_root, HistorySeries::Load, PUCK_COLOUR_HOME);
  if (s_band != nullptr) {
    lv_obj_set_size(s_band, BAND_WIDTH, BAND_HEIGHT);
    lv_obj_align(s_band, LV_ALIGN_CENTER, 0, BAND_Y);
    chart_band_set_range(s_band, 0.0f, 0.0f);
    chart_band_set_intensity(s_band, BAND_GHOST);
    chart_band_set_smoothing(s_band, BAND_SMOOTHING);
  }

  // Self-sufficiency traces the bezel; hidden until there is a figure for it.
  s_edge = edge_bar_create(s_root);
  edge_bar_set_hidden(s_edge, true);

  // Same top-quadrant layout as the solar screen: left column of caption /
  // headline / unit / pill, top lines at -156.
  lv_obj_t* caption = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_MUTED, -110, -156);
  lv_label_set_text(caption, "CONSUMED");
  s_headline = make_label(s_root, PUCK_FONT_HERO, PUCK_COLOUR_TEXT, -110, -106);
  lv_label_set_text(s_headline, "--");
  lv_obj_t* unit = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_MUTED, -110, -64);
  lv_label_set_text(unit, "kWh used");
  s_pill = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_HOME, -110, -24);
  lv_label_set_text(s_pill, "");

  // Self-sufficiency, top-right — moved here off the flows screen. Only shown when
  // the source carries the day's flow split (server), so on Modbus/HA the quadrant
  // stays empty rather than reading a false 100%.
  s_selfsuff_caption = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_MUTED, 150, -156);
  lv_label_set_text(s_selfsuff_caption, "SELF-SUFFICIENT");
  lv_obj_add_flag(s_selfsuff_caption, LV_OBJ_FLAG_HIDDEN);
  s_selfsuff = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_HOME, 150, -122);
  lv_label_set_text(s_selfsuff, "");
  return s_root;
}

void screen_load_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }
  if (s_band != nullptr) {
    chart_band_refresh(s_band);
  }
  char text[24];
  if (snapshot.valid && snapshot.today.present && snapshot.today.load.known) {
    snprintf(text, sizeof(text), "%.1f", snapshot.today.load.value);
    lv_label_set_text(s_headline, text);
  } else {
    lv_label_set_text(s_headline, "--");
  }

  // Live draw is house plus car.
  if (s_live && snapshot.valid && snapshot.power.home.known) {
    float draw = snapshot.power.home.value;
    if (snapshot.power.ev.known) {
      draw += snapshot.power.ev.value;
    }
    snprintf(text, sizeof(text), "%.2f kW now", draw);
    lv_label_set_text(s_pill, text);
    lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
  }

  // Self-sufficiency: the share of the day's consumption not drawn from the grid.
  // grid_load is the whole grid->consumption flow (house and car), so the rest of
  // today.load came from solar and the battery. Needs the flow split, which only
  // the server carries — grid_load unknown means hide, not a false 100%.
  const Snapshot::Today::Flows& f = snapshot.today.flows;
  if (snapshot.valid && snapshot.today.present && snapshot.today.load.known &&
      snapshot.today.load.value > 0.0f && f.grid_load.known) {
    const float from_grid = f.grid_load.value > 0.0f ? f.grid_load.value : 0.0f;
    float pct = (snapshot.today.load.value - from_grid) / snapshot.today.load.value * 100.0f;
    if (pct < 0.0f) {
      pct = 0.0f;
    }
    if (pct > 100.0f) {
      pct = 100.0f;
    }
    snprintf(text, sizeof(text), "%.0f%%", pct);
    lv_label_set_text(s_selfsuff, text);
    lv_obj_clear_flag(s_selfsuff_caption, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_selfsuff, LV_OBJ_FLAG_HIDDEN);
    edge_bar_set_hidden(s_edge, false);
    edge_bar_set(s_edge, pct / 100.0f, PUCK_COLOUR_HOME);
  } else {
    lv_label_set_text(s_selfsuff, "");
    lv_obj_add_flag(s_selfsuff_caption, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_selfsuff, LV_OBJ_FLAG_HIDDEN);
    edge_bar_set_hidden(s_edge, true);
  }
}

void screen_load_set_live(bool live) {
  s_live = live;
}
