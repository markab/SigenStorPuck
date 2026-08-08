// Fetching GET /api/summary from a SigenStor Display server.

#pragma once

#include <Arduino.h>

#include "fetch_result.h"
#include "history.h"
#include "snapshot.h"

// Fetches and parses one snapshot. On any failure `out` is left untouched, so the
// caller keeps rendering the last good reading.
//
// `status_code` receives the HTTP status when there was one, or 0.
FetchResult sigen_api_fetch(Snapshot* out, int* status_code);

// Fetches a day's curves and files them straight into the history ring, so a
// chart shows the whole day rather than only the part this device was awake for.
//
// `date` is "YYYY-MM-DD", or empty for today. `bank` says which ring it lands
// in: today's poll keeps filling Live while a past day sits in Day, because one
// ring cannot hold both (see history.h).
//
// Server source only — a plant exposes daily counters, not a day's shape — and
// far less often than the summary poll: it is ~3.5 KB against ~500 bytes, and a
// curve that already happened does not change.
FetchResult sigen_api_fetch_day(HistoryBank bank, const String& date, int* status_code);

// One dated summary, for a day the buttons have stepped back to.
//
// Only `today`, `cost` and `solar` follow the date. `power`, `battery` and
// `alarms` are built from live registers and are always now, whatever date is
// asked for — the server has no history of them to serve.
FetchResult sigen_api_fetch_dated(const String& date, Snapshot* out, int* status_code);
