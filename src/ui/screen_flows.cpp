#include "screen_flows.h"

#include <stdio.h>

#include "board_config.h"
#include "format.h"
#include "theme.h"

namespace {

// ------------------------------------------------------------------ layout ---
//
//   ring   self-sufficiency: the share of the day's load that never came off
//          the grid. The natural headline for this screen and the same motif
//          screens 1-3 use.
//   bar 1  where the solar went:      house | battery | grid
//   bar 2  where the load came from:  solar | battery | grid
//
// No legend. The three colours are the same ones the legs on screen 1 carry, so
// by the time anyone swipes this far they have been learned — and the key line
// under each bar names them anyway, in their own colour.

constexpr lv_coord_t BAR_WIDTH = 300;
constexpr lv_coord_t BAR_HEIGHT = 18;
constexpr int BAR_SEGMENTS = 3;

// A segment thinner than this is a rounding error wearing a colour. Below it the
// segment is dropped rather than drawn as a sliver that cannot be identified.
constexpr lv_coord_t MIN_SEGMENT_PX = 2;

constexpr lv_coord_t GROUP_ONE_Y = -34;   // header; bar and key follow it
constexpr lv_coord_t GROUP_TWO_Y = 72;
constexpr lv_coord_t BAR_DY = 26;         // header -> bar
constexpr lv_coord_t KEY_DY = 52;         // header -> key line

struct Bar {
  lv_obj_t* header = nullptr;
  lv_obj_t* title = nullptr;
  lv_obj_t* total = nullptr;
  lv_obj_t* track = nullptr;
  lv_obj_t* segment[BAR_SEGMENTS] = {};
  lv_obj_t* key = nullptr;
};

lv_obj_t* s_root = nullptr;
lv_obj_t* s_arc = nullptr;
lv_obj_t* s_hero = nullptr;
lv_obj_t* s_caption = nullptr;
Bar s_from_solar;   // 22.6 kWh of solar, split by where it went
Bar s_to_load;      // 14.8 kWh of load, split by where it came from

lv_obj_t* make_group(lv_obj_t* parent) {
  lv_obj_t* group = lv_obj_create(parent);
  lv_obj_remove_style_all(group);
  lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
  return group;
}

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t colour) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(colour), LV_PART_MAIN);
  return label;
}

void build_bar(Bar* bar, const char* title, const uint32_t colour[BAR_SEGMENTS],
               lv_coord_t y) {
  // Header: the name on the left, the total on the right, both pinned to the
  // bar's own ends so the block reads as one object.
  bar->header = make_group(s_root);
  lv_obj_set_size(bar->header, BAR_WIDTH, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(bar->header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar->header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_align(bar->header, LV_ALIGN_CENTER, 0, y);

  bar->title = make_label(bar->header, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_obj_set_style_text_letter_space(bar->title, 2, LV_PART_MAIN);
  lv_label_set_text(bar->title, title);

  bar->total = make_label(bar->header, PUCK_FONT_BODY, PUCK_COLOUR_TEXT);
  lv_label_set_text(bar->total, "--");

  bar->track = make_group(s_root);
  lv_obj_set_size(bar->track, BAR_WIDTH, BAR_HEIGHT);
  lv_obj_set_style_radius(bar->track, BAR_HEIGHT / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar->track, lv_color_hex(PUCK_COLOUR_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar->track, LV_OPA_COVER, LV_PART_MAIN);
  // The segments are square-ended and the track rounds them off, so the bar has
  // one silhouette rather than three.
  lv_obj_set_style_clip_corner(bar->track, true, LV_PART_MAIN);
  lv_obj_align(bar->track, LV_ALIGN_CENTER, 0, y + BAR_DY);

  for (int i = 0; i < BAR_SEGMENTS; ++i) {
    lv_obj_t* segment = make_group(bar->track);
    lv_obj_set_size(segment, 0, BAR_HEIGHT);
    lv_obj_set_style_bg_color(segment, lv_color_hex(colour[i]), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(segment, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(segment, LV_OBJ_FLAG_HIDDEN);
    bar->segment[i] = segment;
  }

  // One recoloured line, so each figure is named in the colour of the block it
  // belongs to without a separate legend to look up.
  bar->key = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_label_set_recolor(bar->key, true);
  lv_obj_set_style_text_align(bar->key, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(bar->key, "");
  lv_obj_align(bar->key, LV_ALIGN_CENTER, 0, y + KEY_DY);
}

// Lays out one bar from three values. Widths are apportioned by share of their
// own sum rather than of a fixed scale, so both bars run the full width and the
// screen compares *proportions* — the thing a Sankey is read for. The absolute
// totals are on the header line for anyone who wants them.
void set_bar(Bar* bar, const MaybeFloat value[BAR_SEGMENTS], const char* name[BAR_SEGMENTS],
             const uint32_t colour[BAR_SEGMENTS]) {
  float total = 0.0f;
  bool any = false;
  for (int i = 0; i < BAR_SEGMENTS; ++i) {
    if (value[i].known && value[i].value > 0.0f) {
      total += value[i].value;
      any = true;
    }
  }

  if (!any || total <= 0.0f) {
    lv_label_set_text(bar->total, "--");
    lv_label_set_text(bar->key, "");
    for (lv_obj_t* segment : bar->segment) {
      lv_obj_add_flag(segment, LV_OBJ_FLAG_HIDDEN);
    }
    return;
  }

  char text[16];
  snprintf(text, sizeof(text), "%.1f kWh", total);
  lv_label_set_text(bar->total, text);

  lv_coord_t x = 0;
  char key[128] = {};
  size_t used = 0;
  for (int i = 0; i < BAR_SEGMENTS; ++i) {
    const float share = value[i].known && value[i].value > 0.0f ? value[i].value : 0.0f;
    lv_coord_t width = static_cast<lv_coord_t>(share / total * BAR_WIDTH);
    // The last drawn segment takes whatever the rounding left, so the bar always
    // ends flush with its track.
    if (i == BAR_SEGMENTS - 1 && width > 0) {
      width = BAR_WIDTH - x;
    }
    if (width < MIN_SEGMENT_PX) {
      lv_obj_add_flag(bar->segment[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_set_size(bar->segment[i], width, BAR_HEIGHT);
    lv_obj_set_pos(bar->segment[i], x, 0);
    lv_obj_clear_flag(bar->segment[i], LV_OBJ_FLAG_HIDDEN);
    x += width;

    used += snprintf(key + used, sizeof(key) - used, "%s#%06X %s %.1f#",
                     used ? "  " : "", static_cast<unsigned>(colour[i]), name[i], share);
    if (used >= sizeof(key)) {
      break;
    }
  }
  lv_label_set_text(bar->key, key);
}

}  // namespace

lv_obj_t* screen_flows_create(lv_obj_t* parent) {
  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

  s_arc = lv_arc_create(s_root);
  lv_obj_set_size(s_arc, PUCK_RING_DIAMETER, PUCK_RING_DIAMETER);
  lv_obj_center(s_arc);
  lv_arc_set_rotation(s_arc, 270);
  lv_arc_set_bg_angles(s_arc, 0, 360);
  lv_arc_set_range(s_arc, 0, 100);
  lv_arc_set_value(s_arc, 0);
  lv_obj_remove_style(s_arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(s_arc, PUCK_RING_WIDTH, LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_arc, PUCK_RING_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_arc, lv_color_hex(PUCK_COLOUR_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_color(s_arc, lv_color_hex(PUCK_COLOUR_HOME), LV_PART_INDICATOR);
  lv_obj_add_flag(s_arc, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* title = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_obj_set_style_text_letter_space(title, 3, LV_PART_MAIN);
  lv_label_set_text(title, "FLOWS");
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -166);

  s_hero = make_label(s_root, PUCK_FONT_HERO, PUCK_COLOUR_TEXT);
  lv_label_set_text(s_hero, "--");
  lv_obj_align(s_hero, LV_ALIGN_CENTER, 0, -118);

  s_caption = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_label_set_text(s_caption, "self-sufficient today");
  lv_obj_align(s_caption, LV_ALIGN_CENTER, 0, -80);

  static const uint32_t FROM_SOLAR[BAR_SEGMENTS] = {
      PUCK_COLOUR_HOME, PUCK_COLOUR_BATTERY, PUCK_COLOUR_GRID};
  static const uint32_t TO_LOAD[BAR_SEGMENTS] = {
      PUCK_COLOUR_SOLAR, PUCK_COLOUR_BATTERY, PUCK_COLOUR_GRID};
  build_bar(&s_from_solar, "SOLAR", FROM_SOLAR, GROUP_ONE_Y);
  build_bar(&s_to_load, "USED", TO_LOAD, GROUP_TWO_Y);

  return s_root;
}

void screen_flows_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }

  const bool have = snapshot.valid && snapshot.today.present;
  const Snapshot::Today::Flows& flows = snapshot.today.flows;
  const MaybeFloat unknown;

  const MaybeFloat from_solar[BAR_SEGMENTS] = {
      have ? flows.solar_load : unknown,
      have ? flows.solar_batt : unknown,
      have ? flows.solar_grid : unknown,
  };
  static const char* FROM_SOLAR_NAMES[BAR_SEGMENTS] = {"home", "batt", "grid"};
  static const uint32_t FROM_SOLAR_COLOURS[BAR_SEGMENTS] = {
      PUCK_COLOUR_HOME, PUCK_COLOUR_BATTERY, PUCK_COLOUR_GRID};
  set_bar(&s_from_solar, from_solar, FROM_SOLAR_NAMES, FROM_SOLAR_COLOURS);

  const MaybeFloat to_load[BAR_SEGMENTS] = {
      have ? flows.solar_load : unknown,
      have ? flows.batt_load : unknown,
      have ? flows.grid_load : unknown,
  };
  static const char* TO_LOAD_NAMES[BAR_SEGMENTS] = {"solar", "batt", "grid"};
  static const uint32_t TO_LOAD_COLOURS[BAR_SEGMENTS] = {
      PUCK_COLOUR_SOLAR, PUCK_COLOUR_BATTERY, PUCK_COLOUR_GRID};
  set_bar(&s_to_load, to_load, TO_LOAD_NAMES, TO_LOAD_COLOURS);

  // Self-sufficiency: the share of the day's load that did not come off the
  // grid. Derived here rather than sent, on the same footing as the battery
  // screen's stored-kWh figure — one division between two numbers already in the
  // payload, not a piece of the server's business logic.
  const bool can_derive = have && snapshot.today.load.known &&
                          snapshot.today.load.value > 0.0f && flows.grid_load.known;
  if (!can_derive) {
    lv_label_set_text(s_hero, "--");
    lv_obj_add_flag(s_arc, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  float pct = (snapshot.today.load.value - flows.grid_load.value) /
              snapshot.today.load.value * 100.0f;
  if (pct < 0.0f) {
    pct = 0.0f;
  }
  if (pct > 100.0f) {
    pct = 100.0f;
  }
  char text[16];
  snprintf(text, sizeof(text), "%.0f%%", pct);
  lv_label_set_text(s_hero, text);
  lv_arc_set_value(s_arc, static_cast<int16_t>(pct));
  lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_HIDDEN);
}
