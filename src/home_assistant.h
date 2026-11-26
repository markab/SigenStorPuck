#pragma once

#include <stddef.h>
#include <stdint.h>

#include "snapshot.h"

enum class HaEntity : uint8_t {
  PvPower = 0,
  GridPower,
  BatteryPower,
  HomePower,
  EvPower,
  PlantPower,
  OffGrid,
  BatterySoc,
  BatterySoh,
  BatteryCapacity,
  BatteryTemperature,
  TodayPv,
  TodayLoad,
  TodayGridImport,
  TodayGridExport,
  TodayBatteryCharge,
  TodayBatteryDischarge,
  // PV string voltage and current, for the strings' own power (V x I) that the
  // losses under PLANT are measured against; see power_balance.h. Optional, and
  // here rather than after the forecast block because everything from
  // ForecastToday on is treated as forecast-only. NVS keys are by name, so the
  // forecast entities moving along does not disturb a stored mapping.
  Pv1Voltage,
  Pv1Current,
  Pv2Voltage,
  Pv2Current,
  Pv3Voltage,
  Pv3Current,
  Pv4Voltage,
  Pv4Current,
  ForecastToday,
  ForecastRemaining,
  ForecastPercentage,
  ForecastPeak,
  Count,
};

enum class HaValueKind : uint8_t { Power, Energy, Percent, Temperature, Boolean, Voltage, Current };

// Units are captured from the live template response and reused for Recorder
// history. The history request deliberately uses `no_attributes`, so it cannot
// discover a unit from each historical state without downloading substantially
// more JSON.
enum class HaUnit : uint8_t {
  Unknown = 0,
  Watts,
  Kilowatts,
  WattHours,
  KilowattHours,
  Percent,
  Celsius,
  Volts,
  Millivolts,
  Amps,
  Milliamps,
};

enum class HaValueStatus : uint8_t {
  Available = 0,
  Unavailable,
  InvalidNumber,
  UnsupportedUnit,
};

struct HaEntityDescriptor {
  const char* payload_key;
  const char* nvs_key;
  const char* form_name;
  const char* label;
  const char* placeholder;
  HaValueKind kind;
};

static constexpr size_t HA_ENTITY_COUNT = static_cast<size_t>(HaEntity::Count);
static constexpr size_t HA_FORECAST_ENTITY_FIRST =
    static_cast<size_t>(HaEntity::ForecastToday);
// PV1..PV4: as many string pairs as the HA mappings carry.
static constexpr size_t HA_PV_STRINGS = 4;
static constexpr size_t HA_ENTITY_ID_MAX = 96;
extern const HaEntityDescriptor HA_ENTITIES[HA_ENTITY_COUNT];

struct HaParseInfo {
  size_t configured = 0;
  size_t available = 0;
  size_t unavailable = 0;
  size_t invalid_number = 0;
  size_t unsupported_unit = 0;
  uint32_t local_midnight_ts = 0;
  uint32_t next_local_midnight_ts = 0;
  HaUnit units[HA_ENTITY_COUNT] = {};
};

// Which side of the plant the mapped EV charger sits on. It decides where the
// EV comes off: an AC charger is house wiring, so house load is less the AC
// charger; a DC charger leaves the DC bus and counts in the losses balance
// instead (power_balance.h). Stored in NVS as its value, so append only.
enum class HaEvKind : uint8_t {
  Auto = 0,  // from the entity ID: "dc_charger" in it means DC, otherwise AC
  Ac = 1,
  Dc = 2,
};

// Resolves Auto against the mapped EV entity. The default mapping,
// sensor.sigen_inverter_dc_charger_output_power, comes out DC.
bool ha_ev_is_dc(HaEvKind kind, const char* entity_id);

// Inputs to the parse that come from settings rather than from the payload.
struct HaParseOptions {
  bool ev_is_dc = true;  // matches the default EV mapping
};

bool ha_entity_id_valid(const char* entity_id);

HaUnit ha_unit_from_text(const char* unit);
HaValueStatus ha_normalise_state(const char* state, HaUnit unit, HaValueKind kind,
                                 MaybeFloat* out);

// Each mapped entity costs 85 characters plus its key and its ID twice, and an ID
// may be 96: all 29 at the limit come to 8,310 with the prefix. Allocated only
// while the request is built, and freed before the TLS handshake.
static constexpr size_t HA_TEMPLATE_MAX = 9216;
// Builds the Jinja template posted to Home Assistant. Only configured entities
// are referenced, so HA does no all-state download and the response stays small.
bool ha_template_build(const char* const entity_ids[HA_ENTITY_COUNT], char* out,
                       size_t out_size);

// Parses the compact JSON rendered by the /api/template request. A malformed
// document returns false and leaves *out untouched. Individual bad states remain
// unknown and are counted in info, allowing a partial response to stay useful.
//
// With no "Home/load power" mapped, house load is worked out on the AC side
// from the plant entity, and losses whenever their inputs are known; see
// power_balance.h. A mapped home entity is used as it is.
bool ha_payload_parse(const char* json, size_t length, Snapshot* out,
                      HaParseInfo* info = nullptr, const HaParseOptions* options = nullptr);

// Maps REST status codes without requiring a live Home Assistant in tests.
enum class HaHttpStatus : uint8_t { Ok, Unauthorised, HttpError };
HaHttpStatus ha_http_status(int status_code);
