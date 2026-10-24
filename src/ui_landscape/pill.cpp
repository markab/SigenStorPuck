#include "pill.h"

#include "theme.h"

lv_obj_t* pill_create(lv_obj_t* parent, uint32_t colour, lv_coord_t dx, lv_coord_t dy) {
  lv_obj_t* pill = lv_obj_create(parent);
  lv_obj_remove_style_all(pill);
  lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(pill, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(pill, 6, LV_PART_MAIN);
  lv_obj_set_style_bg_color(pill, lv_color_hex(colour), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(pill, LV_OPA_20, LV_PART_MAIN);
  lv_obj_align(pill, LV_ALIGN_CENTER, dx, dy);

  lv_obj_t* label = lv_label_create(pill);
  lv_obj_set_style_text_font(label, PUCK_FONT_BODY, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(colour), LV_PART_MAIN);
  lv_label_set_text(label, "");
  lv_obj_center(label);

  lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);
  return pill;
}

void pill_set_text(lv_obj_t* pill, const char* text) {
  if (pill == nullptr) {
    return;
  }
  lv_obj_t* label = lv_obj_get_child(pill, 0);
  if (label != nullptr) {
    lv_label_set_text(label, text);
  }
}
