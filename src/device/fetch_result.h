// Why a poll failed, shared by both data sources (docs/PLAN.md §D1).
//
// One enum rather than one per source, so poller.cpp, the status line and the
// settings page keep working whichever source is configured. Values that can
// only arise on one path are marked as such; nothing has to handle the ones its
// own source cannot produce.
//
// The distinctions matter because they need different words on screen: a
// network fault will fix itself, a revoked token never will, and a certificate
// error caused by a wrong clock is not a certificate problem.

#pragma once

enum class FetchResult {
  Ok,
  NotConfigured,   // no server URL and token, or no Modbus host, stored yet
  NoNetwork,       // WiFi down
  ConnectFailed,   // could not reach the host
  TlsFailed,       // server path: handshake refused — a real certificate problem
  ClockUnset,      // server path: TLS cannot be validated because the clock is wrong
  Unauthorised,    // server path: 401/403, the kiosk token has been revoked
  HttpError,       // server path: any other HTTP status
  BadPayload,      // server path: 200 but the JSON did not parse
  ProtocolError,   // Modbus path: an exception response, or a frame that made no sense
  ReadTimeout,     // Modbus path: connected, but the plant did not answer in time
};

const char* fetch_result_name(FetchResult result);
