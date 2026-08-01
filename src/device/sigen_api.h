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

// Fetches today's curves and files them straight into the history ring, so a
// chart shows the whole day rather than only the part this device was awake for.
//
// Server source only — a plant exposes daily counters, not a day's shape — and
// far less often than the summary poll: it is ~3.5 KB against ~500 bytes, and a
// curve that already happened does not change.
FetchResult sigen_api_fetch_day(int* status_code);
