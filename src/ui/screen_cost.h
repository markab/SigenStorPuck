// Screen 4: cost and tariff (docs/PLAN.md §B4).

#pragma once

#include <lvgl.h>

#include "snapshot.h"

lv_obj_t* screen_cost_create(lv_obj_t* parent);
void screen_cost_update(const Snapshot& snapshot);
