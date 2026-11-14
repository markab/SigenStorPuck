// Grid events: the one that matters most right now.
//
// The live event, else the next booked one, else the most recently finished one
// (events_featured()), as its kind, its day and time window in local time, where
// it stands ("Live · 42 min left", "In 2 h 15 min", "Done", "Deferred"), and its
// key figures. Nothing to show — no events yet, or a server without the endpoint
// — is a quiet "No events".
//
// Not a day screen: the list is not dated by the buttons, so it carries no day
// indicator and stepping the day does nothing here. Server source only (the
// `events` capability). Built for both boards: src/ui/screen_events.cpp is the
// round layout, src/ui_landscape/screen_events.cpp the wide one.

#pragma once

#include <lvgl.h>

#include "events.h"

lv_obj_t* screen_events_create(lv_obj_t* parent);

// `now` is unix seconds (UTC); `tz_offset_min` the site's offset east of UTC.
// Cheap to call repeatedly with an unchanged list: labels are only rewritten
// when their text changes.
void screen_events_update(const EventList& events, uint32_t now, int32_t tz_offset_min);
