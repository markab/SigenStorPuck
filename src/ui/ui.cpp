#include "ui.h"

#include "board_config.h"
#include "history.h"
#include "screen_battery.h"
#include "screen_cost.h"
#include "screen_power.h"
#include "screen_today.h"
#include "theme.h"

namespace {

// The cost screen only exists when a SigenStor Display server is behind us: it
// needs the tariff tables, the Octopus API and a priced integration over the
// day, none of which a plant exposes over Modbus (§D1). So the count is decided
// at build-the-UI time rather than compiled in.
//
// Every caller outside this file already goes through ui_screen_count(), which
// is what makes this a contained change.
constexpr int MAX_SCREENS = 4;
int s_screen_count = MAX_SCREENS;
bool s_has_cost = true;

// Just above the bottom of the bezel, below everything the screens draw. Screen
// 1's state-of-charge readout was moved inboard to make room.
constexpr lv_coord_t DOTS_Y = 212;
constexpr lv_coord_t DOT_SIZE = 7;
constexpr lv_coord_t DOT_GAP = 10;

lv_obj_t* s_tileview = nullptr;
lv_obj_t* s_tiles[MAX_SCREENS] = {};
lv_obj_t* s_dots[MAX_SCREENS] = {};
lv_obj_t* s_shift_root = nullptr;
lv_obj_t* s_sweep = nullptr;
uint32_t s_rotate_seconds = 0;
uint32_t s_sweep_minutes = 0;
lv_obj_t* s_overlay = nullptr;
lv_obj_t* s_overlay_title = nullptr;
lv_obj_t* s_overlay_detail = nullptr;

void highlight_active_dot() {
  lv_obj_t* active = lv_tileview_get_tile_act(s_tileview);
  for (int i = 0; i < s_screen_count; ++i) {
    const bool here = s_tiles[i] == active;
    lv_obj_set_style_bg_color(s_dots[i],
                              lv_color_hex(here ? PUCK_COLOUR_TEXT : PUCK_COLOUR_TRACK),
                              LV_PART_MAIN);
  }
}

void sweep_finished(lv_anim_t* /*anim*/) {
  lv_obj_add_flag(s_sweep, LV_OBJ_FLAG_HIDDEN);
}

// A band crossing the panel. Every pixel is lit once, but only a band's worth at
// any instant — flashing the whole screen would even out wear by adding a great
// deal more of it.
void run_sweep() {
  lv_obj_clear_flag(s_sweep, LV_OBJ_FLAG_HIDDEN);
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, s_sweep);
  lv_anim_set_exec_cb(&anim, [](void* target, int32_t value) {
    lv_obj_set_x(static_cast<lv_obj_t*>(target), value);
  });
  lv_anim_set_values(&anim, -PUCK_SWEEP_BAND_PX, PUCK_LCD_WIDTH);
  lv_anim_set_time(&anim, PUCK_SWEEP_DURATION_MS);
  lv_anim_set_ready_cb(&anim, sweep_finished);
  lv_anim_start(&anim);
}

// Screen rotation and the sweep, both deferred while someone is using the device:
// a screen that changes under your finger is worse than a little extra wear.
void housekeeping_tick(lv_timer_t* /*timer*/) {
  const uint32_t idle_ms = lv_disp_get_inactive_time(nullptr);
  const bool busy = idle_ms < 4000;

  static uint32_t rotate_elapsed = 0;
  if (s_rotate_seconds > 0 && !busy) {
    if (++rotate_elapsed >= s_rotate_seconds) {
      rotate_elapsed = 0;
      const int next = (ui_current_screen() + 1) % s_screen_count;
      lv_obj_set_tile(s_tileview, s_tiles[next], LV_ANIM_ON);
      highlight_active_dot();
    }
  } else if (busy) {
    rotate_elapsed = 0;
  }

  static uint32_t sweep_elapsed = 0;
  if (s_sweep_minutes > 0 && !busy) {
    if (++sweep_elapsed >= s_sweep_minutes * 60) {
      sweep_elapsed = 0;
      run_sweep();
    }
  }
}

void on_tile_changed(lv_event_t* /*event*/) {
  highlight_active_dot();
}

}  // namespace

lv_obj_t* ui_create(lv_obj_t* parent, bool with_cost_screen) {
  s_has_cost = with_cost_screen;
  s_screen_count = with_cost_screen ? MAX_SCREENS : MAX_SCREENS - 1;

  lv_obj_set_style_bg_color(parent, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);

  // A container holding everything, so pixel shift can move the whole UI as one
  // rather than nudging each element.
  s_shift_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_shift_root);
  lv_obj_set_size(s_shift_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_center(s_shift_root);
  lv_obj_clear_flag(s_shift_root, LV_OBJ_FLAG_SCROLLABLE);
  parent = s_shift_root;

  s_tileview = lv_tileview_create(parent);
  lv_obj_set_size(s_tileview, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_center(s_tileview);
  lv_obj_set_style_bg_color(s_tileview, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  // No scrollbar: on a round screen it would be clipped by the bezel anyway, and
  // the page dots do the same job where they can actually be seen.
  lv_obj_set_scrollbar_mode(s_tileview, LV_SCROLLBAR_MODE_OFF);

  for (int i = 0; i < s_screen_count; ++i) {
    s_tiles[i] = lv_tileview_add_tile(s_tileview, static_cast<uint8_t>(i), 0, LV_DIR_HOR);
  }

  screen_power_create(s_tiles[0]);
  screen_battery_create(s_tiles[1]);
  screen_today_create(s_tiles[2]);
  if (s_has_cost) {
    screen_cost_create(s_tiles[3]);
  }

  // Dots are siblings of the tileview, not children, so they stay put while the
  // screens slide underneath them.
  lv_obj_t* dots = lv_obj_create(parent);
  lv_obj_remove_style_all(dots);
  lv_obj_clear_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(dots, s_screen_count * (DOT_SIZE + DOT_GAP), DOT_SIZE);
  lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(dots, DOT_GAP, LV_PART_MAIN);
  lv_obj_align(dots, LV_ALIGN_CENTER, 0, DOTS_Y);

  for (int i = 0; i < s_screen_count; ++i) {
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

  // The sweep band, above everything including the overlay.
  s_sweep = lv_obj_create(s_shift_root);
  lv_obj_remove_style_all(s_sweep);
  lv_obj_set_size(s_sweep, PUCK_SWEEP_BAND_PX, PUCK_LCD_HEIGHT);
  lv_obj_set_style_bg_color(s_sweep, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_sweep, PUCK_SWEEP_OPACITY, LV_PART_MAIN);
  lv_obj_clear_flag(s_sweep, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_sweep, LV_OBJ_FLAG_HIDDEN);

  lv_timer_create(housekeeping_tick, 1000, nullptr);

  lv_obj_add_event_cb(s_tileview, on_tile_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  highlight_active_dot();

  return s_tileview;
}

void ui_update(const Snapshot& snapshot) {
  // Filed before anything draws, so the charts see this reading on this pass.
  // Here rather than in the poll task because this is the one call every data
  // source goes through — the same reason the screens hang off it.
  history_record(snapshot);

  // Every screen is refreshed, not just the visible one. They are cheap to
  // update and it means a swipe never lands on a screen showing an older
  // reading than the one you just swiped away from.
  screen_power_update(snapshot);
  screen_battery_update(snapshot);
  screen_today_update(snapshot);
  if (s_has_cost) {
    screen_cost_update(snapshot);
  }
}

void ui_set_fine_rotation(int16_t tenths_of_a_degree) {
  if (s_shift_root == nullptr) {
    return;
  }
  // Deliberately inert, and loudly so.
  //
  // transform_angle renders the content through an intermediate layer, and that
  // layer needs an alpha channel: LVGL refuses with "needs LV_COLOR_SCREEN_TRANSP
  // 1", which in LVGL 8 requires LV_COLOR_DEPTH 32. This project is RGB565 to match
  // the panel, so enabling it would double every buffer and add a 32->16 conversion
  // to every flush — permanently, tilted or not.
  //
  // Applying it anyway leaves the container unrendered, which is worse than not
  // offering the feature. The settings field, the NVS value and the inverse touch
  // transform are all correct and stay, so whichever mechanism is chosen can use
  // them.
  if (tenths_of_a_degree != 0) {
    LV_LOG_WARN("fine rotation needs 32-bit colour; ignoring");
    return;
  }
  // Pivot on the middle of the panel, not the container's own origin, so the whole
  // face turns about its centre.
  lv_obj_set_style_transform_pivot_x(s_shift_root, PUCK_LCD_WIDTH / 2, LV_PART_MAIN);
  lv_obj_set_style_transform_pivot_y(s_shift_root, PUCK_LCD_HEIGHT / 2, LV_PART_MAIN);
  lv_obj_set_style_transform_angle(s_shift_root, tenths_of_a_degree, LV_PART_MAIN);
  lv_obj_invalidate(s_shift_root);
}

void ui_set_rotate_interval(uint32_t seconds) {
  s_rotate_seconds = seconds;
}

void ui_set_sweep_interval(uint32_t minutes) {
  s_sweep_minutes = minutes;
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
  return s_screen_count;
}

int ui_current_screen() {
  lv_obj_t* active = lv_tileview_get_tile_act(s_tileview);
  for (int i = 0; i < s_screen_count; ++i) {
    if (s_tiles[i] == active) {
      return i;
    }
  }
  return 0;
}

void ui_show_screen(int index) {
  if (s_tileview == nullptr || index < 0 || index >= s_screen_count) {
    return;
  }
  // No animation: used to jump straight to a screen for a screenshot, where a
  // half-finished slide would be captured instead of the screen.
  lv_obj_set_tile(s_tileview, s_tiles[index], LV_ANIM_OFF);
  highlight_active_dot();
}
