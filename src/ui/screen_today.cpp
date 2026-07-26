#include "screen_today.h"

#include <stdio.h>

#include "board_config.h"
#include "chart_band.h"
#include "format.h"
#include "theme.h"

namespace {

// Rows are a fixed width so labels and values line up in two columns. Narrower
// than the inscribed square because these rows sit low, where the circle has
// already started closing in.
constexpr lv_coord_t ROW_WIDTH = 268;
constexpr lv_coord_t ROWS_TOP = 8;

// The day's PV curve (§D3), in the strip between the last row and the page dots.
//
// Behind the headline figure was tried first and abandoned: solar yellow at any
// opacity that shows the curve also fights white digits sitting on top of it,
// and the peak lands squarely behind the number around the middle of the day.
// Down here it costs no legibility and still reads as a sparkline for the total
// above it. The width is set by the bezel, not by taste — the circle has closed
// in to about 266 px by the bottom of this strip.
constexpr lv_coord_t BAND_WIDTH = 258;
constexpr lv_coord_t BAND_HEIGHT = 38;
constexpr lv_coord_t BAND_Y = 172;

// Solar is the headline; the rest are a breakdown beneath it. Colours match the
// legs on screen 1, so a figure means the same thing on both.
struct RowSpec {
  const char* label;
  uint32_t colour;
};

enum RowId { ROW_HOUSE = 0, ROW_IMPORT, ROW_EXPORT, ROW_CHARGED, ROW_DISCHARGED, ROW_COUNT };

constexpr RowSpec ROWS[ROW_COUNT] = {
    {"house", PUCK_COLOUR_HOME},
    {"imported", PUCK_COLOUR_GRID},
    {"exported", PUCK_COLOUR_GRID},
    {"into battery", PUCK_COLOUR_BATTERY},
    {"from battery", PUCK_COLOUR_BATTERY},
};

lv_obj_t* s_root = nullptr;
lv_obj_t* s_solar_name = nullptr;
lv_obj_t* s_solar = nullptr;
lv_obj_t* s_solar_unit = nullptr;
lv_obj_t* s_rows_box = nullptr;
lv_obj_t* s_values[ROW_COUNT] = {};
lv_obj_t* s_absent = nullptr;
lv_obj_t* s_band = nullptr;

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t colour) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(colour), LV_PART_MAIN);
  return label;
}

lv_obj_t* make_group(lv_obj_t* parent) {
  lv_obj_t* group = lv_obj_create(parent);
  lv_obj_remove_style_all(group);
  lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
  return group;
}

}  // namespace

lv_obj_t* screen_today_create(lv_obj_t* parent) {
  s_root = make_group(parent);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);

  // First, so every label that follows sits on top of it.
  s_band = chart_band_create(s_root, HistorySeries::Pv, PUCK_COLOUR_SOLAR);
  if (s_band != nullptr) {
    lv_obj_set_size(s_band, BAND_WIDTH, BAND_HEIGHT);
    lv_obj_align(s_band, LV_ALIGN_CENTER, 0, BAND_Y);
    // Autoscaled from zero: a quiet day should look quiet rather than be
    // stretched to fill the band.
    chart_band_set_range(s_band, 0.0f, 0.0f);
  }

  lv_obj_t* title = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_label_set_text(title, "TODAY");
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -140);

  // Generation is what a solar owner looks at first, so it gets the hero slot.
  s_solar_name = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_SOLAR);
  lv_obj_t* solar_name = s_solar_name;
  lv_label_set_text(solar_name, "SOLAR");
  lv_obj_align(solar_name, LV_ALIGN_CENTER, 0, -108);

  s_solar = make_label(s_root, PUCK_FONT_HERO, PUCK_COLOUR_TEXT);
  lv_label_set_text(s_solar, "--");
  lv_obj_align(s_solar, LV_ALIGN_CENTER, 0, -68);

  s_solar_unit = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_label_set_text(s_solar_unit, "kWh");
  lv_obj_align(s_solar_unit, LV_ALIGN_CENTER, 0, -30);

  s_rows_box = make_group(s_root);
  lv_obj_set_size(s_rows_box, ROW_WIDTH, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_rows_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_rows_box, 4, LV_PART_MAIN);
  lv_obj_align(s_rows_box, LV_ALIGN_CENTER, 0, ROWS_TOP + 70);

  for (int i = 0; i < ROW_COUNT; ++i) {
    lv_obj_t* row = make_group(s_rows_box);
    lv_obj_set_size(row, ROW_WIDTH, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    // Label hard left, value hard right: a shared right edge makes the column of
    // numbers scannable without drawing any rules.
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* label = make_label(row, PUCK_FONT_SMALL, ROWS[i].colour);
    lv_label_set_text(label, ROWS[i].label);

    s_values[i] = make_label(row, PUCK_FONT_BODY, PUCK_COLOUR_TEXT);
    lv_label_set_text(s_values[i], "--");
  }

  // Shown instead of the figures when the server reports today as null.
  s_absent = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_WARN);
  lv_label_set_text(s_absent, "today's totals\nunavailable");
  lv_obj_set_style_text_align(s_absent, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(s_absent, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(s_absent, LV_OBJ_FLAG_HIDDEN);

  return s_root;
}

void screen_today_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }

  // The documented gotcha: today is null, not an object, when the day cannot be
  // read. Showing a column of dashes would imply six separately-missing
  // registers rather than one absent day, so the whole block is replaced.
  const bool present = snapshot.valid && snapshot.today.present;
  if (!present) {
    // Hide the headline too. A dashed "SOLAR -- kWh" above the message says the
    // same thing twice, and worse, implies solar specifically is missing rather
    // than the whole day.
    lv_obj_add_flag(s_solar_name, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_solar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_solar_unit, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rows_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_absent, LV_OBJ_FLAG_HIDDEN);
    // The PV curve is still true — it comes from live power, not from the day
    // block — but a chart behind a warning message reads as decoration on an
    // error. Hidden with the rest of the figures.
    if (s_band != nullptr) {
      lv_obj_add_flag(s_band, LV_OBJ_FLAG_HIDDEN);
    }
    return;
  }
  if (s_band != nullptr) {
    lv_obj_clear_flag(s_band, LV_OBJ_FLAG_HIDDEN);
    chart_band_refresh(s_band);
  }
  lv_obj_clear_flag(s_solar_name, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_solar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_solar_unit, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_rows_box, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_absent, LV_OBJ_FLAG_HIDDEN);

  const Snapshot::Today& today = snapshot.today;
  char text[16];

  puck_format_magnitude(today.solar, 1, text, sizeof(text));
  lv_label_set_text(s_solar, text);

  const MaybeFloat* values[ROW_COUNT] = {
      &today.load, &today.imported, &today.exported, &today.charge, &today.discharge,
  };
  for (int i = 0; i < ROW_COUNT; ++i) {
    puck_format_magnitude(*values[i], 1, text, sizeof(text));
    lv_label_set_text(s_values[i], text);
  }
}
