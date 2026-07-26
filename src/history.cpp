#include "history.h"

#include <string.h>

namespace {

// No sample here. INT16_MIN rather than 0, because 0 kW at night and 0 % on a
// flat battery are both real readings that must not read as "missing".
constexpr int16_t EMPTY = INT16_MIN;

constexpr size_t SERIES_COUNT = static_cast<size_t>(HistorySeries::Count);

struct Ring {
  int16_t sample[HISTORY_MINUTES];
};

Ring s_ring[SERIES_COUNT];

// Absolute minute of the newest sample in any series. Shared, because every
// series is fed from the same snapshot and so advances together; keeping one
// head is what lets a slot's absolute minute be derived rather than stored,
// which would otherwise triple the cost of the ring.
uint32_t s_head = 0;
bool s_any = false;

// Minutes east of UTC. Kept here rather than asked for per call because every
// chart wants the same day boundary.
int32_t s_tz_minutes = 0;
bool s_tz_known = false;

uint32_t s_generation = 0;

bool s_initialised = false;

void ensure_initialised() {
  if (s_initialised) {
    return;
  }
  history_reset();
}

int16_t encode(float value) {
  const int32_t scaled = static_cast<int32_t>(value * HISTORY_SCALE +
                                              (value >= 0.0f ? 0.5f : -0.5f));
  // Clamped rather than allowed to wrap. A wrapped sample would draw a spike to
  // the opposite rail, which looks like real data and is not.
  if (scaled > INT16_MAX) {
    return INT16_MAX;
  }
  // EMPTY is INT16_MIN, so the usable floor is one above it.
  if (scaled < INT16_MIN + 1) {
    return INT16_MIN + 1;
  }
  return static_cast<int16_t>(scaled);
}

float decode(int16_t stored) {
  return static_cast<float>(stored) / static_cast<float>(HISTORY_SCALE);
}

// Blanks (after, up to and including to) across every series, so minutes the
// Puck was asleep or unreachable stay holes instead of inheriting whatever the
// ring held a day ago.
void clear_span(uint32_t after, uint32_t to) {
  const uint32_t span = to - after;
  if (span >= HISTORY_MINUTES) {
    for (size_t s = 0; s < SERIES_COUNT; ++s) {
      for (uint32_t i = 0; i < HISTORY_MINUTES; ++i) {
        s_ring[s].sample[i] = EMPTY;
      }
    }
    return;
  }
  for (uint32_t minute = after + 1; minute <= to; ++minute) {
    const uint32_t slot = minute % HISTORY_MINUTES;
    for (size_t s = 0; s < SERIES_COUNT; ++s) {
      s_ring[s].sample[slot] = EMPTY;
    }
  }
}

// Moves the head to `minute`, blanking anything skipped.
void advance_to(uint32_t minute) {
  if (!s_any) {
    s_head = minute;
    s_any = true;
    return;
  }
  if (minute > s_head) {
    clear_span(s_head, minute);
    s_head = minute;
    return;
  }
  // Same minute, or a slightly late sample still inside the window: fine.
  if (s_head - minute < HISTORY_MINUTES) {
    return;
  }
  // A jump backwards of more than a day. An NTP correction or a plant clock
  // change; either way the ring can no longer be indexed consistently, so start
  // again rather than interleave two eras of samples.
  history_reset();
  s_head = minute;
  s_any = true;
}

}  // namespace

void history_reset() {
  for (size_t s = 0; s < SERIES_COUNT; ++s) {
    for (uint32_t i = 0; i < HISTORY_MINUTES; ++i) {
      s_ring[s].sample[i] = EMPTY;
    }
  }
  s_head = 0;
  s_any = false;
  s_initialised = true;
  ++s_generation;
  // The timezone deliberately survives. A reset means the clock jumped or we are
  // starting up; neither says anything about which timezone we are in, and
  // dropping it would put the charts back on the rolling window for no reason.
}

void history_put(HistorySeries series, uint32_t minute, float value) {
  ensure_initialised();
  const size_t index = static_cast<size_t>(series);
  if (index >= SERIES_COUNT || minute == 0) {
    return;
  }
  advance_to(minute);
  s_ring[index].sample[minute % HISTORY_MINUTES] = encode(value);
}

void history_record(const Snapshot& snapshot) {
  ensure_initialised();
  if (!snapshot.valid || snapshot.ts == 0) {
    return;
  }
  if (snapshot.tz_offset_min.known) {
    history_set_timezone(snapshot.tz_offset_min.value);
  }
  const uint32_t minute = snapshot.ts / 60;
  if (minute == 0) {
    return;
  }

  // Advance once for the whole snapshot, so a series that happens to be unknown
  // this cycle still gets its slot blanked rather than keeping yesterday's value.
  advance_to(minute);
  const uint32_t slot = minute % HISTORY_MINUTES;

  if (snapshot.power.pv.known) {
    s_ring[static_cast<size_t>(HistorySeries::Pv)].sample[slot] = encode(snapshot.power.pv.value);
  }
  if (snapshot.battery.soc_pct.known) {
    s_ring[static_cast<size_t>(HistorySeries::Soc)].sample[slot] =
        encode(snapshot.battery.soc_pct.value);
  }
}

uint32_t history_head_minute() {
  return s_any ? s_head : 0;
}

uint32_t history_generation() {
  return s_generation;
}

size_t history_sample_count(HistorySeries series) {
  ensure_initialised();
  const size_t index = static_cast<size_t>(series);
  if (index >= SERIES_COUNT) {
    return 0;
  }
  size_t count = 0;
  for (uint32_t i = 0; i < HISTORY_MINUTES; ++i) {
    if (s_ring[index].sample[i] != EMPTY) {
      ++count;
    }
  }
  return count;
}

void history_set_timezone(int32_t minutes_east) {
  s_tz_minutes = minutes_east;
  s_tz_known = true;
}

bool history_window(uint32_t* from_minute, uint32_t* to_minute) {
  if (!s_any || from_minute == nullptr || to_minute == nullptr) {
    return false;
  }
  if (s_tz_known) {
    // Local midnight, expressed back in the UTC minutes the ring is indexed by.
    const int64_t local = static_cast<int64_t>(s_head) + s_tz_minutes;
    const int64_t midnight_local = (local / HISTORY_MINUTES) * HISTORY_MINUTES;
    const int64_t midnight = midnight_local - s_tz_minutes;
    if (midnight >= 0) {
      *from_minute = static_cast<uint32_t>(midnight);
      *to_minute = static_cast<uint32_t>(midnight) + HISTORY_MINUTES;
      return true;
    }
  }
  *to_minute = s_head + 1;  // half-open, so the newest sample is included
  *from_minute = (s_head >= HISTORY_MINUTES - 1) ? s_head + 1 - HISTORY_MINUTES : 0;
  return true;
}

void history_reduce(HistorySeries series, uint32_t from_minute, uint32_t to_minute,
                    HistoryColumn* out, size_t columns) {
  ensure_initialised();
  if (out == nullptr || columns == 0) {
    return;
  }
  for (size_t c = 0; c < columns; ++c) {
    out[c] = HistoryColumn{};
  }
  const size_t index = static_cast<size_t>(series);
  if (index >= SERIES_COUNT || !s_any || to_minute <= from_minute) {
    return;
  }

  const uint32_t span = to_minute - from_minute;
  for (uint32_t minute = from_minute; minute < to_minute; ++minute) {
    // Outside the ring's reach: older than a full day, or ahead of the head.
    if (minute > s_head || s_head - minute >= HISTORY_MINUTES) {
      continue;
    }
    const int16_t stored = s_ring[index].sample[minute % HISTORY_MINUTES];
    if (stored == EMPTY) {
      continue;
    }
    // Integer maths throughout: a float column index rounds inconsistently at
    // the boundaries and drops the odd sample into the wrong column.
    const size_t column = static_cast<size_t>((static_cast<uint64_t>(minute - from_minute) *
                                               columns) / span);
    if (column >= columns) {
      continue;
    }
    const float value = decode(stored);
    if (!out[column].known) {
      out[column].known = true;
      out[column].min_value = value;
      out[column].max_value = value;
    } else if (value < out[column].min_value) {
      out[column].min_value = value;
    } else if (value > out[column].max_value) {
      out[column].max_value = value;
    }
  }
}
