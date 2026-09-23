#include "screen_grid.h"

#include <math.h>
#include <stdio.h>

#include "board_config.h"
#include "chart_band.h"
#include "format.h"
#include "theme.h"

namespace {

// ------------------------------------------------------------------ layout ---
//
// A day screen like battery, solar and load, but with no bezel ring: import and
// export have no ceiling to be a fraction of. The figures sit in the upper half —
// the two day totals, the two live AC figures, and a live import/export pill —
// and the day's grid power runs as a bipolar chart across the lower half, import
// filling up in grid blue and export down in a paler shade about a zero line.
//
// The chart is sized to sit inside the bezel rather than run full width and clip
// to it: a bipolar band's fill straddles its own middle, and a rectangle that
// reaches both edges would leave its corners outside the ring with no floor to
// follow. Kept to ~330 px it clears the glass at every row.

constexpr lv_coord_t BAND_WIDTH = 330;
constexpr lv_coord_t BAND_HEIGHT = 120;
constexpr lv_coord_t BAND_Y = 78;  // centre; the zero line lands here
constexpr lv_opa_t BAND_GHOST = 120;
constexpr uint8_t BAND_SMOOTHING = 5;

// Import in grid blue, export a paler blue below the line.
constexpr uint32_t EXPORT_COLOUR = 0x7FB2F0;

constexpr lv_coord_t COLUMN_X = 90;
constexpr lv_coord_t ROW_ONE_LABEL_Y = -132;
constexpr lv_coord_t ROW_ONE_VALUE_Y = -106;
constexpr lv_coord_t PILL_Y = -60;
constexpr lv_coord_t ROW_TWO_LABEL_Y = -18;
constexpr lv_coord_t ROW_TWO_VALUE_Y = 8;

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

}  // namespace

lv_obj_t* screen_grid_create(lv_obj_t* parent) {
  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

  // Before the labels, so they sit on top of it.
  s_band = chart_band_create(s_root, HistorySeries::Grid, PUCK_COLOUR_GRID);
  if (s_band != nullptr) {
    lv_obj_set_size(s_band, BAND_WIDTH, BAND_HEIGHT);
    lv_obj_align(s_band, LV_ALIGN_CENTER, 0, BAND_Y);
    chart_band_set_intensity(s_band, BAND_GHOST);
    chart_band_set_smoothing(s_band, BAND_SMOOTHING);
    chart_band_set_bipolar(s_band, true, EXPORT_COLOUR);
  }

  lv_obj_t* title = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 0, -172);
  lv_obj_set_style_text_letter_space(title, 3, LV_PART_MAIN);
  lv_label_set_text(title, "GRID");

  make_caption(s_root, "IMPORTED", -COLUMN_X, ROW_ONE_LABEL_Y);
  s_imported = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, -COLUMN_X, ROW_ONE_VALUE_Y);
  lv_label_set_text(s_imported, "--");
  make_caption(s_root, "EXPORTED", COLUMN_X, ROW_ONE_LABEL_Y);
  s_exported = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, COLUMN_X, ROW_ONE_VALUE_Y);
  lv_label_set_text(s_exported, "--");

  s_pill = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_GRID, 0, PILL_Y);
  lv_label_set_text(s_pill, "");

  s_freq_caption = make_caption(s_root, "FREQUENCY", -COLUMN_X, ROW_TWO_LABEL_Y);
  s_freq = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, -COLUMN_X, ROW_TWO_VALUE_Y);
  lv_label_set_text(s_freq, "");
  s_volt_caption = make_caption(s_root, "VOLTAGE", COLUMN_X, ROW_TWO_LABEL_Y);
  s_volt = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, COLUMN_X, ROW_TWO_VALUE_Y);
  lv_label_set_text(s_volt, "");

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

  // The live import/export rate, hidden on a past day.
  if (s_live && snapshot.valid && snapshot.power.grid.known) {
    const float g = snapshot.power.grid.value;
    if (fabsf(g) < 0.05f) {
      lv_label_set_text(s_pill, "grid balanced");
    } else {
      char num[16];
      MaybeFloat mag;
      mag.known = true;
      mag.value = fabsf(g);
      puck_format_magnitude(mag, PUCK_KW_DECIMALS, num, sizeof(num));
      snprintf(text, sizeof(text), "%s kW %s", num, g > 0.0f ? "importing" : "exporting");
      lv_label_set_text(s_pill, text);
    }
    lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
  }

  // Frequency and voltage: Modbus-only and live. The caption travels with its
  // figure so the server source shows a clean gap, not two dangling labels.
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
