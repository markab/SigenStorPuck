// Events screen, landscape (600x450): the 2.41" board's layout of the one event
// worth showing. Same content and wording as the round screen (src/ui/), both
// taken from the helpers in events.h; only the geometry differs.

#include "screen_events.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "theme.h"

namespace {

// ------------------------------------------------------------------ layout ---
//
// The kind in its own colour across the top, when and where it stands beneath,
// a hairline, then the figures: one large in the middle, or two in columns the
// way the grid screen sets its stats. No title — like the other landscape
// screens, the first line says what this is. The page dots and the (hidden)
// day indicator keep the bottom 80 px.

constexpr lv_coord_t KIND_Y = -150;
constexpr lv_coord_t WHEN_Y = -100;
constexpr lv_coord_t STATUS_Y = -62;
constexpr lv_coord_t RULE_Y = -24;
constexpr lv_coord_t RULE_WIDTH = 320;
constexpr lv_coord_t FIGURE_CAPTION_Y = 14;
constexpr lv_coord_t FIGURE_VALUE_Y = 60;
constexpr lv_coord_t FIGURE_COLUMN_X = 150;

constexpr size_t MAX_FIGURES = 2;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_kind = nullptr;
lv_obj_t* s_when = nullptr;
lv_obj_t* s_status = nullptr;
lv_obj_t* s_rule = nullptr;
lv_obj_t* s_caption[MAX_FIGURES] = {};
lv_obj_t* s_value[MAX_FIGURES] = {};
lv_obj_t* s_empty = nullptr;

// Saving sessions are the warning orange: the event that asks something of the
// house. Free import of either kind is battery green — it is the battery that
// soaks it up. Axle is grid blue, being an event at the grid.
uint32_t colour_for(EventKind kind) {
  switch (kind) {
    case EventKind::Saving:
      return PUCK_COLOUR_WARN;
    case EventKind::HappyHour:
    case EventKind::FreeImport:
      return PUCK_COLOUR_BATTERY;
    case EventKind::Axle:
      return PUCK_COLOUR_GRID;
    case EventKind::Unknown:
      break;
  }
  return PUCK_COLOUR_MUTED;
}

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t colour, lv_coord_t x,
                     lv_coord_t y) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(colour), LV_PART_MAIN);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(label, LV_ALIGN_CENTER, x, y);
  lv_label_set_text(label, "");
  return label;
}

// The screen is refreshed every few seconds to keep its countdown honest, and
// lv_label_set_text() invalidates even when nothing changed.
void set_text(lv_obj_t* label, const char* text) {
  if (strcmp(lv_label_get_text(label), text) != 0) {
    lv_label_set_text(label, text);
  }
}

void set_colour(lv_obj_t* label, uint32_t colour) {
  const lv_color_t wanted = lv_color_hex(colour);
  if (lv_obj_get_style_text_color(label, LV_PART_MAIN).full != wanted.full) {
    lv_obj_set_style_text_color(label, wanted, LV_PART_MAIN);
  }
}

void set_visible(lv_obj_t* object, bool visible) {
  if (visible == lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) {
    if (visible) {
      lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

}  // namespace

lv_obj_t* screen_events_create(lv_obj_t* parent) {
  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_center(s_root);

  s_kind = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_MUTED, 0, KIND_Y);
  lv_obj_set_style_text_letter_space(s_kind, 2, LV_PART_MAIN);
  s_when = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, 0, WHEN_Y);
  s_status = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_MUTED, 0, STATUS_Y);

  s_rule = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_rule);
  lv_obj_set_size(s_rule, RULE_WIDTH, 1);
  lv_obj_set_style_bg_color(s_rule, lv_color_hex(PUCK_COLOUR_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_rule, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(s_rule, LV_ALIGN_CENTER, 0, RULE_Y);

  for (size_t i = 0; i < MAX_FIGURES; ++i) {
    s_caption[i] = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 0, FIGURE_CAPTION_Y);
    lv_obj_set_style_text_letter_space(s_caption[i], 2, LV_PART_MAIN);
    s_value[i] = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_TEXT, 0, FIGURE_VALUE_Y);
  }

  s_empty = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_MUTED, 0, 0);
  lv_label_set_text(s_empty, "No events");

  // Start in the empty state until a list arrives.
  screen_events_update(EventList(), 0, 0);
  return s_root;
}

void screen_events_update(const EventList& events, uint32_t now, int32_t tz_offset_min) {
  if (s_root == nullptr) {
    return;
  }
  const GridEvent* event = events_featured(events);
  const bool have = event != nullptr;
  set_visible(s_empty, !have);
  set_visible(s_kind, have);
  set_visible(s_when, have);
  set_visible(s_status, have);
  if (!have) {
    set_visible(s_rule, false);
    for (size_t i = 0; i < MAX_FIGURES; ++i) {
      set_visible(s_caption[i], false);
      set_visible(s_value[i], false);
    }
    return;
  }

  const uint32_t colour = colour_for(event->kind);
  set_text(s_kind, events_title(*event));
  set_colour(s_kind, colour);

  char text[48];
  events_format_when(*event, now, tz_offset_min, text, sizeof(text));
  set_text(s_when, text);

  // In the event's colour only while it is happening: that is the state worth
  // noticing from across the room. Booked, done and deferred read muted.
  events_format_status(*event, now, text, sizeof(text));
  set_text(s_status, text);
  set_colour(s_status,
             events_state_at(*event, now) == EventState::Live ? colour : PUCK_COLOUR_MUTED);

  EventFigure figures[MAX_FIGURES];
  const size_t count = events_figures(*event, figures, MAX_FIGURES);
  // The hairline divides the event from its figures; a booked event may have
  // none, and a rule over nothing reads as something missing.
  set_visible(s_rule, count > 0);
  for (size_t i = 0; i < MAX_FIGURES; ++i) {
    const bool shown = i < count;
    set_visible(s_caption[i], shown);
    set_visible(s_value[i], shown);
    if (!shown) {
      continue;
    }
    // One figure sits in the middle; two share the width in columns.
    const lv_coord_t x = count == 1 ? 0 : (i == 0 ? -FIGURE_COLUMN_X : FIGURE_COLUMN_X);
    // A lone figure has the whole width, so it gets the hero size the cost
    // screen's headline uses; two share it at the large size.
    const lv_font_t* font = count == 1 ? PUCK_FONT_HERO : PUCK_FONT_LARGE;
    if (lv_obj_get_style_text_font(s_value[i], LV_PART_MAIN) != font) {
      lv_obj_set_style_text_font(s_value[i], font, LV_PART_MAIN);
    }
    lv_obj_align(s_caption[i], LV_ALIGN_CENTER, x, FIGURE_CAPTION_Y);
    lv_obj_align(s_value[i], LV_ALIGN_CENTER, x, FIGURE_VALUE_Y);
    set_text(s_caption[i], figures[i].caption);
    set_text(s_value[i], figures[i].value);
  }
}
