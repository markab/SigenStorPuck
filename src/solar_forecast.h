// PV generation forecast from hourly irradiance (docs/PLAN.md §D4).
//
// A second implementation of a model the server already has, which is normally
// exactly what CLAUDE.md forbids — so the reason is worth stating. On the Modbus
// source there is no server to ask, and the forecast is the only thing screen 3
// cannot fill from the plant. The choice was to reimplement ~60 lines of trig or
// leave a screen permanently showing four dashes.
//
// It is deliberately a *port* of app/solar.py rather than a fresh derivation:
// same solar-position formulae, same plane-of-array transposition, same slot
// integration, same 30-minute slots the server's summary block uses. Two figures
// for one house that disagree would be worse than one figure that is missing, so
// where the two differ it should be because of the inputs, not the arithmetic.
//
// The one deliberate difference: the server blends ERA5 reanalysis for hours that
// have already elapsed with forecast for the rest, and this uses the forecast for
// the whole day. Reanalysis is closer to what the weather actually did, so the
// server's figure for a past hour is the better one — but it costs a second HTTP
// request, and on a device whose screen shows one day at a time it is not worth
// it. Expect small disagreements on an unsettled afternoon.
//
// Lives outside src/device/ so the simulator's selftest compiles it: this is the
// part that is arithmetic rather than sockets, and arithmetic is what is worth
// testing.

#pragma once

#include <stddef.h>
#include <stdint.h>

// Four is enough for any domestic roof with room to spare, and keeps the settings
// page a fixed shape rather than a list that grows.
static constexpr size_t SOLAR_MAX_ARRAYS = 4;

// Half-hourly, matching the server's own summary block so the two agree on the
// reduction as well as the model.
static constexpr int SOLAR_SLOT_MINUTES = 30;
static constexpr size_t SOLAR_SLOTS_PER_DAY = 1440 / SOLAR_SLOT_MINUTES;

struct PvArray {
  float kwp = 0.0f;           // peak DC rating
  float tilt_deg = 35.0f;     // from horizontal
  float azimuth_deg = 180.0f; // clockwise from north; 180 is due south
};

struct SolarSite {
  float latitude = 0.0f;
  float longitude = 0.0f;
  // AC derate: inverter efficiency, wiring, soiling, everything between the
  // panel's rating and the meter. The server's default, and the same field.
  float system_loss = 0.85f;
  // Clips the *combined* per-slot throughput. 0 disables it. Not an AC-only cap:
  // on a DC-coupled inverter solar reaches the battery over DC-DC as well.
  float inverter_cap_kw = 0.0f;
  PvArray array[SOLAR_MAX_ARRAYS];
  size_t array_count = 0;
};

// One hour of irradiance, as Open-Meteo reports it.
struct SolarHour {
  float direct_normal = 0.0f;   // W/m2 normal to the beam
  float diffuse = 0.0f;         // W/m2 on the horizontal
};

// Sun elevation and azimuth in degrees at a UTC instant. Azimuth is clockwise
// from north, so 180 is due south — the same convention the array config uses.
void solar_position(uint32_t epoch_utc, float latitude, float longitude, float* elevation_deg,
                    float* azimuth_deg);

// Cosine of the angle between the sun and a panel's normal; 0 when the sun is
// behind the panel or below the horizon.
float solar_poa_factor(float elevation_deg, float azimuth_deg, float tilt_deg,
                       float panel_azimuth_deg);

// Expected generation for one local day, in kWh per SOLAR_SLOT_MINUTES slot.
//
// `hours` is hourly irradiance starting at `first_hour_epoch`; hours the array
// does not cover contribute nothing, which is what an incomplete fetch should do.
// Writes SOLAR_SLOTS_PER_DAY values.
void solar_forecast_day(const SolarSite& site, uint32_t day_start_epoch, const SolarHour* hours,
                        size_t hour_count, uint32_t first_hour_epoch, float* out_slot_kwh);

// The four figures screen 3 shows, reduced from a day of slots exactly as the
// server reduces its own — see _solar_block() in app/summary.py.
struct SolarSummary {
  float forecast_kwh = 0.0f;
  float remaining_kwh = 0.0f;
  float peak_kw = 0.0f;
};
SolarSummary solar_summarise(const float* slot_kwh, uint32_t day_start_epoch, uint32_t now_epoch);
