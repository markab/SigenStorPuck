#include "screen_power.h"

#include <math.h>
#include <stdio.h>

#include "board_config.h"
#include "format.h"
#include "theme.h"

namespace {

// ------------------------------------------------------------------ layout ---
//
// A five-point star. Solar at the top, then clockwise: battery, home, EV, grid.
// The centre is a single virtual "plant" node standing in for the inverter and
// the gateway together.
//
// This follows the real SigenStor topology as the dashboard draws it
// (SigenStorDisplay app/web/static/overview.js):
//
//   Solar -> Inverter,  Battery <-> Inverter
//   Inverter -> Gateway,  Grid <-> Gateway
//   Gateway -> Home,  Gateway -> AC charger
//
// Home and the EV charger are siblings hanging off the gateway, not one off the
// other — so neither belongs in the middle. Collapsing the inverter, gateway and
// grid contactor into one node drops plumbing that a 466 px glanceable screen
// cannot usefully show, while keeping every leg attached to the right place.
// "Plant" is the server's own term for the combined unit.
//
// Angles are degrees clockwise from north, so the arithmetic below matches the
// way the layout is described rather than maths convention.

constexpr lv_coord_t CENTRE = PUCK_LCD_WIDTH / 2;

// 160 is the largest radius at which a leg's text block still clears the ring.
// The binding case is battery and grid at 72 deg and 288 deg, whose blocks sit
// furthest out horizontally while still being well above the vertical centre.
constexpr lv_coord_t LEG_RADIUS = 160;
constexpr lv_coord_t LEG_BOX_WIDTH = 100;

// The plant node, holding a name, its total power and a status line. Sized to
// the widest text it has to hold and no wider — every pixel taken off the disc
// is a pixel of flow channel gained.
constexpr lv_coord_t HUB_DIAMETER = 108;

// The channel the flow dots travel down: outside the hub, inside the leg text.
constexpr lv_coord_t FLOW_INNER = 60;
constexpr lv_coord_t FLOW_OUTER = 112;
constexpr int FLOW_DOTS = 3;
constexpr lv_coord_t FLOW_DOT_SIZE = 7;

// State of charge sits due south, in the gap between home and EV — the one
// bearing the star leaves free. Pulled in from 178 to leave the day indicator a
// clear band above the page dots; the home and EV blocks sit at x = +-94 so
// there is no collision, only a smaller gap.
constexpr lv_coord_t SOC_RADIUS = 169;

// The Puck's own battery goes at 36 deg, between solar and the battery leg. The
// star's five points leave gaps at 36, 108, 180, 252 and 324; 180 is taken by the
// state of charge, so this is the next one nothing else reaches.
constexpr float DEVICE_BATTERY_BEARING = 36.0f;
constexpr lv_coord_t DEVICE_BATTERY_RADIUS = 186;

// Dot speed maps |kW| onto a travel period, so a heavy flow visibly runs faster
// than a trickle. Above FLOW_SPEED_FULL_KW it stops getting quicker.
constexpr uint32_t FLOW_PERIOD_SLOW_MS = 2400;
constexpr uint32_t FLOW_PERIOD_FAST_MS = 700;
constexpr float FLOW_SPEED_FULL_KW = 6.0f;

constexpr uint32_t FLOW_TICK_MS = 33;

constexpr float DEGREES_TO_RADIANS = 3.14159265f / 180.0f;

// Declared in clockwise order, matching the star.
enum LegId { LEG_SOLAR = 0, LEG_BATTERY, LEG_HOME, LEG_EV, LEG_GRID, LEG_COUNT };

// Which way the dots run, relative to the plant in the middle. Generation and
// import travel inward; consumption, export and charging travel outward.
enum FlowDirection { FLOW_IDLE = 0, FLOW_INWARD, FLOW_OUTWARD };

struct Leg {
  lv_obj_t* box = nullptr;
  lv_obj_t* name = nullptr;
  lv_obj_t* value = nullptr;
  lv_obj_t* detail = nullptr;
  lv_obj_t* dots[FLOW_DOTS] = {};

  // Endpoints of the dot path, in screen coordinates.
  lv_point_t inner = {};
  lv_point_t outer = {};

  FlowDirection direction = FLOW_IDLE;
  uint32_t period_ms = FLOW_PERIOD_SLOW_MS;
  float phase = 0.0f;
};

lv_obj_t* s_root = nullptr;
lv_obj_t* s_ring = nullptr;
lv_obj_t* s_plant_value = nullptr;
lv_obj_t* s_plant_status = nullptr;
lv_obj_t* s_soc_label = nullptr;
lv_obj_t* s_device_battery = nullptr;
Leg s_legs[LEG_COUNT];

// ------------------------------------------------------------- formatting ---

uint32_t period_for(float kw) {
  float fraction = fabsf(kw) / FLOW_SPEED_FULL_KW;
  if (fraction > 1.0f) {
    fraction = 1.0f;
  }
  const float span = static_cast<float>(FLOW_PERIOD_SLOW_MS - FLOW_PERIOD_FAST_MS);
  return static_cast<uint32_t>(FLOW_PERIOD_SLOW_MS - fraction * span);
}

// Offset from the centre for a bearing in degrees clockwise from north.
void offset_for(float degrees, lv_coord_t radius, lv_coord_t* dx, lv_coord_t* dy) {
  const float radians = degrees * DEGREES_TO_RADIANS;
  *dx = static_cast<lv_coord_t>(lroundf(sinf(radians) * radius));
  *dy = static_cast<lv_coord_t>(lroundf(-cosf(radians) * radius));
}

// ---------------------------------------------------------------- builders ---

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t colour) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(colour), LV_PART_MAIN);
  return label;
}

// A borderless, transparent container — LVGL's default object has padding, a
// background and a border, none of which we want for pure layout.
lv_obj_t* make_group(lv_obj_t* parent) {
  lv_obj_t* group = lv_obj_create(parent);
  lv_obj_remove_style_all(group);
  lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
  return group;
}

void build_leg(LegId id, const char* name, uint32_t colour, float degrees) {
  Leg& leg = s_legs[id];

  lv_coord_t dx = 0;
  lv_coord_t dy = 0;
  offset_for(degrees, LEG_RADIUS, &dx, &dy);

  leg.box = make_group(s_root);
  lv_obj_set_size(leg.box, LEG_BOX_WIDTH, LV_SIZE_CONTENT);
  // The box width is what centres the three lines on the leg's bearing, not a
  // limit on them: "kW generating" is wider than it and would otherwise be
  // clipped at both ends. Widening the box instead would push the battery and
  // grid blocks into the ring, which is what fixes LEG_RADIUS at 160.
  lv_obj_add_flag(leg.box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_set_flex_flow(leg.box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(leg.box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_align(leg.box, LV_ALIGN_CENTER, dx, dy);

  leg.name = make_label(leg.box, PUCK_FONT_SMALL, colour);
  lv_label_set_text(leg.name, name);

  leg.value = make_label(leg.box, PUCK_FONT_LARGE, PUCK_COLOUR_TEXT);
  lv_label_set_text(leg.value, "--");

  leg.detail = make_label(leg.box, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_label_set_text(leg.detail, "");

  offset_for(degrees, FLOW_INNER, &leg.inner.x, &leg.inner.y);
  offset_for(degrees, FLOW_OUTER, &leg.outer.x, &leg.outer.y);
  leg.inner.x += CENTRE;
  leg.inner.y += CENTRE;
  leg.outer.x += CENTRE;
  leg.outer.y += CENTRE;

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
// leg is drawn correctly on its very first frame — otherwise every dot sits at
// the origin until the first tick, visible both in a screenshot and as a flicker
// on the device.
void place_dots(Leg& leg) {
  for (int i = 0; i < FLOW_DOTS; ++i) {
    float t = leg.phase + static_cast<float>(i) / static_cast<float>(FLOW_DOTS);
    while (t >= 1.0f) {
      t -= 1.0f;
    }
    // Inward legs travel the same path backwards.
    const float travel = leg.direction == FLOW_INWARD ? 1.0f - t : t;

    const lv_coord_t x =
        leg.inner.x + static_cast<lv_coord_t>((leg.outer.x - leg.inner.x) * travel);
    const lv_coord_t y =
        leg.inner.y + static_cast<lv_coord_t>((leg.outer.y - leg.inner.y) * travel);
    lv_obj_set_pos(leg.dots[i], x - FLOW_DOT_SIZE / 2, y - FLOW_DOT_SIZE / 2);

    // Fade in and out at the ends so dots do not pop into existence.
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

  // Only claim a direction when there is a value to derive one from. An unread
  // register makes `value > 0` false, which would otherwise render as a
  // confident "export" or "discharge" for a leg we know nothing about — and a
  // true 0.00 kW is not flowing either way.
  const bool moving = power.known && fabsf(power.value) > 0.0f;

  // A known zero is a leg that is genuinely doing nothing, and saying so is
  // worth more than "0.00 kW". One dash and "idle", against the two dashes an
  // unknown gets — a leg we cannot read is not the same as a leg at rest.
  char text[16];
  if (power.known && !moving) {
    lv_label_set_text(leg.value, "-");
  } else {
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

// The plant node carries no power figure of its own — every flow is on a leg —
// so the middle is where the plant's health goes. Worst news wins.
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

  // Fresh and healthy, so the line goes back to being the unit. No age and no
  // "OK": a current reading needs no annotation, and a permanent status word
  // reads as something you are meant to act on.
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

  // State of charge around the bezel: always visible, never in the way.
  s_ring = lv_arc_create(s_root);
  lv_obj_set_size(s_ring, PUCK_RING_DIAMETER, PUCK_RING_DIAMETER);
  lv_obj_center(s_ring);
  lv_arc_set_rotation(s_ring, 270);  // zero at the top
  lv_arc_set_bg_angles(s_ring, 0, 360);
  lv_arc_set_range(s_ring, 0, 100);
  lv_arc_set_value(s_ring, 0);
  lv_obj_remove_style(s_ring, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(s_ring, PUCK_RING_WIDTH, LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_ring, PUCK_RING_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_ring, lv_color_hex(PUCK_COLOUR_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_color(s_ring, lv_color_hex(PUCK_COLOUR_BATTERY), LV_PART_INDICATOR);

  // The virtual plant node: inverter and gateway as one. A hairline rather than
  // a filled disc, so it reads as a boundary without competing with the legs.
  lv_obj_t* hub = make_group(s_root);
  lv_obj_set_size(hub, HUB_DIAMETER, HUB_DIAMETER);
  lv_obj_center(hub);
  lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_border_width(hub, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(hub, lv_color_hex(PUCK_COLOUR_TRACK), LV_PART_MAIN);

  lv_obj_t* plant_name = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_label_set_text(plant_name, "PLANT");
  lv_obj_align(plant_name, LV_ALIGN_CENTER, 0, -30);

  s_plant_value = make_label(s_root, PUCK_FONT_LARGE, PUCK_COLOUR_TEXT);
  lv_label_set_text(s_plant_value, "--");
  lv_obj_align(s_plant_value, LV_ALIGN_CENTER, 0, -4);

  // Doubles as the unit when all is well and as the warning when it is not. A
  // stale reading or a live alarm matters more than repeating "kW".
  s_plant_status = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_label_set_text(s_plant_status, "kW");
  lv_obj_align(s_plant_status, LV_ALIGN_CENTER, 0, 22);

  // Solar at the top, then clockwise. Every leg has a fixed bearing, so the EV
  // appearing or disappearing never moves anything else.
  build_leg(LEG_SOLAR, "SOLAR", PUCK_COLOUR_SOLAR, 0.0f);
  build_leg(LEG_BATTERY, "BATTERY", PUCK_COLOUR_BATTERY, 72.0f);
  build_leg(LEG_HOME, "HOME", PUCK_COLOUR_HOME, 144.0f);
  build_leg(LEG_EV, "EV", PUCK_COLOUR_EV, 216.0f);
  build_leg(LEG_GRID, "GRID", PUCK_COLOUR_GRID, 288.0f);

  // Labelled, unlike the bare percentage it replaces — the colour ties it to the
  // ring, but only if you already know the ring is the battery.
  lv_coord_t soc_dx = 0;
  lv_coord_t soc_dy = 0;
  offset_for(180.0f, SOC_RADIUS, &soc_dx, &soc_dy);
  s_soc_label = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_BATTERY);
  lv_label_set_text(s_soc_label, "SOC --");
  lv_obj_align(s_soc_label, LV_ALIGN_CENTER, soc_dx, soc_dy);

  lv_coord_t battery_dx = 0;
  lv_coord_t battery_dy = 0;
  offset_for(DEVICE_BATTERY_BEARING, DEVICE_BATTERY_RADIUS, &battery_dx, &battery_dy);
  s_device_battery = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED);
  lv_label_set_text(s_device_battery, "");
  lv_obj_align(s_device_battery, LV_ALIGN_CENTER, battery_dx, battery_dy);
  lv_obj_add_flag(s_device_battery, LV_OBJ_FLAG_HIDDEN);

  lv_timer_create(flow_tick, FLOW_TICK_MS, nullptr);

  return s_root;
}

void screen_power_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }

  set_plant_status(snapshot);

  // The plant's own throughput, from plant_active_power. Shows "--" against a
  // server that does not send power.plant yet.
  char plant_text[16];
  puck_format_magnitude(snapshot.valid ? snapshot.power.plant : MaybeFloat{},
                        PUCK_KW_DECIMALS, plant_text, sizeof(plant_text));
  lv_label_set_text(s_plant_value, plant_text);

  if (!snapshot.valid) {
    const MaybeFloat unknown;
    lv_arc_set_value(s_ring, 0);
    lv_label_set_text(s_soc_label, "SOC --");
    set_leg(LEG_SOLAR, unknown, FLOW_IDLE, "", true);
    set_leg(LEG_BATTERY, unknown, FLOW_IDLE, "", true);
    set_leg(LEG_HOME, unknown, FLOW_IDLE, "", true);
    set_leg(LEG_EV, unknown, FLOW_IDLE, "", true);
    set_leg(LEG_GRID, unknown, FLOW_IDLE, "", true);
    return;
  }

  // Battery ring and its readout.
  if (snapshot.battery.soc_pct.known) {
    lv_arc_set_value(s_ring, static_cast<int16_t>(snapshot.battery.soc_pct.value));
    char text[16];
    snprintf(text, sizeof(text), "SOC %.0f%%", snapshot.battery.soc_pct.value);
    lv_label_set_text(s_soc_label, text);
  } else {
    lv_arc_set_value(s_ring, 0);
    lv_label_set_text(s_soc_label, "SOC --");
  }

  // Generation only ever flows into the plant.
  set_leg(LEG_SOLAR, snapshot.power.pv, FLOW_INWARD, "kW generating", true);

  // Charging takes power out of the plant; discharging returns it.
  const bool charging = snapshot.power.batt.known && snapshot.power.batt.value > 0.0f;
  set_leg(LEG_BATTERY, snapshot.power.batt, charging ? FLOW_OUTWARD : FLOW_INWARD,
          charging ? "kW charge" : "kW discharge", true);

  // Consumers always draw outward.
  set_leg(LEG_HOME, snapshot.power.home, FLOW_OUTWARD, "kW on load", true);

  // The EV leg is always drawn, idle or not: a point of the star that comes and
  // goes makes the shape itself flicker, and "EV / - / idle" is information.
  set_leg(LEG_EV, snapshot.power.ev, FLOW_OUTWARD, "kW charge", true);

  // Off grid: the leg is suppressed rather than drawn as a real zero, but it is
  // labelled so the gap reads as a state and not a fault.
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
  // Amber below a fifth: on battery, that is the point at which it stops being
  // information and starts being something to act on.
  lv_obj_set_style_text_color(
      s_device_battery,
      lv_color_hex(percent >= 0 && percent < 20 ? PUCK_COLOUR_WARN : PUCK_COLOUR_MUTED),
      LV_PART_MAIN);
  lv_obj_clear_flag(s_device_battery, LV_OBJ_FLAG_HIDDEN);
}
