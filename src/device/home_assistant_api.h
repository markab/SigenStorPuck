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
