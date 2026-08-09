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

// One bank per day being held. The ring indexes by minute modulo a day and keeps
// a single head, so it can only ever describe one 24-hour window: feeding it a
// minute more than a day behind the head is indistinguishable from a clock jump,
// and advance_to() rightly wipes it. Two banks is what lets today keep running
// while a past day is on screen.
struct Bank {
  Ring ring[SERIES_COUNT];

  // Absolute minute of the newest sample in any series. Shared across the
  // series, because they are fed from the same snapshot and so advance together;
  // one head is what lets a slot's absolute minute be derived rather than
  // stored, which would otherwise triple the cost of the ring.
  uint32_t head = 0;
  bool any = false;

  // Minutes east of UTC. Kept here rather than asked for per call because every
  // chart wants the same day boundary.
  int32_t tz_minutes = 0;
  bool tz_known = false;

  uint32_t generation = 0;
  bool initialised = false;
};

Bank s_bank[static_cast<size_t>(HistoryBank::Count)];

// Which bank the charts read. Writing goes wherever the caller says; reading is
// a property of what is on screen, so it is held here rather than threaded
// through every band.
HistoryBank s_view = HistoryBank::Live;

Bank& bank_of(HistoryBank which) {
  const size_t index = static_cast<size_t>(which);
  return s_bank[index < static_cast<size_t>(HistoryBank::Count) ? index : 0];
}

void ensure_initialised(HistoryBank which) {
  if (bank_of(which).initialised) {
    return;
  }
  history_reset(which);
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
void clear_span(Bank& bank, uint32_t after, uint32_t to) {
  const uint32_t span = to - after;
  if (span >= HISTORY_MINUTES) {
    for (size_t s = 0; s < SERIES_COUNT; ++s) {
      for (uint32_t i = 0; i < HISTORY_MINUTES; ++i) {
        bank.ring[s].sample[i] = EMPTY;
      }
    }
    return;
  }
  for (uint32_t minute = after + 1; minute <= to; ++minute) {
    const uint32_t slot = minute % HISTORY_MINUTES;
    for (size_t s = 0; s < SERIES_COUNT; ++s) {
      bank.ring[s].sample[slot] = EMPTY;
    }
  }
}

// Moves the head to `minute`, blanking anything skipped.
void advance_to(HistoryBank which, uint32_t minute) {
  Bank& bank = bank_of(which);
  if (!bank.any) {
    bank.head = minute;
    bank.any = true;
    return;
  }
  if (minute > bank.head) {
    clear_span(bank, bank.head, minute);
    bank.head = minute;
    return;
  }
  // Same minute, or a slightly late sample still inside the window: fine.
  if (bank.head - minute < HISTORY_MINUTES) {
    return;
  }
  // A jump backwards of more than a day. An NTP correction, a plant clock change
  // or — on the day bank — simply a different day being loaded; either way the
  // ring can no longer be indexed consistently, so start again rather than
  // interleave two eras of samples.
  history_reset(which);
  bank.head = minute;
  bank.any = true;
}

}  // namespace

void history_reset(HistoryBank which) {
  Bank& bank = bank_of(which);
  for (size_t s = 0; s < SERIES_COUNT; ++s) {
    for (uint32_t i = 0; i < HISTORY_MINUTES; ++i) {
      bank.ring[s].sample[i] = EMPTY;
    }
  }
  bank.head = 0;
  bank.any = false;
  bank.initialised = true;
  ++bank.generation;
  // The timezone deliberately survives. A reset means the clock jumped or we are
  // starting up; neither says anything about which timezone we are in, and
  // dropping it would put the charts back on the rolling window for no reason.
}

void history_put(HistoryBank which, HistorySeries series, uint32_t minute, float value) {
  ensure_initialised(which);
  const size_t index = static_cast<size_t>(series);
  if (index >= SERIES_COUNT || minute == 0) {
    return;
  }
  advance_to(which, minute);
  bank_of(which).ring[index].sample[minute % HISTORY_MINUTES] = encode(value);
}

void history_record(const Snapshot& snapshot) {
  // Always the live bank: this is the reading that just arrived, and the day bank
  // holds a day that has already finished.
  ensure_initialised(HistoryBank::Live);
  Bank& bank = bank_of(HistoryBank::Live);
  if (!snapshot.valid || snapshot.ts == 0) {
    return;
  }
  if (snapshot.tz_offset_min.known) {
    history_set_timezone(HistoryBank::Live, snapshot.tz_offset_min.value);
  }
  const uint32_t minute = snapshot.ts / 60;
  if (minute == 0) {
    return;
  }

  // Advance once for the whole snapshot, so a series that happens to be unknown
  // this cycle still gets its slot blanked rather than keeping yesterday's value.
  advance_to(HistoryBank::Live, minute);
  const uint32_t slot = minute % HISTORY_MINUTES;

  if (snapshot.power.pv.known) {
    bank.ring[static_cast<size_t>(HistorySeries::Pv)].sample[slot] = encode(snapshot.power.pv.value);
  }
  if (snapshot.battery.soc_pct.known) {
    bank.ring[static_cast<size_t>(HistorySeries::Soc)].sample[slot] =
        encode(snapshot.battery.soc_pct.value);
  }
  // Consumption is the house and the car together, to match today.load. `home`
  // already has EV taken out of it server-side, so the two are added back here
  // rather than either standing alone. An unread `home` leaves the slot empty:
  // the car charging on its own is not the house's consumption.
  if (snapshot.power.home.known) {
    const float ev = snapshot.power.ev.known ? snapshot.power.ev.value : 0.0f;
    bank.ring[static_cast<size_t>(HistorySeries::Load)].sample[slot] =
        encode(snapshot.power.home.value + ev);
  }
}

uint32_t history_head_minute(HistoryBank which) {
  const Bank& bank = bank_of(which);
  return bank.any ? bank.head : 0;
}

uint32_t history_generation(HistoryBank which) {
  return bank_of(which).generation;
}

size_t history_sample_count(HistoryBank which, HistorySeries series) {
  ensure_initialised(which);
  const size_t index = static_cast<size_t>(series);
  if (index >= SERIES_COUNT) {
    return 0;
  }
  const Bank& bank = bank_of(which);
  size_t count = 0;
  for (uint32_t i = 0; i < HISTORY_MINUTES; ++i) {
    if (bank.ring[index].sample[i] != EMPTY) {
      ++count;
    }
  }
  return count;
}

void history_set_timezone(HistoryBank which, int32_t minutes_east) {
  Bank& bank = bank_of(which);
  bank.tz_minutes = minutes_east;
  bank.tz_known = true;
}

void history_set_view(HistoryBank which) {
  s_view = which;
}

HistoryBank history_view() {
  return s_view;
}

bool history_window(HistoryBank which, uint32_t* from_minute, uint32_t* to_minute) {
  const Bank& bank = bank_of(which);
  if (!bank.any || from_minute == nullptr || to_minute == nullptr) {
    return false;
  }
  if (bank.tz_known) {
    // Local midnight, expressed back in the UTC minutes the ring is indexed by.
    const int64_t local = static_cast<int64_t>(bank.head) + bank.tz_minutes;
    const int64_t midnight_local = (local / HISTORY_MINUTES) * HISTORY_MINUTES;
    const int64_t midnight = midnight_local - bank.tz_minutes;
    if (midnight >= 0) {
      *from_minute = static_cast<uint32_t>(midnight);
      *to_minute = static_cast<uint32_t>(midnight) + HISTORY_MINUTES;
      return true;
    }
  }
  *to_minute = bank.head + 1;  // half-open, so the newest sample is included
  *from_minute = (bank.head >= HISTORY_MINUTES - 1) ? bank.head + 1 - HISTORY_MINUTES : 0;
  return true;
}

void history_reduce(HistoryBank which, HistorySeries series, uint32_t from_minute,
                    uint32_t to_minute, HistoryColumn* out, size_t columns) {
  ensure_initialised(which);
  const Bank& bank = bank_of(which);
  if (out == nullptr || columns == 0) {
    return;
  }
  for (size_t c = 0; c < columns; ++c) {
    out[c] = HistoryColumn{};
  }
  const size_t index = static_cast<size_t>(series);
  if (index >= SERIES_COUNT || !bank.any || to_minute <= from_minute) {
    return;
  }

  const uint32_t span = to_minute - from_minute;
  for (uint32_t minute = from_minute; minute < to_minute; ++minute) {
    // Outside the ring's reach: older than a full day, or ahead of the head.
    if (minute > bank.head || bank.head - minute >= HISTORY_MINUTES) {
      continue;
    }
    const int16_t stored = bank.ring[index].sample[minute % HISTORY_MINUTES];
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
