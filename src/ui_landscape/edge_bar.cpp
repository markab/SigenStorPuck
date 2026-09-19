#include "edge_bar.h"

#include <math.h>

#include "board_config.h"
#include "theme.h"

namespace {

// Bezel inset and corner radius for the rounded-rect path. Matches the mock.
constexpr float INSET = 12.0f;
constexpr float RADIUS = 36.0f;
constexpr lv_coord_t STROKE = 8;
constexpr int ARC_STEPS = 8;   // points per 90-degree corner
constexpr int MAX_PTS = 80;

// Per-bar state: the two point buffers must outlive the call, because lv_line
// references them rather than copying.
struct EdgeBar {
  lv_obj_t* track;
  lv_obj_t* indicator;
  lv_point_t track_pts[MAX_PTS];
  float cum_len[MAX_PTS];  // path length up to each track point
  int count;
  float total;
  lv_point_t ind_pts[MAX_PTS];
};

void add_point(EdgeBar* b, float x, float y) {
  if (b->count >= MAX_PTS) {
    return;
  }
  b->track_pts[b->count].x = static_cast<lv_coord_t>(lroundf(x));
  b->track_pts[b->count].y = static_cast<lv_coord_t>(lroundf(y));
  if (b->count == 0) {
    b->cum_len[0] = 0.0f;
  } else {
    const float dx = b->track_pts[b->count].x - b->track_pts[b->count - 1].x;
    const float dy = b->track_pts[b->count].y - b->track_pts[b->count - 1].y;
    b->cum_len[b->count] = b->cum_len[b->count - 1] + sqrtf(dx * dx + dy * dy);
  }
  ++b->count;
}

void add_arc(EdgeBar* b, float cx, float cy, float start_deg, float end_deg) {
  for (int i = 1; i <= ARC_STEPS; ++i) {
    const float t = static_cast<float>(i) / ARC_STEPS;
    const float a = (start_deg + (end_deg - start_deg) * t) * 3.14159265f / 180.0f;
    add_point(b, cx + RADIUS * cosf(a), cy + RADIUS * sinf(a));
  }
}

// Walk the rounded rectangle from top-centre, clockwise. Angles are the standard
// screen convention (0 = +x, 90 = +y down), so a corner turns from one edge
// tangent to the next.
void build_path(EdgeBar* b) {
  const float left = INSET;
  const float right = PUCK_LCD_WIDTH - INSET;
  const float top = INSET;
  const float bottom = PUCK_LCD_HEIGHT - INSET;
  const float cx = PUCK_LCD_WIDTH / 2.0f;

  b->count = 0;
  add_point(b, cx, top);                                 // top centre
  add_point(b, right - RADIUS, top);                     // to top-right corner
  add_arc(b, right - RADIUS, top + RADIUS, -90.0f, 0.0f);
  add_point(b, right, bottom - RADIUS);                  // down the right edge
  add_arc(b, right - RADIUS, bottom - RADIUS, 0.0f, 90.0f);
  add_point(b, left + RADIUS, bottom);                   // along the bottom
  add_arc(b, left + RADIUS, bottom - RADIUS, 90.0f, 180.0f);
  add_point(b, left, top + RADIUS);                      // up the left edge
  add_arc(b, left + RADIUS, top + RADIUS, 180.0f, 270.0f);
  add_point(b, cx, top);                                 // back to top centre
  b->total = b->cum_len[b->count - 1];
}

lv_obj_t* make_line(lv_obj_t* parent, uint32_t colour) {
  lv_obj_t* line = lv_line_create(parent);
  lv_obj_set_pos(line, 0, 0);
  lv_obj_set_style_line_width(line, STROKE, LV_PART_MAIN);
  lv_obj_set_style_line_color(line, lv_color_hex(colour), LV_PART_MAIN);
  lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
  return line;
}

}  // namespace

lv_obj_t* edge_bar_create(lv_obj_t* parent) {
  lv_obj_t* root = lv_obj_create(parent);
  lv_obj_remove_style_all(root);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_align(root, LV_ALIGN_TOP_LEFT, 0, 0);

  EdgeBar* b = static_cast<EdgeBar*>(lv_mem_alloc(sizeof(EdgeBar)));
  build_path(b);

  b->track = make_line(root, PUCK_COLOUR_TRACK);
  lv_line_set_points(b->track, b->track_pts, b->count);

  b->indicator = make_line(root, PUCK_COLOUR_BATTERY);
  lv_obj_add_flag(b->indicator, LV_OBJ_FLAG_HIDDEN);

  root->user_data = b;
  return root;
}

void edge_bar_set(lv_obj_t* bar, float fraction, uint32_t colour) {
  if (bar == nullptr || bar->user_data == nullptr) {
    return;
  }
  EdgeBar* b = static_cast<EdgeBar*>(bar->user_data);
  if (fraction < 0.0f) {
    fraction = 0.0f;
  }
  if (fraction > 1.0f) {
    fraction = 1.0f;
  }
  lv_obj_set_style_line_color(b->indicator, lv_color_hex(colour), LV_PART_MAIN);

  const float target = fraction * b->total;
  int n = 0;
  b->ind_pts[n++] = b->track_pts[0];
  for (int i = 1; i < b->count; ++i) {
    if (b->cum_len[i] <= target) {
      b->ind_pts[n++] = b->track_pts[i];
      continue;
    }
    // Interpolate the final point at exactly the target length.
    const float seg = b->cum_len[i] - b->cum_len[i - 1];
    const float t = seg > 0.0f ? (target - b->cum_len[i - 1]) / seg : 0.0f;
    const float x = b->track_pts[i - 1].x + (b->track_pts[i].x - b->track_pts[i - 1].x) * t;
    const float y = b->track_pts[i - 1].y + (b->track_pts[i].y - b->track_pts[i - 1].y) * t;
    b->ind_pts[n].x = static_cast<lv_coord_t>(lroundf(x));
    b->ind_pts[n].y = static_cast<lv_coord_t>(lroundf(y));
    ++n;
    break;
  }

  if (n < 2) {
    lv_obj_add_flag(b->indicator, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_line_set_points(b->indicator, b->ind_pts, n);
  lv_obj_clear_flag(b->indicator, LV_OBJ_FLAG_HIDDEN);
}

void edge_bar_set_hidden(lv_obj_t* bar, bool hidden) {
  if (bar == nullptr) {
    return;
  }
  if (hidden) {
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
  }
}
