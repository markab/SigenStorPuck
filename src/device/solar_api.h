// Today's PV forecast, fetched straight from Open-Meteo (docs/PLAN.md §D4).
//
// Only used on the Modbus source. With a server in the path the forecast arrives
// already reduced in /api/summary's `solar` block, computed by the model that
// also feeds the dashboard — one house, one number. This exists because a plant
// read directly has no such block, and screen 3's ring would otherwise be hidden
// for the whole of §D.
//
// The fetch is deliberately small: one request an hour, ~2 KB, from the same
// free endpoint the server uses. Open-Meteo's `timeformat=unixtime` means no
// ISO-8601 parsing, and `timezone=auto` makes it answer with the offset for the
// coordinates — which is worth as much as the irradiance is, because the Puck
// itself runs on UTC (net.cpp sets no timezone) and plant_system_timezone
// reads 0 on this plant even in summer. That offset is the only thing on the
// Modbus path that can anchor the day charts to a real local midnight.

#pragma once

#include "sigen_api.h"
#include "snapshot.h"

// Fetches if one is due — first call, an hour elapsed, or the local day has
// rolled over. Cheap and returns FetchResult::Ok immediately when it is not.
//
// Call from the poll task, between polls: this is a TLS session, and two of
// those overlapping is what exhausted the heap once already.
FetchResult solar_api_service();

// Writes the cached forecast into a snapshot's `solar` block, and the fetched
// timezone into `tz_offset_min`.
//
// The timezone overrides whatever the plant reported. That is not a preference
// between two sources of the same fact — plant_system_timezone is a value
// somebody set in the Sigen app and may never have touched, whereas this one is
// derived from the coordinates and follows daylight saving on its own.
void solar_api_apply(Snapshot* snapshot);

// True once a forecast has been fetched, for the settings page's status line.
//
// Read from the web server's task while the poll task writes it, deliberately
// without a lock: it is one word, and the worst a torn read can do is put last
// minute's status on a page somebody is about to reload. A mutex here would have
// the settings page able to block the poll loop, which is a real cost for a
// line of text.
bool solar_api_ready();

// The last fetch's outcome, for the same line and with the same caveat.
// FetchResult::NotConfigured until a location and at least one array are set.
FetchResult solar_api_last_result();
