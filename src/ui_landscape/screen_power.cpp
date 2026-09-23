// Screen 1, landscape (600x450): live power flow for the 2.41" board.
//
// Same topology, data binding and animated flow dots as src/ui/screen_power.cpp,
// re-laid for a wide rectangle instead of a round bezel: the plant sits in the
// middle, the three grid-connected legs (solar, grid, battery) run across the
// top, and the two consumers (EV, home) sit in the bottom corners. State of
// charge traces the bezel as a rounded-rect edge bar. Dots run along each active
// leg between the plant and the leg — inward for generation and import, outward
// for consumption, export and charging — the round screen's system, with each
// leg's path taken from its fixed rectangular position rather than a bearing.

#include "screen_power.h"

#include <math.h>
#include <stdio.h>

#include "board_config.h"
#include "edge_bar.h"
#include "format.h"
#include "theme.h"

namespace {

constexpr lv_coord_t CENTRE_X = PUCK_LCD_WIDTH / 2;
constexpr lv_coord_t CENTRE_Y = PUCK_LCD_HEIGHT / 2;

// Leg bearings become fixed positions on the rectangle. Offsets from the centre.
struct LegPos {
  lv_coord_t dx;
  lv_coord_t dy;
};
constexpr LegPos POS_SOLAR = {0, -151};     // top-centre apex — the array leads
constexpr LegPos POS_GRID = {-165, -105};   // top-left
constexpr LegPos POS_BATTERY = {165, -105};
constexpr LegPos POS_EV = {-165, 105};
constexpr LegPos POS_HOME = {165, 105};

constexpr lv_coord_t LEG_BOX_WIDTH = 150;

// The channel the flow dots travel down: from just outside the hub (FLOW_INNER)
// to a fixed margin short of each leg's text, along the line from the centre to
// the leg's position. The outer end is per-leg (leg distance minus
// LEG_TEXT_MARGIN) rather than a fixed radius, so a far corner leg and the nearer
// top leg leave the same small gap to their label instead of the corner cutting
// off early with a long empty run.
constexpr lv_coord_t FLOW_INNER = 56;
constexpr lv_coord_t LEG_TEXT_MARGIN = 50;
constexpr int FLOW_DOTS = 2;
constexpr lv_coord_t FLOW_DOT_SIZE = 9;

// Dot speed maps |kW| onto a travel period, so a heavy flow visibly runs faster
// than a trickle. Above FLOW_SPEED_FULL_KW it stops getting quicker.
constexpr uint32_t FLOW_PERIOD_SLOW_MS = 2400;
constexpr uint32_t FLOW_PERIOD_FAST_MS = 700;
constexpr float FLOW_SPEED_FULL_KW = 6.0f;
// 20 fps rather than 30. On the rotated panel each dot move is a small rotated
// flush; fewer of them per second leaves headroom for the poll and swipes.
constexpr uint32_t FLOW_TICK_MS = 50;

enum LegId { LEG_SOLAR = 0, LEG_GRID, LEG_BATTERY, LEG_EV, LEG_HOME, LEG_COUNT };

// Which way the dots run, relative to the plant in the middle. Generation and
// import travel inward; consumption, export and charging travel outward.
enum FlowDirection { FLOW_IDLE = 0, FLOW_INWARD, FLOW_OUTWARD };

struct Leg {
  lv_obj_t* box = nullptr;
  lv_obj_t* name = nullptr;
  lv_obj_t* value = nullptr;
  lv_obj_t* detail = nullptr;
  lv_obj_t* dots[FLOW_DOTS] = {};

  // Endpoints of the dot path, in s_root coordinates.
  lv_point_t inner = {};
  lv_point_t outer = {};

  FlowDirection direction = FLOW_IDLE;
  uint32_t period_ms = FLOW_PERIOD_SLOW_MS;
  float phase = 0.0f;
};

lv_obj_t* s_root = nullptr;
lv_obj_t* s_edge = nullptr;
lv_obj_t* s_plant_value = nullptr;
lv_obj_t* s_plant_status = nullptr;
lv_obj_t* s_soc_label = nullptr;
lv_obj_t* s_device_battery = nullptr;
Leg s_legs[LEG_COUNT];

uint32_t period_for(float kw) {
  float fraction = fabsf(kw) / FLOW_SPEED_FULL_KW;
  if (fraction > 1.0f) {
    fraction = 1.0f;
  }
  const float span = static_cast<float>(FLOW_PERIOD_SLOW_MS - FLOW_PERIOD_FAST_MS);
  return static_cast<uint32_t>(FLOW_PERIOD_SLOW_MS - fraction * span);
}

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t colour) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(colour), LV_PART_MAIN);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  return label;
}

lv_obj_t* make_group(lv_obj_t* parent) {
  lv_obj_t* group = lv_obj_create(parent);
  lv_obj_remove_style_all(group);
  lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
  return group;
}

void build_leg(LegId id, const char* name, uint32_t colour, LegPos pos) {
  Leg& leg = s_legs[id];
  leg.box = make_group(s_root);
  lv_obj_set_size(leg.box, LEG_BOX_WIDTH, LV_SIZE_CONTENT);
  lv_obj_add_flag(leg.box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_set_flex_flow(leg.box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(leg.box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_align(leg.box, LV_ALIGN_CENTER, pos.dx, pos.dy);

  leg.name = make_label(leg.box, PUCK_FONT_BODY, colour);
  lv_label_set_text(leg.name, name);
  leg.value = make_label(leg.box, PUCK_FONT_LARGE, PUCK_COLOUR_TEXT);
  lv_label_set_text(leg.value, "--");
  leg.detail = make_label(leg.box, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_label_set_text(leg.detail, "");

  // Dot path: from just outside the hub out toward this leg, along the unit
  // vector from the centre to the leg's position.
  const float d = sqrtf(static_cast<float>(pos.dx) * pos.dx +
                        static_cast<float>(pos.dy) * pos.dy);
  const float ux = d > 0.0f ? pos.dx / d : 0.0f;
  const float uy = d > 0.0f ? pos.dy / d : 0.0f;
  const float outer = d - static_cast<float>(LEG_TEXT_MARGIN);
  leg.inner.x = static_cast<lv_coord_t>(CENTRE_X + lroundf(ux * FLOW_INNER));
  leg.inner.y = static_cast<lv_coord_t>(CENTRE_Y + lroundf(uy * FLOW_INNER));
  leg.outer.x = static_cast<lv_coord_t>(CENTRE_X + lroundf(ux * outer));
  leg.outer.y = static_cast<lv_coord_t>(CENTRE_Y + lroundf(uy * outer));

  for (int i = 0; i < FLOW_DOTS; ++i) {
    lv_obj_t* dot = make_group(s_root);
    lv_obj_set_size(dot, FLOW_DOT_SIZE, FLOW_DOT_SIZE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(colour), LV_PART_MAIN);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    leg.dots[i] = dot;
  }
}

// Positions a leg's dots for its current phase. Split out from the timer so a
// leg is drawn correctly on its very first frame.
void place_dots(Leg& leg) {
  for (int i = 0; i < FLOW_DOTS; ++i) {
    float t = leg.phase + static_cast<float>(i) / static_cast<float>(FLOW_DOTS);
    while (t >= 1.0f) {
      t -= 1.0f;
    }
    const float travel = leg.direction == FLOW_INWARD ? 1.0f - t : t;

    const lv_coord_t x =
        leg.inner.x + static_cast<lv_coord_t>((leg.outer.x - leg.inner.x) * travel);
    const lv_coord_t y =
        leg.inner.y + static_cast<lv_coord_t>((leg.outer.y - leg.inner.y) * travel);
    lv_obj_set_pos(leg.dots[i], x - FLOW_DOT_SIZE / 2, y - FLOW_DOT_SIZE / 2);

    const lv_opa_t opacity = static_cast<lv_opa_t>(LV_OPA_COVER * sinf(t * 3.14159265f));
    lv_obj_set_style_bg_opa(leg.dots[i], opacity, LV_PART_MAIN);
  }
}

// One timer for every leg rather than an animation per dot, so the cost stays
// flat as legs come and go.
void flow_tick(lv_timer_t* /*timer*/) {
  for (int id = 0; id < LEG_COUNT; ++id) {
    Leg& leg = s_legs[id];
    if (leg.direction == FLOW_IDLE) {
      continue;
    }
    leg.phase += static_cast<float>(FLOW_TICK_MS) / static_cast<float>(leg.period_ms);
    while (leg.phase >= 1.0f) {
      leg.phase -= 1.0f;
    }
    place_dots(leg);
  }
}

void set_leg(LegId id, const MaybeFloat& power, FlowDirection direction, const char* detail,
             bool visible) {
  Leg& leg = s_legs[id];
  if (!visible) {
    lv_obj_add_flag(leg.box, LV_OBJ_FLAG_HIDDEN);
    leg.direction = FLOW_IDLE;
    for (lv_obj_t* dot : leg.dots) {
      lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    }
    return;
  }
  lv_obj_clear_flag(leg.box, LV_OBJ_FLAG_HIDDEN);

  const bool moving = power.known && fabsf(power.value) > 0.0f;
  if (power.known && !moving) {
    lv_label_set_text(leg.value, "-");
  } else {
    char text[16];
    puck_format_magnitude(power, PUCK_KW_DECIMALS, text, sizeof(text));
    lv_label_set_text(leg.value, text);
  }
  lv_label_set_text(leg.detail, moving ? detail : (power.known ? "idle" : ""));

  const bool flowing = moving && direction != FLOW_IDLE;
  leg.direction = flowing ? direction : FLOW_IDLE;
  leg.period_ms = power.known ? period_for(power.value) : FLOW_PERIOD_SLOW_MS;

  for (lv_obj_t* dot : leg.dots) {
    if (flowing) {
      lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (flowing) {
    place_dots(leg);
  }
}

void set_plant_status(const Snapshot& snapshot) {
  char text[24];
  if (!snapshot.valid) {
    lv_label_set_text(s_plant_status, "OFFLINE");
    lv_obj_set_style_text_color(s_plant_status, lv_color_hex(PUCK_COLOUR_ALARM), LV_PART_MAIN);
    return;
  }
  if (snapshot.alarms > 0) {
    snprintf(text, sizeof(text), "%d ALARM%s", snapshot.alarms, snapshot.alarms == 1 ? "" : "S");
    lv_label_set_text(s_plant_status, text);
    lv_obj_set_style_text_color(s_plant_status, lv_color_hex(PUCK_COLOUR_ALARM), LV_PART_MAIN);
    return;
  }
  if (!snapshot.ok) {
    lv_label_set_text(s_plant_status, "NO DATA");
    lv_obj_set_style_text_color(s_plant_status, lv_color_hex(PUCK_COLOUR_WARN), LV_PART_MAIN);
    return;
  }
  if (snapshot.age_s >= PUCK_STALE_AFTER_S) {
    if (snapshot.age_s >= 120) {
      snprintf(text, sizeof(text), "STALE %um", snapshot.age_s / 60);
    } else {
      snprintf(text, sizeof(text), "STALE %us", snapshot.age_s);
    }
    lv_label_set_text(s_plant_status, text);
    lv_obj_set_style_text_color(s_plant_status, lv_color_hex(PUCK_COLOUR_WARN), LV_PART_MAIN);
    return;
  }
  lv_label_set_text(s_plant_status, "kW");
  lv_obj_set_style_text_color(s_plant_status, lv_color_hex(PUCK_COLOUR_MUTED), LV_PART_MAIN);
}

}  // namespace

lv_obj_t* screen_power_create(lv_obj_t* parent) {
  s_root = make_group(parent);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_center(s_root);

  // State of charge traces the bezel, behind everything else.
  s_edge = edge_bar_create(s_root);

  // The virtual plant node: inverter and gateway as one, a hairline disc.
  lv_obj_t* hub = make_group(s_root);
  lv_obj_set_size(hub, 104, 104);
  lv_obj_center(hub);
  lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_border_width(hub, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(hub, lv_color_hex(PUCK_COLOUR_TRACK), LV_PART_MAIN);

  // PLANT above and kW below, held clear of the taller HERO value in the middle.
  lv_obj_t* plant_name = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_label_set_text(plant_name, "PLANT");
  lv_obj_align(plant_name, LV_ALIGN_CENTER, 0, -42);
  s_plant_value = make_label(s_root, PUCK_FONT_HERO, PUCK_COLOUR_TEXT);
  lv_label_set_text(s_plant_value, "--");
  lv_obj_align(s_plant_value, LV_ALIGN_CENTER, 0, 0);
  s_plant_status = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_label_set_text(s_plant_status, "kW");
  lv_obj_align(s_plant_status, LV_ALIGN_CENTER, 0, 42);

  build_leg(LEG_SOLAR, "SOLAR", PUCK_COLOUR_SOLAR, POS_SOLAR);
  build_leg(LEG_GRID, "GRID", PUCK_COLOUR_GRID, POS_GRID);
  build_leg(LEG_BATTERY, "BATTERY", PUCK_COLOUR_BATTERY, POS_BATTERY);
  build_leg(LEG_EV, "EV", PUCK_COLOUR_EV, POS_EV);
  build_leg(LEG_HOME, "HOME", PUCK_COLOUR_HOME, POS_HOME);

  // State of charge, bottom-centre (screen 1 carries no day chip there).
  s_soc_label = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_BATTERY);
  lv_label_set_text(s_soc_label, "SOC --");
  lv_obj_align(s_soc_label, LV_ALIGN_CENTER, 0, 168);

  s_device_battery = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_label_set_text(s_device_battery, "");
  lv_obj_align(s_device_battery, LV_ALIGN_CENTER, 0, 196);
  lv_obj_add_flag(s_device_battery, LV_OBJ_FLAG_HIDDEN);

  lv_timer_create(flow_tick, FLOW_TICK_MS, nullptr);

  return s_root;
}

void screen_power_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }
  set_plant_status(snapshot);

  char plant_text[16];
  puck_format_magnitude(snapshot.valid ? snapshot.power.plant : MaybeFloat{},
                        PUCK_KW_DECIMALS, plant_text, sizeof(plant_text));
  lv_label_set_text(s_plant_value, plant_text);

  if (!snapshot.valid) {
    const MaybeFloat unknown;
    edge_bar_set(s_edge, 0.0f, PUCK_COLOUR_BATTERY);
    lv_label_set_text(s_soc_label, "SOC --");
    set_leg(LEG_SOLAR, unknown, FLOW_IDLE, "", true);
    set_leg(LEG_GRID, unknown, FLOW_IDLE, "", true);
    set_leg(LEG_BATTERY, unknown, FLOW_IDLE, "", true);
    set_leg(LEG_EV, unknown, FLOW_IDLE, "", true);
    set_leg(LEG_HOME, unknown, FLOW_IDLE, "", true);
    return;
  }

  if (snapshot.battery.soc_pct.known) {
    char text[16];
    snprintf(text, sizeof(text), "SOC %.0f%%", snapshot.battery.soc_pct.value);
    lv_label_set_text(s_soc_label, text);
    edge_bar_set(s_edge, snapshot.battery.soc_pct.value / 100.0f, PUCK_COLOUR_BATTERY);
  } else {
    lv_label_set_text(s_soc_label, "SOC --");
    edge_bar_set(s_edge, 0.0f, PUCK_COLOUR_BATTERY);
  }

  // Generation flows into the plant.
  set_leg(LEG_SOLAR, snapshot.power.pv, FLOW_INWARD, "kW generating", true);
  // Charging takes power out of the plant; discharging returns it.
  const bool charging = snapshot.power.batt.known && snapshot.power.batt.value > 0.0f;
  set_leg(LEG_BATTERY, snapshot.power.batt, charging ? FLOW_OUTWARD : FLOW_INWARD,
          charging ? "kW charge" : "kW discharge", true);
  // Consumers always draw outward.
  set_leg(LEG_HOME, snapshot.power.home, FLOW_OUTWARD, "kW on load", true);
  set_leg(LEG_EV, snapshot.power.ev, FLOW_OUTWARD, "kW charge", true);

  if (snapshot.power.off_grid) {
    Leg& grid = s_legs[LEG_GRID];
    lv_obj_clear_flag(grid.box, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(grid.value, "--");
    lv_label_set_text(grid.detail, "off grid");
    lv_obj_set_style_text_color(grid.value, lv_color_hex(PUCK_COLOUR_MUTED), LV_PART_MAIN);
    grid.direction = FLOW_IDLE;
    for (lv_obj_t* dot : grid.dots) {
      lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    }
  } else {
    lv_obj_set_style_text_color(s_legs[LEG_GRID].value, lv_color_hex(PUCK_COLOUR_TEXT),
                                LV_PART_MAIN);
    const bool importing = snapshot.power.grid.known && snapshot.power.grid.value > 0.0f;
    set_leg(LEG_GRID, snapshot.power.grid, importing ? FLOW_INWARD : FLOW_OUTWARD,
            importing ? "kW import" : "kW export", true);
  }
}

void screen_power_set_device_battery(bool show, int percent, bool charging) {
  if (s_device_battery == nullptr) {
    return;
  }
  if (!show) {
    lv_obj_add_flag(s_device_battery, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  char text[24];
  if (percent < 0) {
    snprintf(text, sizeof(text), "batt --");
  } else {
    snprintf(text, sizeof(text), "batt %d%%%s", percent, charging ? " +" : "");
  }
  lv_label_set_text(s_device_battery, text);
  lv_obj_set_style_text_color(
      s_device_battery,
      lv_color_hex(percent >= 0 && percent < 20 ? PUCK_COLOUR_WARN : PUCK_COLOUR_MUTED),
      LV_PART_MAIN);
  lv_obj_clear_flag(s_device_battery, LV_OBJ_FLAG_HIDDEN);
}
