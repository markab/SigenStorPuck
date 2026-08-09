#include "screen_load.h"

#include <stdio.h>

#include "board_config.h"
#include "chart_band.h"
#include "format.h"
#include "theme.h"

namespace {

// Geometry copied from screens 2 and 3 rather than derived again: these three are
// one family, and a figure that sits four pixels lower here than there is the
// kind of thing you only notice as a wobble when swiping between them.
constexpr lv_coord_t BAND_WIDTH = PUCK_LCD_WIDTH;
constexpr lv_coord_t BAND_HEIGHT = 156;
constexpr lv_coord_t BAND_Y = 90;
constexpr lv_coord_t BAND_CLIP_RADIUS = PUCK_RING_DIAMETER / 2 - PUCK_RING_WIDTH - 4;
// Lower than the other two: this band is drawn in the home colour, which is very
// nearly white, and white at a given opacity reads far brighter than the yellow
// or green they use. Matching the number would not match the weight.
constexpr lv_opa_t BAND_GHOST = 80;

// House load is spikier than a solar curve — a kettle is a step, not a slope —
// so this is wider than screen 2's and about the same as screen 3's. The shape of
// the day is what a backdrop is for; the individual kettle is not.
constexpr uint8_t BAND_SMOOTHING = 15;

constexpr lv_coord_t COLUMN_X = 88;
constexpr lv_coord_t ROW_ONE_LABEL_Y = 44;
constexpr lv_coord_t ROW_ONE_VALUE_Y = 70;
constexpr lv_coord_t ROW_TWO_LABEL_Y = 116;
constexpr lv_coord_t ROW_TWO_VALUE_Y = 142;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_band = nullptr;
lv_obj_t* s_total = nullptr;
lv_obj_t* s_pill = nullptr;
lv_obj_t* s_rate = nullptr;
lv_obj_t* s_house = nullptr;
lv_obj_t* s_ev = nullptr;
lv_obj_t* s_from_solar = nullptr;
lv_obj_t* s_from_grid = nullptr;
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

// The four figures all come from the day's flow split, which only a server has.
void set_figure(lv_obj_t* label, bool known, float kwh) {
  char text[24];
  if (!known) {
    lv_label_set_text(label, "--");
    return;
  }
  snprintf(text, sizeof(text), "%.1f kWh", kwh);
  lv_label_set_text(label, text);
}

}  // namespace

lv_obj_t* screen_load_create(lv_obj_t* parent) {
  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

  // No arc at the bezel, deliberately — see the note in the header.
  s_band = chart_band_create(s_root, HistorySeries::Load, PUCK_COLOUR_HOME);
  if (s_band != nullptr) {
    lv_obj_set_size(s_band, BAND_WIDTH, BAND_HEIGHT);
    lv_obj_align(s_band, LV_ALIGN_CENTER, 0, BAND_Y);
    // Autoscaled: what a house draws has no rated ceiling to be a fraction of.
    chart_band_set_range(s_band, 0.0f, 0.0f);
    chart_band_set_intensity(s_band, BAND_GHOST);
    chart_band_set_bezel_clip(s_band, BAND_CLIP_RADIUS);
    chart_band_set_smoothing(s_band, BAND_SMOOTHING);
  }

  lv_obj_t* title = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 0, -166);
  lv_obj_set_style_text_letter_space(title, 3, LV_PART_MAIN);
  lv_label_set_text(title, "LOAD");

  s_total = make_label(s_root, PUCK_FONT_HERO, PUCK_COLOUR_TEXT, 0, -114);
  lv_label_set_text(s_total, "--");

  lv_obj_t* caption = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 0, -70);
  lv_label_set_text(caption, "used");

  s_pill = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_pill);
  lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(s_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(s_pill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(s_pill, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(s_pill, 7, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_pill, lv_color_hex(PUCK_COLOUR_HOME), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_pill, LV_OPA_20, LV_PART_MAIN);
  lv_obj_align(s_pill, LV_ALIGN_CENTER, 0, -26);

  s_rate = lv_label_create(s_pill);
  lv_obj_set_style_text_font(s_rate, PUCK_FONT_BODY, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_rate, lv_color_hex(PUCK_COLOUR_HOME), LV_PART_MAIN);
  lv_label_set_text(s_rate, "--");
  lv_obj_center(s_rate);

  // The split first, then where it came from: what you used, then what paid for
  // it. Battery's share is on the flows screen rather than repeated here.
  make_caption(s_root, "HOUSE", -COLUMN_X, ROW_ONE_LABEL_Y);
  s_house = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, -COLUMN_X, ROW_ONE_VALUE_Y);
  lv_label_set_text(s_house, "--");

  make_caption(s_root, "EV", COLUMN_X, ROW_ONE_LABEL_Y);
  s_ev = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, COLUMN_X, ROW_ONE_VALUE_Y);
  lv_label_set_text(s_ev, "--");

  make_caption(s_root, "FROM SOLAR", -COLUMN_X, ROW_TWO_LABEL_Y);
  s_from_solar = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, -COLUMN_X, ROW_TWO_VALUE_Y);
  lv_label_set_text(s_from_solar, "--");

  make_caption(s_root, "FROM GRID", COLUMN_X, ROW_TWO_LABEL_Y);
  s_from_grid = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, COLUMN_X, ROW_TWO_VALUE_Y);
  lv_label_set_text(s_from_grid, "--");

  return s_root;
}

void screen_load_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }

  char text[48];
  char scratch[24];

  if (s_band != nullptr) {
    chart_band_refresh(s_band);
  }

  const bool have = snapshot.valid && snapshot.today.present;
  const Snapshot::Today::Flows& f = snapshot.today.flows;

  // The headline: everything the house and the car drew. today.load rather than
  // anything derived, so it is the same number the flows screen adds up to and
  // the one figure that also works on a plant read directly over Modbus.
  if (have && snapshot.today.load.known) {
    snprintf(text, sizeof(text), "%.1f kWh", snapshot.today.load.value);
    lv_label_set_text(s_total, text);
  } else {
    lv_label_set_text(s_total, "--");
  }

  // The pill is live-only, so a past day simply does not have one.
  if (s_live) {
    lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
  }

  // What is being drawn right now — the house plus the car, matching the total
  // above rather than the house alone.
  const MaybeFloat home = snapshot.valid ? snapshot.power.home : MaybeFloat{};
  const float ev_now = snapshot.valid && snapshot.power.ev.known ? snapshot.power.ev.value : 0.0f;
  if (!home.known) {
    lv_label_set_text(s_rate, "-- kW");
  } else if (home.value + ev_now <= 0.0f) {
    lv_label_set_text(s_rate, "idle");
  } else {
    const MaybeFloat total{true, home.value + ev_now};
    puck_format_magnitude(total, PUCK_KW_DECIMALS, scratch, sizeof(scratch));
    snprintf(text, sizeof(text), "%s kW now", scratch);
    lv_label_set_text(s_rate, text);
  }

  // The four figures come out of the day's flow split, which is server-only: the
  // plant's daily counters cannot say which source served the load.
  const bool split = have && f.solar_load.known;
  const float ev_day = (f.solar_ev.known ? f.solar_ev.value : 0.0f) +
                       (f.batt_ev.known ? f.batt_ev.value : 0.0f) +
                       (f.grid_ev.known ? f.grid_ev.value : 0.0f);
  const float total_day = have && snapshot.today.load.known ? snapshot.today.load.value : 0.0f;
  const float house_day = total_day - ev_day > 0.0f ? total_day - ev_day : 0.0f;

  set_figure(s_house, split, house_day);
  set_figure(s_ev, split, ev_day);
  set_figure(s_from_solar, split, f.solar_load.known ? f.solar_load.value : 0.0f);
  set_figure(s_from_grid, f.grid_load.known, f.grid_load.known ? f.grid_load.value : 0.0f);
}

void screen_load_set_live(bool live) {
  s_live = live;
}
