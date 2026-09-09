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
  ForecastToday,
  ForecastRemaining,
  ForecastPercentage,
  ForecastPeak,
  Count,
};

enum class HaValueKind : uint8_t { Power, Energy, Percent, Temperature, Boolean };

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

bool ha_entity_id_valid(const char* entity_id);

HaUnit ha_unit_from_text(const char* unit);
HaValueStatus ha_normalise_state(const char* state, HaUnit unit, HaValueKind kind,
                                 MaybeFloat* out);

static constexpr size_t HA_TEMPLATE_MAX = 6144;
// Builds the Jinja template posted to Home Assistant. Only configured entities
// are referenced, so HA does no all-state download and the response stays small.
bool ha_template_build(const char* const entity_ids[HA_ENTITY_COUNT], char* out,
                       size_t out_size);

// Parses the compact JSON rendered by the /api/template request. A malformed
// document returns false and leaves *out untouched. Individual bad states remain
// unknown and are counted in info, allowing a partial response to stay useful.
bool ha_payload_parse(const char* json, size_t length, Snapshot* out,
                      HaParseInfo* info = nullptr);

// Maps REST status codes without requiring a live Home Assistant in tests.
enum class HaHttpStatus : uint8_t { Ok, Unauthorised, HttpError };
HaHttpStatus ha_http_status(int status_code);
