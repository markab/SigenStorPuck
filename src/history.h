// A day of readings, kept in RAM so the screens can draw a curve behind the
// number (docs/PLAN.md §D3).
//
// Deliberately not persisted. NVS is a key-value store that a per-minute blob
// write would wear out, and a filesystem partition is real complexity for a
// mains-powered device that rarely restarts. A reboot starts the chart empty and
// it fills over the following hours.
//
// Lives outside src/device/ on purpose: the simulator compiles this file, which
// is the only way chart work can happen on the desktop as CLAUDE.md requires.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "snapshot.h"

// One minute per sample. Finer than the ~4.5 minutes a pixel column covers on a
// 466 px panel, which is the point: the extra resolution is what the min/max
// envelope needs to show PV under broken cloud as a band rather than a line.
static constexpr uint32_t HISTORY_MINUTES = 1440;  // 24 h

// Samples are int16 scaled by this, so one series costs 2.9 KB. The range is
// then +-327.67, comfortably past any domestic kW figure and any percentage.
static constexpr int32_t HISTORY_SCALE = 100;

enum class HistorySeries : uint8_t {
  Pv,   // kW from solar
  Soc,  // battery state of charge, %
  Count,
};

// Discards everything. Called when the clock jumps backwards, which would
// otherwise interleave old and new samples in the ring.
void history_reset();

// Files one reading. Uses the snapshot's own `ts`, so a series is timestamped by
// the plant rather than by however long the Puck has been awake.
//
// Ignored when the snapshot is invalid or carries no timestamp — there is
// nowhere to put a sample with no time on it.
//
// Within a minute the last sample wins rather than being averaged. At a 5 s poll
// that discards eleven of every twelve readings, which sounds worse than it is:
// the draw pass reduces ~4.5 minutes into each pixel column anyway.
void history_record(const Snapshot& snapshot);

// Absolute minute (unix seconds / 60) of the newest sample, or 0 when empty.
uint32_t history_head_minute();

// Bumped every time the ring is cleared. A cache keyed only on the newest minute
// would miss a reset that happened to land on the same minute — which is exactly
// what stepping between fixtures in the simulator does, since they share a
// timestamp, and what a clock correction could do on a device.
uint32_t history_generation();

// How many minutes of the window actually hold a sample. Screens use this to
// tell "nothing recorded yet" from "recorded, and it was zero" — a chart that
// draws a flat line along the bottom before any data arrives is a lie.
size_t history_sample_count(HistorySeries series);

// One pixel column's worth of reduced samples.
struct HistoryColumn {
  bool known = false;
  float min_value = 0.0f;
  float max_value = 0.0f;
};

// Reduces [from_minute, to_minute) onto `columns` output columns, each carrying
// the min and max of the samples that fall in it.
//
// Min/max rather than an average: averaging four minutes of a cloud-broken solar
// curve flattens exactly the detail worth seeing, and the envelope between the
// two is what gives the band its shape.
//
// Columns with no samples come back with known = false, so a gap renders as a
// gap instead of being bridged.
void history_reduce(HistorySeries series, uint32_t from_minute, uint32_t to_minute,
                    HistoryColumn* out, size_t columns);

// Minutes east of UTC, for anchoring the window to local midnight. Taken from
// the snapshot when it carries one; call this directly only to feed a series by
// hand, as the simulator does.
void history_set_timezone(int32_t minutes_east);

// The window a chart should draw.
//
// With a known timezone this is the **whole local day**, midnight to midnight,
// so the x axis means the same thing all day and the curve grows left to right
// into an empty right-hand side. Anchoring to midnight but ending at "now" would
// stretch the morning across the full width and then squash it as the day went
// on, which makes two glances an hour apart hard to compare.
//
// With no timezone it falls back to a rolling 24 h window ending at the newest
// sample. That spans two part-days — yesterday's afternoon, the night, then
// today — which is honest but reads as a U. Anchored is much the better picture;
// see the note on Snapshot::tz_offset_min for why we may not get one.
//
// Returns false when there is nothing recorded yet.
bool history_window(uint32_t* from_minute, uint32_t* to_minute);

// Feeds a series directly, for the simulator's synthetic day (§D3) and for tests.
// `minute` is absolute, matching history_head_minute().
void history_put(HistorySeries series, uint32_t minute, float value);
