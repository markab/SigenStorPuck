#pragma once

#include <Arduino.h>

#include "fetch_result.h"
#include "home_assistant.h"
#include "snapshot.h"

// One POST /api/template using the stored semantic mappings. The output is only
// committed after the rendered compact payload parses and at least one mapped
// state is usable, preserving the caller's last good Snapshot on failure.
FetchResult home_assistant_api_fetch(Snapshot* out, int* status_code,
                                     HaParseInfo* parse_info = nullptr);

// One optional boot-time Recorder request, filtered to only the mapped entities
// used by the existing chart curves. Requires a preceding successful live fetch
// so historical states can reuse its units and HA-local midnight boundaries.
FetchResult home_assistant_api_fetch_history(uint32_t cutoff_ts, int* status_code,
                                             size_t* points_written = nullptr);
