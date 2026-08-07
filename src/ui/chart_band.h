// The faded day curve drawn behind a reading (docs/PLAN.md §D3).
//
// A min/max envelope rather than a polyline: each pixel column covers several
// minutes, and averaging them flattens exactly the detail worth seeing on a
// cloud-broken solar curve. The band between the two is the chart.
//
// Hand-drawn because LVGL 8 has no area fill under a chart line — the same
// reason the five-point star in screen_power.cpp is hand-drawn.

#pragma once

#include <lvgl.h>

#include "history.h"

// Creates a band under `parent`. Size and position it with the usual lv_obj
// calls; it draws inside whatever area it is given.
//
// `colour` is the series colour from theme.h — solar yellow for PV, battery
// green for SoC. The fill is drawn faint and the upper edge solid, so the shape
// reads without competing with the number in front of it.
lv_obj_t* chart_band_create(lv_obj_t* parent, HistorySeries series, uint32_t colour);

// Fixes the vertical range. Pass max <= min to scale to whatever the window
// holds, which is what PV wants; SoC wants a fixed 0-100 so the curve's height
// means the same thing every time you look at it.
void chart_band_set_range(lv_obj_t* band, float min_value, float max_value);

// How far back to push the band. LV_OPA_COVER is the normal strength, for a band
// drawn in a clear strip of its own; anything lower switches it to backdrop
// rendering, where the fill becomes a gradient running from this strength just
// under the curve down to nothing at the foot, and the cap stays about twice as
// strong so the shape of the day still reads through the text on top.
void chart_band_set_intensity(lv_obj_t* band, lv_opa_t intensity);

// Smooths the reduced curve over `columns` columns, centred; 1 (the default)
// leaves it alone. Even windows are rounded up, because a blur through a window
// with no centre shifts the curve half a column sideways.
//
// Worth having on a backdrop, where the point is the shape of the day rather
// than any individual minute — a raw min/max envelope of PV under broken cloud
// is a picket fence, and a picket fence behind text is just noise.
void chart_band_set_smoothing(lv_obj_t* band, uint8_t columns);

// Recomputes the reduced columns from the history ring.
//
// Cheap to call every poll: it returns immediately unless the newest sample has
// moved to a new minute. At a 5 s poll the curve would otherwise be recomputed
// and the area invalidated twelve times for a shift of a fifth of a pixel, and
// on an AMOLED driven by a 40-line partial buffer that is real work for nothing.
void chart_band_refresh(lv_obj_t* band);
