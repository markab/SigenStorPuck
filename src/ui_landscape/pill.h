// The live rate in a pill: a fully rounded box tinted in the series colour with the
// text inside it, the same shape the round screens use for the same job. The one
// thing on a day screen that changes minute to minute, so it should be findable
// without reading — and set smaller than the headline above it, so it reads as a
// detail of that figure rather than as a second headline.

#pragma once

#include <lvgl.h>

// Creates the pill under `parent`, centred at (dx, dy), hidden until it has text.
lv_obj_t* pill_create(lv_obj_t* parent, uint32_t colour, lv_coord_t dx, lv_coord_t dy);

// Sets the text. Showing and hiding stay with the caller, on the pill itself.
void pill_set_text(lv_obj_t* pill, const char* text);
