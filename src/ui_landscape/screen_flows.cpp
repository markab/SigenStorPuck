// Screen (swiped 5th), landscape: today's energy sources and sinks for the
// 2.41" board. Server source only.
//
// Phase 3a scaffold: the three sources and four sinks as labelled kWh figures in
// two columns. The hand-drawn ribbons and the self-sufficiency edge bar are
// follow-up polish.

#include "screen_flows.h"

#include <stdio.h>

#include "board_config.h"
#include "theme.h"

namespace {

lv_obj_t* s_root = nullptr;
lv_obj_t* s_solar = nullptr;
lv_obj_t* s_grid_in = nullptr;
lv_obj_t* s_batt_in = nullptr;
lv_obj_t* s_home = nullptr;
lv_obj_t* s_ev = nullptr;
lv_obj_t* s_batt_out = nullptr;
lv_obj_t* s_grid_out = nullptr;

lv_obj_t* label_at(lv_obj_t* parent, const char* name, uint32_t colour, lv_coord_t dx,
                   lv_coord_t dy, lv_text_align_t align) {
  lv_obj_t* title = lv_label_create(parent);
  lv_obj_set_style_text_font(title, PUCK_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(colour), LV_PART_MAIN);
  lv_label_set_text(title, name);
  lv_obj_align(title, LV_ALIGN_CENTER, dx, dy - 12);

  lv_obj_t* value = lv_label_create(parent);
  lv_obj_set_style_text_font(value, PUCK_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(value, lv_color_hex(PUCK_COLOUR_TEXT), LV_PART_MAIN);
  lv_obj_set_style_text_align(value, align, LV_PART_MAIN);
  lv_label_set_text(value, "--");
  lv_obj_align(value, LV_ALIGN_CENTER, dx, dy + 10);
  return value;
}

void set_kwh(lv_obj_t* label, const MaybeFloat& a, const MaybeFloat& b, const MaybeFloat& c) {
  float total = 0.0f;
  bool known = false;
  if (a.known) { total += a.value; known = true; }
  if (b.known) { total += b.value; known = true; }
  if (c.known) { total += c.value; known = true; }
  char text[16];
  if (known) {
    snprintf(text, sizeof(text), "%.1f kWh", total);
  } else {
    snprintf(text, sizeof(text), "--");
  }
  lv_label_set_text(label, text);
}

}  // namespace

lv_obj_t* screen_flows_create(lv_obj_t* parent) {
  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_center(s_root);

  s_solar = label_at(s_root, "SOLAR", PUCK_COLOUR_SOLAR, -210, -75, LV_TEXT_ALIGN_LEFT);
  s_grid_in = label_at(s_root, "GRID", PUCK_COLOUR_GRID, -210, 0, LV_TEXT_ALIGN_LEFT);
  s_batt_in = label_at(s_root, "BATTERY", PUCK_COLOUR_BATTERY, -210, 75, LV_TEXT_ALIGN_LEFT);

  s_home = label_at(s_root, "HOME", PUCK_COLOUR_HOME, 210, -105, LV_TEXT_ALIGN_RIGHT);
  s_ev = label_at(s_root, "EV", PUCK_COLOUR_EV, 210, -35, LV_TEXT_ALIGN_RIGHT);
  s_batt_out = label_at(s_root, "BATTERY", PUCK_COLOUR_BATTERY, 210, 35, LV_TEXT_ALIGN_RIGHT);
  s_grid_out = label_at(s_root, "GRID", PUCK_COLOUR_GRID, 210, 105, LV_TEXT_ALIGN_RIGHT);
  return s_root;
}

void screen_flows_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }
  const Snapshot::Today::Flows& f = snapshot.today.flows;
  const MaybeFloat none;
  // Sources: what each supplied across all its sinks.
  set_kwh(s_solar, f.solar_load, f.solar_batt, f.solar_grid);
  set_kwh(s_grid_in, f.grid_load, f.grid_batt, none);
  set_kwh(s_batt_in, f.batt_load, f.batt_grid, none);
  // Sinks: what each received.
  set_kwh(s_home, f.solar_load, f.grid_load, f.batt_load);
  set_kwh(s_ev, f.solar_ev, f.grid_ev, f.batt_ev);
  set_kwh(s_batt_out, f.solar_batt, f.grid_batt, none);
  set_kwh(s_grid_out, f.solar_grid, f.batt_grid, none);
}
