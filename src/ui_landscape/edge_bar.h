// A rounded-rectangle progress bar tracing the screen bezel — the landscape
// analogue of the round board's state-of-charge ring. Used for state of charge
// (power, battery), generation-vs-forecast (solar) and self-sufficiency (flows).
//
// LVGL 8 has no perimeter-progress primitive, so this draws the track and the
// filled fraction as two lv_line polylines: the track traces the whole rounded
// rectangle from top-centre clockwise, and the indicator is the prefix of that
// path up to the fraction. Memory-light — no full-screen canvas.

#pragma once

#include <lvgl.h>

// Creates the bar under `parent` (a full-size transparent container) and returns
// it. Call edge_bar_set() to fill it.
lv_obj_t* edge_bar_create(lv_obj_t* parent);

// Sets the filled fraction (0..1, clamped) and the indicator colour.
void edge_bar_set(lv_obj_t* bar, float fraction, uint32_t colour);

// Hides or shows the whole bar (both track and indicator). The solar screen
// hides it when there is no forecast, exactly as the round ring is hidden.
void edge_bar_set_hidden(lv_obj_t* bar, bool hidden);
