// The day at the grid: how much was imported and exported, drawn as a bipolar
// day chart with import above a zero line and export below it, plus the live AC
// figures — grid frequency and phase voltage.
//
// A day screen like battery, solar and load: the totals and the chart follow the
// day you step back to, while the live pill and the AC figures are hidden on a
// past day because they are right-now values the server cannot date.
//
// Frequency and voltage come only from the Modbus source (read from the
// inverter's running-info registers); /api/summary carries neither, so on the
// server source those two stats stay empty and the rest of the screen stands on
// its own. Built for both boards: src/ui/screen_grid.cpp is the round layout,
// src/ui_landscape/screen_grid.cpp the wide one.

#pragma once

#include <lvgl.h>

#include "snapshot.h"

lv_obj_t* screen_grid_create(lv_obj_t* parent);
void screen_grid_update(const Snapshot& snapshot);

// Whether the reading is live. False on a past day, where the live import/export
// pill and the frequency and voltage figures are hidden — they are now's values,
// and the server cannot date them.
void screen_grid_set_live(bool live);
