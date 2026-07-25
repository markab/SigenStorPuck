// Screen 3: today's energy totals (docs/PLAN.md §B4).

#pragma once

#include <lvgl.h>

#include "snapshot.h"

lv_obj_t* screen_today_create(lv_obj_t* parent);
void screen_today_update(const Snapshot& snapshot);
