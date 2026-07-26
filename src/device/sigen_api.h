// Fetching GET /api/summary from a SigenStor Display server.

#pragma once

#include <Arduino.h>

#include "fetch_result.h"
#include "snapshot.h"

// Fetches and parses one snapshot. On any failure `out` is left untouched, so the
// caller keeps rendering the last good reading.
//
// `status_code` receives the HTTP status when there was one, or 0.
FetchResult sigen_api_fetch(Snapshot* out, int* status_code);
