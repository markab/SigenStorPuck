// Screen 4: where today's energy came from and went (docs/PLAN.md §B4).
//
// The dashboard draws this as a Sankey. Three sources to three sinks needs
// ribbons 10-30 px wide once labelled, which is not a thing to read on a 466 px
// circle, and LVGL 8 has no ribbon primitive to draw them with. The Sankey is
// really answering two questions — where the solar went, and where the house's
// energy came from — and two stacked bars answer both at a glance.
//
// Server source only: the flows cannot be derived from the six daily totals.
// See Snapshot::Today::Flows.

#pragma once

#include <lvgl.h>

#include "snapshot.h"

lv_obj_t* screen_flows_create(lv_obj_t* parent);
void screen_flows_update(const Snapshot& snapshot);
