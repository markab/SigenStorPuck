// Screen (swiped 6th), landscape: cost and tariff for the 2.41" board. Server
// source only.
//
// Phase 3a scaffold: net saving headline and the current rate. The next-slot
// timeline is follow-up polish.

#include "screen_cost.h"

#include <stdio.h>

#include "board_config.h"
#include "theme.h"

namespace {

lv_obj_t* s_root = nullptr;
lv_obj_t* s_headline = nullptr;
lv_obj_t* s_rate = nullptr;

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

lv_obj_t* screen_cost_create(lv_obj_t* parent) {
  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_center(s_root);

  lv_obj_t* caption = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, -110, -70);
  lv_label_set_text(caption, "SAVED TODAY");
  s_headline = make_label(s_root, PUCK_FONT_HERO, PUCK_COLOUR_TEXT, -110, -20);
  lv_label_set_text(s_headline, "--");

  s_rate = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_TEXT, 150, -10);
  lv_label_set_text(s_rate, "");
  return s_root;
}

void screen_cost_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }
  char text[24];
  if (snapshot.valid && snapshot.cost.configured && snapshot.cost.saving_gbp.known) {
    snprintf(text, sizeof(text), "\xC2\xA3%.2f", snapshot.cost.saving_gbp.value);
    lv_label_set_text(s_headline, text);
  } else {
    lv_label_set_text(s_headline, "--");
  }
  if (snapshot.valid && snapshot.cost.configured && snapshot.cost.rate_p.known) {
    snprintf(text, sizeof(text), "%.1fp/kWh", snapshot.cost.rate_p.value);
    lv_label_set_text(s_rate, text);
  } else {
    lv_label_set_text(s_rate, "");
  }
}
