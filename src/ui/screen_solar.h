// Screen 3: the solar day (docs/PLAN.md §B4).
//
// Replaced the old TODAY screen. Its house/import/export totals went with it:
// house load is already live on screen 1, and the grid figures belong with the
// cost they produce.

#pragma once

#include <lvgl.h>

#include "snapshot.h"

lv_obj_t* screen_solar_create(lv_obj_t* parent);
void screen_solar_update(const Snapshot& snapshot);
