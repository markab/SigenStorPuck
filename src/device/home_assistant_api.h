#pragma once

#include <Arduino.h>

#include "fetch_result.h"
#include "home_assistant.h"
#include "snapshot.h"

// One POST /api/template using the stored semantic mappings. The output is only
// committed after the rendered compact payload parses and at least one mapped
// state is usable, preserving the caller's last good Snapshot on failure.
//
// `latch_day_bounds` false leaves the day boundaries and units that the Recorder
// backfill reuses untouched. The settings page's connection test passes false: it
// runs on the web server's task, and those values belong to the poll task, which
// reads them without a lock between its own fetches.
FetchResult home_assistant_api_fetch(Snapshot* out, int* status_code,
                                     HaParseInfo* parse_info = nullptr,
                                     bool latch_day_bounds = true);

// Returns the DST-aware local-day bounds latched by the successful live fetch.
bool home_assistant_api_history_bounds(uint32_t* local_midnight_ts,
                                       uint32_t* next_local_midnight_ts);

// One optional Recorder window, filtered to only the mapped entities used by
// the chart curves. Requires a preceding successful live fetch so historical
// states can reuse its units and HA-local day boundaries.
FetchResult home_assistant_api_fetch_history(uint32_t window_start_ts,
                                             uint32_t window_end_ts,
                                             int* status_code,
                                             size_t* points_written = nullptr);
