// The day's consumption: what the house and the car used, and when.
//
// The same shape as the battery and solar screens — a day's total as the
// headline, the live rate in a pill, four occasional figures over that day's
// ghosted curve — with one difference: there is no ring. A ring says "how full",
// and consumption has no ceiling to be a fraction of. An unfilled track would
// read as a real zero, which is the same reason the solar screen hides its ring
// when there is no forecast behind it.

#pragma once

#include <lvgl.h>

#include "snapshot.h"

lv_obj_t* screen_load_create(lv_obj_t* parent);
void screen_load_update(const Snapshot& snapshot);

// Whether the reading being shown is live. False on a past day, where the live
// pill is hidden — the server cannot date it, so it would be right now's draw
// under a past date.
void screen_load_set_live(bool live);
