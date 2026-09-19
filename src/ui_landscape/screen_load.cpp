// Screen (swiped 4th), landscape: the day's consumption for the 2.41" board.
//
// Phase 3a scaffold: headline consumption and a live draw pill. No ring (as on
// the round board — consumption has no ceiling). The ghosted day curve and the
// four-figure breakdown are follow-up polish.

#include "screen_load.h"

#include <math.h>
#include <stdio.h>

#include "board_config.h"
#include "format.h"
#include "theme.h"

namespace {

lv_obj_t* s_root = nullptr;
lv_obj_t* s_headline = nullptr;
lv_obj_t* s_pill = nullptr;
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

  lv_obj_t* caption = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 0, -70);
  lv_label_set_text(caption, "CONSUMED");
  s_headline = make_label(s_root, PUCK_FONT_HERO, PUCK_COLOUR_TEXT, 0, -20);
  lv_label_set_text(s_headline, "--");
  lv_obj_t* unit = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 0, 20);
  lv_label_set_text(unit, "kWh used");
  s_pill = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_HOME, 0, 60);
  lv_label_set_text(s_pill, "");
  return s_root;
}

void screen_load_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
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
}

void screen_load_set_live(bool live) {
  s_live = live;
}
