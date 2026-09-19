// Screen 2, landscape: battery detail for the 2.41" board.
//
// Phase 3a scaffold: headline state of charge, a live charge/discharge pill and
// two supporting figures. The ghosted day chart and the full four-figure block
// from the mock are follow-up polish.

#include "screen_battery.h"

#include <math.h>
#include <stdio.h>

#include "board_config.h"
#include "format.h"
#include "theme.h"

namespace {

lv_obj_t* s_root = nullptr;
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

  lv_obj_t* caption = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, -110, -80);
  lv_label_set_text(caption, "STATE OF CHARGE");
  s_headline = make_label(s_root, PUCK_FONT_HERO, PUCK_COLOUR_TEXT, -110, -30);
  lv_label_set_text(s_headline, "--%");
  s_stored = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, -110, 10);
  lv_label_set_text(s_stored, "");
  s_pill = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_BATTERY, -110, 50);
  lv_label_set_text(s_pill, "");

  s_health = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, 150, -20);
  lv_label_set_text(s_health, "");
  return s_root;
}

void screen_battery_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }
  char text[24];
  if (snapshot.valid && snapshot.battery.soc_pct.known) {
    snprintf(text, sizeof(text), "%.0f%%", snapshot.battery.soc_pct.value);
    lv_label_set_text(s_headline, text);
    if (snapshot.battery.capacity_kwh.known) {
      const float stored = snapshot.battery.capacity_kwh.value *
                           snapshot.battery.soc_pct.value / 100.0f;
      snprintf(text, sizeof(text), "%.1f kWh stored", stored);
      lv_label_set_text(s_stored, text);
    }
  } else {
    lv_label_set_text(s_headline, "--%");
    lv_label_set_text(s_stored, "");
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
