// Screen 1: live power flow (docs/PLAN.md §B4).
//
// A radial hub — house load in the middle, one source per cardinal direction,
// battery state of charge as a ring around the bezel. Cardinal points carry the
// power legs; the free diagonals carry status, which is what makes a round
// canvas worth having.

#pragma once

#include <lvgl.h>

#include "snapshot.h"

// Builds the screen under `parent` and returns its root object, so the tileview
// that will hold all four screens owns placement rather than this file.
lv_obj_t* screen_power_create(lv_obj_t* parent);

// Re-renders from a snapshot. Safe to call with an invalid one: every field
// falls back to a dash rather than showing a stale or invented number.
void screen_power_update(const Snapshot& snapshot);
