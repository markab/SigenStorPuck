// The overnight screen-off window: when "now" falls inside it, and what "now"
// even is on a device whose clock runs on UTC.
//
// Two functions and about twenty lines, in a file of their own outside
// `src/device/` for the same reason `button_gesture.cpp` is: both of them are
// arithmetic that is easy to get subtly wrong and impossible to check by looking
// at the glass. A window that wraps midnight is the normal case here, not the
// edge case, and an offset west of Greenwich makes the local minute go negative
// before it is reduced. Neither shows up until 2 am in the wrong timezone.

#pragma once

#include <stdint.h>

static constexpr uint16_t SCREEN_MINUTES_PER_DAY = 1440;

// Minutes since local midnight, 0..1439, for a UTC epoch and an offset east of
// UTC in minutes. Negative offsets and epochs near midnight both wrap correctly.
uint16_t screen_local_minute(uint32_t epoch_utc, int32_t tz_offset_min);

// Whether `now` falls in the half-open window [start, end).
//
// `start > end` is a window that wraps midnight — 22:30 to 07:00 — which is what
// anyone setting this actually wants, so it is the case that has to be right
// rather than the one that is tolerated. `start == end` is an empty window, not
// a whole day: the settings page uses blank fields to switch the feature off, so
// a pair of equal times can only be a mistake, and blanking the screen for
// twenty-four hours is the worst possible reading of one.
bool screen_window_contains(uint16_t start_min, uint16_t end_min, uint16_t now_min);
