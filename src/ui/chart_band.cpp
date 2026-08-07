#include "chart_band.h"

#include "theme.h"

namespace {

// Two pixels per column. One would be finer than the panel deserves at this
// size and doubles the number of draw calls; anything wider starts to read as a
// bar chart rather than a curve.
constexpr lv_coord_t COLUMN_PX = 2;

// Enough for the full usable width inside PUCK_SAFE_SQUARE at COLUMN_PX.
constexpr size_t MAX_COLUMNS = 208;

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

constexpr size_t MAX_BANDS = 4;

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
};

Band s_bands[MAX_BANDS];

Band* band_for(lv_obj_t* obj) {
  if (obj == nullptr) {
    return nullptr;
  }
  return static_cast<Band*>(lv_obj_get_user_data(obj));
}

void draw_band(lv_event_t* event) {
  lv_obj_t* obj = lv_event_get_target(event);
  Band* band = band_for(obj);
  if (band == nullptr || !band->has_data || band->columns == 0) {
    return;
  }

  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(event);
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);

  const lv_coord_t height = lv_area_get_height(&coords);
  if (height <= 0) {
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

    lv_area_t area;
    area.x1 = x;
    area.x2 = x + COLUMN_PX - 1;
    if (area.x2 > coords.x2) {
      area.x2 = coords.x2;
    }

    // The wash: from the column's high down to the baseline, so the band sits on
    // the floor of the chart rather than floating as a detached ribbon.
    area.y1 = y_top;
    area.y2 = coords.y2;
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
    if (area.y2 > coords.y2) {
      area.y2 = coords.y2;
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
  band->last_minute = UINT32_MAX;

  lv_obj_set_user_data(obj, band);
  lv_obj_add_event_cb(obj, draw_band, LV_EVENT_DRAW_MAIN_END, nullptr);
  return obj;
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

void chart_band_refresh(lv_obj_t* obj) {
  Band* band = band_for(obj);
  if (band == nullptr) {
    return;
  }

  const uint32_t head = history_head_minute();
  const uint32_t generation = history_generation();
  if (head == band->last_minute && generation == band->last_generation) {
    return;
  }
  band->last_generation = generation;

  uint32_t from = 0;
  uint32_t to = 0;
  if (!history_window(&from, &to)) {
    // Nothing recorded yet. Draw nothing at all rather than a flat line along
    // the bottom, which would read as a real day of zero generation.
    band->last_minute = head;
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
  band->last_minute = head;

  history_reduce(band->series, from, to, band->column, columns);
  band->columns = columns;
  smooth_columns(band);

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

  if (band->autoscale) {
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
