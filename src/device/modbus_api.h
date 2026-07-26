// Reading a Snapshot straight off the plant over Modbus TCP (docs/PLAN.md §D2).
//
// The alternative to sigen_api.cpp, selected at boot by the `source` setting.
// No TLS, no token, LAN only: this variant cannot work over the VPS, and drops
// the certificate bundle and the enrolment flow entirely.
//
// Read-only by construction. Only the two read function codes are implemented,
// and there is no path here that writes a register — a status display has no
// business changing a plant's settings.

#pragma once

#include <Arduino.h>

#include "fetch_result.h"
#include "snapshot.h"

// Fetches and parses one snapshot. On failure `out` is left untouched, so the
// caller keeps rendering the last good reading.
//
// `detail` receives the Modbus exception code when the plant returned one, or 0.
FetchResult modbus_api_fetch(Snapshot* out, int* detail);

// Drops the cached slow-cycle readings and the midnight baseline. Call when the
// Modbus settings change, so a new plant does not inherit the old one's day.
void modbus_api_reset();
