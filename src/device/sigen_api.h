// Fetching GET /api/summary from a SigenStor Display server.

#pragma once

#include <Arduino.h>

#include "snapshot.h"

// Why a fetch failed. The distinctions matter because they need different words
// on screen: a network fault will fix itself, a revoked token never will, and a
// certificate error caused by a wrong clock is not a certificate problem.
enum class FetchResult {
  Ok,
  NotConfigured,   // no server URL or token stored yet
  NoNetwork,       // WiFi down
  ConnectFailed,   // could not reach the host
  TlsFailed,       // handshake refused — a real certificate problem
  ClockUnset,      // TLS cannot be validated because the clock is wrong
  Unauthorised,    // 401/403: the kiosk token has been revoked, re-enrol needed
  HttpError,       // any other HTTP status
  BadPayload,      // 200 but the JSON did not parse
};

const char* fetch_result_name(FetchResult result);

// Fetches and parses one snapshot. On any failure `out` is left untouched, so the
// caller keeps rendering the last good reading.
//
// `status_code` receives the HTTP status when there was one, or 0.
FetchResult sigen_api_fetch(Snapshot* out, int* status_code);
