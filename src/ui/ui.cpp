#include "ui.h"

#include "board_config.h"
#include "screen_battery.h"
#include "screen_cost.h"
#include "screen_power.h"
#include "screen_today.h"
#include "theme.h"

namespace {

constexpr int SCREEN_COUNT = 4;

// Just above the bottom of the bezel, below everything the screens draw. Screen
// 1's state-of-charge readout was moved inboard to make room.
constexpr lv_coord_t DOTS_Y = 212;
constexpr lv_coord_t DOT_SIZE = 7;
constexpr lv_coord_t DOT_GAP = 10;

lv_obj_t* s_tileview = nullptr;
lv_obj_t* s_tiles[SCREEN_COUNT] = {};
lv_obj_t* s_dots[SCREEN_COUNT] = {};
lv_obj_t* s_overlay = nullptr;
lv_obj_t* s_overlay_title = nullptr;
lv_obj_t* s_overlay_detail = nullptr;

void highlight_active_dot() {
  lv_obj_t* active = lv_tileview_get_tile_act(s_tileview);
  for (int i = 0; i < SCREEN_COUNT; ++i) {
    const bool here = s_tiles[i] == active;
    lv_obj_set_style_bg_color(s_dots[i],
                              lv_color_hex(here ? PUCK_COLOUR_TEXT : PUCK_COLOUR_TRACK),
                              LV_PART_MAIN);
  }
}

void on_tile_changed(lv_event_t* /*event*/) {
  highlight_active_dot();
}

}  // namespace

lv_obj_t* ui_create(lv_obj_t* parent) {
  lv_obj_set_style_bg_color(parent, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);

  s_tileview = lv_tileview_create(parent);
  lv_obj_set_size(s_tileview, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_center(s_tileview);
  lv_obj_set_style_bg_color(s_tileview, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  // No scrollbar: on a round screen it would be clipped by the bezel anyway, and
  // the page dots do the same job where they can actually be seen.
  lv_obj_set_scrollbar_mode(s_tileview, LV_SCROLLBAR_MODE_OFF);

  for (int i = 0; i < SCREEN_COUNT; ++i) {
    s_tiles[i] = lv_tileview_add_tile(s_tileview, static_cast<uint8_t>(i), 0, LV_DIR_HOR);
  }

  screen_power_create(s_tiles[0]);
  screen_battery_create(s_tiles[1]);
  screen_today_create(s_tiles[2]);
  screen_cost_create(s_tiles[3]);

  // Dots are siblings of the tileview, not children, so they stay put while the
  // screens slide underneath them.
  lv_obj_t* dots = lv_obj_create(parent);
  lv_obj_remove_style_all(dots);
  lv_obj_clear_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(dots, SCREEN_COUNT * (DOT_SIZE + DOT_GAP), DOT_SIZE);
  lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(dots, DOT_GAP, LV_PART_MAIN);
  lv_obj_align(dots, LV_ALIGN_CENTER, 0, DOTS_Y);

  for (int i = 0; i < SCREEN_COUNT; ++i) {
    lv_obj_t* dot = lv_obj_create(dots);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(PUCK_COLOUR_TRACK), LV_PART_MAIN);
    s_dots[i] = dot;
  }

  // Built last so it sits above the tileview and the dots in z-order.
  s_overlay = lv_obj_create(parent);
  lv_obj_remove_style_all(s_overlay);
  lv_obj_set_size(s_overlay, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_center(s_overlay);
  lv_obj_set_style_bg_color(s_overlay, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

  s_overlay_title = lv_label_create(s_overlay);
  lv_obj_set_style_text_font(s_overlay_title, PUCK_FONT_LARGE, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_overlay_title, lv_color_hex(PUCK_COLOUR_TEXT), LV_PART_MAIN);
  lv_obj_set_style_text_align(s_overlay_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(s_overlay_title, PUCK_SAFE_SQUARE);
  lv_label_set_long_mode(s_overlay_title, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_overlay_title, LV_ALIGN_CENTER, 0, -40);

  s_overlay_detail = lv_label_create(s_overlay);
  lv_obj_set_style_text_font(s_overlay_detail, PUCK_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_overlay_detail, lv_color_hex(PUCK_COLOUR_MUTED), LV_PART_MAIN);
  lv_obj_set_style_text_align(s_overlay_detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(s_overlay_detail, PUCK_SAFE_SQUARE);
  lv_label_set_long_mode(s_overlay_detail, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_overlay_detail, LV_ALIGN_CENTER, 0, 30);

  lv_obj_add_event_cb(s_tileview, on_tile_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  highlight_active_dot();

  return s_tileview;
}

void ui_update(const Snapshot& snapshot) {
  // Every screen is refreshed, not just the visible one. They are cheap to
  // update and it means a swipe never lands on a screen showing an older
  // reading than the one you just swiped away from.
  screen_power_update(snapshot);
  screen_battery_update(snapshot);
  screen_today_update(snapshot);
  screen_cost_update(snapshot);
}

void ui_set_device_battery(bool show, int percent, bool charging) {
  screen_power_set_device_battery(show, percent, charging);
}

void ui_set_overlay(const char* title, const char* detail) {
  if (s_overlay == nullptr) {
    return;
  }
  if (title == nullptr) {
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_label_set_text(s_overlay_title, title);
  lv_label_set_text(s_overlay_detail, detail != nullptr ? detail : "");
  lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

int ui_screen_count() {
  return SCREEN_COUNT;
}

int ui_current_screen() {
  lv_obj_t* active = lv_tileview_get_tile_act(s_tileview);
  for (int i = 0; i < SCREEN_COUNT; ++i) {
    if (s_tiles[i] == active) {
      return i;
    }
  }
  return 0;
}

void ui_show_screen(int index) {
  if (s_tileview == nullptr || index < 0 || index >= SCREEN_COUNT) {
    return;
  }
  // No animation: used to jump straight to a screen for a screenshot, where a
  // half-finished slide would be captured instead of the screen.
  lv_obj_set_tile(s_tileview, s_tiles[index], LV_ANIM_OFF);
  highlight_active_dot();
}
