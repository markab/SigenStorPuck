// GET /api/day/series -> the history ring (docs/PLAN.md §D3).
//
// The ring is filled a minute at a time from live polls, which means a chart is
// empty for hours after a reboot or an OTA update and only ever holds what this
// device happened to be awake for. The server has the whole day, integrated from
// samples far denser than a 5 s poll, so on that data source it is simply a
// better curve as well as a durable one.
//
// Lives outside src/device/ on purpose, same as enrol_url.cpp and modbus_regs.cpp:
// only the socket is Arduino-only, and the parse is what wants testing.

#pragma once

#include <stddef.h>
#include <stdint.h>

// Files a day payload into the history ring, returning false if it did not parse
// or carried no usable day.
//
// `now_minute` is the current absolute minute (unix seconds / 60) and is a
// deliberate parameter rather than a call to time(): it is what stops the day's
// unelapsed slots being written.
//
// That cut matters. The server reports a slot with no samples as 0.0 kW, not as
// null — for the elapsed day that is correct, but the slots after now are only
// zero because they have not happened. Writing them would draw a flat line along
// the bottom from now to midnight, which is the exact lie history.h's
// sample-count check exists to prevent.
bool day_series_parse(const char* json, size_t length, uint32_t now_minute);
