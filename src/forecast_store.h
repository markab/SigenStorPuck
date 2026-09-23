// Today's PV forecast as a per-slot curve, kept apart from the history ring (which
// cannot hold the future half of a day — history_put advances the ring's head, so
// a future minute would strand the live recording). The solar screen draws it as
// the "forecast ahead" behind the actual generation. Fed by the server
// (/api/day/series forecast_kw) or the Puck's native forecast on the Modbus path.
//
// Outside src/device/ so the simulator compiles it, same trick as history.{h,cpp}.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "history.h"  // HistoryColumn

// Sets the forecast: `slot_kwh` is per-slot energy for one local day starting at
// `day_start_minute` (local midnight, in unix-minutes, the same index space the
// history ring uses), `slot_minutes` the slot length. Stored internally as kW so
// it charts on the same axis as the actual PV. Passing an empty array clears it.
void forecast_store_set(const float* slot_kwh, size_t slots, uint32_t day_start_minute,
                        uint32_t slot_minutes);
void forecast_store_clear();

// Fills `columns` reduced columns for the window [from_minute, to_minute) by
// sampling the forecast curve — but only when that window is the forecast's own
// day, so a past day shows no forecast. Returns false (touching nothing)
// otherwise. `out_peak`, when non-null, receives the highest kW across the
// window, for aligning the band's scale with the actual chart.
bool forecast_store_columns(uint32_t from_minute, uint32_t to_minute, HistoryColumn* out,
                            size_t columns, float* out_peak);
