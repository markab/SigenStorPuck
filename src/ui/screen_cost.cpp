#include "screen_cost.h"

#include <stdio.h>

#include "board_config.h"
#include "format.h"
#include "theme.h"

namespace {

constexpr lv_coord_t ROW_WIDTH = 250;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_saving = nullptr;
lv_obj_t* s_saving_caption = nullptr;
lv_obj_t* s_rate_now = nullptr;
lv_obj_t* s_slots_box = nullptr;
lv_obj_t* s_slot_when[SNAPSHOT_MAX_TARIFF_SLOTS] = {};
lv_obj_t* s_slot_price[SNAPSHOT_MAX_TARIFF_SLOTS] = {};
lv_obj_t* s_slot_rows[SNAPSHOT_MAX_TARIFF_SLOTS] = {};
lv_obj_t* s_unconfigured = nullptr;

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

// Cheap or dear relative to what you are paying right now, not to a threshold
// somebody has to configure. "Cheaper than now" is the question you actually ask
// when deciding whether to wait before running something.
uint32_t colour_for_price(float pence, const MaybeFloat& rate_now) {
  if (!rate_now.known) {
    return PUCK_COLOUR_TEXT;
  }
  // A hair either side of the current rate is the same rate, not a change.
  const float margin = 0.2f;
  if (pence < rate_now.value - margin) {
    return PUCK_COLOUR_BATTERY;  // green: cheaper than now
  }
  if (pence > rate_now.value + margin) {
    return PUCK_COLOUR_WARN;  // amber: dearer than now
  }
  return PUCK_COLOUR_MUTED;
}

}  // namespace

lv_obj_t* screen_cost_create(lv_obj_t* parent) {
  s_root = make_group(parent);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t* title = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_label_set_text(title, "COST");
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -138);

  // The pound sign here is the whole reason for the custom font subset: LVGL's
  // built-in Montserrat stops at ASCII.
  s_saving = make_label(s_root, PUCK_FONT_HERO, PUCK_COLOUR_BATTERY);
  lv_label_set_text(s_saving, "--");
  lv_obj_align(s_saving, LV_ALIGN_CENTER, 0, -90);

  s_saving_caption = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_label_set_text(s_saving_caption, "saved today");
  lv_obj_align(s_saving_caption, LV_ALIGN_CENTER, 0, -52);

  s_rate_now = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT);
  lv_label_set_text(s_rate_now, "--");
  lv_obj_align(s_rate_now, LV_ALIGN_CENTER, 0, -12);

  s_slots_box = make_group(s_root);
  lv_obj_set_size(s_slots_box, ROW_WIDTH, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_slots_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_slots_box, 6, LV_PART_MAIN);
  lv_obj_align(s_slots_box, LV_ALIGN_CENTER, 0, 90);

  for (size_t i = 0; i < SNAPSHOT_MAX_TARIFF_SLOTS; ++i) {
    lv_obj_t* row = make_group(s_slots_box);
    lv_obj_set_size(row, ROW_WIDTH, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_slot_rows[i] = row;

    s_slot_when[i] = make_label(row, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
    lv_label_set_text(s_slot_when[i], "");

    s_slot_price[i] = make_label(row, PUCK_FONT_BODY, PUCK_COLOUR_TEXT);
    lv_label_set_text(s_slot_price[i], "");
  }

  s_unconfigured = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_MUTED);
  lv_label_set_text(s_unconfigured, "no tariff set");
  lv_obj_set_style_text_align(s_unconfigured, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(s_unconfigured, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(s_unconfigured, LV_OBJ_FLAG_HIDDEN);

  return s_root;
}

void screen_cost_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }

  const bool configured = snapshot.valid && snapshot.cost.configured;

  // The server says configured:false rather than omitting the block, so there is
  // a real difference between "no tariff set up" and "we could not reach it".
  if (!configured) {
    // No dashed hero above the message, for the same reason as screen 3: it
    // reads as a missing saving rather than an absent tariff.
    lv_obj_add_flag(s_saving, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_saving_caption, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_rate_now, "");
    lv_obj_add_flag(s_slots_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_unconfigured, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_unconfigured, snapshot.valid ? "no tariff set" : "offline");
    return;
  }
  lv_obj_clear_flag(s_saving, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_saving_caption, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_slots_box, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_unconfigured, LV_OBJ_FLAG_HIDDEN);

  const Snapshot::Cost& cost = snapshot.cost;
  char text[32];
  char scratch[16];

  if (cost.saving_gbp.known) {
    snprintf(text, sizeof(text), "£%.2f", cost.saving_gbp.value);
    lv_label_set_text(s_saving, text);
    // A negative saving is a real outcome on a bad day, and colouring it green
    // would be a lie.
    lv_obj_set_style_text_color(
        s_saving,
        lv_color_hex(cost.saving_gbp.value < 0.0f ? PUCK_COLOUR_WARN : PUCK_COLOUR_BATTERY),
        LV_PART_MAIN);
  } else {
    lv_label_set_text(s_saving, "--");
  }

  if (cost.rate_p.known) {
    puck_format_magnitude(cost.rate_p, 1, scratch, sizeof(scratch));
    snprintf(text, sizeof(text), "%sp per kWh now", scratch);
    lv_label_set_text(s_rate_now, text);
  } else {
    lv_label_set_text(s_rate_now, "rate unknown");
  }

  for (size_t i = 0; i < SNAPSHOT_MAX_TARIFF_SLOTS; ++i) {
    if (i >= cost.next_count) {
      lv_obj_add_flag(s_slot_rows[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(s_slot_rows[i], LV_OBJ_FLAG_HIDDEN);

    // Offsets are measured from the payload's own timestamp, not the device
    // clock, so they stay right even before NTP has ever run.
    const int32_t ahead = static_cast<int32_t>(cost.next[i].from) - static_cast<int32_t>(snapshot.ts);
    puck_format_offset(ahead, scratch, sizeof(scratch));
    lv_label_set_text(s_slot_when[i], scratch);

    snprintf(text, sizeof(text), "%.1fp", cost.next[i].pence);
    lv_label_set_text(s_slot_price[i], text);
    lv_obj_set_style_text_color(s_slot_price[i],
                                lv_color_hex(colour_for_price(cost.next[i].pence, cost.rate_p)),
                                LV_PART_MAIN);
  }
}
