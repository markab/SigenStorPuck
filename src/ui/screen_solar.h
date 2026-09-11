// Screen 3: the solar day (docs/PLAN.md §B4).
//
// Replaced the old TODAY screen. Its house/import/export totals went with it:
// house load is already live on screen 1, and the grid figures belong with the
// cost they produce.

#pragma once

#include <lvgl.h>

#include "snapshot.h"

// `figures` is the SOLAR_FIGURE_* set the source can supply. It fixes where the
// optional figures sit for the life of the screen: see solar_metric_layout.h.
lv_obj_t* screen_solar_create(lv_obj_t* parent, uint8_t figures);
void screen_solar_update(const Snapshot& snapshot);

// Whether the reading being shown is live. False on a past day, where the live
// PV pill is hidden — the server cannot date it, so it would be right now's
// output under a past date.
void screen_solar_set_live(bool live);
