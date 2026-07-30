// The parsed form of one GET /api/summary payload.
//
// This is the whole contract between the server and the screens: fetching fills
// a Snapshot, rendering reads one. Nothing downstream of here knows about JSON,
// and nothing here computes anything — the server already did (see CLAUDE.md).

#pragma once

#include <stddef.h>
#include <stdint.h>

// A number the server may report as null. Unread Modbus registers come back as
// null rather than 0.0, and "unknown" must never render as "0.0 kW" — so the
// two states are kept distinguishable all the way to the screen.
struct MaybeFloat {
  bool known = false;
  float value = 0.0f;
};

struct MaybeInt {
  bool known = false;
  int32_t value = 0;
};

struct TariffSlot {
  uint32_t from = 0;      // unix seconds, when this rate starts
  float pence = 0.0f;     // unit rate in p/kWh
};

// The server sends a handful of upcoming slots; more than this is not useful on
// a 466 px screen, so extras are dropped at parse time.
static constexpr size_t SNAPSHOT_MAX_TARIFF_SLOTS = 4;

struct Snapshot {
  // False until a payload has parsed. Screens must not read anything below
  // this while it is false.
  bool valid = false;

  int32_t version = 0;   // payload's "v" — schema version, for forward compat
  uint32_t ts = 0;       // server's unix timestamp
  bool ok = false;       // server's own health flag
  uint32_t age_s = 0;    // seconds since the plant reading was taken
  int32_t alarms = 0;    // count of active alarms

  // Minutes east of UTC. Optional, and absent from /api/summary today — the
  // server knows the timezone but does not send it (see the tariff-slot note in
  // CLAUDE.md). Parsed anyway so it starts working the day it is added, and
  // filled from plant_system_timezone (30002) on the Modbus path.
  //
  // What needs it: anchoring the day charts to local midnight. Without it they
  // fall back to a rolling 24 h window, which spans two part-days.
  MaybeInt tz_offset_min;

  struct Power {
    MaybeFloat pv;     // kW from solar
    MaybeFloat grid;   // kW, >0 importing, <0 exporting
    MaybeFloat batt;   // kW, >0 charging, <0 discharging
    MaybeFloat home;   // kW house load — derived server-side, not a register
    MaybeFloat ev;     // kW into the EV charger
    // kW total active power of the plant, straight from plant_active_power.
    // Unknown until the server adds it to /api/summary — deliberately not
    // derived here from pv - batt, which would disagree with the register
    // whenever losses, a DC charger or a second inverter are in play.
    MaybeFloat plant;
    bool off_grid = false;
  };
  Power power;

  struct Battery {
    MaybeFloat soc_pct;
    MaybeFloat soh_pct;
    MaybeFloat capacity_kwh;
    MaybeFloat temp_c;
    MaybeInt eta_min;  // to full when charging, to empty when discharging
  };
  Battery battery;

  // "today" arrives as null, not an object, when the day cannot be read.
  // Check present before touching the totals.
  struct Today {
    bool present = false;
    MaybeFloat solar;
    MaybeFloat imported;
    MaybeFloat exported;
    MaybeFloat load;
    MaybeFloat charge;
    MaybeFloat discharge;

    // The day's source->sink split, as the server's Sankey decomposes it.
    //
    // Sent rather than derived because it cannot be derived: the six totals
    // above are six equations in these seven flows, and the one thing they
    // cannot pin down is how imports divide between serving load and charging
    // the battery. Assuming the battery never charges from the grid would be
    // wrong on exactly the days a scheduler makes it do so.
    //
    // Unknown throughout on the Modbus source — the plant exposes daily
    // counters, not a decomposition — which is why the flows screen is one of
    // the two that source does not get.
    struct Flows {
      MaybeFloat solar_load;
      MaybeFloat solar_batt;
      MaybeFloat solar_grid;
      MaybeFloat grid_load;
      MaybeFloat grid_batt;
      MaybeFloat batt_load;
      MaybeFloat batt_grid;
    };
    Flows flows;
  };
  Today today;

  // Today's PV forecast, from the server's Open-Meteo model.
  //
  // No `generated` field: that is `today.solar`, which the Modbus path fills
  // from the pv_daily registers. One number that cannot disagree with itself,
  // and the screen's headline figure works on both data sources.
  //
  // `configured` is false when the server has no location or no array with a
  // kWp set, and on the Modbus source there is no server to ask — so it is also
  // false for the whole of §D. Same shape as Cost, for the same reason: a client
  // should prompt rather than render a misleading zero.
  struct Solar {
    bool configured = false;
    MaybeFloat forecast_kwh;    // the whole of today
    MaybeFloat remaining_kwh;   // still to come between now and dusk
    MaybeFloat vs_forecast_pct; // actual against forecast-so-far; >100 is ahead
    MaybeFloat peak_kw;         // highest forecast slot today
  };
  Solar solar;

  struct Cost {
    bool configured = false;  // false when no tariff is set on the server
    MaybeFloat saving_gbp;
    MaybeFloat rate_p;
    TariffSlot next[SNAPSHOT_MAX_TARIFF_SLOTS];
    size_t next_count = 0;
  };
  Cost cost;
};

// Parses an /api/summary payload.
//
// Returns false and leaves *out completely untouched when the JSON is
// malformed, so one bad response can never blank out a good previous reading —
// the caller can keep rendering what it already had.
//
// An unrecognised "v" is not an error: the payload is parsed as best it can be
// and the version recorded, so a newer server does not brick a fielded device.
bool snapshot_parse(const char* json, size_t length, Snapshot* out);
