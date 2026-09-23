#include "chart_band.h"

#include <math.h>

#include "board_config.h"
#include "theme.h"
#include "ui_perf.h"

namespace {

// Two pixels per column. One would be finer than the panel deserves at this
// size and doubles the number of draw calls; anything wider starts to read as a
// bar chart rather than a curve.
constexpr lv_coord_t COLUMN_PX = 2;

// Enough for a band spanning the whole panel, not just the safe square: a
// bezel-clipped band is drawn edge to edge and lets the glass cut the ends. The
// 2.41's landscape band is 564 px wide (282 columns at COLUMN_PX), so this must
// clear that or a wide band stops short of its right edge.
constexpr size_t MAX_COLUMNS = 288;  // a full-width landscape band at COLUMN_PX

// Three weights, which is what makes this an envelope rather than a silhouette:
// a faint wash under the curve, the column's own min-to-max spread picked out a
// little stronger, and a thin solid cap along the top.
//
// The spread must stay well below full opacity. A cloud-broken column runs from
// near zero to peak, so drawing that span solid fills most of the band and the
// chart turns into a block.
constexpr lv_opa_t FILL_OPA = LV_OPA_20;
constexpr lv_opa_t SPREAD_OPA = LV_OPA_40;
constexpr lv_coord_t EDGE_PX = 2;

// A band whose samples are all equal (a flat SoC overnight) would otherwise
// divide by zero when scaling. Give it a minimum span so it draws as a line
// somewhere sensible rather than collapsing or exploding.
constexpr float MIN_SPAN = 0.1f;

// The widest smoothing window a band may ask for, in columns. Beyond about this
// a day's shape stops being a curve and becomes a hill.
constexpr uint8_t MAX_SMOOTHING = 15;

// The clip circle is concentric with the panel, which is also where the
// state-of-charge ring is drawn — so a radius just inside the ring keeps every
// band clear of it. Rotation happens in display.cpp's flush callback, so LVGL's
// coordinates are always the unrotated panel and this stays true at any
// orientation.
constexpr lv_coord_t CENTRE_X = PUCK_LCD_WIDTH / 2;
constexpr lv_coord_t CENTRE_Y = PUCK_LCD_HEIGHT / 2;

// Enough for every band alive at once: battery (1), solar (2, actual + forecast),
// load (1) and, on the landscape UI, grid (1). Each carries a MAX_COLUMNS reduce
// cache, ~3.4 KB, so the pool is sized to what the screens actually build.
constexpr size_t MAX_BANDS = 6;

struct Band {
  bool used = false;
  lv_obj_t* obj = nullptr;
  HistorySeries series = HistorySeries::Pv;
  uint32_t colour = 0;
  float range_min = 0.0f;
  float range_max = 0.0f;
  bool autoscale = true;
  lv_opa_t intensity = LV_OPA_COVER;
  uint8_t smoothing = 1;
  lv_coord_t clip_radius = 0;

  // Bipolar: a signed series drawn about a centre zero line, filling up in
  // `colour` for positive and down in `colour_neg` for negative. The range is
  // forced symmetric so the baseline sits at the band's middle. Used by the grid
  // screen, where positive is import and negative export.
  bool bipolar = false;
  uint32_t colour_neg = 0;

  // The reduced window, cached.
  //
  // Not recomputed inside the draw callback on purpose: LVGL calls that once per
  // buffer slice the object intersects, and with a 40-line buffer a ~90 px band
  // is three slices. Reducing 1440 samples three times per redraw to draw the
  // same picture would be pure waste.
  HistoryColumn column[MAX_COLUMNS];
  size_t columns = 0;
  float drawn_min = 0.0f;
  float drawn_max = 0.0f;
  bool has_data = false;

  uint32_t last_minute = UINT32_MAX;  // forces the first refresh
  uint32_t last_generation = 0;
  uint32_t last_revision = 0;
};

Band s_bands[MAX_BANDS];

// Global draw pause. On a software-rotated panel a full-width band is hundreds of
// draw_rects, and redrawing that on every frame of a tileview swipe is what makes
// the swipe drag. Paused while a swipe is in flight (ui.cpp), the bands skip their
// draw and the sliding frames stay light; they redraw once on release.
bool s_paused = false;

Band* band_for(lv_obj_t* obj) {
  if (obj == nullptr) {
    return nullptr;
  }
  return static_cast<Band*>(lv_obj_get_user_data(obj));
}

// A signed series about a centre zero line: positive fills up in `colour`,
// negative down in `colour_neg`, with a faint rule on zero. A ghosted backdrop
// like the others, so flat intensity fills rather than the monopolar gradient,
// and no bezel clip — the grid screen is the rectangular 2.41 panel. A column
// whose envelope straddles zero draws both halves, so a minute that swung from
// import to export shows as both.
void draw_band_bipolar(lv_draw_ctx_t* ctx, Band* band, const lv_area_t& coords,
                       lv_coord_t height) {
  const float span = band->drawn_max - band->drawn_min;
  const float scale = static_cast<float>(height - 1) / (span < MIN_SPAN ? MIN_SPAN : span);
  const auto y_of = [&](float v) -> lv_coord_t {
    lv_coord_t y = coords.y2 - static_cast<lv_coord_t>((v - band->drawn_min) * scale);
    if (y < coords.y1) {
      y = coords.y1;
    }
    if (y > coords.y2) {
      y = coords.y2;
    }
    return y;
  };
  const lv_coord_t baseline = y_of(0.0f);

  lv_draw_rect_dsc_t imp;
  lv_draw_rect_dsc_init(&imp);
  imp.bg_color = lv_color_hex(band->colour);
  imp.bg_opa = band->intensity;
  lv_draw_rect_dsc_t exp;
  lv_draw_rect_dsc_init(&exp);
  exp.bg_color = lv_color_hex(band->colour_neg);
  exp.bg_opa = band->intensity;

  for (size_t c = 0; c < band->columns; ++c) {
    if (!band->column[c].known) {
      continue;
    }
    const lv_coord_t x = coords.x1 + static_cast<lv_coord_t>(c) * COLUMN_PX;
    if (x > coords.x2) {
      break;
    }
    lv_area_t area;
    area.x1 = x;
    area.x2 = x + COLUMN_PX - 1;
    if (area.x2 > coords.x2) {
      area.x2 = coords.x2;
    }
    const float hi = band->column[c].max_value;
    const float lo = band->column[c].min_value;
    if (hi > 0.0f) {
      area.y1 = y_of(hi);
      area.y2 = baseline;
      lv_draw_rect(ctx, &imp, &area);
    }
    if (lo < 0.0f) {
      area.y1 = baseline;
      area.y2 = y_of(lo);
      lv_draw_rect(ctx, &exp, &area);
    }
  }

  // The zero line, so import above and export below read against a fixed datum.
  lv_draw_rect_dsc_t zero;
  lv_draw_rect_dsc_init(&zero);
  zero.bg_color = lv_color_hex(PUCK_COLOUR_MUTED);
  zero.bg_opa = LV_OPA_40;
  lv_area_t line;
  line.x1 = coords.x1;
  line.x2 = coords.x2;
  line.y1 = baseline;
  line.y2 = baseline;
  lv_draw_rect(ctx, &zero, &line);
}

void draw_band(lv_event_t* event) {
  if (s_paused) {
    return;
  }
  lv_obj_t* obj = lv_event_get_target(event);
  Band* band = band_for(obj);
  if (band == nullptr || !band->has_data || band->columns == 0) {
    return;
  }
  UiPerfTimer perf(UiPerfSection::ChartDraw);

  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(event);
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);

  const lv_coord_t height = lv_area_get_height(&coords);
  if (height <= 0) {
    return;
  }

  if (band->bipolar) {
    draw_band_bipolar(ctx, band, coords, height);
    return;
  }

  const float span = band->drawn_max - band->drawn_min;
  const float scale = static_cast<float>(height - 1) / (span < MIN_SPAN ? MIN_SPAN : span);

  const bool ghosted = band->intensity < LV_OPA_COVER;
  const auto scaled = [band](lv_opa_t base) {
    return static_cast<lv_opa_t>((static_cast<uint16_t>(base) * band->intensity) / 255);
  };
  const auto lifted = [](uint16_t value) {
    return static_cast<lv_opa_t>(value > LV_OPA_COVER ? LV_OPA_COVER : value);
  };

  lv_draw_rect_dsc_t fill;
  lv_draw_rect_dsc_init(&fill);
  fill.bg_color = lv_color_hex(band->colour);

  // A ghosted band is a backdrop, and a backdrop must not have edges. Left as a
  // flat wash it reads as a translucent rectangle sitting behind the text —
  // which is what it is — so instead each column fades to the background colour
  // on the way down and the block has no foot to notice.
  //
  // A gradient in the colour rather than in the alpha because LVGL 8's gradient
  // stops carry no opacity. On a true-black AMOLED background the two are the
  // same picture.
  //
  // Because the gradient is what does the fading, the fill's own opacity does not
  // also have to be tiny — which is the mistake the first version made, dimming
  // twice and leaving a curve nobody could see. Ghosted, the intensity *is* the
  // fill's strength just under the cap, and the gradient takes it to nothing by
  // the foot.
  if (ghosted) {
    fill.bg_opa = band->intensity;
    fill.bg_grad.dir = LV_GRAD_DIR_VER;
    fill.bg_grad.stops_count = 2;
    fill.bg_grad.stops[0].color = lv_color_hex(band->colour);
    fill.bg_grad.stops[0].frac = 0;
    fill.bg_grad.stops[1].color = lv_color_hex(PUCK_COLOUR_BG);
    fill.bg_grad.stops[1].frac = 255;
  } else {
    fill.bg_opa = FILL_OPA;
  }

  lv_draw_rect_dsc_t spread;
  lv_draw_rect_dsc_init(&spread);
  spread.bg_color = lv_color_hex(band->colour);
  spread.bg_opa = ghosted ? scaled(SPREAD_OPA) : SPREAD_OPA;

  // The cap is the one part that should stay crisp however far back the rest is
  // pushed: it is the line the eye follows, and a gradient fill with no outline
  // reads as a smudge rather than as a day.
  lv_draw_rect_dsc_t edge;
  lv_draw_rect_dsc_init(&edge);
  edge.bg_color = lv_color_hex(band->colour);
  edge.bg_opa = ghosted ? lifted(static_cast<uint16_t>(band->intensity) * 2) : LV_OPA_COVER;

  // The previous column's cap height, so the outline can be drawn as a connected
  // line; -1 whenever the run breaks, because a gap must not be spanned.
  lv_coord_t bridge_from = -1;

  for (size_t c = 0; c < band->columns; ++c) {
    if (!band->column[c].known) {
      bridge_from = -1;
      continue;  // a gap stays a gap; bridging it would invent readings
    }
    const lv_coord_t x = coords.x1 + static_cast<lv_coord_t>(c) * COLUMN_PX;
    if (x > coords.x2) {
      break;
    }

    // How far down this column may be drawn. Normally the foot of the band; on a
    // bezel-clipped band, wherever the circle cuts it, so the fill lands on an
    // arc rather than on a straight edge lying across the ring.
    lv_coord_t floor_y = coords.y2;
    if (band->clip_radius > 0) {
      // Measured at whichever edge of the column is further from the centre, not
      // at its midpoint: a column is COLUMN_PX wide, and clipping to the middle
      // lets its outer pixel sit a pixel beyond the radius — which on this screen
      // is a pixel into the ring.
      const int32_t left = static_cast<int32_t>(x) - CENTRE_X;
      const int32_t right = static_cast<int32_t>(x + COLUMN_PX - 1) - CENTRE_X;
      const int32_t dx = (left < 0 ? -left : left) > (right < 0 ? -right : right) ? left : right;
      const int32_t inside = static_cast<int32_t>(band->clip_radius) * band->clip_radius - dx * dx;
      if (inside <= 0) {
        bridge_from = -1;  // wholly behind the bezel
        continue;
      }
      const lv_coord_t reach = static_cast<lv_coord_t>(sqrtf(static_cast<float>(inside)));
      if (CENTRE_Y + reach < floor_y) {
        floor_y = CENTRE_Y + reach;
      }
    }

    const float low = band->column[c].min_value - band->drawn_min;
    const float high = band->column[c].max_value - band->drawn_min;
    // y grows downwards, so the larger value gets the smaller y.
    lv_coord_t y_top = coords.y2 - static_cast<lv_coord_t>(high * scale);
    lv_coord_t y_bottom = coords.y2 - static_cast<lv_coord_t>(low * scale);
    if (y_top < coords.y1) {
      y_top = coords.y1;
    }
    if (y_bottom > coords.y2) {
      y_bottom = coords.y2;
    }
    if (y_bottom < y_top) {
      y_bottom = y_top;
    }
    // The curve itself is below the arc here: there is nothing of this column on
    // the glass, and drawing the cap alone would leave a line hanging under the
    // ring with no fill beneath it.
    if (y_top > floor_y) {
      bridge_from = -1;
      continue;
    }
    if (y_bottom > floor_y) {
      y_bottom = floor_y;
    }

    lv_area_t area;
    area.x1 = x;
    area.x2 = x + COLUMN_PX - 1;
    if (area.x2 > coords.x2) {
      area.x2 = coords.x2;
    }

    // The wash: from the column's high down to the baseline, so the band sits on
    // the floor of the chart rather than floating as a detached ribbon.
    area.y1 = y_top;
    area.y2 = floor_y;
    lv_draw_rect(ctx, &fill, &area);

    // The spread between this column's min and max. Only worth drawing when the
    // column actually covers more than the cap will — and not at all on a
    // backdrop, where a second darker shape inside the gradient reads as two
    // overlapping charts rather than as one. A ghosted band is an area chart:
    // one curve, one fill under it.
    if (!ghosted && y_bottom - y_top > EDGE_PX) {
      area.y1 = y_top;
      area.y2 = y_bottom;
      lv_draw_rect(ctx, &spread, &area);
    }

    // The cap along the top, solid and thin, so the outline of the day reads at
    // a glance even where the wash behind it is faint.
    //
    // Stretched to meet the previous column rather than drawn flat at this
    // column's own height. A 2 px cap per 2 px column is fine on a gentle slope,
    // but on a steep one consecutive columns are tens of pixels apart and the
    // line breaks into a row of dashes climbing the screen. Bridging the gap is
    // what makes it a curve.
    area.y1 = y_top;
    area.y2 = y_top + EDGE_PX - 1;
    if (bridge_from >= 0) {
      if (bridge_from < area.y1) {
        area.y1 = bridge_from;
      }
      if (bridge_from > area.y2) {
        area.y2 = bridge_from;
      }
    }
    if (area.y1 < coords.y1) {
      area.y1 = coords.y1;
    }
    if (area.y2 > floor_y) {
      area.y2 = floor_y;
    }
    lv_draw_rect(ctx, &edge, &area);
    bridge_from = y_top;
  }
}

// A centred box blur over the reduced columns.
//
// Applied after the reduction, not before: the ring holds a minute a sample and
// the columns are already the picture, so smoothing here costs a pass over ~150
// values rather than over 1440.
//
// Unknown columns are left unknown and contribute nothing to their neighbours. A
// gap is a gap — bridging one would draw a curve across minutes nobody recorded,
// which is the whole reason history_reduce reports `known` per column.
void smooth_columns(Band* band) {
  const int half = band->smoothing / 2;
  if (half < 1 || band->columns == 0) {
    return;
  }
  // Static rather than on the stack: this runs on the UI task, one band at a
  // time, and 208 columns is 2.5 KB that has no business on a task stack.
  static HistoryColumn source[MAX_COLUMNS];
  for (size_t c = 0; c < band->columns; ++c) {
    source[c] = band->column[c];
  }

  for (size_t c = 0; c < band->columns; ++c) {
    if (!source[c].known) {
      continue;
    }
    float lowest = 0.0f;
    float highest = 0.0f;
    int counted = 0;
    for (int k = -half; k <= half; ++k) {
      const long j = static_cast<long>(c) + k;
      if (j < 0 || j >= static_cast<long>(band->columns) || !source[j].known) {
        continue;
      }
      lowest += source[j].min_value;
      highest += source[j].max_value;
      ++counted;
    }
    band->column[c].min_value = lowest / static_cast<float>(counted);
    band->column[c].max_value = highest / static_cast<float>(counted);
  }
}

}  // namespace

lv_obj_t* chart_band_create(lv_obj_t* parent, HistorySeries series, uint32_t colour) {
  Band* band = nullptr;
  for (size_t i = 0; i < MAX_BANDS; ++i) {
    if (!s_bands[i].used) {
      band = &s_bands[i];
      break;
    }
  }
  if (band == nullptr) {
    LV_LOG_WARN("no free chart band slot");
    return nullptr;
  }

  lv_obj_t* obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  // Not clickable, so a swipe that starts on the band still reaches the tileview
  // underneath and changes screen.
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);

  *band = Band{};
  band->used = true;
  band->obj = obj;
  band->series = series;
  band->colour = colour;
  band->autoscale = true;
  band->intensity = LV_OPA_COVER;
  band->smoothing = 1;
  band->clip_radius = 0;
  band->last_minute = UINT32_MAX;

  lv_obj_set_user_data(obj, band);
  lv_obj_add_event_cb(obj, draw_band, LV_EVENT_DRAW_MAIN_END, nullptr);
  return obj;
}

void chart_band_pause_all(bool paused) {
  if (s_paused == paused) {
    return;
  }
  s_paused = paused;
  // On resume, redraw every band once — the sliding frames drew nothing.
  if (!paused) {
    for (size_t i = 0; i < MAX_BANDS; ++i) {
      if (s_bands[i].used && s_bands[i].obj != nullptr) {
        lv_obj_invalidate(s_bands[i].obj);
      }
    }
  }
}

void chart_band_set_range(lv_obj_t* obj, float min_value, float max_value) {
  Band* band = band_for(obj);
  if (band == nullptr) {
    return;
  }
  band->autoscale = max_value <= min_value;
  band->range_min = min_value;
  band->range_max = max_value;
  band->last_minute = UINT32_MAX;  // the picture changes even if the data has not
}

void chart_band_set_columns(lv_obj_t* obj, const HistoryColumn* cols, size_t n,
                            float drawn_min, float drawn_max) {
  Band* band = band_for(obj);
  if (band == nullptr) {
    return;
  }
  // Externally-supplied columns, for a curve that does not live in the history
  // ring — the solar forecast, whose future half the ring cannot hold. The band
  // is never chart_band_refresh()ed; the screen feeds it this and it draws it.
  if (n > MAX_COLUMNS) {
    n = MAX_COLUMNS;
  }
  for (size_t i = 0; i < n; ++i) {
    band->column[i] = cols[i];
  }
  band->columns = n;
  band->has_data = n > 0;
  band->drawn_min = drawn_min;
  band->drawn_max = (drawn_max - drawn_min < MIN_SPAN) ? drawn_min + MIN_SPAN : drawn_max;
  lv_obj_invalidate(obj);
}

void chart_band_clear(lv_obj_t* obj) {
  Band* band = band_for(obj);
  if (band == nullptr) {
    return;
  }
  band->has_data = false;
  band->columns = 0;
  lv_obj_invalidate(obj);
}

size_t chart_band_column_count(lv_obj_t* obj) {
  if (obj == nullptr) {
    return 0;
  }
  lv_obj_update_layout(obj);
  const lv_coord_t width = lv_obj_get_width(obj);
  const size_t n = width > 0 ? static_cast<size_t>(width / COLUMN_PX) : 0;
  return n > MAX_COLUMNS ? MAX_COLUMNS : n;
}


void chart_band_set_bezel_clip(lv_obj_t* obj, lv_coord_t radius) {
  Band* band = band_for(obj);
  if (band == nullptr) {
    return;
  }
  band->clip_radius = radius < 0 ? 0 : radius;
  lv_obj_invalidate(obj);
}

void chart_band_set_smoothing(lv_obj_t* obj, uint8_t columns) {
  Band* band = band_for(obj);
  if (band == nullptr) {
    return;
  }
  if (columns > MAX_SMOOTHING) {
    columns = MAX_SMOOTHING;
  }
  // Even windows have no centre, so a blur through one shifts the curve half a
  // column sideways. Round up rather than reject.
  band->smoothing = columns < 1 ? 1 : (columns | 1);
  band->last_minute = UINT32_MAX;  // the picture changes even if the data has not
}

void chart_band_set_intensity(lv_obj_t* obj, lv_opa_t intensity) {
  Band* band = band_for(obj);
  if (band == nullptr) {
    return;
  }
  band->intensity = intensity;
  lv_obj_invalidate(obj);
}

void chart_band_set_bipolar(lv_obj_t* obj, bool on, uint32_t colour_neg) {
  Band* band = band_for(obj);
  if (band == nullptr) {
    return;
  }
  band->bipolar = on;
  band->colour_neg = colour_neg;
  band->last_minute = UINT32_MAX;  // the scale becomes symmetric; re-reduce
  lv_obj_invalidate(obj);
}

void chart_band_refresh(lv_obj_t* obj) {
  Band* band = band_for(obj);
  if (band == nullptr) {
    return;
  }
  UiPerfTimer perf(UiPerfSection::ChartRefresh);

  // Whichever day is on screen. A band does not choose: stepping back a day
  // moves every chart at once, so the choice belongs to the view, not to the
  // band. Generation catches bank resets/switches even when two days share a
  // head minute; revision catches Recorder writes behind an unchanged head.
  const HistoryBank bank = history_view();
  const uint32_t head = history_head_minute(bank);
  const uint32_t generation = history_generation(bank) ^ (static_cast<uint32_t>(bank) << 24);
  const uint32_t revision = history_revision(bank);
  if (head == band->last_minute && generation == band->last_generation &&
      revision == band->last_revision) {
    return;
  }

  uint32_t from = 0;
  uint32_t to = 0;
  if (!history_window(bank, &from, &to)) {
    // Nothing recorded yet. Draw nothing at all rather than a flat line along
    // the bottom, which would read as a real day of zero generation.
    band->last_minute = head;
    band->last_generation = generation;
    band->last_revision = revision;
    band->has_data = false;
    band->columns = 0;
    lv_obj_invalidate(obj);
    return;
  }

  // The first refresh can land before LVGL has resolved coordinates, and a width
  // of zero here would otherwise be cached as "done" and never retried.
  lv_obj_update_layout(obj);
  const lv_coord_t width = lv_obj_get_width(obj);
  size_t columns = width > 0 ? static_cast<size_t>(width / COLUMN_PX) : 0;
  if (columns > MAX_COLUMNS) {
    columns = MAX_COLUMNS;
  }
  if (columns == 0) {
    band->has_data = false;
    return;  // deliberately without recording last_minute, so this is retried
  }
  {
    UiPerfTimer reduce_perf(UiPerfSection::HistoryReduce);
    history_reduce(bank, band->series, from, to, band->column, columns);
  }
  band->columns = columns;
  {
    UiPerfTimer smooth_perf(UiPerfSection::SmoothColumns);
    smooth_columns(band);
  }

  float lowest = 0.0f;
  float highest = 0.0f;
  bool any = false;
  for (size_t c = 0; c < columns; ++c) {
    if (!band->column[c].known) {
      continue;
    }
    if (!any) {
      any = true;
      lowest = band->column[c].min_value;
      highest = band->column[c].max_value;
      continue;
    }
    if (band->column[c].min_value < lowest) {
      lowest = band->column[c].min_value;
    }
    if (band->column[c].max_value > highest) {
      highest = band->column[c].max_value;
    }
  }
  band->has_data = any;
  // Do not mark the revision handled until a usable reduction has completed.
  // A Recorder write behind `head` can otherwise be consumed by the cache key
  // without its samples ever reaching the columns.
  band->last_minute = head;
  band->last_generation = generation;
  band->last_revision = revision;

  if (band->bipolar) {
    // Symmetric about zero, so the baseline is the band's centre and a kW of
    // import stands as tall above it as a kW of export hangs below.
    float m = highest > 0.0f ? highest : 0.0f;
    if (-lowest > m) {
      m = -lowest;
    }
    band->drawn_min = -m;
    band->drawn_max = m;
  } else if (band->autoscale) {
    // Anchored at zero for a series that never goes negative, so the height of
    // the curve stays proportional to the reading rather than to its spread —
    // a quiet day should look quiet, not be stretched to fill the band.
    band->drawn_min = lowest < 0.0f ? lowest : 0.0f;
    band->drawn_max = highest;
  } else {
    band->drawn_min = band->range_min;
    band->drawn_max = band->range_max;
  }
  if (band->drawn_max - band->drawn_min < MIN_SPAN) {
    band->drawn_max = band->drawn_min + MIN_SPAN;
  }

  lv_obj_invalidate(obj);
}
