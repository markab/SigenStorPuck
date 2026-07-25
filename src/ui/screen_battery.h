// Screen 2: battery detail (docs/PLAN.md §B4).

#pragma once

#include <lvgl.h>

#include "snapshot.h"

lv_obj_t* screen_battery_create(lv_obj_t* parent);
void screen_battery_update(const Snapshot& snapshot);
