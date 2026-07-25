// Number and time formatting shared by the screens.
//
// The one rule all of it obeys: an unknown value renders as a dash, never as a
// zero. See MaybeFloat in snapshot.h for why.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "snapshot.h"

// "3.42", or "--" when unknown. Magnitude only — screens carry direction in a
// word or an animation rather than a minus sign.
void puck_format_magnitude(const MaybeFloat& value, int decimals, char* out, size_t size);

// "3.42" or "-3.42", keeping the sign. For figures whose sign is the point.
void puck_format_signed(const MaybeFloat& value, int decimals, char* out, size_t size);

// "1h 36m", "45m", or "--" when unknown.
void puck_format_duration(const MaybeInt& minutes, char* out, size_t size);

// A future instant as an offset from now: "now", "in 30m", "in 2h 30m".
//
// Deliberately relative rather than a clock time. An absolute "23:30" needs the
// device's timezone, and it has no correct clock until NTP and the RTC are wired
// up (PLAN.md §B3) — a wrong time presented confidently is worse than a right
// offset. See screen_cost.cpp for the longer-term fix.
void puck_format_offset(int32_t seconds_ahead, char* out, size_t size);
