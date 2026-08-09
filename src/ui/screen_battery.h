// Screen 2: battery detail (docs/PLAN.md §B4).

#pragma once

#include <lvgl.h>

#include "snapshot.h"

lv_obj_t* screen_battery_create(lv_obj_t* parent);
void screen_battery_update(const Snapshot& snapshot);

// Whether the reading being shown is live. False on a past day, where the pill
// and the time-to-full line are hidden: the server cannot date them, so they
// would be today's rate and today's estimate sitting under yesterday's date. The
// figures that *are* dated stay.
void screen_battery_set_live(bool live);
