// Grid screen, landscape (600x450): the day at the grid for the 2.41" board.
//
// Two totals down the left (imported, exported), the live AC figures down the
// right (frequency, phase voltage), a live import/export pill between them, and
// the day's grid power as a bipolar chart behind it all — import filling up in
// grid blue, export down in a paler shade, about a centre zero line.

#include "screen_grid.h"

#include <math.h>
#include <stdio.h>

#include "board_config.h"
#include "chart_band.h"
#include "format.h"
#include "history.h"
#include "theme.h"

namespace {

// The day's grid power, ghosted behind the figures. Bipolar, so it fills both
// ways from the middle; the range is symmetric, set by chart_band.
constexpr lv_coord_t BAND_WIDTH = 564;  // 00:00..24:00 touch the inner ring both sides
constexpr lv_coord_t BAND_HEIGHT = 150;
constexpr lv_coord_t BAND_Y = 96;
constexpr lv_opa_t BAND_GHOST = 90;
constexpr uint8_t BAND_SMOOTHING = 5;

// Import in grid blue, export a paler blue behind and below the line.
constexpr uint32_t EXPORT_COLOUR = 0x7FB2F0;

// Two stats a column, symmetric: imported/exported on the left, frequency/voltage
// on the right, both columns' top caption on the same -152 line.
constexpr lv_coord_t COL_L = -150;
constexpr lv_coord_t COL_R = 150;
constexpr lv_coord_t ROW1_CAP_Y = -152;
constexpr lv_coord_t ROW1_VAL_Y = -114;
constexpr lv_coord_t ROW2_CAP_Y = -76;
constexpr lv_coord_t ROW2_VAL_Y = -38;
constexpr lv_coord_t PILL_Y = 16;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_band = nullptr;
lv_obj_t* s_imported = nullptr;
lv_obj_t* s_exported = nullptr;
lv_obj_t* s_pill = nullptr;
lv_obj_t* s_freq_caption = nullptr;
lv_obj_t* s_freq = nullptr;
lv_obj_t* s_volt_caption = nullptr;
lv_obj_t* s_volt = nullptr;
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

lv_obj_t* screen_grid_create(lv_obj_t* parent) {
  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_center(s_root);

  s_band = chart_band_create(s_root, HistorySeries::Grid, PUCK_COLOUR_GRID);
  if (s_band != nullptr) {
    lv_obj_set_size(s_band, BAND_WIDTH, BAND_HEIGHT);
    lv_obj_align(s_band, LV_ALIGN_CENTER, 0, BAND_Y);
    chart_band_set_intensity(s_band, BAND_GHOST);
    chart_band_set_smoothing(s_band, BAND_SMOOTHING);
    chart_band_set_bipolar(s_band, true, EXPORT_COLOUR);
  }

  lv_obj_t* imp_cap = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_MUTED, COL_L, ROW1_CAP_Y);
  lv_label_set_text(imp_cap, "IMPORTED");
  s_imported = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_TEXT, COL_L, ROW1_VAL_Y);
  lv_label_set_text(s_imported, "--");
  lv_obj_t* exp_cap = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_MUTED, COL_L, ROW2_CAP_Y);
  lv_label_set_text(exp_cap, "EXPORTED");
  s_exported = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_TEXT, COL_L, ROW2_VAL_Y);
  lv_label_set_text(s_exported, "--");

  s_freq_caption = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_MUTED, COL_R, ROW1_CAP_Y);
  lv_label_set_text(s_freq_caption, "FREQUENCY");
  s_freq = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_TEXT, COL_R, ROW1_VAL_Y);
  lv_label_set_text(s_freq, "");
  s_volt_caption = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_MUTED, COL_R, ROW2_CAP_Y);
  lv_label_set_text(s_volt_caption, "VOLTAGE");
  s_volt = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_TEXT, COL_R, ROW2_VAL_Y);
  lv_label_set_text(s_volt, "");

  s_pill = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_GRID, 0, PILL_Y);
  lv_label_set_text(s_pill, "");
  return s_root;
}

void screen_grid_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }
  if (s_band != nullptr) {
    chart_band_refresh(s_band);
  }

  char text[32];
  // The day totals follow the date. today.imported/exported are the whole day,
  // house and car alike.
  if (snapshot.valid && snapshot.today.present && snapshot.today.imported.known) {
    snprintf(text, sizeof(text), "%.1f kWh", snapshot.today.imported.value);
    lv_label_set_text(s_imported, text);
  } else {
    lv_label_set_text(s_imported, "--");
  }
  if (snapshot.valid && snapshot.today.present && snapshot.today.exported.known) {
    snprintf(text, sizeof(text), "%.1f kWh", snapshot.today.exported.value);
    lv_label_set_text(s_exported, text);
  } else {
    lv_label_set_text(s_exported, "--");
  }

  // The live import/export rate, from the same grid power the main screen shows.
  // Hidden on a past day, where "now" is not what the totals are about, and when
  // next to nothing is flowing either way — a meter's idle jitter needs no label.
  if (s_live && snapshot.valid && snapshot.power.grid.known &&
      fabsf(snapshot.power.grid.value) >= 0.05f) {
    const float g = snapshot.power.grid.value;
    char num[16];
    MaybeFloat mag;
    mag.known = true;
    mag.value = fabsf(g);
    puck_format_magnitude(mag, PUCK_KW_DECIMALS, num, sizeof(num));
    snprintf(text, sizeof(text), "%s kW %s", num, g > 0.0f ? "importing" : "exporting");
    lv_label_set_text(s_pill, text);
    lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
  }

  // Frequency and voltage are Modbus-only and live: shown only on the live day,
  // and only when the source read them. Caption travels with its figure so the
  // server source shows a clean empty quadrant rather than two dangling labels.
  const bool freq_known = s_live && snapshot.valid && snapshot.power.grid_freq_hz.known;
  if (freq_known) {
    snprintf(text, sizeof(text), "%.2f Hz", snapshot.power.grid_freq_hz.value);
    lv_label_set_text(s_freq, text);
    lv_obj_clear_flag(s_freq_caption, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_label_set_text(s_freq, "");
    lv_obj_add_flag(s_freq_caption, LV_OBJ_FLAG_HIDDEN);
  }

  const bool volt_known = s_live && snapshot.valid && snapshot.power.grid_voltage_v.known;
  if (volt_known) {
    snprintf(text, sizeof(text), "%.0f V", snapshot.power.grid_voltage_v.value);
    lv_label_set_text(s_volt, text);
    lv_obj_clear_flag(s_volt_caption, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_label_set_text(s_volt, "");
    lv_obj_add_flag(s_volt_caption, LV_OBJ_FLAG_HIDDEN);
  }
}

void screen_grid_set_live(bool live) {
  s_live = live;
}
