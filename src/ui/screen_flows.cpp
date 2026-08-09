#include "screen_flows.h"

#include <math.h>
#include <stdio.h>

#include "board_config.h"
#include "format.h"
#include "theme.h"

namespace {

// ------------------------------------------------------------------ layout ---
//
// Three source nodes down the left, three sink nodes down the right, joined by
// one ribbon per flow whose thickness is the energy that took that path.
//
// The rule that makes it readable: **a colour is always a node, and a ribbon
// takes the colour of the node it leaves**. Direction is carried by position —
// left is out, right is in — and by the ribbon itself. The stacked bars this
// replaced coloured segments by their counterpart, so green meant "into the
// battery" in one bar and "out of it" in the other, and blue meant export then
// import. Whatever rule the eye learned from one was wrong for the next.
//
// Nodes sit at fixed rows rather than stacked contiguously as a true Sankey
// would. A contiguous stack makes the column height mean the day's total, which
// is a fact nobody reads off a 466 px circle, and it moves every label whenever
// the split changes. Fixed rows keep the six labels in the same place all day,
// which is what makes it glanceable.

// Three sources, four sinks, so the two columns get their own rows. They share a
// top and a bottom so the shape stays balanced; only the spacing differs.
//
// The bottom row is fixed by the bezel: a label reaches 21 px below its row and
// sits 182 px out, and the glass has closed to that at y = 120.
constexpr lv_coord_t LEFT_ROW_Y[3] = {-76, 5, 86};
constexpr lv_coord_t RIGHT_ROW_Y[4] = {-76, -22, 32, 86};

// The node bars, and the channel the ribbons run down between them.
constexpr lv_coord_t NODE_X = 132;
constexpr lv_coord_t NODE_WIDTH = 9;
constexpr lv_coord_t RIBBON_X = NODE_X - NODE_WIDTH / 2 - 1;  // inner edge

// Labels outboard of the bars. 162 rather than further out because the bottom
// row's label reaches y = 116, where the bezel has closed in to 184 px.
constexpr lv_coord_t LABEL_X = 162;

// The tallest a node bar may be drawn. Set by the tighter of the two columns —
// the sinks, 54 apart — so neighbours never touch.
constexpr lv_coord_t MAX_NODE_PX = 44;

// A flow that happened must be visible, however small its share. Below a pixel
// or two it would round away and the diagram would claim it never happened.
constexpr lv_coord_t MIN_FLOW_PX = 2;

// Ribbons are drawn as a run of vertical slices. Three pixels is fine for a shape
// this size and keeps the slice count — and so the draw cost — to about a third
// of what one-pixel columns would need.
constexpr lv_coord_t SLICE_PX = 3;

// Translucent so overlaps read as overlaps. Solid ribbons crossing each other
// would look like one shape with a corner in it.
constexpr lv_opa_t RIBBON_OPA = LV_OPA_60;

enum Source { SRC_SOLAR = 0, SRC_BATTERY, SRC_GRID, SRC_COUNT };

// EV is its own sink rather than part of the house. A 7 kW charger dwarfs a
// house, so folding the two together leaves a "home" figure that says nothing
// about the home — and screen 1 already draws the charger as a leg of its own.
enum Sink { SNK_HOME = 0, SNK_EV, SNK_BATTERY, SNK_GRID, SNK_COUNT };

// Declared source-major, which is also the order ribbons stack at each end: it
// keeps the same flow in the same place on both nodes and cuts the crossings.
struct FlowSpec {
  Source from;
  Sink to;
};
constexpr FlowSpec FLOWS[] = {
    {SRC_SOLAR, SNK_HOME},   {SRC_SOLAR, SNK_EV},   {SRC_SOLAR, SNK_BATTERY},
    {SRC_SOLAR, SNK_GRID},   {SRC_BATTERY, SNK_HOME}, {SRC_BATTERY, SNK_EV},
    {SRC_BATTERY, SNK_GRID}, {SRC_GRID, SNK_HOME},  {SRC_GRID, SNK_EV},
    {SRC_GRID, SNK_BATTERY},
};
constexpr size_t FLOW_COUNT = sizeof(FLOWS) / sizeof(FLOWS[0]);

constexpr uint32_t SOURCE_COLOUR[SRC_COUNT] = {PUCK_COLOUR_SOLAR, PUCK_COLOUR_BATTERY,
                                               PUCK_COLOUR_GRID};
constexpr uint32_t SINK_COLOUR[SNK_COUNT] = {PUCK_COLOUR_HOME, PUCK_COLOUR_EV,
                                             PUCK_COLOUR_BATTERY, PUCK_COLOUR_GRID};

// Everything the draw callback needs, computed once per reading rather than per
// buffer slice: LVGL calls the callback once for every slice the object crosses,
// and at ~210 px tall against a 40-line buffer that is six times.
struct Geometry {
  bool valid = false;
  struct Ribbon {
    bool drawn = false;
    uint32_t colour = 0;
    lv_coord_t left_top = 0;
    lv_coord_t right_top = 0;
    lv_coord_t thickness = 0;
  };
  Ribbon ribbon[FLOW_COUNT];
};
Geometry s_geometry;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_canvas = nullptr;
lv_obj_t* s_arc = nullptr;
lv_obj_t* s_hero = nullptr;
lv_obj_t* s_caption = nullptr;

struct NodeUi {
  lv_obj_t* bar = nullptr;
  lv_obj_t* name = nullptr;
  lv_obj_t* value = nullptr;
};
NodeUi s_source[SRC_COUNT];
NodeUi s_sink[SNK_COUNT];

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t colour, lv_coord_t x,
                     lv_coord_t y) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(colour), LV_PART_MAIN);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(label, LV_ALIGN_CENTER, x, y);
  return label;
}

void build_node(NodeUi* node, const char* name, uint32_t colour, lv_coord_t x, lv_coord_t y) {
  node->bar = lv_obj_create(s_root);
  lv_obj_remove_style_all(node->bar);
  lv_obj_clear_flag(node->bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(node->bar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(node->bar, NODE_WIDTH, MAX_NODE_PX);
  lv_obj_set_style_radius(node->bar, NODE_WIDTH / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(node->bar, lv_color_hex(colour), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(node->bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(node->bar, LV_ALIGN_CENTER, x, y);

  const lv_coord_t label_x = x < 0 ? -LABEL_X : LABEL_X;
  node->name = make_label(s_root, PUCK_FONT_SMALL, colour, label_x, y - 11);
  lv_label_set_text(node->name, name);
  node->value = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, label_x, y + 11);
  lv_label_set_text(node->value, "--");
}

void show_node(const NodeUi& node, bool visible) {
  lv_obj_t* const parts[] = {node.bar, node.name, node.value};
  for (lv_obj_t* part : parts) {
    if (visible) {
      lv_obj_clear_flag(part, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(part, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

// Eases the ribbon into and out of its endpoints so it leaves each node square
// on. A straight line between two offset nodes reads as a wire; this reads as a
// flow, which is the only reason to draw it as a ribbon at all.
float smoothstep(float t) {
  return t * t * (3.0f - 2.0f * t);
}

void draw_ribbons(lv_event_t* event) {
  if (!s_geometry.valid) {
    return;
  }
  lv_obj_t* obj = lv_event_get_target(event);
  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(event);
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);

  const lv_coord_t x0 = coords.x1;
  const lv_coord_t x1 = coords.x2;
  const float span = static_cast<float>(x1 - x0);
  if (span <= 0.0f) {
    return;
  }

  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.bg_opa = RIBBON_OPA;

  for (const Geometry::Ribbon& ribbon : s_geometry.ribbon) {
    if (!ribbon.drawn) {
      continue;
    }
    dsc.bg_color = lv_color_hex(ribbon.colour);
    const float rise = static_cast<float>(ribbon.right_top - ribbon.left_top);
    for (lv_coord_t x = x0; x <= x1; x += SLICE_PX) {
      lv_coord_t next = x + SLICE_PX;
      if (next > x1) {
        next = x1;
      }
      // Each slice spans its own height *and* the next one's, so the ribbon has
      // no steps down a steep diagonal. A slice drawn flat at its own height is
      // fine where the flow runs level, but where it climbs, consecutive slices
      // are several pixels apart and the edge turns into a staircase.
      const float here = static_cast<float>(ribbon.left_top) +
                         rise * smoothstep(static_cast<float>(x - x0) / span);
      const float there = static_cast<float>(ribbon.left_top) +
                          rise * smoothstep(static_cast<float>(next - x0) / span);
      const lv_coord_t top = static_cast<lv_coord_t>(lroundf(here < there ? here : there));
      const lv_coord_t bottom = static_cast<lv_coord_t>(lroundf(here < there ? there : here));

      lv_area_t area;
      area.x1 = x;
      area.x2 = x + SLICE_PX - 1;
      if (area.x2 > x1) {
        area.x2 = x1;
      }
      area.y1 = top;
      area.y2 = bottom + ribbon.thickness - 1;
      lv_draw_rect(ctx, &dsc, &area);
    }
  }
}

void clear_geometry() {
  s_geometry.valid = false;
  for (Geometry::Ribbon& ribbon : s_geometry.ribbon) {
    ribbon.drawn = false;
  }
}

void set_node_value(const NodeUi& node, float kwh) {
  char text[16];
  snprintf(text, sizeof(text), "%.1f", kwh);
  lv_label_set_text(node.value, text);
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

  // The ribbon channel, drawn before the node bars so the bars cap the ends.
  s_canvas = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_canvas);
  lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(s_canvas, RIBBON_X * 2, PUCK_LCD_HEIGHT);
  lv_obj_center(s_canvas);
  lv_obj_add_event_cb(s_canvas, draw_ribbons, LV_EVENT_DRAW_MAIN_END, nullptr);

  lv_obj_t* title = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 0, -186);
  lv_obj_set_style_text_letter_space(title, 3, LV_PART_MAIN);
  lv_label_set_text(title, "FLOWS  ·  kWh");

  s_hero = make_label(s_root, PUCK_FONT_HERO, PUCK_COLOUR_TEXT, 0, -152);
  lv_label_set_text(s_hero, "--");

  s_caption = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 0, -112);
  lv_label_set_text(s_caption, "self-sufficient");

  // Short names on purpose: the labels sit where the bezel has already closed in,
  // and "BATTERY" twice over would not fit beside the bars.
  static const char* const SOURCE_NAME[SRC_COUNT] = {"SOLAR", "BATT", "GRID"};
  static const char* const SINK_NAME[SNK_COUNT] = {"HOME", "EV", "BATT", "GRID"};
  for (int i = 0; i < SRC_COUNT; ++i) {
    build_node(&s_source[i], SOURCE_NAME[i], SOURCE_COLOUR[i], -NODE_X, LEFT_ROW_Y[i]);
  }
  for (int i = 0; i < SNK_COUNT; ++i) {
    build_node(&s_sink[i], SINK_NAME[i], SINK_COLOUR[i], NODE_X, RIGHT_ROW_Y[i]);
  }

  return s_root;
}

void screen_flows_update(const Snapshot& snapshot) {
  if (s_root == nullptr) {
    return;
  }

  const bool have = snapshot.valid && snapshot.today.present;
  const Snapshot::Today::Flows& f = snapshot.today.flows;

  // Unknown counts as nothing here rather than as a dash: this screen draws a
  // shape, and there is no shape for "unknown".
  const auto amount_of = [have](const MaybeFloat& v) {
    return have && v.known && v.value > 0.0f ? v.value : 0.0f;
  };
  // The EV's share is carved out of the matching load flow, so the house gets
  // what is left. A server with no split sends no *_ev, which reads as zero and
  // leaves the whole flow on the house — which is what such a server meant.
  const auto to_home = [&amount_of](const MaybeFloat& load, const MaybeFloat& ev) {
    const float home = amount_of(load) - amount_of(ev);
    return home > 0.0f ? home : 0.0f;
  };

  // Source-major, matching FLOWS above.
  const float amount[FLOW_COUNT] = {
      to_home(f.solar_load, f.solar_ev), amount_of(f.solar_ev),
      amount_of(f.solar_batt),           amount_of(f.solar_grid),
      to_home(f.batt_load, f.batt_ev),   amount_of(f.batt_ev),
      amount_of(f.batt_grid),            to_home(f.grid_load, f.grid_ev),
      amount_of(f.grid_ev),              amount_of(f.grid_batt),
  };

  float source_total[SRC_COUNT] = {};
  float sink_total[SNK_COUNT] = {};
  float from_grid = 0.0f;  // grid straight to consumption, for self-sufficiency
  float largest = 0.0f;
  bool any = false;
  for (size_t i = 0; i < FLOW_COUNT; ++i) {
    source_total[FLOWS[i].from] += amount[i];
    sink_total[FLOWS[i].to] += amount[i];
    if (FLOWS[i].from == SRC_GRID && (FLOWS[i].to == SNK_HOME || FLOWS[i].to == SNK_EV)) {
      from_grid += amount[i];
    }
    if (amount[i] > 0.0f) {
      any = true;
    }
  }
  for (float total : source_total) {
    largest = total > largest ? total : largest;
  }
  for (float total : sink_total) {
    largest = total > largest ? total : largest;
  }

  if (!any || largest <= 0.0f) {
    clear_geometry();
    lv_obj_invalidate(s_canvas);
    lv_label_set_text(s_hero, "--");
    lv_obj_add_flag(s_arc, LV_OBJ_FLAG_HIDDEN);
    for (const NodeUi& node : s_source) {
      show_node(node, false);
    }
    for (const NodeUi& node : s_sink) {
      show_node(node, false);
    }
    return;
  }

  // One scale for both columns — the two sides carry the same total, so scaling
  // them separately would make a kWh mean two different heights on one screen.
  const float scale = static_cast<float>(MAX_NODE_PX) / largest;

  lv_coord_t source_top[SRC_COUNT];
  lv_coord_t sink_top[SNK_COUNT];
  for (int i = 0; i < SRC_COUNT; ++i) {
    const lv_coord_t height = static_cast<lv_coord_t>(lroundf(source_total[i] * scale));
    lv_obj_set_height(s_source[i].bar, height > 0 ? height : 1);
    lv_obj_align(s_source[i].bar, LV_ALIGN_CENTER, -NODE_X, LEFT_ROW_Y[i]);
    source_top[i] = PUCK_LCD_HEIGHT / 2 + LEFT_ROW_Y[i] - height / 2;
    show_node(s_source[i], source_total[i] > 0.0f);
    set_node_value(s_source[i], source_total[i]);
  }
  for (int i = 0; i < SNK_COUNT; ++i) {
    const lv_coord_t height = static_cast<lv_coord_t>(lroundf(sink_total[i] * scale));
    lv_obj_set_height(s_sink[i].bar, height > 0 ? height : 1);
    lv_obj_align(s_sink[i].bar, LV_ALIGN_CENTER, NODE_X, RIGHT_ROW_Y[i]);
    sink_top[i] = PUCK_LCD_HEIGHT / 2 + RIGHT_ROW_Y[i] - height / 2;
    show_node(s_sink[i], sink_total[i] > 0.0f);
    set_node_value(s_sink[i], sink_total[i]);
  }

  // Stack the ribbons up each node in declaration order, so a flow sits at the
  // same rank on the node it leaves and the node it enters.
  lv_coord_t source_used[SRC_COUNT] = {};
  lv_coord_t sink_used[SNK_COUNT] = {};
  clear_geometry();
  for (size_t i = 0; i < FLOW_COUNT; ++i) {
    if (amount[i] <= 0.0f) {
      continue;
    }
    lv_coord_t thickness = static_cast<lv_coord_t>(lroundf(amount[i] * scale));
    if (thickness < MIN_FLOW_PX) {
      thickness = MIN_FLOW_PX;
    }
    Geometry::Ribbon& ribbon = s_geometry.ribbon[i];
    ribbon.drawn = true;
    ribbon.colour = SOURCE_COLOUR[FLOWS[i].from];
    ribbon.thickness = thickness;
    ribbon.left_top = source_top[FLOWS[i].from] + source_used[FLOWS[i].from];
    ribbon.right_top = sink_top[FLOWS[i].to] + sink_used[FLOWS[i].to];
    source_used[FLOWS[i].from] += thickness;
    sink_used[FLOWS[i].to] += thickness;
  }
  s_geometry.valid = true;
  lv_obj_invalidate(s_canvas);

  // Self-sufficiency: the share of the day's load that did not come off the
  // grid. Derived here rather than sent, on the same footing as the battery
  // screen's stored-kWh figure — one division between two numbers already in the
  // payload, not a piece of the server's business logic.
  // Consumption is the house and the car together: charging one is still using
  // energy, and a day spent filling the car off the grid is not self-sufficient
  // however little the house drew.
  const float consumed = sink_total[SNK_HOME] + sink_total[SNK_EV];
  if (consumed <= 0.0f) {
    lv_label_set_text(s_hero, "--");
    lv_obj_add_flag(s_arc, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  float pct = (consumed - from_grid) / consumed * 100.0f;
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
